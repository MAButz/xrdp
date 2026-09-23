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
 * MS-RDPECAM - Video Capture Virtual Channel Extension
 *
 * The server opens the device enumeration channel. The client answers
 * with a version request and then announces each camera together with
 * the name of a device channel, which the server opens in turn. On a
 * device channel the server asks for the streams and media types once,
 * and later starts a stream and pulls samples with sample requests.
 *
 * Each device runs a small state machine. The sink says whether it
 * wants a stream (and in which media type); dev_advance() then sends
 * whatever request moves the device towards that, one request at a
 * time.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "defines.h"
#include "os_calls.h"
#include "string_calls.h"
#include "parse.h"
#include "log.h"
#include "chansrv.h"
#include "rdpecam.h"

#define RDPECAM_ENUM_NAME "RDCamera_Device_Enumerator"
#define RDPECAM_FLAGS 1 /* WTS_CHANNEL_OPTION_DYNAMIC */

/* Highest protocol version we speak. [MS-RDPECAM] Appendix A:
 * version 2 is Windows 10 v1809 and later */
#define RDPECAM_VERSION 2

/* [MS-RDPECAM] 2.2.1 CAM_MSG_ID */
#define CAM_MSG_ID_SuccessResponse              0x01
#define CAM_MSG_ID_ErrorResponse                0x02
#define CAM_MSG_ID_SelectVersionRequest         0x03
#define CAM_MSG_ID_SelectVersionResponse        0x04
#define CAM_MSG_ID_DeviceAddedNotification      0x05
#define CAM_MSG_ID_DeviceRemovedNotification    0x06
#define CAM_MSG_ID_ActivateDeviceRequest        0x07
#define CAM_MSG_ID_DeactivateDeviceRequest      0x08
#define CAM_MSG_ID_StreamListRequest            0x09
#define CAM_MSG_ID_StreamListResponse           0x0A
#define CAM_MSG_ID_MediaTypeListRequest         0x0B
#define CAM_MSG_ID_MediaTypeListResponse        0x0C
#define CAM_MSG_ID_CurrentMediaTypeRequest      0x0D
#define CAM_MSG_ID_CurrentMediaTypeResponse     0x0E
#define CAM_MSG_ID_StartStreamsRequest          0x0F
#define CAM_MSG_ID_StopStreamsRequest           0x10
#define CAM_MSG_ID_SampleRequest                0x11
#define CAM_MSG_ID_SampleResponse               0x12
#define CAM_MSG_ID_SampleErrorResponse          0x13

/* [MS-RDPECAM] 2.2.3.2 CAM_STREAM_DESCRIPTION */
#define CAM_STREAM_DESCRIPTION_SIZE 5
#define CAM_STREAM_FRAME_SOURCE_TYPE_Color 0x0001
#define CAM_STREAM_CATEGORY_Capture 0x01

/* [MS-RDPECAM] 2.2.3.1 CAM_MEDIA_TYPE_DESCRIPTION */
#define CAM_MEDIA_TYPE_DESCRIPTION_SIZE 26

#define MAX_MEDIA_TYPES 64
#define MAX_CHANNEL_NAME 256
#define MAX_DEVICE_NAME 256

/* Sample requests kept outstanding while streaming. With a single one
 * every frame costs a full round trip, which on a slower link caps the
 * frame rate below the camera's (25 instead of 30 fps measured through
 * a VPN) */
#define SAMPLE_CREDITS 3

enum dev_state
{
    DEV_UNUSED = 0,
    DEV_OPEN_SENT,   /* waiting for the device channel to open */
    DEV_IDLE,        /* channel open, device not activated */
    DEV_ACTIVE,      /* activated, no stream running */
    DEV_STREAMING,   /* stream running, samples flowing */
    DEV_CLOSE_SENT,  /* waiting for the device channel to close */
    DEV_FAILED       /* initialisation failed, left alone */
};

struct rdpecam_device
{
    enum dev_state state;
    int chan_id;
    char channel_name[MAX_CHANNEL_NAME];
    char name[MAX_DEVICE_NAME];

