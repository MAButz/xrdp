/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 Marc-Andre Beckmann-Butz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * PipeWire camera sink for the RDPECAM module
 *
 * Every client camera becomes a Video/Source node with media.role Camera
 * in the user's own PipeWire instance, so it is visible to that user only
 * and nothing needs root. The node offers the H.264 media types of the
 * camera as raw I420; the samples are decoded with openh264.
 *
 * The client camera only runs while something in the session consumes
 * the node: the stream going to STREAMING starts it, going back to
 * PAUSED stops it again.
 *
 * When the client disconnects, the nodes stay (offline, no new frames)
 * so applications keep their stream. A camera with the same name and
 * modes announced after a reconnect takes its node over again; nodes
 * nobody claims are removed a little after the reconnect.
 *
 * The PipeWire loop is driven from the chansrv main loop through its fd,
 * so everything here runs on the chansrv thread.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <wels/codec_api.h>

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "log.h"
#include "chansrv.h"
#include "rdpecam.h"
#include "rdpecam_pipewire.h"

#define MAX_OFFERED 32
/* Nodes left over from a previous connection are dropped this long after
 * the client has reconnected, unless a camera claimed them */
#define ORPHAN_GRACE_MS 30000

struct pw_camera
{
    int in_use;
    int dev;                    /* protocol device index, -1 while offline */
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    char name[256];
    char node_name[64];

    /* the offered formats and the protocol media type index of each */
    struct rdpecam_media_type offered[MAX_OFFERED];
    int offered_index[MAX_OFFERED];
    int num_offered;

    int format;                 /* negotiated, index into offered, or -1 */
    unsigned int width;
    unsigned int height;
    int consumed;               /* the node is linked to a consumer */

    ISVCDecoder *decoder;
    unsigned char *frame;       /* last decoded picture, packed I420 */
    int have_frame;
    uint32_t seq;
};

static struct pw_camera g_cams[RDPECAM_MAX_DEVICES];
static struct pw_loop *g_loop;
static struct pw_context *g_context;
static struct pw_core *g_core;
static struct spa_hook g_core_listener;
static int g_pw_initialised;
static int g_node_serial;

/*****************************************************************************/
static struct pw_camera *
cam_from_dev(int dev)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_cams[index].in_use && g_cams[index].dev == dev)
        {
            return g_cams + index;
        }
    }
    return NULL;
}

/*****************************************************************************/
static void
cam_free_decoder(struct pw_camera *cam)
{
    if (cam->decoder != NULL)
    {
        (*cam->decoder)->Uninitialize(cam->decoder);
        WelsDestroyDecoder(cam->decoder);
        cam->decoder = NULL;
    }
    free(cam->frame);
    cam->frame = NULL;
    cam->have_frame = 0;
}

/*****************************************************************************/
static int
cam_create_decoder(struct pw_camera *cam)
{
    SDecodingParam param;

    cam_free_decoder(cam);
    if (WelsCreateDecoder(&cam->decoder) != 0 || cam->decoder == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "rdpecam pipewire: can't create H.264 decoder");
        cam->decoder = NULL;
        return 1;
    }
    g_memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    if ((*cam->decoder)->Initialize(cam->decoder, &param) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "rdpecam pipewire: can't initialise H.264 "
            "decoder");
        WelsDestroyDecoder(cam->decoder);
        cam->decoder = NULL;
        return 1;
    }
    cam->frame = (unsigned char *)malloc(cam->width * cam->height * 3 / 2);
    if (cam->frame == NULL)
    {
        cam_free_decoder(cam);
        return 1;
    }
    return 0;
}

/*****************************************************************************/
/* Starts the client camera in the negotiated format, if it can */
static void
cam_start(struct pw_camera *cam)
{
    if (cam->dev < 0 || cam->format < 0)
    {
        return;
    }
    LOG(LOG_LEVEL_INFO, "rdpecam pipewire: camera %d in use, starting it at "
        "%ux%u", cam->dev, cam->width, cam->height);
    rdpecam_start_stream(cam->dev, cam->offered_index[cam->format]);
}

/*****************************************************************************/
static void
cam_destroy(struct pw_camera *cam)
{
    cam_free_decoder(cam);
    if (cam->stream != NULL)
    {
        spa_hook_remove(&cam->stream_listener);
        pw_stream_destroy(cam->stream);
    }
    LOG(LOG_LEVEL_INFO, "rdpecam pipewire: camera \"%s\" withdrawn",
        cam->name);
    g_memset(cam, 0, sizeof(*cam));
}

