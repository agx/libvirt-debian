/*
 * storage_backend_zfs.c: storage backend for ZFS handling
 *
 * Copyright (C) 2014 Roman Bogorodskiy
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
 */

#include <config.h>

#include "viralloc.h"
#include "virerror.h"
#include "virfile.h"
#include "storage_backend_zfs.h"
#include "virlog.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE

VIR_LOG_INIT("storage.storage_backend_zfs");

/*
 * Some common flags of zfs and zpool commands we use:
 * -H -- don't print headers and separate fields by tab
 * -p -- show exact numbers instead of human-readable, i.e.
 *       for size, show just a number instead of 2G etc
 */


static int
virStorageBackendZFSCheckPool(virConnectPtr conn ATTRIBUTE_UNUSED,
                              virStoragePoolObjPtr pool ATTRIBUTE_UNUSED,
                              bool *isActive)
{
    char *devpath;

    if (virAsprintf(&devpath, "/dev/zvol/%s",
                    pool->def->source.name) == -1)
        return -1;
    *isActive = virFileIsDir(devpath);
    VIR_FREE(devpath);

    return 0;
}

static int
virStorageBackendZFSParseVol(virStoragePoolObjPtr pool,
                             const char *volume)
{
    int ret = -1;
    char **tokens;
    size_t count;
    char **name_tokens = NULL;
    char *vol_name;
    bool is_new_vol = false;
    virStorageVolDefPtr vol = NULL;

    if (!(tokens = virStringSplitCount(volume, "\t", 0, &count)))
        return -1;

    if (count != 2)
        goto cleanup;

    if (!(name_tokens = virStringSplit(tokens[0], "/", 2)))
        goto cleanup;

    vol_name = name_tokens[1];

    vol = virStorageVolDefFindByName(pool, vol_name);

    if (vol == NULL) {
        if (VIR_ALLOC(vol) < 0)
            goto cleanup;

        is_new_vol = true;
        vol->type = VIR_STORAGE_VOL_BLOCK;

        if (VIR_STRDUP(vol->name, vol_name) < 0)
            goto cleanup;
    }

    if (!vol->key && VIR_STRDUP(vol->key, tokens[0]) < 0)
        goto cleanup;

    if (vol->target.path == NULL) {
        if (virAsprintf(&vol->target.path, "%s/%s",
                        pool->def->target.path, vol->name) < 0)
            goto cleanup;
    }

    if (virStrToLong_ull(tokens[1], NULL, 10, &vol->target.capacity) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("malformed volsize reported"));
        goto cleanup;
    }

    if (is_new_vol &&
        VIR_APPEND_ELEMENT(pool->volumes.objs, pool->volumes.count, vol) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virStringFreeList(tokens);
    virStringFreeList(name_tokens);
    if (is_new_vol && (ret == -1))
        virStorageVolDefFree(vol);
    return ret;
}

static int
virStorageBackendZFSFindVols(virStoragePoolObjPtr pool)
{
    virCommandPtr cmd = NULL;
    char *volumes_list = NULL;
    char **lines = NULL;
    size_t i;

    /**
     * $ zfs list -Hp -t volume -o name,volsize -r test
     * test/vol1       5368709120
     * test/vol3       1073741824
     * test/vol4       1572864000
     * $
     *
     * Arguments description:
     *  -t volume -- we want to see only volumes
     *  -o name,volsize -- limit output to name and volume size
     *  -r -- we want to see all the childer of our pool
     */
    cmd = virCommandNewArgList(ZFS,
                               "list", "-Hp",
                               "-t", "volume", "-r",
                               "-o", "name,volsize",
                               pool->def->source.name,
                               NULL);
    virCommandSetOutputBuffer(cmd, &volumes_list);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (!(lines = virStringSplit(volumes_list, "\n", 0)))
        goto cleanup;

    for (i = 0; lines[i]; i++) {
        if (STREQ(lines[i], ""))
            continue;

        if (virStorageBackendZFSParseVol(pool, lines[i]) < 0)
            continue;
    }

 cleanup:
    virCommandFree(cmd);
    virStringFreeList(lines);
    VIR_FREE(volumes_list);

    return 0;
}

