/*
 * Copyright (C) 2016 Red Hat, Inc.
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
 *
 * Author: Michal Privoznik <mprivozn@redhat.com>
 */

#include <config.h>

#ifdef NSS
# include <stdio.h>
# include <stdlib.h>
# include <dlfcn.h>
# include <sys/types.h>
# include <dirent.h>
# include <sys/stat.h>
# include <fcntl.h>

# include "configmake.h"
# include "internal.h"
# include "virstring.h"
# include "viralloc.h"

static int (*realopen)(const char *path, int flags, ...);
static DIR * (*realopendir)(const char *name);

# define LEASEDIR LOCALSTATEDIR "/lib/libvirt/dnsmasq/"

# define STDERR(...)                                                    \
    fprintf(stderr, "%s %zu: ", __FUNCTION__, (size_t) __LINE__);       \
    fprintf(stderr, __VA_ARGS__);                                       \
    fprintf(stderr, "\n");                                              \

# define ABORT(...)                                                     \
    do {                                                                \
        STDERR(__VA_ARGS__);                                            \
        abort();                                                        \
    } while (0)

# define ABORT_OOM()                                                    \
    ABORT("Out of memory")

/*
 * Functions to load the symbols and init the environment
 */
static void
init_syms(void)
{
    if (realopen)
        return;

# define LOAD_SYM(name)                                                 \
    do {                                                                \
        if (!(real ## name = dlsym(RTLD_NEXT, #name)))                  \
            ABORT("Cannot find real '%s' symbol\n", #name);             \
    } while (0)

    LOAD_SYM(open);
    LOAD_SYM(opendir);
}

static int
getrealpath(char **newpath,
            const char *path)
{
    if (STRPREFIX(path, LEASEDIR)) {
        if (virAsprintfQuiet(newpath, "%s/nssdata/%s",
                             abs_srcdir,
                             path + strlen(LEASEDIR)) < 0) {
            errno = ENOMEM;
            return -1;
        }
    } else {
        if (VIR_STRDUP_QUIET(*newpath, path) < 0)
            return -1;
    }

    return 0;
}

int
open(const char *path, int flags, ...)
{
    int ret;
    char *newpath = NULL;

    init_syms();

    if (STRPREFIX(path, LEASEDIR) &&
        getrealpath(&newpath, path) < 0)
        return -1;

    if (flags & O_CREAT) {
        va_list ap;
        mode_t mode;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
        ret = realopen(newpath ? newpath : path, flags, mode);
    } else {
        ret = realopen(newpath ? newpath : path, flags);
    }

    VIR_FREE(newpath);
    return ret;
}

DIR *
opendir(const char *path)
{
    DIR *ret;
    char *newpath = NULL;

    init_syms();

    if (STRPREFIX(path, LEASEDIR) &&
        getrealpath(&newpath, path) < 0)
        return NULL;

    ret = realopendir(newpath ? newpath : path);

    VIR_FREE(newpath);
    return ret;
}
#else
/* Nothing to override if NSS plugin is not enabled */
#endif