/*****************************************************************************/
static void
on_process(void *data)
{
    struct pw_camera *cam = (struct pw_camera *)data;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    struct spa_data *d;
    struct spa_meta_header *h;
    struct timespec ts;
    unsigned int size = cam->width * cam->height * 3 / 2;

    if (!cam->have_frame)
    {
        return;
    }
    b = pw_stream_dequeue_buffer(cam->stream);
    if (b == NULL)
    {
        /* all buffers are with the consumer, drop this picture */
        return;
    }
    buf = b->buffer;
    h = (struct spa_meta_header *)spa_buffer_find_meta_data(buf,
        SPA_META_Header, sizeof(*h));
    if (h != NULL)
    {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        h->pts = SPA_TIMESPEC_TO_NSEC(&ts);
        h->flags = 0;
        h->seq = cam->seq++;
        h->dts_offset = 0;
    }
    d = &buf->datas[0];
    if (d->data != NULL && d->maxsize >= size)
    {
        memcpy(d->data, cam->frame, size);
        d->chunk->offset = 0;
        d->chunk->size = size;
        d->chunk->stride = (int32_t)cam->width;
    }
    pw_stream_queue_buffer(cam->stream, b);
    cam->have_frame = 0;
}

/*****************************************************************************/
static void
on_state_changed(void *data, enum pw_stream_state old,
                 enum pw_stream_state state, const char *error)
{
    struct pw_camera *cam = (struct pw_camera *)data;

    LOG(LOG_LEVEL_DEBUG, "rdpecam pipewire: %s: %s -> %s%s%s",
        cam->node_name, pw_stream_state_as_string(old),
        pw_stream_state_as_string(state),
        error ? ": " : "", error ? error : "");

    if (state == PW_STREAM_STATE_STREAMING && !cam->consumed)
    {
        cam->consumed = 1;
        cam_start(cam);
    }
    else if (state != PW_STREAM_STATE_STREAMING && cam->consumed)
    {
        cam->consumed = 0;
        if (cam->dev >= 0)
        {
            LOG(LOG_LEVEL_INFO, "rdpecam pipewire: camera %d no longer in "
                "use", cam->dev);
            rdpecam_stop_stream(cam->dev);
        }
    }
}

/*****************************************************************************/
static void
on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct pw_camera *cam = (struct pw_camera *)data;
    struct spa_video_info_raw info;
    const struct rdpecam_media_type *mt;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    unsigned int size;
    int index;

    if (param == NULL || id != SPA_PARAM_Format)
    {
        return;
    }
    if (spa_format_video_raw_parse(param, &info) < 0)
    {
        return;
    }

    cam->format = -1;
    for (index = 0; index < cam->num_offered; index++)
    {
        mt = cam->offered + index;
        if (mt->width == info.size.width && mt->height == info.size.height &&
                mt->frame_rate_numerator == info.framerate.num &&
                mt->frame_rate_denominator == info.framerate.denom)
        {
            cam->format = index;
            break;
        }
    }
    if (cam->format < 0)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: negotiated %ux%u is not a "
            "camera media type", info.size.width, info.size.height);
        return;
    }
    cam->width = info.size.width;
    cam->height = info.size.height;
    size = cam->width * cam->height * 3 / 2;

    params[0] = spa_pod_builder_add_object(&b,
                                           SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                           SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
                                           SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                                           SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
                                           SPA_PARAM_BUFFERS_stride, SPA_POD_Int(cam->width));
    /* Consumers expect a header on camera buffers; the libwebrtc in
     * Firefox ESR 140 dereferences it unchecked */
    params[1] = spa_pod_builder_add_object(&b,
                                           SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                                           SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                                           SPA_PARAM_META_size,
                                           SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(cam->stream, params, 2);

    /* A consumer that renegotiates while streaming gets the new size */
    if (cam->consumed)
    {
        cam_start(cam);
    }
}

static const struct pw_stream_events g_stream_events =
{
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

/*****************************************************************************/
static void
on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: error on object %u: %s (%s)",
        id, message, spa_strerror(res));
}