    /* request we are waiting for a response to, or 0 */
    int pending;
    /* the client removed the device while its channel was opening */
    int removed;

    /* results of the initialisation */
    int init_done;
    int have_stream;
    int stream_index;
    int have_media_types;
    int num_media_types;
    struct rdpecam_media_type media_types[MAX_MEDIA_TYPES];

    /* what the sink wants */
    int want_stream;
    int want_media_type;

    /* what is running */
    int cur_media_type;
    int samples_outstanding;
};

static struct rdpecam_device g_devices[RDPECAM_MAX_DEVICES];
static const struct rdpecam_sink *g_sink;
static struct chansrv_drdynvc_procs g_enum_procs;
static struct chansrv_drdynvc_procs g_dev_procs;
static int g_enum_chan_id;
static int g_enum_open;
static int g_version;

/*****************************************************************************/
const char *
rdpecam_format_to_str(int format)
{
    switch (format)
    {
        case RDPECAM_FORMAT_H264:
            return "H264";
        case RDPECAM_FORMAT_MJPG:
            return "MJPG";
        case RDPECAM_FORMAT_YUY2:
            return "YUY2";
        case RDPECAM_FORMAT_NV12:
            return "NV12";
        case RDPECAM_FORMAT_I420:
            return "I420";
        case RDPECAM_FORMAT_RGB24:
            return "RGB24";
        case RDPECAM_FORMAT_RGB32:
            return "RGB32";
    }
    return "unknown";
}

/*****************************************************************************/
static const char *
msg_id_to_str(int msg_id)
{
    switch (msg_id)
    {
        case CAM_MSG_ID_SuccessResponse:
            return "SuccessResponse";
        case CAM_MSG_ID_ErrorResponse:
            return "ErrorResponse";
        case CAM_MSG_ID_SelectVersionRequest:
            return "SelectVersionRequest";
        case CAM_MSG_ID_SelectVersionResponse:
            return "SelectVersionResponse";
        case CAM_MSG_ID_DeviceAddedNotification:
            return "DeviceAddedNotification";
        case CAM_MSG_ID_DeviceRemovedNotification:
            return "DeviceRemovedNotification";
        case CAM_MSG_ID_ActivateDeviceRequest:
            return "ActivateDeviceRequest";
        case CAM_MSG_ID_DeactivateDeviceRequest:
            return "DeactivateDeviceRequest";
        case CAM_MSG_ID_StreamListRequest:
            return "StreamListRequest";
        case CAM_MSG_ID_StreamListResponse:
            return "StreamListResponse";
        case CAM_MSG_ID_MediaTypeListRequest:
            return "MediaTypeListRequest";
        case CAM_MSG_ID_MediaTypeListResponse:
            return "MediaTypeListResponse";
        case CAM_MSG_ID_StartStreamsRequest:
            return "StartStreamsRequest";
        case CAM_MSG_ID_StopStreamsRequest:
            return "StopStreamsRequest";
        case CAM_MSG_ID_SampleRequest:
            return "SampleRequest";
        case CAM_MSG_ID_SampleResponse:
            return "SampleResponse";
        case CAM_MSG_ID_SampleErrorResponse:
            return "SampleErrorResponse";
    }
    return "unknown";
}

/*****************************************************************************/
static struct rdpecam_device *
dev_from_chan_id(int chan_id)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_devices[index].state != DEV_UNUSED &&
                g_devices[index].chan_id == chan_id)
        {
            return g_devices + index;
        }
    }
    return NULL;
}

/*****************************************************************************/
static struct rdpecam_device *
dev_from_channel_name(const char *channel_name)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_devices[index].state != DEV_UNUSED &&
                g_strcmp(g_devices[index].channel_name, channel_name) == 0)
        {
            return g_devices + index;
        }
    }
    return NULL;
}

/*****************************************************************************/
static int
dev_index(const struct rdpecam_device *dev)
{
    return (int)(dev - g_devices);
}

