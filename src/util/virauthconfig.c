/*
 * virauthconfig.c: authentication config handling
 *
 * Copyright (C) 2012 Red Hat, Inc.
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

#include "virauthconfig.h"

#include "virkeyfile.h"
#include "virlog.h"
#include "virerror.h"
#include "virstring.h"
#include "viralloc.h"

struct _virAuthConfig {
    virKeyFilePtr keyfile;
    char *path;
};

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("util.authconfig");

virAuthConfigPtr virAuthConfigNew(const char *path)
{
    virAuthConfigPtr auth;

    if (VIR_ALLOC(auth) < 0)
        goto error;

    auth->path = g_strdup(path);

    if (!(auth->keyfile = virKeyFileNew()))
        goto error;

    if (virKeyFileLoadFile(auth->keyfile, path) < 0)
        goto error;

    return auth;

 error:
    virAuthConfigFree(auth);
    return NULL;
}


virAuthConfigPtr virAuthConfigNewData(const char *path,
                                      const char *data,
                                      size_t len)
{
    virAuthConfigPtr auth;

    if (VIR_ALLOC(auth) < 0)
        goto error;

    auth->path = g_strdup(path);

    if (!(auth->keyfile = virKeyFileNew()))
        goto error;

    if (virKeyFileLoadData(auth->keyfile, path, data, len) < 0)
        goto error;

    return auth;

 error:
    virAuthConfigFree(auth);
    return NULL;
}


void virAuthConfigFree(virAuthConfigPtr auth)
{
    if (!auth)
        return;

    virKeyFileFree(auth->keyfile);
    VIR_FREE(auth->path);
    VIR_FREE(auth);
}


int virAuthConfigLookup(virAuthConfigPtr auth,
                        const char *service,
                        const char *hostname,
                        const char *credname,
                        const char **value)
{
    g_autofree char *authgroup = NULL;
    g_autofree char *credgroup = NULL;
    const char *authcred;

    *value = NULL;

    VIR_DEBUG("Lookup '%s' '%s' '%s'", service, NULLSTR(hostname), credname);

    if (!hostname)
        hostname = "localhost";

    authgroup = g_strdup_printf("auth-%s-%s", service, hostname);

    if (!virKeyFileHasGroup(auth->keyfile, authgroup)) {
       VIR_FREE(authgroup);
       authgroup = g_strdup_printf("auth-%s-%s", service, "default");
    }

    if (!virKeyFileHasGroup(auth->keyfile, authgroup))
        return 0;

    if (!(authcred = virKeyFileGetValueString(auth->keyfile, authgroup, "credentials"))) {
        virReportError(VIR_ERR_CONF_SYNTAX,
                       _("Missing item 'credentials' in group '%s' in '%s'"),
                       authgroup, auth->path);
        return -1;
    }

    credgroup = g_strdup_printf("credentials-%s", authcred);

    if (!virKeyFileHasGroup(auth->keyfile, credgroup)) {
        virReportError(VIR_ERR_CONF_SYNTAX,
                       _("Missing group 'credentials-%s' referenced from group '%s' in '%s'"),
                       authcred, authgroup, auth->path);
        return -1;
    }

    if (!virKeyFileHasValue(auth->keyfile, credgroup, credname))
        return 0;

    *value = virKeyFileGetValueString(auth->keyfile, credgroup, credname);

    return 0;
}
