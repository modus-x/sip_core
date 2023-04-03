/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *  Author: Philippe Gorley <philippe.gorley@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#pragma once

// Generic helper definitions for shared library support
#if defined _WIN32 || defined __CYGWIN__
#define LIBSIP_CORE_IMPORT __declspec(dllimport)
#define LIBSIP_CORE_EXPORT __declspec(dllexport)
#define LIBSIP_CORE_HIDDEN
#else
#define LIBSIP_CORE_IMPORT __attribute__((visibility("default")))
#define LIBSIP_CORE_EXPORT __attribute__((visibility("default")))
#define LIBSIP_CORE_HIDDEN __attribute__((visibility("hidden")))
#endif

// Now we use the generic helper definitions above to define LIBSIP_CORE_PUBLIC and LIBSIP_CORE_LOCAL.
// LIBSIP_CORE_PUBLIC is used for the public API symbols. It is either DLL imports or DLL exports (or does
// nothing for static build) LIBSIP_CORE_LOCAL is used for non-api symbols.

#ifdef sip_core_EXPORTS // defined if sip_core is compiled as a shared library
#ifdef LIBSIP_CORE_BUILD  // defined if we are building the sip_core shared library (instead of using it)
#define LIBSIP_CORE_PUBLIC LIBSIP_CORE_EXPORT
#else
#define LIBSIP_CORE_PUBLIC LIBSIP_CORE_IMPORT
#endif // LIBSIP_CORE_BUILD
#define LIBSIP_CORE_LOCAL LIBSIP_CORE_HIDDEN
#else // sip_core_EXPORTS is not defined: this means sip_core is a static lib.
#define LIBSIP_CORE_PUBLIC
#define LIBSIP_CORE_LOCAL
#endif // sip_core_EXPORTS

#ifdef DEBUG
#define LIBSIP_CORE_TESTABLE LIBSIP_CORE_EXPORT
#else
#define LIBSIP_CORE_TESTABLE
#endif
