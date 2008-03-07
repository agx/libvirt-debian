/*
 * storage_driver.c: core driver for storage APIs
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>

#include "driver.h"
#include "util.h"
#include "storage_driver.h"
#include "storage_conf.h"

#include "storage_backend.h"

#define storageLog(msg...) fprintf(stderr, msg)

static virStorageDriverStatePtr driverState;

static int storageDriverShutdown(void);


static void
storageDriverAutostart(virStorageDriverStatePtr driver) {
    virStoragePoolObjPtr pool;

    pool = driver->pools;
    while (pool != NULL) {
        virStoragePoolObjPtr next = pool->next;

        if (pool->autostart &&
            !virStoragePoolObjIsActive(pool)) {
            virStorageBackendPtr backend;
            if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
                storageLog("Missing backend %d",
                           pool->def->type);
                pool = next;
                continue;
            }

            if (backend->startPool &&
                backend->startPool(NULL, pool) < 0) {
                virErrorPtr err = virGetLastError();
                storageLog("Failed to autostart storage pool '%s': %s",
                           pool->def->name, err ? err->message : NULL);
                pool = next;
                continue;
            }

            if (backend->refreshPool(NULL, pool) < 0) {
                virErrorPtr err = virGetLastError();
                if (backend->stopPool)
                    backend->stopPool(NULL, pool);
                storageLog("Failed to autostart storage pool '%s': %s",
                           pool->def->name, err ? err->message : NULL);
                pool = next;
                continue;
            }
            pool->active = 1;
            driver->nactivePools++;
            driver->ninactivePools--;
        }

        pool = next;
    }
}

/**
 * virStorageStartup:
 *
 * Initialization function for the QEmu daemon
 */
static int
storageDriverStartup(void) {
    uid_t uid = geteuid();
    struct passwd *pw;
    char *base = NULL;
    char driverConf[PATH_MAX];

    if (!(driverState = calloc(1, sizeof(virStorageDriverState)))) {
        return -1;
    }

    if (!uid) {
        if ((base = strdup (SYSCONF_DIR "/libvirt")) == NULL)
            goto out_of_memory;
    } else {
        if (!(pw = getpwuid(uid))) {
            storageLog("Failed to find user record for uid '%d': %s",
                       uid, strerror(errno));
            goto out_of_memory;
        }

        if (asprintf (&base, "%s/.libvirt", pw->pw_dir) == -1) {
            storageLog("out of memory in asprintf");
            goto out_of_memory;
        }
    }

    /* Configuration paths are either ~/.libvirt/storage/... (session) or
     * /etc/libvirt/storage/... (system).
     */
    if (snprintf (driverConf, sizeof(driverConf),
                  "%s/storage.conf", base) == -1)
        goto out_of_memory;
    driverConf[sizeof(driverConf)-1] = '\0';

    if (asprintf (&driverState->configDir,
                  "%s/storage", base) == -1)
        goto out_of_memory;

    if (asprintf (&driverState->autostartDir,
                  "%s/storage/autostart", base) == -1)
        goto out_of_memory;

    free(base);
    base = NULL;

    /*
    if (virStorageLoadDriverConfig(driver, driverConf) < 0) {
        virStorageDriverShutdown();
        return -1;
    }
    */

    if (virStoragePoolObjScanConfigs(driverState) < 0) {
        storageDriverShutdown();
        return -1;
    }
    storageDriverAutostart(driverState);

    return 0;

 out_of_memory:
    storageLog("virStorageStartup: out of memory");
    free(base);
    free(driverState);
    driverState = NULL;
    return -1;
}

/**
 * virStorageReload:
 *
 * Function to restart the storage driver, it will recheck the configuration
 * files and update its state
 */
static int
storageDriverReload(void) {
    virStoragePoolObjScanConfigs(driverState);
    storageDriverAutostart(driverState);

    return 0;
}

/**
 * virStorageActive:
 *
 * Checks if the storage driver is active, i.e. has an active pool
 *
 * Returns 1 if active, 0 otherwise
 */
static int
storageDriverActive(void) {
    /* If we've any active networks or guests, then we
     * mark this driver as active
     */
    if (driverState->nactivePools)
        return 1;

    /* Otherwise we're happy to deal with a shutdown */
    return 0;
}

