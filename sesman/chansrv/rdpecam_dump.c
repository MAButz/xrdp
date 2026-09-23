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
 * Development sink for the RDPECAM module: when XRDP_RDPECAM_DUMP is set
 * in the session environment, every client camera is started once and
 * XRDP_RDPECAM_DUMP_FRAMES samples (default 150) are written to
 * ${XRDP_RDPECAM_DUMP}.<device>.<format>. Not meant for production.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include "arch.h"
#include "os_calls.h"
#include "string_calls.h"
#include "log.h"
#include "rdpecam.h"
#include "rdpecam_dump.h"

struct dump_device
{
    FILE *fp;
    int frames;
};

static struct dump_device g_dump[RDPECAM_MAX_DEVICES];
static const char *g_prefix;
static int g_max_frames;

/*****************************************************************************/
/* Prefers H.264, then MJPG, at the size closest to 640x480 */
static int
pick_media_type(const struct rdpecam_media_type *types, int num_types)
{
    int best = 0;
    long best_score = -1;
    long score;
    long dist;
    int index;

    for (index = 0; index < num_types; index++)
    {
        dist = labs((long)types[index].width - 640) +
               labs((long)types[index].height - 480);
        score = (types[index].format == RDPECAM_FORMAT_H264) ? 2000000 :
                (types[index].format == RDPECAM_FORMAT_MJPG) ? 1000000 : 0;
        score -= dist;
        if (score > best_score)
        {
            best_score = score;
            best = index;
        }
    }
    return best;
}

/*****************************************************************************/
static void
dump_device_added(int dev, const char *name,
                  const struct rdpecam_media_type *types, int num_types)
{
    int mt = pick_media_type(types, num_types);

    LOG(LOG_LEVEL_INFO, "rdpecam dump: starting camera %d \"%s\" as %s "
        "%ux%u", dev, name, rdpecam_format_to_str(types[mt].format),
        types[mt].width, types[mt].height);
    rdpecam_start_stream(dev, mt);
}

/*****************************************************************************/
static void
dump_stream_started(int dev, const struct rdpecam_media_type *type)
{
    char path[512];

    g_snprintf(path, sizeof(path), "%s.%d.%s", g_prefix, dev,
               rdpecam_format_to_str(type->format));
    g_dump[dev].frames = 0;
    g_dump[dev].fp = fopen(path, "wb");
    LOG(LOG_LEVEL_INFO, "rdpecam dump: writing %d samples to %s",
        g_max_frames, path);
}

/*****************************************************************************/
static void
dump_close(int dev)
{
    if (g_dump[dev].fp != NULL)
    {
        fclose(g_dump[dev].fp);
        g_dump[dev].fp = NULL;
        LOG(LOG_LEVEL_INFO, "rdpecam dump: camera %d, %d samples written",
            dev, g_dump[dev].frames);
    }
}

/*****************************************************************************/
static void
dump_stream_stopped(int dev, int error)
{
    dump_close(dev);
}

/*****************************************************************************/
static void
dump_device_removed(int dev, int client_gone)
{
    dump_close(dev);
}

/*****************************************************************************/
static void
dump_sample(int dev, const char *data, int bytes)
{
    if (g_dump[dev].fp != NULL)
    {
        fwrite(data, 1, bytes, g_dump[dev].fp);
    }
    if (++g_dump[dev].frames >= g_max_frames)
    {
        rdpecam_stop_stream(dev);
    }
}

static const struct rdpecam_sink g_dump_sink =
{
    dump_device_added,
    dump_device_removed,
    dump_stream_started,
    dump_stream_stopped,
    dump_sample
};

/*****************************************************************************/
void
rdpecam_dump_init(void)
{
    const char *frames;

    g_prefix = g_getenv("XRDP_RDPECAM_DUMP");
    if (g_prefix == NULL || g_prefix[0] == '\0')
    {
        return;
    }
    frames = g_getenv("XRDP_RDPECAM_DUMP_FRAMES");
    g_max_frames = (frames != NULL) ? atoi(frames) : 0;
    if (g_max_frames <= 0)
    {
        g_max_frames = 150;
    }
    g_memset(g_dump, 0, sizeof(g_dump));
    rdpecam_set_sink(&g_dump_sink);
}