static int
virStorageBackendZFSRefreshPool(virConnectPtr conn ATTRIBUTE_UNUSED,
                                virStoragePoolObjPtr pool ATTRIBUTE_UNUSED)
{
    virCommandPtr cmd = NULL;
    char *zpool_props = NULL;
    char **lines = NULL;
    char **tokens = NULL;
    size_t i;

    /**
     * $ zpool get -Hp health,size,free,allocated test
     * test    health  ONLINE  -
     * test    size    199715979264    -
     * test    free    198899976704    -
     * test    allocated       816002560       -
     * $
     *
     * Here we just provide a list of properties we want to see
     */
    cmd = virCommandNewArgList(ZPOOL,
                               "get", "-Hp",
                               "health,size,free,allocated",
                               pool->def->source.name,
                               NULL);
    virCommandSetOutputBuffer(cmd, &zpool_props);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (!(lines = virStringSplit(zpool_props, "\n", 0)))
        goto cleanup;

    for (i = 0; lines[i]; i++) {
        size_t count;
        char *prop_name;

        if (STREQ(lines[i], ""))
            continue;

        virStringFreeList(tokens);
        if (!(tokens = virStringSplitCount(lines[i], "\t", 0, &count)))
            goto cleanup;

        if (count != 4)
            continue;

        prop_name = tokens[1];

        if (STREQ(prop_name, "free") || STREQ(prop_name, "size") ||
            STREQ(prop_name, "allocated")) {
            unsigned long long value;
            if (virStrToLong_ull(tokens[2], NULL, 10, &value) < 0)
                goto cleanup;

            if (STREQ(prop_name, "free"))
                pool->def->available = value;
            else if (STREQ(prop_name, "size"))
                pool->def->capacity = value;
            else if (STREQ(prop_name, "allocated"))
                pool->def->allocation = value;
        }
    }

    /* Obtain a list of volumes */
    if (virStorageBackendZFSFindVols(pool) < 0)
        goto cleanup;

 cleanup:
    virCommandFree(cmd);
    virStringFreeList(lines);
    virStringFreeList(tokens);
    VIR_FREE(zpool_props);

    return 0;
}

static int
virStorageBackendZFSCreateVol(virConnectPtr conn ATTRIBUTE_UNUSED,
                              virStoragePoolObjPtr pool,
                              virStorageVolDefPtr vol)
{
    virCommandPtr cmd = NULL;
    int ret = -1;

    vol->type = VIR_STORAGE_VOL_BLOCK;

    if (vol->target.path != NULL) {
        /* A target path passed to CreateVol has no meaning */
        VIR_FREE(vol->target.path);
    }

    if (virAsprintf(&vol->target.path, "%s/%s",
                    pool->def->target.path, vol->name) == -1)
        return -1;

    if (VIR_STRDUP(vol->key, vol->target.path) < 0)
        goto cleanup;

    /**
     * $ zfs create -o volmode=dev -V 10240K test/volname
     *
     * -o volmode=dev -- we want to get volumes exposed as cdev
     *                   devices. If we don't specify that zfs
     *                   will lookup vfs.zfs.vol.mode sysctl value
     * -V -- tells to create a volume with the specified size
     */
    cmd = virCommandNewArgList(ZFS, "create", "-o", "volmode=dev",
                               "-V", NULL);
    virCommandAddArgFormat(cmd, "%lluK",
                           VIR_DIV_UP(vol->target.capacity, 1024));
    virCommandAddArgFormat(cmd, "%s/%s",
                           pool->def->source.name, vol->name);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (virStorageBackendZFSFindVols(pool) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virCommandFree(cmd);
    return ret;

}

static int
virStorageBackendZFSDeleteVol(virConnectPtr conn ATTRIBUTE_UNUSED,
                              virStoragePoolObjPtr pool,
                              virStorageVolDefPtr vol,
                              unsigned int flags)
{
    int ret = -1;
    virCommandPtr destroy_cmd = virCommandNewArgList(ZFS, "destroy", NULL);

    virCheckFlags(0, -1);

    virCommandAddArgFormat(destroy_cmd, "%s/%s",
                           pool->def->source.name, vol->name);

    if (virCommandRun(destroy_cmd, NULL) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virCommandFree(destroy_cmd);
    return ret;
}


virStorageBackend virStorageBackendZFS = {
    .type = VIR_STORAGE_POOL_ZFS,

    .checkPool = virStorageBackendZFSCheckPool,
    .refreshPool = virStorageBackendZFSRefreshPool,
    .createVol = virStorageBackendZFSCreateVol,
    .deleteVol = virStorageBackendZFSDeleteVol,
    .uploadVol = virStorageBackendVolUploadLocal,
    .downloadVol = virStorageBackendVolDownloadLocal,
};