/**
 * virStorageShutdown:
 *
 * Shutdown the storage driver, it will stop all active storage pools
 */
static int
storageDriverShutdown(void) {
    virStoragePoolObjPtr pool;

    if (!driverState)
        return -1;

    /* shutdown active pools */
    pool = driverState->pools;
    while (pool) {
        virStoragePoolObjPtr next = pool->next;
        if (virStoragePoolObjIsActive(pool)) {
            virStorageBackendPtr backend;
            if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
                storageLog("Missing backend");
                continue;
            }

            if (backend->stopPool &&
                backend->stopPool(NULL, pool) < 0) {
                virErrorPtr err = virGetLastError();
                storageLog("Failed to stop storage pool '%s': %s",
                           pool->def->name, err->message);
            }
            virStoragePoolObjClearVols(pool);
        }
        pool = next;
    }

    /* free inactive pools */
    pool = driverState->pools;
    while (pool) {
        virStoragePoolObjPtr next = pool->next;
        virStoragePoolObjFree(pool);
        pool = next;
    }
    driverState->pools = NULL;
    driverState->nactivePools = 0;
    driverState->ninactivePools = 0;

    free(driverState->configDir);
    free(driverState->autostartDir);
    free(driverState);
    driverState = NULL;

    return 0;
}



static virStoragePoolPtr
storagePoolLookupByUUID(virConnectPtr conn,
                        const unsigned char *uuid) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, uuid);
    virStoragePoolPtr ret;

    if (!pool) {
        virStorageReportError(conn, VIR_ERR_NO_STORAGE_POOL,
                              "%s", _("no pool with matching uuid"));
        return NULL;
    }

    ret = virGetStoragePool(conn, pool->def->name, pool->def->uuid);
    return ret;
}

static virStoragePoolPtr
storagePoolLookupByName(virConnectPtr conn,
                        const char *name) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByName(driver, name);
    virStoragePoolPtr ret;

    if (!pool) {
        virStorageReportError(conn, VIR_ERR_NO_STORAGE_POOL,
                              "%s", _("no pool with matching name"));
        return NULL;
    }

    ret = virGetStoragePool(conn, pool->def->name, pool->def->uuid);
    return ret;
}

static virStoragePoolPtr
storagePoolLookupByVolume(virStorageVolPtr vol) {
    return storagePoolLookupByName(vol->conn, vol->pool);
}

static virDrvOpenStatus
storageOpen(virConnectPtr conn,
            xmlURIPtr uri ATTRIBUTE_UNUSED,
            virConnectAuthPtr auth ATTRIBUTE_UNUSED,
            int flags ATTRIBUTE_UNUSED) {
    if (!driverState)
        return VIR_DRV_OPEN_DECLINED;

    conn->storagePrivateData = driverState;
    return VIR_DRV_OPEN_SUCCESS;
}

static int
storageClose(virConnectPtr conn) {
    conn->storagePrivateData = NULL;
    return 0;
}

static int
storageNumPools(virConnectPtr conn) {
    virStorageDriverStatePtr driver
        = (virStorageDriverStatePtr)conn->storagePrivateData;
    return driver->nactivePools;
}

static int
storageListPools(virConnectPtr conn,
                 char **const names,
                 int nnames) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)conn->storagePrivateData;
    virStoragePoolObjPtr pool = driver->pools;
    int got = 0, i;
    while (pool && got < nnames) {
        if (virStoragePoolObjIsActive(pool)) {
            if (!(names[got] = strdup(pool->def->name))) {
                virStorageReportError(conn, VIR_ERR_NO_MEMORY,
                                      "%s", _("names"));
                goto cleanup;
            }
            got++;
        }
        pool = pool->next;
    }
    return got;

 cleanup:
    for (i = 0 ; i < got ; i++) {
        free(names[i]);
        names[i] = NULL;
    }
    memset(names, 0, nnames);
    return -1;
}

static int
storageNumDefinedPools(virConnectPtr conn) {
    virStorageDriverStatePtr driver
        = (virStorageDriverStatePtr)conn->storagePrivateData;
    return driver->ninactivePools;
}

