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
 */

#ifndef _RDPECAM_PIPEWIRE_H_
#define _RDPECAM_PIPEWIRE_H_

#include "arch.h"

/** Installs the PipeWire sink. Connects to PipeWire lazily */
void
rdpecam_pipewire_init(void);

/** Disconnects from PipeWire. Call when chansrv exits */
void
rdpecam_pipewire_deinit(void);

int
rdpecam_pipewire_get_wait_objs(tbus *objs, int *count, int *timeout);

int
rdpecam_pipewire_check_wait_objs(void);

#endif
