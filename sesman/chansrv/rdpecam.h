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
 * This module speaks the protocol. What a camera looks like inside the
 * session (v4l2loopback, PipeWire, ...) is up to a sink, which gets told
 * about devices and samples through struct rdpecam_sink and asks for a
 * stream with rdpecam_start_stream() / rdpecam_stop_stream().
 */

#ifndef _RDPECAM_H_
#define _RDPECAM_H_

#include "arch.h"

/* [MS-RDPECAM] 2.2.3.1 CAM_MEDIA_FORMAT */
enum rdpecam_format
{
    RDPECAM_FORMAT_H264 = 0x01,
    RDPECAM_FORMAT_MJPG = 0x02,
    RDPECAM_FORMAT_YUY2 = 0x03,
    RDPECAM_FORMAT_NV12 = 0x04,
    RDPECAM_FORMAT_I420 = 0x05,
    RDPECAM_FORMAT_RGB24 = 0x06,
    RDPECAM_FORMAT_RGB32 = 0x07
};

/* [MS-RDPECAM] 2.2.3.1 CAM_MEDIA_TYPE_DESCRIPTION */
struct rdpecam_media_type
{
    int format; /* enum rdpecam_format */
    unsigned int width;
    unsigned int height;
    unsigned int frame_rate_numerator;
    unsigned int frame_rate_denominator;
    unsigned int pixel_aspect_ratio_numerator;
    unsigned int pixel_aspect_ratio_denominator;
    int flags;
};

/* Devices are identified by a small index, stable while the device exists */
#define RDPECAM_MAX_DEVICES 8

/**
 * Callbacks from the protocol handler to whatever provides the camera
 * inside the session. All of them are optional.
 */
struct rdpecam_sink
{
    /** A client camera is available. The media types stay valid until
     *  device_removed() is called for the device */
    void (*device_added)(int dev, const char *name,
                         const struct rdpecam_media_type *types,
                         int num_types);
    /** The camera is gone. client_gone is set when the whole client
     *  connection went away (the same camera may come back with a
     *  reconnect), and clear when the client removed the camera or its
     *  channel closed. A running stream has already been stopped */
    void (*device_removed)(int dev, int client_gone);
    /** The client has started sending samples in the given media type */
    void (*stream_started)(int dev, const struct rdpecam_media_type *type);
    /** The stream has stopped, either because the sink asked for it
     *  (error == 0) or because the client reported an error */
    void (*stream_stopped)(int dev, int error);
    /** One sample (for H.264 an Annex-B access unit) */
    void (*sample)(int dev, const char *data, int bytes);
    /** A client (re)connected and camera enumeration is starting */
    void (*client_connected)(void);
};

/** Set the sink. Pass NULL to remove it */
void
rdpecam_set_sink(const struct rdpecam_sink *sink);

int
rdpecam_init(void);
int
rdpecam_deinit(void);

/**
 * Opens the device enumeration channel. Only call this when the client
 * has the drdynvc static channel
 */
int
rdpecam_start(void);

/**
 * Ask for samples from a device in one of the media types reported by
 * device_added(). Returns non-zero if the device or index is invalid.
 * The stream starts asynchronously, stream_started() confirms it.
 */
int
rdpecam_start_stream(int dev, int media_type_index);

/** Stop the samples of a device and release the client camera */
int
rdpecam_stop_stream(int dev);

const char *
rdpecam_format_to_str(int format);

#endif