static int
storageListDefinedPools(virConnectPtr conn,
                        char **const names,
                        int nnames) {
    virStorageDriverStatePtr driver
        = (virStorageDriverStatePtr)conn->storagePrivateData;
    virStoragePoolObjPtr pool = driver->pools;
    int got = 0, i;
    while (pool && got < nnames) {
        if (!virStoragePoolObjIsActive(pool)) {
            if (!(names[got] = strdup(pool->def->name))) {
                virStorageReportError(conn, VIR_ERR_NO_MEMORY,
                                      "%s", _("names"));
                goto cleanup;
            }
            got++;
        }
        pool = pool->next;
    }
    return got;

 cleanup:
    for (i = 0 ; i < got ; i++) {
        free(names[i]);
        names[i] = NULL;
    }
    memset(names, 0, nnames);
    return -1;
}

static virStoragePoolPtr
storagePoolCreate(virConnectPtr conn,
                  const char *xml,
                  unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr )conn->storagePrivateData;
    virStoragePoolDefPtr def;
    virStoragePoolObjPtr pool;
    virStoragePoolPtr ret;
    virStorageBackendPtr backend;

    if (!(def = virStoragePoolDefParse(conn, xml, NULL)))
        return NULL;

    if (virStoragePoolObjFindByUUID(driver, def->uuid) ||
        virStoragePoolObjFindByName(driver, def->name)) {
        virStorageReportError(conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool already exists"));
        virStoragePoolDefFree(def);
        return NULL;
    }

    if ((backend = virStorageBackendForType(def->type)) == NULL) {
        virStoragePoolDefFree(def);
        return NULL;
    }

    if (!(pool = virStoragePoolObjAssignDef(conn, driver, def))) {
        virStoragePoolDefFree(def);
        return NULL;
    }

    if (backend->startPool(conn, pool) < 0) {
        virStoragePoolObjRemove(driver, pool);
        return NULL;
    }
    pool->active = 1;
    driver->nactivePools++;
    driver->ninactivePools--;

    ret = virGetStoragePool(conn, pool->def->name, pool->def->uuid);

    return ret;
}

static virStoragePoolPtr
storagePoolDefine(virConnectPtr conn,
                  const char *xml,
                  unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver
        = (virStorageDriverStatePtr )conn->storagePrivateData;
    virStoragePoolDefPtr def;
    virStoragePoolObjPtr pool;
    virStoragePoolPtr ret;
    virStorageBackendPtr backend;

    if (!(def = virStoragePoolDefParse(conn, xml, NULL)))
        return NULL;

    if ((backend = virStorageBackendForType(def->type)) == NULL) {
        virStoragePoolDefFree(def);
        return NULL;
    }

    if (!(pool = virStoragePoolObjAssignDef(conn, driver, def))) {
        virStoragePoolDefFree(def);
        return NULL;
    }

    if (virStoragePoolObjSaveDef(conn, driver, pool, def) < 0) {
        virStoragePoolObjRemove(driver, pool);
        return NULL;
    }

    ret = virGetStoragePool(conn, pool->def->name, pool->def->uuid);
    return ret;
}

static int
storagePoolUndefine(virStoragePoolPtr obj) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if (virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("pool is still active"));
        return -1;
    }

    if (virStoragePoolObjDeleteDef(obj->conn, pool) < 0)
        return -1;

    if (unlink(pool->autostartLink) < 0 && errno != ENOENT && errno != ENOTDIR)
        storageLog("Failed to delete autostart link '%s': %s",
                   pool->autostartLink, strerror(errno));

    free(pool->configFile);
    pool->configFile = NULL;
    free(pool->autostartLink);
    pool->autostartLink = NULL;

    virStoragePoolObjRemove(driver, pool);

    return 0;
}

static int
storagePoolStart(virStoragePoolPtr obj,
                 unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
        return -1;
    }

    if (virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("pool already active"));
        return -1;
    }
    if (backend->startPool &&
        backend->startPool(obj->conn, pool) < 0)
        return -1;
    if (backend->refreshPool(obj->conn, pool) < 0) {
        if (backend->stopPool)
            backend->stopPool(obj->conn, pool);
        return -1;
    }

    pool->active = 1;
    driver->nactivePools++;
    driver->ninactivePools--;

    return 0;
}