/*****************************************************************************/
/* Sends a message consisting of the header and 'bytes' of payload */
static int
send_msg(int chan_id, int msg_id, const char *payload, int bytes)
{
    struct stream *s;
    int error;

    make_stream(s);
    init_stream(s, 2 + bytes);
    out_uint8(s, g_version);
    out_uint8(s, msg_id);
    if (bytes > 0)
    {
        out_uint8a(s, payload, bytes);
    }
    s_mark_end(s);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "rdpecam: send %s on channel %d",
              msg_id_to_str(msg_id), chan_id);
    error = chansrv_drdynvc_send_data(chan_id, s->data,
                                      (int)(s->end - s->data));
    free_stream(s);
    return error;
}

/*****************************************************************************/
static int
dev_request(struct rdpecam_device *dev, int msg_id,
            const char *payload, int bytes)
{
    dev->pending = msg_id;
    return send_msg(dev->chan_id, msg_id, payload, bytes);
}

/*****************************************************************************/
static void
out_media_type(struct stream *s, const struct rdpecam_media_type *mt)
{
    out_uint8(s, mt->format);
    out_uint32_le(s, mt->width);
    out_uint32_le(s, mt->height);
    out_uint32_le(s, mt->frame_rate_numerator);
    out_uint32_le(s, mt->frame_rate_denominator);
    out_uint32_le(s, mt->pixel_aspect_ratio_numerator);
    out_uint32_le(s, mt->pixel_aspect_ratio_denominator);
    out_uint8(s, mt->flags);
}

/*****************************************************************************/
static void
in_media_type(struct stream *s, struct rdpecam_media_type *mt)
{
    in_uint8(s, mt->format);
    in_uint32_le(s, mt->width);
    in_uint32_le(s, mt->height);
    in_uint32_le(s, mt->frame_rate_numerator);
    in_uint32_le(s, mt->frame_rate_denominator);
    in_uint32_le(s, mt->pixel_aspect_ratio_numerator);
    in_uint32_le(s, mt->pixel_aspect_ratio_denominator);
    in_uint8(s, mt->flags);
}

/*****************************************************************************/
static void
dev_send_sample_requests(struct rdpecam_device *dev)
{
    char stream_index = (char)dev->stream_index;

    while (dev->samples_outstanding < SAMPLE_CREDITS)
    {
        if (send_msg(dev->chan_id, CAM_MSG_ID_SampleRequest,
                     &stream_index, 1) != 0)
        {
            break;
        }
        dev->samples_outstanding++;
    }
}

/*****************************************************************************/
static void
dev_send_start_streams(struct rdpecam_device *dev)
{
    struct stream *s;

    /* [MS-RDPECAM] 2.2.3.10: an array of CAM_START_STREAM_INFO, we only
     * ever start the one stream */
    make_stream(s);
    init_stream(s, 1 + CAM_MEDIA_TYPE_DESCRIPTION_SIZE);
    out_uint8(s, dev->stream_index);
    out_media_type(s, dev->media_types + dev->want_media_type);
    s_mark_end(s);
    dev_request(dev, CAM_MSG_ID_StartStreamsRequest, s->data,
                (int)(s->end - s->data));
    free_stream(s);
}

/*****************************************************************************/
/* Sends the next request that brings the device closer to what the sink
 * wants. Does nothing while a request is outstanding */
