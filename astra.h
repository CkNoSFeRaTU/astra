/*
 * Astra
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2013, Andrey Dyldin <and@cesbo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _ASTRA_H_
#define _ASTRA_H_ 1

#include "core/asc.h"

#include "modules/astra/base.h"
#include "modules/astra/module_lua.h"
#include "modules/astra/module_stream.h"

#include "modules/mpegts/mpegts.h"

#define STACK_DEBUG() printf("%s:%d %s(): stack:%d\n"                                           \
                             , __FILE__, __LINE__, __FUNCTION__, lua_gettop(lua))

/* version */

#include "version.h"
#define __VSTR(_x) #_x
#define _VSTR(_x) __VSTR(_x)
#define _VERSION _VSTR(ASTRA_VERSION_MAJOR) "." _VSTR(ASTRA_VERSION_MINOR)

#if ASTRA_VERSION_DEV > 0
#   define _VDEV " dev:" _VSTR(ASTRA_VERSION_DEV)
#else
#   define _VDEV
#endif

#ifdef DEBUG
#   define _VDEBUG " debug"
#else
#   define _VDEBUG
#endif

#define ASTRA_VERSION_STR _VERSION _VDEV _VDEBUG

/* main app */

void astra_exit(void);
void astra_abort(void);

#endif /* _ASTRA_H_ */
