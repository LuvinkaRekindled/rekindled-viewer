/**
 * @file llrenderheaders.h
 * @brief LLGL definitions
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLGLHEADERS_H
#define LL_LLGLHEADERS_H

// F5: this header used to pull in the OpenGL headers (gl.h, glext.h, the extension function pointers, WGL). The viewer runs without OpenGL (Vulkan device, no context) and its code
// names no GL symbol (F3.6g gate); what is left here is what the files that include it relied on.

#if LL_WINDOWS
// windows.h and its friends (the GL headers used to bring them)
#include "llwin32headers.h"
#endif

// the Vulkan render thread's shared header (the resource tables of the typed command records)
#include "llvkworker.h"

#endif // LL_LLGLHEADERS_H