static void
dev_advance(struct rdpecam_device *dev)
{
    char stream_index = (char)dev->stream_index;

    if (dev->pending != 0)
    {
        return;
    }

    if (!dev->init_done)
    {
        /* Device initialisation: activate, then ask for the streams and
         * the media types of the capture stream */
        if (!dev->have_stream || !dev->have_media_types)
        {
            switch (dev->state)
            {
                case DEV_IDLE:
                    dev_request(dev, CAM_MSG_ID_ActivateDeviceRequest,
                                NULL, 0);
                    break;
                case DEV_ACTIVE:
                    if (!dev->have_stream)
                    {
                        dev_request(dev, CAM_MSG_ID_StreamListRequest,
                                    NULL, 0);
                    }
                    else
                    {
                        dev_request(dev, CAM_MSG_ID_MediaTypeListRequest,
                                    &stream_index, 1);
                    }
                    break;
                default:
                    break;
            }
            return;
        }

        dev->init_done = 1;
        LOG(LOG_LEVEL_INFO, "rdpecam: camera %d \"%s\" ready, %d media types",
            dev_index(dev), dev->name, dev->num_media_types);
        if (g_sink != NULL && g_sink->device_added != NULL)
        {
            g_sink->device_added(dev_index(dev), dev->name,
                                 dev->media_types, dev->num_media_types);
        }
        if (dev->pending != 0)
        {
            /* the sink started a stream from the callback */
            return;
        }
        /* Without a stream wanted, the code below releases the camera */
    }

    if (dev->want_stream)
    {
        switch (dev->state)
        {
            case DEV_IDLE:
                dev_request(dev, CAM_MSG_ID_ActivateDeviceRequest, NULL, 0);
                break;
            case DEV_ACTIVE:
                dev_send_start_streams(dev);
                break;
            case DEV_STREAMING:
                if (dev->cur_media_type != dev->want_media_type)
                {
                    /* restarted in the other media type once stopped */
                    dev_request(dev, CAM_MSG_ID_StopStreamsRequest, NULL, 0);
                }
                break;
            default:
                break;
        }
    }
    else
    {
        switch (dev->state)
        {
            case DEV_STREAMING:
                dev_request(dev, CAM_MSG_ID_StopStreamsRequest, NULL, 0);
                break;
            case DEV_ACTIVE:
                dev_request(dev, CAM_MSG_ID_DeactivateDeviceRequest, NULL, 0);
                break;
            default:
                break;
        }
    }
}

/*****************************************************************************/
static void
dev_stream_ended(struct rdpecam_device *dev, int error)
{
    dev->samples_outstanding = 0;
    if (g_sink != NULL && g_sink->stream_stopped != NULL)
    {
        g_sink->stream_stopped(dev_index(dev), error);
    }
}

/*****************************************************************************/
/* Forgets a device. Does not touch the channel */
static void
dev_forget(struct rdpecam_device *dev, int client_gone)
{
    int was_ready = dev->init_done;

    if (dev->state == DEV_STREAMING)
    {
        dev_stream_ended(dev, 0);
    }
    g_memset(dev, 0, sizeof(*dev));
    if (was_ready && g_sink != NULL && g_sink->device_removed != NULL)
    {
        g_sink->device_removed(dev_index(dev), client_gone);
    }
}

/*****************************************************************************/
static int
dev_process_success(struct rdpecam_device *dev)
{
    int request = dev->pending;

    dev->pending = 0;
    switch (request)
    {
        case CAM_MSG_ID_ActivateDeviceRequest:
            dev->state = DEV_ACTIVE;
            break;
        case CAM_MSG_ID_DeactivateDeviceRequest:
            dev->state = DEV_IDLE;
            break;
        case CAM_MSG_ID_StartStreamsRequest:
            dev->state = DEV_STREAMING;
            dev->cur_media_type = dev->want_media_type;
            dev->samples_outstanding = 0;
            LOG(LOG_LEVEL_INFO, "rdpecam: camera %d streaming %s %ux%u",
                dev_index(dev),
                rdpecam_format_to_str(dev->media_types[dev->cur_media_type].format),
                dev->media_types[dev->cur_media_type].width,
                dev->media_types[dev->cur_media_type].height);
            if (g_sink != NULL && g_sink->stream_started != NULL)
            {
                g_sink->stream_started(dev_index(dev),
                                       dev->media_types + dev->cur_media_type);
            }
            if (dev->state == DEV_STREAMING && dev->want_stream &&
                    dev->cur_media_type == dev->want_media_type)
            {
                dev_send_sample_requests(dev);
            }
            break;
        case CAM_MSG_ID_StopStreamsRequest:
            dev->state = DEV_ACTIVE;
            LOG(LOG_LEVEL_INFO, "rdpecam: camera %d stopped", dev_index(dev));
            dev_stream_ended(dev, 0);
            break;
        default:
            LOG(LOG_LEVEL_WARNING, "rdpecam: unexpected SuccessResponse "
                "on camera %d", dev_index(dev));
            break;
    }
    dev_advance(dev);
    return 0;
}

