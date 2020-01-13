/*
 * virsh-completer-checkpoint.c: virsh completer callbacks related to checkpoints
 *
 * Copyright (C) 2019 Red Hat, Inc.
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

#include "virsh-completer-checkpoint.h"
#include "viralloc.h"
#include "virsh-util.h"
#include "virsh.h"
#include "virstring.h"

char **
virshCheckpointNameCompleter(vshControl *ctl,
                             const vshCmd *cmd,
                             unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virDomainPtr dom = NULL;
    virDomainCheckpointPtr *checkpoints = NULL;
    int ncheckpoints = 0;
    size_t i = 0;
    char **ret = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(dom = virshCommandOptDomain(ctl, cmd, NULL)))
        return NULL;

    if ((ncheckpoints = virDomainListAllCheckpoints(dom, &checkpoints,
                                                    flags)) < 0)
        goto error;

    if (VIR_ALLOC_N(ret, ncheckpoints + 1) < 0)
        goto error;

    for (i = 0; i < ncheckpoints; i++) {
        const char *name = virDomainCheckpointGetName(checkpoints[i]);

        ret[i] = g_strdup(name);

        virshDomainCheckpointFree(checkpoints[i]);
    }
    VIR_FREE(checkpoints);
    virshDomainFree(dom);

    return ret;

 error:
    for (; i < ncheckpoints; i++)
        virshDomainCheckpointFree(checkpoints[i]);
    VIR_FREE(checkpoints);
    for (i = 0; i < ncheckpoints; i++)
        VIR_FREE(ret[i]);
    VIR_FREE(ret);
    virshDomainFree(dom);
    return NULL;
}