static int
storagePoolBuild(virStoragePoolPtr obj,
                 unsigned int flags) {
    virStorageDriverStatePtr driver
        = (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
        return -1;
    }

    if (virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is already active"));
        return -1;
    }

    if (backend->buildPool &&
        backend->buildPool(obj->conn, pool, flags) < 0)
        return -1;

    return 0;
}


static int
storagePoolDestroy(virStoragePoolPtr obj) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
        return -1;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return -1;
    }

    if (backend->stopPool &&
        backend->stopPool(obj->conn, pool) < 0)
        return -1;

    virStoragePoolObjClearVols(pool);

    pool->active = 0;
    driver->nactivePools--;
    driver->ninactivePools++;

    if (pool->configFile == NULL)
        virStoragePoolObjRemove(driver, pool);

    return 0;
}


static int
storagePoolDelete(virStoragePoolPtr obj,
                  unsigned int flags) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
        return -1;
    }

    if (virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is still active"));
        return -1;
    }

    if (!backend->deletePool) {
        virStorageReportError(obj->conn, VIR_ERR_NO_SUPPORT,
                              "%s", _("pool does not support volume delete"));
        return -1;
    }
    if (backend->deletePool(obj->conn, pool, flags) < 0)
        return -1;

    return 0;
}


static int
storagePoolRefresh(virStoragePoolPtr obj,
                   unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;
    int ret = 0;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
        return -1;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return -1;
    }

    virStoragePoolObjClearVols(pool);
    if ((ret = backend->refreshPool(obj->conn, pool)) < 0) {
        if (backend->stopPool)
            backend->stopPool(obj->conn, pool);

        pool->active = 0;
        driver->nactivePools--;
        driver->ninactivePools++;

        if (pool->configFile == NULL)
            virStoragePoolObjRemove(driver, pool);
    }

    return ret;
}


static int
storagePoolGetInfo(virStoragePoolPtr obj,
                   virStoragePoolInfoPtr info) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL) {
        return -1;
    }

    memset(info, 0, sizeof(virStoragePoolInfo));
    if (pool->active)
        info->state = VIR_STORAGE_POOL_RUNNING;
    else
        info->state = VIR_STORAGE_POOL_INACTIVE;
    info->capacity = pool->def->capacity;
    info->allocation = pool->def->allocation;
    info->available = pool->def->available;

    return 0;
}

static char *
storagePoolDumpXML(virStoragePoolPtr obj,
                   unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return NULL;
    }

    return virStoragePoolDefFormat(obj->conn, pool->def);
}

static int
storagePoolGetAutostart(virStoragePoolPtr obj,
                        int *autostart) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no pool with matching uuid"));
        return -1;
    }

    if (!pool->configFile) {
        *autostart = 0;
    } else {
        *autostart = pool->autostart;
    }

    return 0;
}

static int
storagePoolSetAutostart(virStoragePoolPtr obj,
                        int autostart) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no pool with matching uuid"));
        return -1;
    }

    if (!pool->configFile) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_ARG,
                              "%s", _("pool has no config file"));
        return -1;
    }

    autostart = (autostart != 0);

    if (pool->autostart == autostart)
        return 0;

    if (autostart) {
        int err;

        if ((err = virFileMakePath(driver->autostartDir))) {
            virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                                  _("cannot create autostart directory %s: %s"),
                                  driver->autostartDir, strerror(err));
            return -1;
        }

        if (symlink(pool->configFile, pool->autostartLink) < 0) {
            virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                                  _("Failed to create symlink '%s' to '%s': %s"),
                                  pool->autostartLink, pool->configFile,
                                  strerror(errno));
            return -1;
        }
    } else {
        if (unlink(pool->autostartLink) < 0 &&
            errno != ENOENT && errno != ENOTDIR) {
            virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                                  _("Failed to delete symlink '%s': %s"),
                                  pool->autostartLink, strerror(errno));
            return -1;
        }
    }

    pool->autostart = autostart;

    return 0;
}


static int
storagePoolNumVolumes(virStoragePoolPtr obj) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return -1;
    }

    return pool->nvolumes;
}