/*****************************************************************************/
static int
dev_process_error(struct rdpecam_device *dev, struct stream *s)
{
    int request = dev->pending;
    int error_code = 0;

    if (s_check_rem(s, 4))
    {
        in_uint32_le(s, error_code);
    }
    dev->pending = 0;
    LOG(LOG_LEVEL_WARNING, "rdpecam: camera %d \"%s\": %s failed, "
        "error code 0x%x", dev_index(dev), dev->name,
        msg_id_to_str(request), error_code);

    if (!dev->init_done)
    {
        /* Nothing sensible to offer. Leave the device alone but keep the
         * channel, so a later DeviceRemovedNotification still matches */
        dev->state = DEV_FAILED;
        return 0;
    }

    switch (request)
    {
        case CAM_MSG_ID_ActivateDeviceRequest:
        case CAM_MSG_ID_StartStreamsRequest:
            /* don't retry on our own, the sink has to ask again */
            dev->want_stream = 0;
            dev_stream_ended(dev, error_code != 0 ? error_code : 1);
            break;
        case CAM_MSG_ID_StopStreamsRequest:
            /* the stream is not running as far as we are concerned */
            dev->state = DEV_ACTIVE;
            dev_stream_ended(dev, 0);
            break;
        case CAM_MSG_ID_DeactivateDeviceRequest:
            dev->state = DEV_IDLE;
            break;
        default:
            break;
    }
    dev_advance(dev);
    return 0;
}

/*****************************************************************************/
static int
dev_process_stream_list(struct rdpecam_device *dev, struct stream *s)
{
    int count;
    int index;
    int frame_source_types;
    int category;
    int selected;
    int can_be_shared;

    dev->pending = 0;
    count = (int)(s_rem(s) / CAM_STREAM_DESCRIPTION_SIZE);
    for (index = 0; index < count; index++)
    {
        in_uint16_le(s, frame_source_types);
        in_uint8(s, category);
        in_uint8(s, selected);
        in_uint8(s, can_be_shared);
        LOG(LOG_LEVEL_DEBUG, "rdpecam: camera %d stream %d: frame source "
            "types 0x%x category %d selected %d can be shared %d",
            dev_index(dev), index, frame_source_types, category, selected,
            can_be_shared);
        if (!dev->have_stream &&
                (frame_source_types & CAM_STREAM_FRAME_SOURCE_TYPE_Color) &&
                category == CAM_STREAM_CATEGORY_Capture)
        {
            dev->have_stream = 1;
            dev->stream_index = index;
        }
    }
    if (!dev->have_stream)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam: camera %d \"%s\" has no colour "
            "capture stream", dev_index(dev), dev->name);
        dev->state = DEV_FAILED;
        return 0;
    }
    dev_advance(dev);
    return 0;
}

/*****************************************************************************/
static int
dev_process_media_type_list(struct rdpecam_device *dev, struct stream *s)
{
    struct rdpecam_media_type *mt;
    int count;
    int index;

    dev->pending = 0;
    count = (int)(s_rem(s) / CAM_MEDIA_TYPE_DESCRIPTION_SIZE);
    if (count > MAX_MEDIA_TYPES)
    {
        LOG(LOG_LEVEL_INFO, "rdpecam: camera %d offers %d media types, "
            "using the first %d", dev_index(dev), count, MAX_MEDIA_TYPES);
        count = MAX_MEDIA_TYPES;
    }
    for (index = 0; index < count; index++)
    {
        mt = dev->media_types + index;
        in_media_type(s, mt);
        LOG(LOG_LEVEL_DEBUG, "rdpecam: camera %d media type %d: %s %ux%u "
            "%u/%u fps flags 0x%x", dev_index(dev), index,
            rdpecam_format_to_str(mt->format), mt->width, mt->height,
            mt->frame_rate_numerator, mt->frame_rate_denominator,
            mt->flags);
    }
    dev->num_media_types = count;
    if (count == 0)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam: camera %d \"%s\" offers no media "
            "types", dev_index(dev), dev->name);
        dev->state = DEV_FAILED;
        return 0;
    }
    dev->have_media_types = 1;
    dev_advance(dev);
    return 0;
}