static const struct pw_core_events g_core_events =
{
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

/*****************************************************************************/
/* Connects to the user's PipeWire. Returns non-zero if it isn't there */
static int
pw_connect(void)
{
    char path[64];

    if (g_core != NULL)
    {
        return 0;
    }
    if (!g_pw_initialised)
    {
        /* chansrv may be started without the session environment */
        if (getenv("XDG_RUNTIME_DIR") == NULL)
        {
            g_snprintf(path, sizeof(path), "/run/user/%d", (int)getuid());
            if (access(path, R_OK | X_OK) == 0)
            {
                setenv("XDG_RUNTIME_DIR", path, 0);
            }
        }
        pw_init(NULL, NULL);
        g_loop = pw_loop_new(NULL);
        if (g_loop == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "rdpecam pipewire: can't create loop");
            return 1;
        }
        pw_loop_enter(g_loop);
        g_context = pw_context_new(g_loop, NULL, 0);
        if (g_context == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "rdpecam pipewire: can't create context");
            return 1;
        }
        g_pw_initialised = 1;
    }
    g_core = pw_context_connect(g_context, NULL, 0);
    if (g_core == NULL)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: can't connect to "
            "PipeWire, cameras will not be offered");
        return 1;
    }
    pw_core_add_listener(g_core, &g_core_listener, &g_core_events, NULL);
    LOG(LOG_LEVEL_INFO, "rdpecam pipewire: connected to PipeWire");
    return 0;
}

/*****************************************************************************/
/* Collects the formats we can offer: H.264 modes, decoded to I420 */
static int
collect_formats(const struct rdpecam_media_type *types, int num_types,
                struct rdpecam_media_type *offered, int *offered_index)
{
    const struct rdpecam_media_type *mt;
    int count = 0;
    int index;

    for (index = 0; index < num_types && count < MAX_OFFERED; index++)
    {
        mt = types + index;
        if (mt->format != RDPECAM_FORMAT_H264 ||
                mt->width == 0 || mt->height == 0 ||
                (mt->width & 1) || (mt->height & 1) ||
                mt->frame_rate_denominator == 0)
        {
            continue;
        }
        offered[count] = *mt;
        offered_index[count] = index;
        count++;
    }
    return count;
}

/*****************************************************************************/
static int
same_formats(const struct pw_camera *cam,
             const struct rdpecam_media_type *offered, int num_offered)
{
    int index;

    if (cam->num_offered != num_offered)
    {
        return 0;
    }
    for (index = 0; index < num_offered; index++)
    {
        if (cam->offered[index].width != offered[index].width ||
                cam->offered[index].height != offered[index].height ||
                cam->offered[index].frame_rate_numerator !=
                offered[index].frame_rate_numerator ||
                cam->offered[index].frame_rate_denominator !=
                offered[index].frame_rate_denominator)
        {
            return 0;
        }
    }
    return 1;
}

/*****************************************************************************/
static int
cam_publish(struct pw_camera *cam)
{
    struct pw_properties *props;
    uint8_t buffer[8192];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[MAX_OFFERED];
    const struct rdpecam_media_type *mt;
    int index;
    int res;

    for (index = 0; index < cam->num_offered; index++)
    {
        mt = cam->offered + index;
        params[index] = spa_pod_builder_add_object(&b,
                        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_I420),
                        SPA_FORMAT_VIDEO_size,
                        SPA_POD_Rectangle(&SPA_RECTANGLE(mt->width, mt->height)),
                        SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_Fraction(&SPA_FRACTION(mt->frame_rate_numerator,
                                         mt->frame_rate_denominator)));
    }

    g_snprintf(cam->node_name, sizeof(cam->node_name), "xrdp-camera-%d",
               g_node_serial++);
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                              PW_KEY_MEDIA_CATEGORY, "Capture",
                              PW_KEY_MEDIA_ROLE, "Camera",
                              PW_KEY_MEDIA_CLASS, "Video/Source",
                              PW_KEY_NODE_NAME, cam->node_name,
                              PW_KEY_NODE_DESCRIPTION, cam->name,
                              NULL);
    cam->stream = pw_stream_new(g_core, cam->name, props);
    if (cam->stream == NULL)
    {
        LOG(LOG_LEVEL_ERROR, "rdpecam pipewire: can't create stream for "
            "\"%s\"", cam->name);
        return 1;
    }
    pw_stream_add_listener(cam->stream, &cam->stream_listener,
                           &g_stream_events, cam);
    res = pw_stream_connect(cam->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                            PW_STREAM_FLAG_DRIVER |
                            PW_STREAM_FLAG_MAP_BUFFERS,
                            params, cam->num_offered);
    if (res < 0)
    {
        LOG(LOG_LEVEL_ERROR, "rdpecam pipewire: can't connect stream for "
            "\"%s\": %s", cam->name, spa_strerror(res));
        spa_hook_remove(&cam->stream_listener);
        pw_stream_destroy(cam->stream);
        cam->stream = NULL;
        return 1;
    }
    LOG(LOG_LEVEL_INFO, "rdpecam pipewire: camera \"%s\" published as %s "
        "with %d formats", cam->name, cam->node_name, cam->num_offered);
    return 0;
}

