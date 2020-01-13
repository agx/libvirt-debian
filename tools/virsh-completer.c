/*
 * virsh-completer.c: virsh completer callbacks
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "virsh-completer.h"
#include "viralloc.h"
#include "virstring.h"

/**
 * A completer callback is a function that accepts three arguments:
 *
 *   @ctl: virsh control structure
 *   @cmd: parsed input
 *   @flags: optional flags to alter completer's behaviour
 *
 * The @ctl contains connection to the daemon (should the
 * completer need it). Any completer that requires a connection
 * must check whether connection is still alive.
 *
 * The @cmd contains parsed user input which might be missing
 * some arguments (if user is still typing the command), but may
 * already contain important data. For instance if the completer
 * needs domain XML it may inspect @cmd to find --domain. Using
 * existing wrappers is advised. If @cmd does not contain all
 * necessary bits, completer might return sensible defaults (i.e.
 * generic values not tailored to specific use case) or return
 * NULL (i.e. no strings are offered to the user for completion).
 *
 * The @flags contains a .completer_flags value defined for each
 * use or 0 if no .completer_flags were specified. If a completer
 * is generic enough @flags can be used to alter its behaviour.
 * For instance, a completer to fetch names of domains can use
 * @flags to return names of only domains in a particular state
 * that the command accepts.
 *
 * Under no circumstances should a completer output anything.
 * Neither to stdout nor to stderr. This would harm the user
 * experience.
 */


/**
 * virshCommaStringListComplete:
 * @input: user input so far
 * @options: ALL options available for argument
 *
 * Some arguments to our commands accept the following form:
 *
 *   virsh command --arg str1,str2,str3
 *
 * This does not play nicely with our completer funtions, because
 * they have to return strings prepended with user's input. For
 * instance:
 *
 *   str1,str2,str3,strA
 *   str1,str2,str3,strB
 *   str1,str2,str3,strC
 *
 * This helper function takes care of that. In this specific case
 * it would be called as follows:
 *
 *   virshCommaStringListComplete("str1,str2,str3",
 *                                {"strA", "strB", "strC", NULL});
 *
 * Returns: string list of completions on success,
 *          NULL otherwise.
 */
char **
virshCommaStringListComplete(const char *input,
                             const char **options)
{
    const size_t optionsLen = virStringListLength(options);
    g_autofree char *inputCopy = NULL;
    VIR_AUTOSTRINGLIST inputList = NULL;
    VIR_AUTOSTRINGLIST ret = NULL;
    size_t nret = 0;
    size_t i;

    if (STREQ_NULLABLE(input, " "))
        input = NULL;

    if (input) {
        char *comma = NULL;

        inputCopy = g_strdup(input);

        if ((comma = strrchr(inputCopy, ',')))
            *comma = '\0';
        else
            VIR_FREE(inputCopy);
    }

    if (inputCopy && !(inputList = virStringSplit(inputCopy, ",", 0)))
        return NULL;

    if (VIR_ALLOC_N(ret, optionsLen + 1) < 0)
        return NULL;

    for (i = 0; i < optionsLen; i++) {
        if (virStringListHasString((const char **)inputList, options[i]))
            continue;

        if (inputCopy)
            ret[nret] = g_strdup_printf("%s,%s", inputCopy, options[i]);
        else
            ret[nret] = g_strdup(options[i]);

        nret++;
    }

    return g_steal_pointer(&ret);
}