/*****************************************************************************/
static int
dev_process_sample(struct rdpecam_device *dev, struct stream *s)
{
    int stream_index;

    if (!s_check_rem(s, 1))
    {
        return 1;
    }
    in_uint8(s, stream_index);
    if (dev->samples_outstanding > 0)
    {
        dev->samples_outstanding--;
    }
    if (dev->state != DEV_STREAMING || stream_index != dev->stream_index)
    {
        /* a late sample after a stop */
        return 0;
    }
    if (g_sink != NULL && g_sink->sample != NULL)
    {
        g_sink->sample(dev_index(dev), s->p, (int)s_rem(s));
    }
    if (dev->state == DEV_STREAMING && dev->want_stream &&
            dev->cur_media_type == dev->want_media_type)
    {
        dev_send_sample_requests(dev);
    }
    return 0;
}

/*****************************************************************************/
static int
dev_process_sample_error(struct rdpecam_device *dev, struct stream *s)
{
    int stream_index = 0;
    int error_code = 0;

    if (s_check_rem(s, 5))
    {
        in_uint8(s, stream_index);
        in_uint32_le(s, error_code);
    }
    if (dev->samples_outstanding > 0)
    {
        dev->samples_outstanding--;
    }
    LOG(LOG_LEVEL_WARNING, "rdpecam: camera %d stream %d: sample error "
        "0x%x", dev_index(dev), stream_index, error_code);
    /* The client could not deliver a sample. Don't keep asking, which
     * would loop as fast as the channel allows; stop the stream and let
     * the sink decide */
    if (dev->state == DEV_STREAMING)
    {
        dev->want_stream = 0;
        dev_advance(dev);
    }
    return 0;
}

/*****************************************************************************/
static int
dev_data(int chan_id, struct stream *s)
{
    struct rdpecam_device *dev;
    int msg_id;

    dev = dev_from_chan_id(chan_id);
    if (dev == NULL || !s_check_rem(s, 2))
    {
        return 1;
    }
    in_uint8s(s, 1); /* version, negotiated on the enumerator */
    in_uint8(s, msg_id);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "rdpecam: camera %d received %s",
              dev_index(dev), msg_id_to_str(msg_id));

    switch (msg_id)
    {
        case CAM_MSG_ID_SuccessResponse:
            return dev_process_success(dev);
        case CAM_MSG_ID_ErrorResponse:
            return dev_process_error(dev, s);
        case CAM_MSG_ID_StreamListResponse:
            return dev_process_stream_list(dev, s);
        case CAM_MSG_ID_MediaTypeListResponse:
            return dev_process_media_type_list(dev, s);
        case CAM_MSG_ID_SampleResponse:
            return dev_process_sample(dev, s);
        case CAM_MSG_ID_SampleErrorResponse:
            return dev_process_sample_error(dev, s);
        default:
            LOG(LOG_LEVEL_WARNING, "rdpecam: camera %d: unexpected message "
                "0x%02x", dev_index(dev), msg_id);
            break;
    }
    return 0;
}

/*****************************************************************************/
static int
dev_open_response(int chan_id, int creation_status)
{
    struct rdpecam_device *dev = dev_from_chan_id(chan_id);

    if (dev == NULL)
    {
        return 0;
    }
    if (creation_status != 0)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam: client refused channel %s, "
            "status 0x%x", dev->channel_name, creation_status);
        dev_forget(dev, 0);
        return 0;
    }
    if (dev->removed)
    {
        dev->state = DEV_CLOSE_SENT;
        chansrv_drdynvc_close(dev->chan_id);
        return 0;
    }
    dev->state = DEV_IDLE;
    dev_advance(dev);
    return 0;
}

/*****************************************************************************/
static int
dev_close_response(int chan_id)
{
    struct rdpecam_device *dev = dev_from_chan_id(chan_id);

    if (dev != NULL)
    {
        LOG(LOG_LEVEL_INFO, "rdpecam: camera %d \"%s\" channel closed",
            dev_index(dev), dev->name);
        dev_forget(dev, 0);
    }
    return 0;
}