/*****************************************************************************/
static void
sink_device_added(int dev, const char *name,
                  const struct rdpecam_media_type *types, int num_types)
{
    struct rdpecam_media_type offered[MAX_OFFERED];
    int offered_index[MAX_OFFERED];
    struct pw_camera *cam = NULL;
    int num_offered;
    int index;

    if (pw_connect() != 0)
    {
        return;
    }
    num_offered = collect_formats(types, num_types, offered, offered_index);
    if (num_offered == 0)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: camera \"%s\" offers no "
            "H.264 media type, not publishing it", name);
        return;
    }

    /* The same camera back after a reconnect keeps its node */
    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_cams[index].in_use && g_cams[index].dev < 0 &&
                g_strcmp(g_cams[index].name, name) == 0 &&
                same_formats(g_cams + index, offered, num_offered))
        {
            cam = g_cams + index;
            cam->dev = dev;
            g_memcpy(cam->offered_index, offered_index,
                     sizeof(offered_index));
            LOG(LOG_LEVEL_INFO, "rdpecam pipewire: camera \"%s\" is back, "
                "reusing %s", name, cam->node_name);
            if (cam->consumed)
            {
                cam_start(cam);
            }
            return;
        }
    }

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (!g_cams[index].in_use)
        {
            cam = g_cams + index;
            break;
        }
    }
    if (cam == NULL)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: no room for camera \"%s\"",
            name);
        return;
    }
    g_memset(cam, 0, sizeof(*cam));
    cam->dev = dev;
    cam->format = -1;
    g_strncpy(cam->name, name, sizeof(cam->name) - 1);
    g_memcpy(cam->offered, offered, sizeof(offered));
    g_memcpy(cam->offered_index, offered_index, sizeof(offered_index));
    cam->num_offered = num_offered;
    if (cam_publish(cam) == 0)
    {
        cam->in_use = 1;
    }
    else
    {
        g_memset(cam, 0, sizeof(*cam));
    }
}

/*****************************************************************************/
static void
sink_device_removed(int dev, int client_gone)
{
    struct pw_camera *cam = cam_from_dev(dev);

    if (cam == NULL)
    {
        return;
    }
    if (!client_gone)
    {
        cam_destroy(cam);
        return;
    }
    /* Keep the node for the reconnect, it just gets no frames meanwhile */
    cam_free_decoder(cam);
    cam->dev = -1;
    LOG(LOG_LEVEL_INFO, "rdpecam pipewire: client gone, keeping %s for a "
        "reconnect", cam->node_name);
}

/*****************************************************************************/
static void
drop_orphans(void *data)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_cams[index].in_use && g_cams[index].dev < 0)
        {
            LOG(LOG_LEVEL_INFO, "rdpecam pipewire: camera \"%s\" did not "
                "come back", g_cams[index].name);
            cam_destroy(g_cams + index);
        }
    }
}

/*****************************************************************************/
static void
sink_client_connected(void)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_cams[index].in_use && g_cams[index].dev < 0)
        {
            add_timeout(ORPHAN_GRACE_MS, drop_orphans, NULL);
            break;
        }
    }
}

/*****************************************************************************/
static void
sink_stream_started(int dev, const struct rdpecam_media_type *type)
{
    struct pw_camera *cam = cam_from_dev(dev);

    if (cam == NULL)
    {
        return;
    }
    if (type->width != cam->width || type->height != cam->height)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: camera %d started at "
            "%ux%u, expected %ux%u", dev, type->width, type->height,
            cam->width, cam->height);
    }
    cam_create_decoder(cam);
}