static int
storagePoolListVolumes(virStoragePoolPtr obj,
                       char **const names,
                       int maxnames) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    int i = 0;
    virStorageVolDefPtr vol;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return -1;
    }

    memset(names, 0, maxnames);
    vol = pool->volumes;
    while (vol && i < maxnames) {
        names[i] = strdup(vol->name);
        if (names[i] == NULL) {
            virStorageReportError(obj->conn, VIR_ERR_NO_MEMORY,
                                  "%s", _("name"));
            goto cleanup;
        }
        vol = vol->next;
        i++;
    }

    return i;

 cleanup:
    for (i = 0 ; i < maxnames ; i++) {
        free(names[i]);
        names[i] = NULL;
    }
    memset(names, 0, maxnames);
    return -1;
}


static virStorageVolPtr
storageVolumeLookupByName(virStoragePoolPtr obj,
                          const char *name) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageVolDefPtr vol;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return NULL;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return NULL;
    }

    vol = virStorageVolDefFindByName(pool, name);

    if (!vol) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage vol with matching name"));
        return NULL;
    }

    return virGetStorageVol(obj->conn, pool->def->name, vol->name, vol->key);
}


static virStorageVolPtr
storageVolumeLookupByKey(virConnectPtr conn,
                         const char *key) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)conn->storagePrivateData;
    virStoragePoolObjPtr pool = driver->pools;

    while (pool) {
        if (virStoragePoolObjIsActive(pool)) {
            virStorageVolDefPtr vol = virStorageVolDefFindByKey(pool, key);

            if (vol)
                return virGetStorageVol(conn,
                                        pool->def->name,
                                        vol->name,
                                        vol->key);
        }
        pool = pool->next;
    }

    virStorageReportError(conn, VIR_ERR_INVALID_STORAGE_VOL,
                          "%s", _("no storage vol with matching key"));
    return NULL;
}

static virStorageVolPtr
storageVolumeLookupByPath(virConnectPtr conn,
                          const char *path) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)conn->storagePrivateData;
    virStoragePoolObjPtr pool = driver->pools;

    while (pool) {
        if (virStoragePoolObjIsActive(pool)) {
            virStorageVolDefPtr vol = virStorageVolDefFindByPath(pool, path);

            if (vol)
                return virGetStorageVol(conn,
                                        pool->def->name,
                                        vol->name,
                                        vol->key);
        }
        pool = pool->next;
    }

    virStorageReportError(conn, VIR_ERR_INVALID_STORAGE_VOL,
                          "%s", _("no storage vol with matching path"));
    return NULL;
}

static virStorageVolPtr
storageVolumeCreateXML(virStoragePoolPtr obj,
                       const char *xmldesc,
                       unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByUUID(driver, obj->uuid);
    virStorageBackendPtr backend;
    virStorageVolDefPtr vol;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return NULL;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return NULL;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL)
        return NULL;

    vol = virStorageVolDefParse(obj->conn, pool->def, xmldesc, NULL);
    if (vol == NULL)
        return NULL;

    if (virStorageVolDefFindByName(pool, vol->name)) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("storage vol already exists"));
        virStorageVolDefFree(vol);
        return NULL;
    }

    if (!backend->createVol) {
        virStorageReportError(obj->conn, VIR_ERR_NO_SUPPORT,
                              "%s", _("storage pool does not support volume creation"));
        virStorageVolDefFree(vol);
        return NULL;
    }

    if (backend->createVol(obj->conn, pool, vol) < 0) {
        virStorageVolDefFree(vol);
        return NULL;
    }

    vol->next = pool->volumes;
    pool->volumes = vol;
    pool->nvolumes++;

    return virGetStorageVol(obj->conn, pool->def->name, vol->name, vol->key);
}

static int
storageVolumeDelete(virStorageVolPtr obj,
                    unsigned int flags) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByName(driver, obj->pool);
    virStorageBackendPtr backend;
    virStorageVolDefPtr vol, tmp, prev;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL)
        return -1;

    vol = virStorageVolDefFindByName(pool, obj->name);

    if (!vol) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage vol with matching name"));
        return -1;
    }

    if (!backend->deleteVol) {
        virStorageReportError(obj->conn, VIR_ERR_NO_SUPPORT,
                              "%s", _("storage pool does not support vol deletion"));
        virStorageVolDefFree(vol);
        return -1;
    }

    if (backend->deleteVol(obj->conn, pool, vol, flags) < 0) {
        return -1;
    }

    prev = NULL;
    tmp = pool->volumes;
    while (tmp) {
        if (tmp == vol) {
            break;
        }
            prev = tmp;
            tmp = tmp->next;
    }
    if (prev) {
        prev->next = vol->next;
    } else {
        pool->volumes = vol->next;
    }
    pool->nvolumes--;
    virStorageVolDefFree(vol);

    return 0;
}

