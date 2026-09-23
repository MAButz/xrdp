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
 * Development sink for the RDPECAM module
 */

#ifndef _RDPECAM_DUMP_H_
#define _RDPECAM_DUMP_H_

/** Installs the dump sink if XRDP_RDPECAM_DUMP is set */
void
rdpecam_dump_init(void);

#endif
