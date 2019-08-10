/*
 * virenum.c: enum value conversion helpers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <config.h>

#include "virenum.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_ENUM_IMPL(virTristateBool,
              VIR_TRISTATE_BOOL_LAST,
              "default",
              "yes",
              "no",
);

VIR_ENUM_IMPL(virTristateSwitch,
              VIR_TRISTATE_SWITCH_LAST,
              "default",
              "on",
              "off",
);


virTristateBool
virTristateBoolFromBool(bool val)
{
    if (val)
        return VIR_TRISTATE_BOOL_YES;
    else
        return VIR_TRISTATE_BOOL_NO;
}


virTristateSwitch
virTristateSwitchFromBool(bool val)
{
    if (val)
        return VIR_TRISTATE_SWITCH_ON;
    else
        return VIR_TRISTATE_SWITCH_OFF;
}


int
virEnumFromString(const char * const *types,
                  unsigned int ntypes,
                  const char *type)
{
    size_t i;
    if (!type)
        return -1;

    for (i = 0; i < ntypes; i++)
        if (STREQ(types[i], type))
            return i;

    return -1;
}


const char *
virEnumToString(const char * const *types,
                unsigned int ntypes,
                int type)
{
    if (type < 0 || type >= ntypes)
        return NULL;

    return types[type];
}