static int
storageVolumeGetInfo(virStorageVolPtr obj,
                     virStorageVolInfoPtr info) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByName(driver, obj->pool);
    virStorageBackendPtr backend;
    virStorageVolDefPtr vol;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return -1;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return -1;
    }

    vol = virStorageVolDefFindByName(pool, obj->name);

    if (!vol) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage vol with matching name"));
        return -1;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL)
        return -1;

    if (backend->refreshVol &&
        backend->refreshVol(obj->conn, pool, vol) < 0)
        return -1;

    memset(info, 0, sizeof(*info));
    info->type = backend->volType;
    info->capacity = vol->capacity;
    info->allocation = vol->allocation;

    return 0;
}

static char *
storageVolumeGetXMLDesc(virStorageVolPtr obj,
                        unsigned int flags ATTRIBUTE_UNUSED) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByName(driver, obj->pool);
    virStorageBackendPtr backend;
    virStorageVolDefPtr vol;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return NULL;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return NULL;
    }

    vol = virStorageVolDefFindByName(pool, obj->name);

    if (!vol) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage vol with matching name"));
        return NULL;
    }

    if ((backend = virStorageBackendForType(pool->def->type)) == NULL)
        return NULL;

    return virStorageVolDefFormat(obj->conn, pool->def, vol);
}

static char *
storageVolumeGetPath(virStorageVolPtr obj) {
    virStorageDriverStatePtr driver =
        (virStorageDriverStatePtr)obj->conn->storagePrivateData;
    virStoragePoolObjPtr pool = virStoragePoolObjFindByName(driver, obj->pool);
    virStorageVolDefPtr vol;
    char *ret;

    if (!pool) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage pool with matching uuid"));
        return NULL;
    }

    if (!virStoragePoolObjIsActive(pool)) {
        virStorageReportError(obj->conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("storage pool is not active"));
        return NULL;
    }

    vol = virStorageVolDefFindByName(pool, obj->name);

    if (!vol) {
        virStorageReportError(obj->conn, VIR_ERR_INVALID_STORAGE_POOL,
                              "%s", _("no storage vol with matching name"));
        return NULL;
    }

    ret = strdup(vol->target.path);
    if (ret == NULL) {
        virStorageReportError(obj->conn, VIR_ERR_NO_MEMORY, "%s", _("path"));
        return NULL;
    }
    return ret;
}





static virStorageDriver storageDriver = {
    "storage",
    storageOpen,
    storageClose,
    storageNumPools,
    storageListPools,
    storageNumDefinedPools,
    storageListDefinedPools,
    storagePoolLookupByName,
    storagePoolLookupByUUID,
    storagePoolLookupByVolume,
    storagePoolCreate,
    storagePoolDefine,
    storagePoolBuild,
    storagePoolUndefine,
    storagePoolStart,
    storagePoolDestroy,
    storagePoolDelete,
    storagePoolRefresh,
    storagePoolGetInfo,
    storagePoolDumpXML,
    storagePoolGetAutostart,
    storagePoolSetAutostart,
    storagePoolNumVolumes,
    storagePoolListVolumes,
    storageVolumeLookupByName,
    storageVolumeLookupByKey,
    storageVolumeLookupByPath,
    storageVolumeCreateXML,
    storageVolumeDelete,
    storageVolumeGetInfo,
    storageVolumeGetXMLDesc,
    storageVolumeGetPath
};


static virStateDriver stateDriver = {
    storageDriverStartup,
    storageDriverShutdown,
    storageDriverReload,
    storageDriverActive,
};

int storageRegister(void) {
    virRegisterStorageDriver(&storageDriver);
    virRegisterStateDriver(&stateDriver);
    return 0;
}

/*
 * vim: set tabstop=4:
 * vim: set shiftwidth=4:
 * vim: set expandtab:
 */
/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