/*****************************************************************************/
static int
enum_process_select_version(int client_version)
{
    g_version = MIN(client_version, RDPECAM_VERSION);
    LOG(LOG_LEVEL_INFO, "rdpecam: client speaks version %d, using %d",
        client_version, g_version);
    return send_msg(g_enum_chan_id, CAM_MSG_ID_SelectVersionResponse,
                    NULL, 0);
}

/*****************************************************************************/
static int
enum_process_device_added(struct stream *s)
{
    struct rdpecam_device *dev = NULL;
    char name[MAX_DEVICE_NAME];
    char channel_name[MAX_CHANNEL_NAME];
    int index;
    int bytes;

    in_utf16_le_terminated_as_utf8(s, name, sizeof(name));
    bytes = 0;
    while (s_check_rem(s, 1) && bytes < (int)sizeof(channel_name) - 1)
    {
        in_uint8(s, channel_name[bytes]);
        if (channel_name[bytes] == '\0')
        {
            break;
        }
        bytes++;
    }
    channel_name[bytes] = '\0';
    if (bytes == 0)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam: camera \"%s\" without channel name",
            name);
        return 0;
    }
    if (dev_from_channel_name(channel_name) != NULL)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam: camera channel %s announced twice",
            channel_name);
        return 0;
    }
    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_devices[index].state == DEV_UNUSED)
        {
            dev = g_devices + index;
            break;
        }
    }
    if (dev == NULL)
    {
        LOG(LOG_LEVEL_WARNING, "rdpecam: too many cameras, ignoring \"%s\"",
            name);
        return 0;
    }

    LOG(LOG_LEVEL_INFO, "rdpecam: client camera \"%s\" on channel %s",
        name, channel_name);
    g_memset(dev, 0, sizeof(*dev));
    g_strncpy(dev->name, name, sizeof(dev->name) - 1);
    g_strncpy(dev->channel_name, channel_name, sizeof(dev->channel_name) - 1);
    dev->state = DEV_OPEN_SENT;
    if (chansrv_drdynvc_open(dev->channel_name, RDPECAM_FLAGS, &g_dev_procs,
                             &dev->chan_id) != 0)
    {
        LOG(LOG_LEVEL_ERROR, "rdpecam: can't open channel %s",
            dev->channel_name);
        g_memset(dev, 0, sizeof(*dev));
    }
    return 0;
}

/*****************************************************************************/
static int
enum_process_device_removed(struct stream *s)
{
    struct rdpecam_device *dev;
    char channel_name[MAX_CHANNEL_NAME];
    int bytes = 0;

    while (s_check_rem(s, 1) && bytes < (int)sizeof(channel_name) - 1)
    {
        in_uint8(s, channel_name[bytes]);
        if (channel_name[bytes] == '\0')
        {
            break;
        }
        bytes++;
    }
    channel_name[bytes] = '\0';

    dev = dev_from_channel_name(channel_name);
    if (dev == NULL)
    {
        return 0;
    }
    LOG(LOG_LEVEL_INFO, "rdpecam: client removed camera \"%s\"", dev->name);
    if (dev->state == DEV_OPEN_SENT)
    {
        /* closed as soon as the open response arrives */
        dev->removed = 1;
        return 0;
    }
    if (dev->state == DEV_CLOSE_SENT)
    {
        return 0;
    }
    if (dev->state == DEV_STREAMING)
    {
        dev_stream_ended(dev, 0);
    }
    dev->state = DEV_CLOSE_SENT;
    chansrv_drdynvc_close(dev->chan_id);
    return 0;
}

/*****************************************************************************/
static int
enum_data(int chan_id, struct stream *s)
{
    int version;
    int msg_id;

    if (!s_check_rem(s, 2))
    {
        return 1;
    }
    in_uint8(s, version);
    in_uint8(s, msg_id);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "rdpecam: enumerator received %s version %d",
              msg_id_to_str(msg_id), version);

    switch (msg_id)
    {
        case CAM_MSG_ID_SelectVersionRequest:
            return enum_process_select_version(version);
        case CAM_MSG_ID_DeviceAddedNotification:
            return enum_process_device_added(s);
        case CAM_MSG_ID_DeviceRemovedNotification:
            return enum_process_device_removed(s);
        default:
            LOG(LOG_LEVEL_WARNING, "rdpecam: enumerator: unexpected message "
                "0x%02x", msg_id);
            break;
    }
    return 0;
}