/*****************************************************************************/
static void
sink_stream_stopped(int dev, int error)
{
    struct pw_camera *cam = cam_from_dev(dev);

    if (cam == NULL)
    {
        return;
    }
    cam_free_decoder(cam);
    if (error != 0)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam pipewire: camera %d stopped by the "
            "client, error 0x%x", dev, error);
    }
}

/*****************************************************************************/
/* Copies a decoded picture into the packed I420 frame buffer */
static void
copy_picture(struct pw_camera *cam, unsigned char *yuv[3],
             const SBufferInfo *info)
{
    const SSysMEMBuffer *sb = &info->UsrData.sSystemBuffer;
    unsigned int w = cam->width;
    unsigned int h = cam->height;
    unsigned char *dst = cam->frame;
    unsigned int row;
    int plane;

    for (row = 0; row < h; row++)
    {
        memcpy(dst, yuv[0] + row * sb->iStride[0], w);
        dst += w;
    }
    for (plane = 1; plane < 3; plane++)
    {
        for (row = 0; row < h / 2; row++)
        {
            memcpy(dst, yuv[plane] + row * sb->iStride[1], w / 2);
            dst += w / 2;
        }
    }
}

/*****************************************************************************/
static void
sink_sample(int dev, const char *data, int bytes)
{
    struct pw_camera *cam = cam_from_dev(dev);
    unsigned char *yuv[3] = { NULL, NULL, NULL };
    SBufferInfo info;
    DECODING_STATE state;

    if (cam == NULL || cam->decoder == NULL || cam->frame == NULL)
    {
        return;
    }
    g_memset(&info, 0, sizeof(info));
    state = (*cam->decoder)->DecodeFrameNoDelay(cam->decoder,
            (const unsigned char *)data,
            bytes, yuv, &info);
    if (state != dsErrorFree)
    {
        LOG_DEVEL(LOG_LEVEL_DEBUG, "rdpecam pipewire: camera %d: decoder "
                  "state 0x%x", dev, state);
    }
    if (info.iBufferStatus != 1)
    {
        return;
    }
    if ((unsigned int)info.UsrData.sSystemBuffer.iWidth != cam->width ||
            (unsigned int)info.UsrData.sSystemBuffer.iHeight != cam->height)
    {
        LOG_DEVEL(LOG_LEVEL_DEBUG, "rdpecam pipewire: camera %d: picture "
                  "%dx%d does not match", dev,
                  info.UsrData.sSystemBuffer.iWidth,
                  info.UsrData.sSystemBuffer.iHeight);
        return;
    }
    copy_picture(cam, yuv, &info);
    cam->have_frame = 1;
    pw_stream_trigger_process(cam->stream);
}

static const struct rdpecam_sink g_pipewire_sink =
{
    sink_device_added,
    sink_device_removed,
    sink_stream_started,
    sink_stream_stopped,
    sink_sample,
    sink_client_connected
};

/*****************************************************************************/
void
rdpecam_pipewire_init(void)
{
    rdpecam_set_sink(&g_pipewire_sink);
}

/*****************************************************************************/
void
rdpecam_pipewire_deinit(void)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_cams[index].in_use)
        {
            cam_destroy(g_cams + index);
        }
    }
    if (g_core != NULL)
    {
        spa_hook_remove(&g_core_listener);
        pw_core_disconnect(g_core);
        g_core = NULL;
    }
    if (g_context != NULL)
    {
        pw_context_destroy(g_context);
        g_context = NULL;
    }
    if (g_loop != NULL)
    {
        pw_loop_leave(g_loop);
        pw_loop_destroy(g_loop);
        g_loop = NULL;
    }
    if (g_pw_initialised)
    {
        pw_deinit();
        g_pw_initialised = 0;
    }
}

/*****************************************************************************/
int
rdpecam_pipewire_get_wait_objs(tbus *objs, int *count, int *timeout)
{
    if (g_loop != NULL)
    {
        objs[*count] = pw_loop_get_fd(g_loop);
        (*count)++;
    }
    return 0;
}

/*****************************************************************************/
int
rdpecam_pipewire_check_wait_objs(void)
{
    if (g_loop != NULL)
    {
        pw_loop_iterate(g_loop, 0);
    }
    return 0;
}