/*****************************************************************************/
static int
enum_open_response(int chan_id, int creation_status)
{
    if (creation_status != 0)
    {
        /* The client has no camera support, or it is turned off */
        LOG(LOG_LEVEL_INFO, "rdpecam: client does not offer camera "
            "redirection, status 0x%x", creation_status);
        g_enum_chan_id = 0;
        return 0;
    }
    LOG(LOG_LEVEL_DEBUG, "rdpecam: device enumeration channel open");
    g_enum_open = 1;
    return 0;
}

/*****************************************************************************/
static int
enum_close_response(int chan_id)
{
    g_enum_open = 0;
    g_enum_chan_id = 0;
    return 0;
}

/*****************************************************************************/
void
rdpecam_set_sink(const struct rdpecam_sink *sink)
{
    g_sink = sink;
}

/*****************************************************************************/
int
rdpecam_init(void)
{
    g_memset(g_devices, 0, sizeof(g_devices));
    g_memset(&g_enum_procs, 0, sizeof(g_enum_procs));
    g_enum_procs.open_response = enum_open_response;
    g_enum_procs.close_response = enum_close_response;
    g_enum_procs.data = enum_data;
    g_memset(&g_dev_procs, 0, sizeof(g_dev_procs));
    g_dev_procs.open_response = dev_open_response;
    g_dev_procs.close_response = dev_close_response;
    g_dev_procs.data = dev_data;
    g_enum_chan_id = 0;
    g_enum_open = 0;
    g_version = RDPECAM_VERSION;
    return 0;
}

/*****************************************************************************/
/* The client connection is gone, and with it all channels */
int
rdpecam_deinit(void)
{
    int index;

    for (index = 0; index < RDPECAM_MAX_DEVICES; index++)
    {
        if (g_devices[index].state != DEV_UNUSED)
        {
            dev_forget(g_devices + index, 1);
        }
    }
    g_enum_chan_id = 0;
    g_enum_open = 0;
    return 0;
}

/*****************************************************************************/
int
rdpecam_start(void)
{
    int error;

    if (g_enum_chan_id != 0)
    {
        return 0;
    }
    error = chansrv_drdynvc_open(RDPECAM_ENUM_NAME, RDPECAM_FLAGS,
                                 &g_enum_procs, &g_enum_chan_id);
    if (error != 0)
    {
        LOG(LOG_LEVEL_ERROR, "rdpecam: can't open %s", RDPECAM_ENUM_NAME);
        g_enum_chan_id = 0;
    }
    else if (g_sink != NULL && g_sink->client_connected != NULL)
    {
        g_sink->client_connected();
    }
    return error;
}

/*****************************************************************************/
int
rdpecam_start_stream(int dev_idx, int media_type_index)
{
    struct rdpecam_device *dev;

    if (dev_idx < 0 || dev_idx >= RDPECAM_MAX_DEVICES)
    {
        return 1;
    }
    dev = g_devices + dev_idx;
    if (!dev->init_done || dev->state == DEV_FAILED ||
            dev->state == DEV_CLOSE_SENT ||
            media_type_index < 0 || media_type_index >= dev->num_media_types)
    {
        return 1;
    }
    dev->want_stream = 1;
    dev->want_media_type = media_type_index;
    dev_advance(dev);
    return 0;
}

/*****************************************************************************/
int
rdpecam_stop_stream(int dev_idx)
{
    struct rdpecam_device *dev;

    if (dev_idx < 0 || dev_idx >= RDPECAM_MAX_DEVICES)
    {
        return 1;
    }
    dev = g_devices + dev_idx;
    if (dev->state == DEV_UNUSED)
    {
        return 1;
    }
    dev->want_stream = 0;
    dev_advance(dev);
    return 0;
}
