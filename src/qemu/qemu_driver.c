/*
 * qemu_driver.c: core driver methods for managing qemu guests
 *
 * Copyright (C) 2006-2019 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
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

#include <sys/types.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <dirent.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/un.h>


#include "qemu_driver.h"
#include "qemu_agent.h"
#include "qemu_alias.h"
#include "qemu_block.h"
#include "qemu_conf.h"
#include "qemu_capabilities.h"
#include "qemu_command.h"
#include "qemu_cgroup.h"
#include "qemu_hostdev.h"
#include "qemu_hotplug.h"
#include "qemu_monitor.h"
#include "qemu_process.h"
#include "qemu_migration.h"
#include "qemu_migration_params.h"
#include "qemu_blockjob.h"
#include "qemu_security.h"
#include "qemu_checkpoint.h"
#include "qemu_backup.h"

#include "virerror.h"
#include "virlog.h"
#include "datatypes.h"
#include "virbuffer.h"
#include "virhostcpu.h"
#include "virhostmem.h"
#include "virnetdevtap.h"
#include "virnetdevopenvswitch.h"
#include "capabilities.h"
#include "viralloc.h"
#include "virarptable.h"
#include "viruuid.h"
#include "domain_conf.h"
#include "domain_audit.h"
#include "node_device_conf.h"
#include "virpci.h"
#include "virusb.h"
#include "virpidfile.h"
#include "virprocess.h"
#include "libvirt_internal.h"
#include "virxml.h"
#include "cpu/cpu.h"
#include "virsysinfo.h"
#include "domain_nwfilter.h"
#include "virhook.h"
#include "virstoragefile.h"
#include "virfile.h"
#include "virfdstream.h"
#include "configmake.h"
#include "virthreadpool.h"
#include "locking/lock_manager.h"
#include "locking/domain_lock.h"
#include "virkeycode.h"
#include "virnodesuspend.h"
#include "virtime.h"
#include "virtypedparam.h"
#include "virbitmap.h"
#include "virstring.h"
#include "viraccessapicheck.h"
#include "viraccessapicheckqemu.h"
#include "virhostdev.h"
#include "domain_capabilities.h"
#include "vircgroup.h"
#include "virperf.h"
#include "virnuma.h"
#include "netdev_bandwidth_conf.h"
#include "virqemu.h"
#include "virdomainsnapshotobjlist.h"
#include "virenum.h"
#include "virdomaincheckpointobjlist.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_driver");

#define QEMU_NB_MEM_PARAM  3

#define QEMU_NB_BLOCK_IO_TUNE_BASE_PARAMS 6
#define QEMU_NB_BLOCK_IO_TUNE_MAX_PARAMS 7
#define QEMU_NB_BLOCK_IO_TUNE_LENGTH_PARAMS 6
#define QEMU_NB_BLOCK_IO_TUNE_GROUP_PARAMS 1
#define QEMU_NB_BLOCK_IO_TUNE_ALL_PARAMS (QEMU_NB_BLOCK_IO_TUNE_BASE_PARAMS + \
                                          QEMU_NB_BLOCK_IO_TUNE_MAX_PARAMS + \
                                          QEMU_NB_BLOCK_IO_TUNE_GROUP_PARAMS + \
                                          QEMU_NB_BLOCK_IO_TUNE_LENGTH_PARAMS)

#define QEMU_NB_NUMA_PARAM 2

#define QEMU_SCHED_MIN_PERIOD              1000LL
#define QEMU_SCHED_MAX_PERIOD           1000000LL
#define QEMU_SCHED_MIN_QUOTA               1000LL
#define QEMU_SCHED_MAX_QUOTA  18446744073709551LL

#define QEMU_GUEST_VCPU_MAX_ID 4096

#define QEMU_NB_BLKIO_PARAM  6

#define QEMU_NB_BANDWIDTH_PARAM 7

static void qemuProcessEventHandler(void *data, void *opaque);

static int qemuStateCleanup(void);

static int qemuDomainObjStart(virConnectPtr conn,
                              virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              unsigned int flags,
                              qemuDomainAsyncJob asyncJob);

static int qemuDomainManagedSaveLoad(virDomainObjPtr vm,
                                     void *opaque);

static int qemuOpenFileAs(uid_t fallback_uid, gid_t fallback_gid,
                          bool dynamicOwnership,
                          const char *path, int oflags,
                          bool *needUnlink);

static virQEMUDriverPtr qemu_driver;

/* Looks up the domain object from snapshot and unlocks the
 * driver. The returned domain object is locked and ref'd and the
 * caller must call virDomainObjEndAPI() on it. */
static virDomainObjPtr
qemuDomObjFromSnapshot(virDomainSnapshotPtr snapshot)
{
    return qemuDomainObjFromDomain(snapshot->domain);
}


/* Looks up snapshot object from VM and name */
static virDomainMomentObjPtr
qemuSnapObjFromName(virDomainObjPtr vm,
                    const char *name)
{
    virDomainMomentObjPtr snap = NULL;
    snap = virDomainSnapshotFindByName(vm->snapshots, name);
    if (!snap)
        virReportError(VIR_ERR_NO_DOMAIN_SNAPSHOT,
                       _("no domain snapshot with matching name '%s'"),
                       name);

    return snap;
}


/* Looks up snapshot object from VM and snapshotPtr */
static virDomainMomentObjPtr
qemuSnapObjFromSnapshot(virDomainObjPtr vm,
                        virDomainSnapshotPtr snapshot)
{
    return qemuSnapObjFromName(vm, snapshot->name);
}


static int
qemuAutostartDomain(virDomainObjPtr vm,
                    void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    int flags = 0;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;

    if (cfg->autoStartBypassCache)
        flags |= VIR_DOMAIN_START_BYPASS_CACHE;

    virObjectLock(vm);
    virObjectRef(vm);
    virResetLastError();
    if (vm->autostart &&
        !virDomainObjIsActive(vm)) {
        if (qemuProcessBeginJob(driver, vm,
                                VIR_DOMAIN_JOB_OPERATION_START, flags) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to start job on VM '%s': %s"),
                           vm->def->name, virGetLastErrorMessage());
            goto cleanup;
        }

        if (qemuDomainObjStart(NULL, driver, vm, flags,
                               QEMU_ASYNC_JOB_START) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to autostart VM '%s': %s"),
                           vm->def->name, virGetLastErrorMessage());
        }

        qemuProcessEndJob(driver, vm);
    }

    ret = 0;
 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static void
qemuAutostartDomains(virQEMUDriverPtr driver)
{
    virDomainObjListForEach(driver->domains, false, qemuAutostartDomain, driver);
}


static int
qemuSecurityChownCallback(const virStorageSource *src,
                          uid_t uid,
                          gid_t gid)
{
    struct stat sb;
    int save_errno = 0;
    int ret = -1;
    int rv;
    g_autoptr(virStorageSource) cpy = NULL;

    rv = virStorageFileSupportsSecurityDriver(src);
    if (rv <= 0)
        return rv;

    if (virStorageSourceIsLocalStorage(src)) {
        /* use direct chown for local files so that the file doesn't
         * need to be initialized */
        if (!src->path)
            return 0;

        if (stat(src->path, &sb) >= 0) {
            if (sb.st_uid == uid &&
                sb.st_gid == gid) {
                /* It's alright, there's nothing to change anyway. */
                return 0;
            }
        }

        if (chown(src->path, uid, gid) < 0)
            goto cleanup;
    } else {
        if (!(cpy = virStorageSourceCopy(src, false)))
            goto cleanup;

        /* src file init reports errors, return -2 on failure */
        if (virStorageFileInit(cpy) < 0) {
            ret = -2;
            goto cleanup;
        }

        if (virStorageFileChown(cpy, uid, gid) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    save_errno = errno;
    virStorageFileDeinit(cpy);
    errno = save_errno;

    return ret;
}


static int
qemuSecurityInit(virQEMUDriverPtr driver)
{
    char **names;
    virSecurityManagerPtr mgr = NULL;
    virSecurityManagerPtr stack = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    unsigned int flags = 0;

    if (cfg->securityDefaultConfined)
        flags |= VIR_SECURITY_MANAGER_DEFAULT_CONFINED;
    if (cfg->securityRequireConfined)
        flags |= VIR_SECURITY_MANAGER_REQUIRE_CONFINED;
    if (virQEMUDriverIsPrivileged(driver))
        flags |= VIR_SECURITY_MANAGER_PRIVILEGED;

    if (cfg->securityDriverNames &&
        cfg->securityDriverNames[0]) {
        names = cfg->securityDriverNames;
        while (names && *names) {
            if (!(mgr = qemuSecurityNew(*names,
                                        QEMU_DRIVER_NAME,
                                        flags)))
                goto error;
            if (!stack) {
                if (!(stack = qemuSecurityNewStack(mgr)))
                    goto error;
            } else {
                if (qemuSecurityStackAddNested(stack, mgr) < 0)
                    goto error;
            }
            mgr = NULL;
            names++;
        }
    } else {
        if (!(mgr = qemuSecurityNew(NULL,
                                    QEMU_DRIVER_NAME,
                                    flags)))
            goto error;
        if (!(stack = qemuSecurityNewStack(mgr)))
            goto error;
        mgr = NULL;
    }

    if (virQEMUDriverIsPrivileged(driver)) {
        if (cfg->dynamicOwnership)
            flags |= VIR_SECURITY_MANAGER_DYNAMIC_OWNERSHIP;
        if (virBitmapIsBitSet(cfg->namespaces, QEMU_DOMAIN_NS_MOUNT))
            flags |= VIR_SECURITY_MANAGER_MOUNT_NAMESPACE;
        if (!(mgr = qemuSecurityNewDAC(QEMU_DRIVER_NAME,
                                       cfg->user,
                                       cfg->group,
                                       flags,
                                       qemuSecurityChownCallback)))
            goto error;
        if (!stack) {
            if (!(stack = qemuSecurityNewStack(mgr)))
                goto error;
        } else {
            if (qemuSecurityStackAddNested(stack, mgr) < 0)
                goto error;
        }
        mgr = NULL;
    }

    driver->securityManager = stack;
    return 0;

 error:
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("Failed to initialize security drivers"));
    virObjectUnref(stack);
    virObjectUnref(mgr);
    return -1;
}


static int
qemuDomainSnapshotLoad(virDomainObjPtr vm,
                       void *data)
{
    char *baseDir = (char *)data;
    g_autofree char *snapDir = NULL;
    DIR *dir = NULL;
    struct dirent *entry;
    virDomainSnapshotDefPtr def = NULL;
    virDomainMomentObjPtr snap = NULL;
    virDomainMomentObjPtr current = NULL;
    bool cur;
    unsigned int flags = (VIR_DOMAIN_SNAPSHOT_PARSE_REDEFINE |
                          VIR_DOMAIN_SNAPSHOT_PARSE_DISKS |
                          VIR_DOMAIN_SNAPSHOT_PARSE_INTERNAL);
    int ret = -1;
    int direrr;
    qemuDomainObjPrivatePtr priv;

    virObjectLock(vm);

    priv = vm->privateData;

    if (!(snapDir = g_strdup_printf("%s/%s", baseDir, vm->def->name))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to allocate memory for "
                       "snapshot directory for domain %s"),
                       vm->def->name);
        goto cleanup;
    }

    VIR_INFO("Scanning for snapshots for domain %s in %s", vm->def->name,
             snapDir);

    if (virDirOpenIfExists(&dir, snapDir) <= 0)
        goto cleanup;

    while ((direrr = virDirRead(dir, &entry, NULL)) > 0) {
        g_autofree char *xmlStr = NULL;
        g_autofree char *fullpath = NULL;

        /* NB: ignoring errors, so one malformed config doesn't
           kill the whole process */
        VIR_INFO("Loading snapshot file '%s'", entry->d_name);

        if (!(fullpath = g_strdup_printf("%s/%s", snapDir, entry->d_name))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Failed to allocate memory for path"));
            continue;
        }

        if (virFileReadAll(fullpath, 1024*1024*1, &xmlStr) < 0) {
            /* Nothing we can do here, skip this one */
            virReportSystemError(errno,
                                 _("Failed to read snapshot file %s"),
                                 fullpath);
            continue;
        }

        def = virDomainSnapshotDefParseString(xmlStr,
                                              qemu_driver->xmlopt,
                                              priv->qemuCaps, &cur,
                                              flags);
        if (def == NULL) {
            /* Nothing we can do here, skip this one */
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to parse snapshot XML from file '%s'"),
                           fullpath);
            continue;
        }

        snap = virDomainSnapshotAssignDef(vm->snapshots, def);
        if (snap == NULL) {
            virObjectUnref(def);
        } else if (cur) {
            if (current)
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Too many snapshots claiming to be current for domain %s"),
                               vm->def->name);
            current = snap;
        }
    }
    if (direrr < 0)
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to fully read directory %s"),
                       snapDir);

    virDomainSnapshotSetCurrent(vm->snapshots, current);
    if (virDomainSnapshotUpdateRelations(vm->snapshots) < 0)
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Snapshots have inconsistent relations for domain %s"),
                       vm->def->name);

    /* FIXME: qemu keeps internal track of snapshots.  We can get access
     * to this info via the "info snapshots" monitor command for running
     * domains, or via "qemu-img snapshot -l" for shutoff domains.  It would
     * be nice to update our internal state based on that, but there is a
     * a problem.  qemu doesn't track all of the same metadata that we do.
     * In particular we wouldn't be able to fill in the <parent>, which is
     * pretty important in our metadata.
     */

    virResetLastError();

    ret = 0;
 cleanup:
    VIR_DIR_CLOSE(dir);
    virObjectUnlock(vm);
    return ret;
}


static int
qemuDomainCheckpointLoad(virDomainObjPtr vm,
                         void *data)
{
    char *baseDir = (char *)data;
    g_autofree char *chkDir = NULL;
    DIR *dir = NULL;
    struct dirent *entry;
    virDomainCheckpointDefPtr def = NULL;
    virDomainMomentObjPtr chk = NULL;
    virDomainMomentObjPtr current = NULL;
    unsigned int flags = VIR_DOMAIN_CHECKPOINT_PARSE_REDEFINE;
    int ret = -1;
    int direrr;
    qemuDomainObjPrivatePtr priv;

    virObjectLock(vm);
    priv = vm->privateData;

    if (!(chkDir = g_strdup_printf("%s/%s", baseDir, vm->def->name))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to allocate memory for "
                       "checkpoint directory for domain %s"),
                       vm->def->name);
        goto cleanup;
    }

    VIR_INFO("Scanning for checkpoints for domain %s in %s", vm->def->name,
             chkDir);

    if (virDirOpenIfExists(&dir, chkDir) <= 0)
        goto cleanup;

    while ((direrr = virDirRead(dir, &entry, NULL)) > 0) {
        g_autofree char *xmlStr = NULL;
        g_autofree char *fullpath = NULL;

        /* NB: ignoring errors, so one malformed config doesn't
           kill the whole process */
        VIR_INFO("Loading checkpoint file '%s'", entry->d_name);

        if (!(fullpath = g_strdup_printf("%s/%s", chkDir, entry->d_name))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Failed to allocate memory for path"));
            continue;
        }

        if (virFileReadAll(fullpath, 1024*1024*1, &xmlStr) < 0) {
            /* Nothing we can do here, skip this one */
            virReportSystemError(errno,
                                 _("Failed to read checkpoint file %s"),
                                 fullpath);
            continue;
        }

        def = virDomainCheckpointDefParseString(xmlStr,
                                                qemu_driver->xmlopt,
                                                priv->qemuCaps,
                                                flags);
        if (!def || virDomainCheckpointAlignDisks(def) < 0) {
            /* Nothing we can do here, skip this one */
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to parse checkpoint XML from file '%s'"),
                           fullpath);
            virObjectUnref(def);
            continue;
        }

        chk = virDomainCheckpointAssignDef(vm->checkpoints, def);
        if (chk == NULL)
            virObjectUnref(def);
    }
    if (direrr < 0)
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to fully read directory %s"),
                       chkDir);

    if (virDomainCheckpointUpdateRelations(vm->checkpoints, &current) < 0)
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Checkpoints have inconsistent relations for domain %s"),
                       vm->def->name);
    virDomainCheckpointSetCurrent(vm->checkpoints, current);

    /* Note that it is not practical to automatically construct
     * checkpoints based solely on qcow2 bitmaps, since qemu does not
     * track parent relations which we find important in our metadata.
     * Perhaps we could double-check that our just-loaded checkpoint
     * metadata is consistent with existing qcow2 bitmaps, but a user
     * that changes things behind our backs deserves what happens. */

    virResetLastError();

    ret = 0;
 cleanup:
    VIR_DIR_CLOSE(dir);
    virObjectUnlock(vm);
    return ret;
}


static int
qemuDomainNetsRestart(virDomainObjPtr vm,
                      void *data G_GNUC_UNUSED)
{
    size_t i;
    virDomainDefPtr def = vm->def;

    virObjectLock(vm);

    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr net = def->nets[i];
        if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_DIRECT &&
            virDomainNetGetActualDirectMode(net) == VIR_NETDEV_MACVLAN_MODE_VEPA) {
            VIR_DEBUG("VEPA mode device %s active in domain %s. Reassociating.",
                      net->ifname, def->name);
            ignore_value(virNetDevMacVLanRestartWithVPortProfile(net->ifname,
                                                                 &net->mac,
                                                                 virDomainNetGetActualDirectDev(net),
                                                                 def->uuid,
                                                                 virDomainNetGetActualVirtPortProfile(net),
                                                                 VIR_NETDEV_VPORT_PROFILE_OP_CREATE));
        }
    }

    virObjectUnlock(vm);
    return 0;
}


static int
qemuDomainFindMaxID(virDomainObjPtr vm,
                    void *data)
{
    int *driver_maxid = data;

    if (vm->def->id > *driver_maxid)
        *driver_maxid = vm->def->id;

    return 0;
}


/**
 * qemuStateInitialize:
 *
 * Initialization function for the QEMU daemon
 */
static int
qemuStateInitialize(bool privileged,
                    virStateInhibitCallback callback,
                    void *opaque)
{
    g_autofree char *driverConf = NULL;
    virQEMUDriverConfigPtr cfg;
    uid_t run_uid = -1;
    gid_t run_gid = -1;
    g_autofree char *memoryBackingPath = NULL;
    bool autostart = true;
    size_t i;
    const char *defsecmodel = NULL;
    g_autofree virSecurityManagerPtr *sec_managers = NULL;

    if (VIR_ALLOC(qemu_driver) < 0)
        return VIR_DRV_STATE_INIT_ERROR;

    qemu_driver->lockFD = -1;

    if (virMutexInit(&qemu_driver->lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("cannot initialize mutex"));
        VIR_FREE(qemu_driver);
        return VIR_DRV_STATE_INIT_ERROR;
    }

    qemu_driver->inhibitCallback = callback;
    qemu_driver->inhibitOpaque = opaque;

    qemu_driver->privileged = privileged;
    qemu_driver->hostarch = virArchFromHost();

    if (!(qemu_driver->domains = virDomainObjListNew()))
        goto error;

    /* Init domain events */
    qemu_driver->domainEventState = virObjectEventStateNew();
    if (!qemu_driver->domainEventState)
        goto error;

    /* read the host sysinfo */
    if (privileged)
        qemu_driver->hostsysinfo = virSysinfoRead();

    if (!(qemu_driver->config = cfg = virQEMUDriverConfigNew(privileged)))
        goto error;

    if (!(driverConf = g_strdup_printf("%s/qemu.conf", cfg->configBaseDir)))
        goto error;

    if (virQEMUDriverConfigLoadFile(cfg, driverConf, privileged) < 0)
        goto error;

    if (virQEMUDriverConfigValidate(cfg) < 0)
        goto error;

    if (virQEMUDriverConfigSetDefaults(cfg) < 0)
        goto error;

    if (virFileMakePath(cfg->stateDir) < 0) {
        virReportSystemError(errno, _("Failed to create state dir %s"),
                             cfg->stateDir);
        goto error;
    }
    if (virFileMakePath(cfg->libDir) < 0) {
        virReportSystemError(errno, _("Failed to create lib dir %s"),
                             cfg->libDir);
        goto error;
    }
    if (virFileMakePath(cfg->cacheDir) < 0) {
        virReportSystemError(errno, _("Failed to create cache dir %s"),
                             cfg->cacheDir);
        goto error;
    }
    if (virFileMakePath(cfg->saveDir) < 0) {
        virReportSystemError(errno, _("Failed to create save dir %s"),
                             cfg->saveDir);
        goto error;
    }
    if (virFileMakePath(cfg->snapshotDir) < 0) {
        virReportSystemError(errno, _("Failed to create snapshot dir %s"),
                             cfg->snapshotDir);
        goto error;
    }
    if (virFileMakePath(cfg->checkpointDir) < 0) {
        virReportSystemError(errno, _("Failed to create checkpoint dir %s"),
                             cfg->checkpointDir);
        goto error;
    }
    if (virFileMakePath(cfg->autoDumpPath) < 0) {
        virReportSystemError(errno, _("Failed to create dump dir %s"),
                             cfg->autoDumpPath);
        goto error;
    }
    if (virFileMakePath(cfg->channelTargetDir) < 0) {
        virReportSystemError(errno, _("Failed to create channel target dir %s"),
                             cfg->channelTargetDir);
        goto error;
    }
    if (virFileMakePath(cfg->nvramDir) < 0) {
        virReportSystemError(errno, _("Failed to create nvram dir %s"),
                             cfg->nvramDir);
        goto error;
    }
    if (virFileMakePath(cfg->memoryBackingDir) < 0) {
        virReportSystemError(errno, _("Failed to create memory backing dir %s"),
                             cfg->memoryBackingDir);
        goto error;
    }
    if (virFileMakePath(cfg->slirpStateDir) < 0) {
        virReportSystemError(errno, _("Failed to create slirp state dir %s"),
                             cfg->slirpStateDir);
        goto error;
    }

    if ((qemu_driver->lockFD =
         virPidFileAcquire(cfg->stateDir, "driver", false, getpid())) < 0)
        goto error;

    qemu_driver->qemuImgBinary = virFindFileInPath("qemu-img");

    if (!(qemu_driver->lockManager =
          virLockManagerPluginNew(cfg->lockManagerName ?
                                  cfg->lockManagerName : "nop",
                                  "qemu",
                                  cfg->configBaseDir,
                                  0)))
        goto error;

   if (cfg->macFilter) {
        if (!(qemu_driver->ebtables = ebtablesContextNew("qemu"))) {
            virReportSystemError(errno,
                                 _("failed to enable mac filter in '%s'"),
                                 __FILE__);
            goto error;
        }

        if (ebtablesAddForwardPolicyReject(qemu_driver->ebtables) < 0)
            goto error;
   }

    /* Allocate bitmap for remote display port reservations. We cannot
     * do this before the config is loaded properly, since the port
     * numbers are configurable now */
    if ((qemu_driver->remotePorts =
         virPortAllocatorRangeNew(_("display"),
                                  cfg->remotePortMin,
                                  cfg->remotePortMax)) == NULL)
        goto error;

    if ((qemu_driver->webSocketPorts =
         virPortAllocatorRangeNew(_("webSocket"),
                                  cfg->webSocketPortMin,
                                  cfg->webSocketPortMax)) == NULL)
        goto error;

    if ((qemu_driver->migrationPorts =
         virPortAllocatorRangeNew(_("migration"),
                                  cfg->migrationPortMin,
                                  cfg->migrationPortMax)) == NULL)
        goto error;

    if (qemuSecurityInit(qemu_driver) < 0)
        goto error;

    if (!(qemu_driver->hostdevMgr = virHostdevManagerGetDefault()))
        goto error;

    if (!(qemu_driver->sharedDevices = virHashCreate(30, qemuSharedDeviceEntryFree)))
        goto error;

    if (qemuMigrationDstErrorInit(qemu_driver) < 0)
        goto error;

    if (privileged) {
        g_autofree char *channeldir = NULL;

        if (chown(cfg->libDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to user %d:%d"),
                                 cfg->libDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->cacheDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->cacheDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->saveDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->saveDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->snapshotDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->snapshotDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->checkpointDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->checkpointDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->autoDumpPath, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->autoDumpPath, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        channeldir = g_path_get_dirname(cfg->channelTargetDir);

        if (chown(channeldir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 channeldir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->channelTargetDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->channelTargetDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->nvramDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->nvramDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->memoryBackingDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->memoryBackingDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }
        if (chown(cfg->slirpStateDir, cfg->user, cfg->group) < 0) {
            virReportSystemError(errno,
                                 _("unable to set ownership of '%s' to %d:%d"),
                                 cfg->slirpStateDir, (int)cfg->user,
                                 (int)cfg->group);
            goto error;
        }

        run_uid = cfg->user;
        run_gid = cfg->group;
    }

    qemu_driver->qemuCapsCache = virQEMUCapsCacheNew(cfg->libDir,
                                                     cfg->cacheDir,
                                                     run_uid,
                                                     run_gid);
    if (!qemu_driver->qemuCapsCache)
        goto error;

    if (!(sec_managers = qemuSecurityGetNested(qemu_driver->securityManager)))
        goto error;

    if (sec_managers[0] != NULL)
        defsecmodel = qemuSecurityGetModel(sec_managers[0]);

    if (!(qemu_driver->xmlopt = virQEMUDriverCreateXMLConf(qemu_driver,
                                                           defsecmodel)))
        goto error;

    /* If hugetlbfs is present, then we need to create a sub-directory within
     * it, since we can't assume the root mount point has permissions that
     * will let our spawned QEMU instances use it. */
    for (i = 0; i < cfg->nhugetlbfs; i++) {
        g_autofree char *hugepagePath = NULL;

        hugepagePath = qemuGetBaseHugepagePath(&cfg->hugetlbfs[i]);

        if (!hugepagePath)
            goto error;

        if (virFileMakePath(hugepagePath) < 0) {
            virReportSystemError(errno,
                                 _("unable to create hugepage path %s"),
                                 hugepagePath);
            goto error;
        }
        if (privileged &&
            virFileUpdatePerm(cfg->hugetlbfs[i].mnt_dir,
                              0, S_IXGRP | S_IXOTH) < 0)
            goto error;
    }

    qemuGetMemoryBackingBasePath(cfg, &memoryBackingPath);

    if (virFileMakePath(memoryBackingPath) < 0) {
        virReportSystemError(errno,
                             _("unable to create memory backing path %s"),
                             memoryBackingPath);
        goto error;
    }

    if (privileged &&
        virFileUpdatePerm(memoryBackingPath,
                          0, S_IXGRP | S_IXOTH) < 0)
        goto error;

    if (!(qemu_driver->closeCallbacks = virCloseCallbacksNew()))
        goto error;

    /* Get all the running persistent or transient configs first */
    if (virDomainObjListLoadAllConfigs(qemu_driver->domains,
                                       cfg->stateDir,
                                       NULL, true,
                                       qemu_driver->xmlopt,
                                       NULL, NULL) < 0)
        goto error;

    /* find the maximum ID from active and transient configs to initialize
     * the driver with. This is to avoid race between autostart and reconnect
     * threads */
    virDomainObjListForEach(qemu_driver->domains,
                            false,
                            qemuDomainFindMaxID,
                            &qemu_driver->lastvmid);

    virDomainObjListForEach(qemu_driver->domains,
                            false,
                            qemuDomainNetsRestart,
                            NULL);

    /* Then inactive persistent configs */
    if (virDomainObjListLoadAllConfigs(qemu_driver->domains,
                                       cfg->configDir,
                                       cfg->autostartDir, false,
                                       qemu_driver->xmlopt,
                                       NULL, NULL) < 0)
        goto error;

    virDomainObjListForEach(qemu_driver->domains,
                            false,
                            qemuDomainSnapshotLoad,
                            cfg->snapshotDir);

    virDomainObjListForEach(qemu_driver->domains,
                            false,
                            qemuDomainCheckpointLoad,
                            cfg->checkpointDir);

    virDomainObjListForEach(qemu_driver->domains,
                            false,
                            qemuDomainManagedSaveLoad,
                            qemu_driver);

    /* must be initialized before trying to reconnect to all the
     * running domains since there might occur some QEMU monitor
     * events that will be dispatched to the worker pool */
    qemu_driver->workerPool = virThreadPoolNew(0, 1, 0, qemuProcessEventHandler, qemu_driver);
    if (!qemu_driver->workerPool)
        goto error;

    qemuProcessReconnectAll(qemu_driver);

    if (virDriverShouldAutostart(cfg->stateDir, &autostart) < 0)
        goto error;

    if (autostart)
        qemuAutostartDomains(qemu_driver);

    return VIR_DRV_STATE_INIT_COMPLETE;

 error:
    qemuStateCleanup();
    return VIR_DRV_STATE_INIT_ERROR;
}

static void qemuNotifyLoadDomain(virDomainObjPtr vm, int newVM, void *opaque)
{
    virQEMUDriverPtr driver = opaque;

    if (newVM) {
        virObjectEventPtr event =
            virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED);
        virObjectEventStateQueue(driver->domainEventState, event);
    }
}

/**
 * qemuStateReload:
 *
 * Function to restart the QEMU daemon, it will recheck the configuration
 * files and update its state and the networking
 */
static int
qemuStateReload(void)
{
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    if (!qemu_driver)
        return 0;

    cfg = virQEMUDriverGetConfig(qemu_driver);
    virDomainObjListLoadAllConfigs(qemu_driver->domains,
                                   cfg->configDir,
                                   cfg->autostartDir, false,
                                   qemu_driver->xmlopt,
                                   qemuNotifyLoadDomain, qemu_driver);
    return 0;
}


/*
 * qemuStateStop:
 *
 * Save any VMs in preparation for shutdown
 *
 */
static int
qemuStateStop(void)
{
    int ret = -1;
    g_autoptr(virConnect) conn = NULL;
    int numDomains = 0;
    size_t i;
    int state;
    virDomainPtr *domains = NULL;
    g_autofree unsigned int *flags = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(qemu_driver);

    if (!(conn = virConnectOpen(cfg->uri)))
        goto cleanup;

    if ((numDomains = virConnectListAllDomains(conn,
                                               &domains,
                                               VIR_CONNECT_LIST_DOMAINS_ACTIVE)) < 0)
        goto cleanup;

    if (VIR_ALLOC_N(flags, numDomains) < 0)
        goto cleanup;

    /* First we pause all VMs to make them stop dirtying
       pages, etc. We remember if any VMs were paused so
       we can restore that on resume. */
    for (i = 0; i < numDomains; i++) {
        flags[i] = VIR_DOMAIN_SAVE_RUNNING;
        if (virDomainGetState(domains[i], &state, NULL, 0) == 0) {
            if (state == VIR_DOMAIN_PAUSED)
                flags[i] = VIR_DOMAIN_SAVE_PAUSED;
        }
        virDomainSuspend(domains[i]);
    }

    ret = 0;
    /* Then we save the VMs to disk */
    for (i = 0; i < numDomains; i++)
        if (virDomainManagedSave(domains[i], flags[i]) < 0)
            ret = -1;

 cleanup:
    if (domains) {
        for (i = 0; i < numDomains; i++)
            virObjectUnref(domains[i]);
        VIR_FREE(domains);
    }

    return ret;
}

/**
 * qemuStateCleanup:
 *
 * Release resources allocated by QEMU driver (no domain is shut off though)
 */
static int
qemuStateCleanup(void)
{
    if (!qemu_driver)
        return -1;

    virObjectUnref(qemu_driver->migrationErrors);
    virObjectUnref(qemu_driver->closeCallbacks);
    virLockManagerPluginUnref(qemu_driver->lockManager);
    virSysinfoDefFree(qemu_driver->hostsysinfo);
    virPortAllocatorRangeFree(qemu_driver->migrationPorts);
    virPortAllocatorRangeFree(qemu_driver->webSocketPorts);
    virPortAllocatorRangeFree(qemu_driver->remotePorts);
    virHashFree(qemu_driver->sharedDevices);
    virObjectUnref(qemu_driver->hostdevMgr);
    virObjectUnref(qemu_driver->securityManager);
    virObjectUnref(qemu_driver->domainEventState);
    virObjectUnref(qemu_driver->qemuCapsCache);
    virObjectUnref(qemu_driver->xmlopt);
    virCPUDefFree(qemu_driver->hostcpu);
    virCapabilitiesHostNUMAUnref(qemu_driver->hostnuma);
    virObjectUnref(qemu_driver->caps);
    ebtablesContextFree(qemu_driver->ebtables);
    VIR_FREE(qemu_driver->qemuImgBinary);
    virObjectUnref(qemu_driver->domains);
    virThreadPoolFree(qemu_driver->workerPool);

    if (qemu_driver->lockFD != -1)
        virPidFileRelease(qemu_driver->config->stateDir, "driver", qemu_driver->lockFD);

    virObjectUnref(qemu_driver->config);
    virMutexDestroy(&qemu_driver->lock);
    VIR_FREE(qemu_driver);

    return 0;
}


static int
qemuConnectURIProbe(char **uri)
{
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    if (qemu_driver == NULL)
        return 0;

    cfg = virQEMUDriverGetConfig(qemu_driver);
    *uri = g_strdup(cfg->uri);

    return 0;
}

static virDrvOpenStatus qemuConnectOpen(virConnectPtr conn,
                                        virConnectAuthPtr auth G_GNUC_UNUSED,
                                        virConfPtr conf G_GNUC_UNUSED,
                                        unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (qemu_driver == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("qemu state driver is not active"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (!virConnectValidateURIPath(conn->uri->path,
                                   "qemu",
                                   virQEMUDriverIsPrivileged(qemu_driver)))
        return VIR_DRV_OPEN_ERROR;

    if (virConnectOpenEnsureACL(conn) < 0)
        return VIR_DRV_OPEN_ERROR;

    conn->privateData = qemu_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int qemuConnectClose(virConnectPtr conn)
{
    virQEMUDriverPtr driver = conn->privateData;

    /* Get rid of callbacks registered for this conn */
    virCloseCallbacksRun(driver->closeCallbacks, conn, driver->domains, driver);

    conn->privateData = NULL;

    return 0;
}

/* Which features are supported by this driver? */
static int
qemuConnectSupportsFeature(virConnectPtr conn, int feature)
{
    if (virConnectSupportsFeatureEnsureACL(conn) < 0)
        return -1;

    switch ((virDrvFeature) feature) {
    case VIR_DRV_FEATURE_MIGRATION_V2:
    case VIR_DRV_FEATURE_MIGRATION_V3:
    case VIR_DRV_FEATURE_MIGRATION_P2P:
    case VIR_DRV_FEATURE_MIGRATE_CHANGE_PROTECTION:
    case VIR_DRV_FEATURE_FD_PASSING:
    case VIR_DRV_FEATURE_TYPED_PARAM_STRING:
    case VIR_DRV_FEATURE_XML_MIGRATABLE:
    case VIR_DRV_FEATURE_MIGRATION_OFFLINE:
    case VIR_DRV_FEATURE_MIGRATION_PARAMS:
        return 1;
    case VIR_DRV_FEATURE_MIGRATION_DIRECT:
    case VIR_DRV_FEATURE_MIGRATION_V1:
    case VIR_DRV_FEATURE_PROGRAM_KEEPALIVE:
    case VIR_DRV_FEATURE_REMOTE:
    case VIR_DRV_FEATURE_REMOTE_CLOSE_CALLBACK:
    case VIR_DRV_FEATURE_REMOTE_EVENT_CALLBACK:
    default:
        return 0;
    }
}

static const char *qemuConnectGetType(virConnectPtr conn) {
    if (virConnectGetTypeEnsureACL(conn) < 0)
        return NULL;

    return "QEMU";
}


static int qemuConnectIsSecure(virConnectPtr conn G_GNUC_UNUSED)
{
    /* Trivially secure, since always inside the daemon */
    return 1;
}

static int qemuConnectIsEncrypted(virConnectPtr conn G_GNUC_UNUSED)
{
    /* Not encrypted, but remote driver takes care of that */
    return 0;
}

static int qemuConnectIsAlive(virConnectPtr conn G_GNUC_UNUSED)
{
    return 1;
}


static char *
qemuConnectGetSysinfo(virConnectPtr conn, unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virCheckFlags(0, NULL);

    if (virConnectGetSysinfoEnsureACL(conn) < 0)
        return NULL;

    if (!driver->hostsysinfo) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Host SMBIOS information is not available"));
        return NULL;
    }

    if (virSysinfoFormat(&buf, driver->hostsysinfo) < 0)
        return NULL;
    return virBufferContentAndReset(&buf);
}

static int
qemuConnectGetMaxVcpus(virConnectPtr conn G_GNUC_UNUSED, const char *type)
{
    if (virConnectGetMaxVcpusEnsureACL(conn) < 0)
        return -1;

    if (!type)
        return 16;

    if (STRCASEEQ(type, "qemu"))
        return 16;

    if (STRCASEEQ(type, "kvm"))
        return virHostCPUGetKVMMaxVCPUs();

    virReportError(VIR_ERR_INVALID_ARG,
                   _("unknown type '%s'"), type);
    return -1;
}


static char *qemuConnectGetCapabilities(virConnectPtr conn) {
    virQEMUDriverPtr driver = conn->privateData;
    g_autoptr(virCaps) caps = NULL;

    if (virConnectGetCapabilitiesEnsureACL(conn) < 0)
        return NULL;

    if (!(caps = virQEMUDriverGetCapabilities(driver, true)))
        return NULL;

    return virCapabilitiesFormatXML(caps);
}


static int
qemuGetSchedInfo(unsigned long long *cpuWait,
                 pid_t pid, pid_t tid)
{
    g_autofree char *proc = NULL;
    g_autofree char *data = NULL;
    char **lines = NULL;
    size_t i;
    int ret = -1;
    double val;

    *cpuWait = 0;

    /* In general, we cannot assume pid_t fits in int; but /proc parsing
     * is specific to Linux where int works fine.  */
    if (tid)
        proc = g_strdup_printf("/proc/%d/task/%d/sched", (int)pid, (int)tid);
    else
        proc = g_strdup_printf("/proc/%d/sched", (int)pid);
    if (!proc)
        goto cleanup;
    ret = -1;

    /* The file is not guaranteed to exist (needs CONFIG_SCHED_DEBUG) */
    if (access(proc, R_OK) < 0) {
        ret = 0;
        goto cleanup;
    }

    if (virFileReadAll(proc, (1<<16), &data) < 0)
        goto cleanup;

    lines = virStringSplit(data, "\n", 0);
    if (!lines)
        goto cleanup;

    for (i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];

        /* Needs CONFIG_SCHEDSTATS. The second check
         * is the old name the kernel used in past */
        if (STRPREFIX(line, "se.statistics.wait_sum") ||
            STRPREFIX(line, "se.wait_sum")) {
            line = strchr(line, ':');
            if (!line) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Missing separator in sched info '%s'"),
                               lines[i]);
                goto cleanup;
            }
            line++;
            while (*line == ' ')
                line++;

            if (virStrToDouble(line, NULL, &val) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Unable to parse sched info value '%s'"),
                               line);
                goto cleanup;
            }

            *cpuWait = (unsigned long long)(val * 1000000);
            break;
        }
    }

    ret = 0;

 cleanup:
    virStringListFree(lines);
    return ret;
}


static int
qemuGetProcessInfo(unsigned long long *cpuTime, int *lastCpu, long *vm_rss,
                   pid_t pid, int tid)
{
    g_autofree char *proc = NULL;
    FILE *pidinfo;
    unsigned long long usertime = 0, systime = 0;
    long rss = 0;
    int cpu = 0;

    /* In general, we cannot assume pid_t fits in int; but /proc parsing
     * is specific to Linux where int works fine.  */
    if (tid)
        proc = g_strdup_printf("/proc/%d/task/%d/stat", (int)pid, tid);
    else
        proc = g_strdup_printf("/proc/%d/stat", (int)pid);
    if (!proc)
        return -1;

    pidinfo = fopen(proc, "r");

    /* See 'man proc' for information about what all these fields are. We're
     * only interested in a very few of them */
    if (!pidinfo ||
        fscanf(pidinfo,
               /* pid -> stime */
               "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu"
               /* cutime -> endcode */
               "%*d %*d %*d %*d %*d %*d %*u %*u %ld %*u %*u %*u"
               /* startstack -> processor */
               "%*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d %d",
               &usertime, &systime, &rss, &cpu) != 4) {
        VIR_WARN("cannot parse process status data");
    }

    /* We got jiffies
     * We want nanoseconds
     * _SC_CLK_TCK is jiffies per second
     * So calculate thus....
     */
    if (cpuTime)
        *cpuTime = 1000ull * 1000ull * 1000ull * (usertime + systime)
            / (unsigned long long)sysconf(_SC_CLK_TCK);
    if (lastCpu)
        *lastCpu = cpu;

    if (vm_rss)
        *vm_rss = rss * virGetSystemPageSizeKB();


    VIR_DEBUG("Got status for %d/%d user=%llu sys=%llu cpu=%d rss=%ld",
              (int)pid, tid, usertime, systime, cpu, rss);

    VIR_FORCE_FCLOSE(pidinfo);

    return 0;
}


static int
qemuDomainHelperGetVcpus(virDomainObjPtr vm,
                         virVcpuInfoPtr info,
                         unsigned long long *cpuwait,
                         int maxinfo,
                         unsigned char *cpumaps,
                         int maplen)
{
    size_t ncpuinfo = 0;
    size_t i;

    if (maxinfo == 0)
        return 0;

    if (!qemuDomainHasVcpuPids(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        return -1;
    }

    if (info)
        memset(info, 0, sizeof(*info) * maxinfo);

    if (cpumaps)
        memset(cpumaps, 0, sizeof(*cpumaps) * maxinfo);

    for (i = 0; i < virDomainDefGetVcpusMax(vm->def) && ncpuinfo < maxinfo; i++) {
        virDomainVcpuDefPtr vcpu = virDomainDefGetVcpu(vm->def, i);
        pid_t vcpupid = qemuDomainGetVcpuPid(vm, i);
        virVcpuInfoPtr vcpuinfo = info + ncpuinfo;

        if (!vcpu->online)
            continue;

        if (info) {
            vcpuinfo->number = i;
            vcpuinfo->state = VIR_VCPU_RUNNING;

            if (qemuGetProcessInfo(&vcpuinfo->cpuTime,
                                   &vcpuinfo->cpu, NULL,
                                   vm->pid, vcpupid) < 0) {
                virReportSystemError(errno, "%s",
                                     _("cannot get vCPU placement & pCPU time"));
                return -1;
            }
        }

        if (cpumaps) {
            unsigned char *cpumap = VIR_GET_CPUMAP(cpumaps, maplen, ncpuinfo);
            virBitmapPtr map = NULL;

            if (!(map = virProcessGetAffinity(vcpupid)))
                return -1;

            virBitmapToDataBuf(map, cpumap, maplen);
            virBitmapFree(map);
        }

        if (cpuwait) {
            if (qemuGetSchedInfo(&(cpuwait[ncpuinfo]), vm->pid, vcpupid) < 0)
                return -1;
        }

        ncpuinfo++;
    }

    return ncpuinfo;
}


static virDomainPtr qemuDomainLookupByID(virConnectPtr conn,
                                         int id)
{
    virQEMUDriverPtr driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByID(driver->domains, id);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching id %d"), id);
        goto cleanup;
    }

    if (virDomainLookupByIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr qemuDomainLookupByUUID(virConnectPtr conn,
                                           const unsigned char *uuid)
{
    virQEMUDriverPtr driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByUUID(driver->domains, uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainLookupByUUIDEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}

static virDomainPtr qemuDomainLookupByName(virConnectPtr conn,
                                           const char *name)
{
    virQEMUDriverPtr driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    vm = virDomainObjListFindByName(driver->domains, name);

    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), name);
        goto cleanup;
    }

    if (virDomainLookupByNameEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainObjEndAPI(&vm);
    return dom;
}


static int qemuDomainIsActive(virDomainPtr dom)
{
    virDomainObjPtr obj;
    int ret = -1;

    if (!(obj = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainIsActiveEnsureACL(dom->conn, obj->def) < 0)
        goto cleanup;

    ret = virDomainObjIsActive(obj);

 cleanup:
    virDomainObjEndAPI(&obj);
    return ret;
}

static int qemuDomainIsPersistent(virDomainPtr dom)
{
    virDomainObjPtr obj;
    int ret = -1;

    if (!(obj = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainIsPersistentEnsureACL(dom->conn, obj->def) < 0)
        goto cleanup;

    ret = obj->persistent;

 cleanup:
    virDomainObjEndAPI(&obj);
    return ret;
}

static int qemuDomainIsUpdated(virDomainPtr dom)
{
    virDomainObjPtr obj;
    int ret = -1;

    if (!(obj = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainIsUpdatedEnsureACL(dom->conn, obj->def) < 0)
        goto cleanup;

    ret = obj->updated;

 cleanup:
    virDomainObjEndAPI(&obj);
    return ret;
}

static int qemuConnectGetVersion(virConnectPtr conn, unsigned long *version)
{
    virQEMUDriverPtr driver = conn->privateData;
    unsigned int qemuVersion = 0;
    g_autoptr(virCaps) caps = NULL;

    if (virConnectGetVersionEnsureACL(conn) < 0)
        return -1;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        return -1;

    if (virQEMUCapsGetDefaultVersion(caps,
                                     driver->qemuCapsCache,
                                     &qemuVersion) < 0)
        return -1;

    *version = qemuVersion;
    return 0;
}


static char *qemuConnectGetHostname(virConnectPtr conn)
{
    if (virConnectGetHostnameEnsureACL(conn) < 0)
        return NULL;

    return virGetHostname();
}


static int qemuConnectListDomains(virConnectPtr conn, int *ids, int nids)
{
    virQEMUDriverPtr driver = conn->privateData;

    if (virConnectListDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListGetActiveIDs(driver->domains, ids, nids,
                                        virConnectListDomainsCheckACL, conn);
}

static int qemuConnectNumOfDomains(virConnectPtr conn)
{
    virQEMUDriverPtr driver = conn->privateData;

    if (virConnectNumOfDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListNumOfDomains(driver->domains, true,
                                        virConnectNumOfDomainsCheckACL, conn);
}


static virDomainPtr qemuDomainCreateXML(virConnectPtr conn,
                                        const char *xml,
                                        unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;
    virObjectEventPtr event = NULL;
    virObjectEventPtr event2 = NULL;
    unsigned int start_flags = VIR_QEMU_PROCESS_START_COLD;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE |
                               VIR_DOMAIN_DEF_PARSE_ABI_UPDATE;

    virCheckFlags(VIR_DOMAIN_START_PAUSED |
                  VIR_DOMAIN_START_AUTODESTROY |
                  VIR_DOMAIN_START_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_START_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;
    if (flags & VIR_DOMAIN_START_PAUSED)
        start_flags |= VIR_QEMU_PROCESS_START_PAUSED;
    if (flags & VIR_DOMAIN_START_AUTODESTROY)
        start_flags |= VIR_QEMU_PROCESS_START_AUTODESTROY;

    virNWFilterReadLockFilterUpdates();

    if (!(def = virDomainDefParseString(xml, driver->xmlopt,
                                        NULL, parse_flags)))
        goto cleanup;

    if (virDomainCreateXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains, def,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto cleanup;
    def = NULL;

    if (qemuProcessBeginJob(driver, vm, VIR_DOMAIN_JOB_OPERATION_START,
                            flags) < 0) {
        qemuDomainRemoveInactiveJob(driver, vm);
        goto cleanup;
    }

    if (qemuProcessStart(conn, driver, vm, NULL, QEMU_ASYNC_JOB_START,
                         NULL, -1, NULL, NULL,
                         VIR_NETDEV_VPORT_PROFILE_OP_CREATE,
                         start_flags) < 0) {
        virDomainAuditStart(vm, "booted", false);
        qemuDomainRemoveInactive(driver, vm);
        qemuProcessEndJob(driver, vm);
        goto cleanup;
    }

    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STARTED,
                                     VIR_DOMAIN_EVENT_STARTED_BOOTED);
    if (event && (flags & VIR_DOMAIN_START_PAUSED)) {
        /* There are two classes of event-watching clients - those
         * that only care about on/off (and must see a started event
         * no matter what, but don't care about suspend events), and
         * those that also care about running/paused.  To satisfy both
         * client types, we have to send two events.  */
        event2 = virDomainEventLifecycleNewFromObj(vm,
                                          VIR_DOMAIN_EVENT_SUSPENDED,
                                          VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);
    }
    virDomainAuditStart(vm, "booted", true);

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

    qemuProcessEndJob(driver, vm);

 cleanup:
    virDomainDefFree(def);
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);
    virObjectEventStateQueue(driver->domainEventState, event2);
    virNWFilterUnlockFilterUpdates();
    return dom;
}


static int qemuDomainSuspend(virDomainPtr dom)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    virDomainPausedReason reason;
    int state;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainSuspendEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    cfg = virQEMUDriverGetConfig(driver);
    priv = vm->privateData;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_SUSPEND) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_OUT)
        reason = VIR_DOMAIN_PAUSED_MIGRATION;
    else if (priv->job.asyncJob == QEMU_ASYNC_JOB_SNAPSHOT)
        reason = VIR_DOMAIN_PAUSED_SNAPSHOT;
    else
        reason = VIR_DOMAIN_PAUSED_USER;

    state = virDomainObjGetState(vm, NULL);
    if (state == VIR_DOMAIN_PMSUSPENDED) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is pmsuspended"));
        goto endjob;
    } else if (state != VIR_DOMAIN_PAUSED) {
        if (qemuProcessStopCPUs(driver, vm, reason, QEMU_ASYNC_JOB_NONE) < 0)
            goto endjob;
    }
    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto endjob;
    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);

    return ret;
}


static int qemuDomainResume(virDomainPtr dom)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    int state;
    int reason;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainResumeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    state = virDomainObjGetState(vm, &reason);
    if (state == VIR_DOMAIN_PMSUSPENDED) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is pmsuspended"));
        goto endjob;
    } else if (state == VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is already running"));
        goto endjob;
    } else if ((state == VIR_DOMAIN_CRASHED &&
                reason == VIR_DOMAIN_CRASHED_PANICKED) ||
               state == VIR_DOMAIN_PAUSED) {
        if (qemuProcessStartCPUs(driver, vm,
                                 VIR_DOMAIN_RUNNING_UNPAUSED,
                                 QEMU_ASYNC_JOB_NONE) < 0) {
            if (virGetLastErrorCode() == VIR_ERR_OK)
                virReportError(VIR_ERR_OPERATION_FAILED,
                               "%s", _("resume operation failed"));
            goto endjob;
        }
    }
    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto endjob;
    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainShutdownFlagsAgent(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             bool isReboot,
                             bool reportError)
{
    int ret = -1;
    qemuAgentPtr agent;
    int agentFlag = isReboot ?  QEMU_AGENT_SHUTDOWN_REBOOT :
        QEMU_AGENT_SHUTDOWN_POWERDOWN;

    if (qemuDomainObjBeginAgentJob(driver, vm,
                                   QEMU_AGENT_JOB_MODIFY) < 0)
        return -1;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is not running"));
        goto endjob;
    }

    if (!qemuDomainAgentAvailable(vm, reportError))
        goto endjob;

    qemuDomainSetFakeReboot(driver, vm, false);
    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentShutdown(agent, agentFlag);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndAgentJob(vm);
    return ret;
}


static int
qemuDomainShutdownFlagsMonitor(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               bool isReboot)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv;

    priv = vm->privateData;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return -1;

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is not running"));
        goto endjob;
    }

    qemuDomainSetFakeReboot(driver, vm, isReboot);
    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorSystemPowerdown(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int qemuDomainShutdownFlags(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    bool useAgent = false, agentRequested, acpiRequested;
    bool isReboot = false;
    bool agentForced;

    virCheckFlags(VIR_DOMAIN_SHUTDOWN_ACPI_POWER_BTN |
                  VIR_DOMAIN_SHUTDOWN_GUEST_AGENT, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (vm->def->onPoweroff == VIR_DOMAIN_LIFECYCLE_ACTION_RESTART ||
        vm->def->onPoweroff == VIR_DOMAIN_LIFECYCLE_ACTION_RESTART_RENAME) {
        isReboot = true;
        VIR_INFO("Domain on_poweroff setting overridden, attempting reboot");
    }

    priv = vm->privateData;
    agentRequested = flags & VIR_DOMAIN_SHUTDOWN_GUEST_AGENT;
    acpiRequested  = flags & VIR_DOMAIN_SHUTDOWN_ACPI_POWER_BTN;

    /* Prefer agent unless we were requested to not to. */
    if (agentRequested || (!flags && priv->agent))
        useAgent = true;

    if (virDomainShutdownFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    agentForced = agentRequested && !acpiRequested;
    if (useAgent) {
        ret = qemuDomainShutdownFlagsAgent(driver, vm, isReboot, agentForced);
        if (ret < 0 && agentForced)
            goto cleanup;
    }

    /* If we are not enforced to use just an agent, try ACPI
     * shutdown as well in case agent did not succeed.
     */
    if (!useAgent || (ret < 0 && (acpiRequested || !flags))) {
        /* Even if agent failed, we have to check if guest went away
         * by itself while our locks were down.  */
        if (useAgent && !virDomainObjIsActive(vm)) {
            ret = 0;
            goto cleanup;
        }

        ret = qemuDomainShutdownFlagsMonitor(driver, vm, isReboot);
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainShutdown(virDomainPtr dom)
{
    return qemuDomainShutdownFlags(dom, 0);
}


static int
qemuDomainRebootAgent(virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      bool isReboot,
                      bool agentForced)
{
    qemuAgentPtr agent;
    int ret = -1;
    int agentFlag = QEMU_AGENT_SHUTDOWN_REBOOT;

    if (!isReboot)
        agentFlag = QEMU_AGENT_SHUTDOWN_POWERDOWN;

    if (qemuDomainObjBeginAgentJob(driver, vm,
                                   QEMU_AGENT_JOB_MODIFY) < 0)
        return -1;

    if (!qemuDomainAgentAvailable(vm, agentForced))
        goto endjob;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    qemuDomainSetFakeReboot(driver, vm, false);
    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentShutdown(agent, agentFlag);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndAgentJob(vm);
    return ret;
}


static int
qemuDomainRebootMonitor(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        bool isReboot)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    if (qemuDomainObjBeginJob(driver, vm,
                              QEMU_JOB_MODIFY) < 0)
        return -1;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    qemuDomainSetFakeReboot(driver, vm, isReboot);
    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorSystemPowerdown(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int
qemuDomainReboot(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    bool useAgent = false, agentRequested, acpiRequested;
    bool isReboot = true;
    bool agentForced;

    virCheckFlags(VIR_DOMAIN_REBOOT_ACPI_POWER_BTN |
                  VIR_DOMAIN_REBOOT_GUEST_AGENT, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (vm->def->onReboot == VIR_DOMAIN_LIFECYCLE_ACTION_DESTROY ||
        vm->def->onReboot == VIR_DOMAIN_LIFECYCLE_ACTION_PRESERVE) {
        isReboot = false;
        VIR_INFO("Domain on_reboot setting overridden, shutting down");
    }

    priv = vm->privateData;
    agentRequested = flags & VIR_DOMAIN_REBOOT_GUEST_AGENT;
    acpiRequested  = flags & VIR_DOMAIN_REBOOT_ACPI_POWER_BTN;

    /* Prefer agent unless we were requested to not to. */
    if (agentRequested || (!flags && priv->agent))
        useAgent = true;

    if (virDomainRebootEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    agentForced = agentRequested && !acpiRequested;
    if (useAgent)
        ret = qemuDomainRebootAgent(driver, vm, isReboot, agentForced);

    if (ret < 0 && agentForced)
        goto cleanup;

    /* If we are not enforced to use just an agent, try ACPI
     * shutdown as well in case agent did not succeed.
     */
    if ((!useAgent) ||
        (ret < 0 && (acpiRequested || !flags))) {
        ret = qemuDomainRebootMonitor(driver, vm, isReboot);
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainReset(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    virDomainState state;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainResetEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;
    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorSystemReset(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    priv->fakeReboot = false;

    state = virDomainObjGetState(vm, NULL);
    if (state == VIR_DOMAIN_CRASHED)
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_CRASHED);

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/* Count how many snapshots in a set are external snapshots.  */
static int
qemuDomainSnapshotCountExternal(void *payload,
                                const void *name G_GNUC_UNUSED,
                                void *data)
{
    virDomainMomentObjPtr snap = payload;
    int *count = data;

    if (virDomainSnapshotIsExternal(snap))
        (*count)++;
    return 0;
}

static int
qemuDomainDestroyFlags(virDomainPtr dom,
                       unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    virObjectEventPtr event = NULL;
    qemuDomainObjPrivatePtr priv;
    unsigned int stopFlags = 0;
    int state;
    int reason;
    bool starting;

    virCheckFlags(VIR_DOMAIN_DESTROY_GRACEFUL, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;

    if (virDomainDestroyFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    state = virDomainObjGetState(vm, &reason);
    starting = (state == VIR_DOMAIN_PAUSED &&
                reason == VIR_DOMAIN_PAUSED_STARTING_UP &&
                !priv->beingDestroyed);

    if (qemuProcessBeginStopJob(driver, vm, QEMU_JOB_DESTROY,
                                !(flags & VIR_DOMAIN_DESTROY_GRACEFUL)) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        if (starting) {
            VIR_DEBUG("Domain %s is not running anymore", vm->def->name);
            ret = 0;
        } else {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           "%s", _("domain is not running"));
        }
        goto endjob;
    }

    qemuDomainSetFakeReboot(driver, vm, false);

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_IN)
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;

    qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED,
                    QEMU_ASYNC_JOB_NONE, stopFlags);
    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
    virDomainAuditStop(vm, "destroyed");

    ret = 0;
 endjob:
    if (ret == 0)
        qemuDomainRemoveInactive(driver, vm);
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);
    return ret;
}

static int
qemuDomainDestroy(virDomainPtr dom)
{
    return qemuDomainDestroyFlags(dom, 0);
}

static char *qemuDomainGetOSType(virDomainPtr dom) {
    virDomainObjPtr vm;
    char *type = NULL;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetOSTypeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    type = g_strdup(virDomainOSTypeToString(vm->def->os.type));

 cleanup:
    virDomainObjEndAPI(&vm);
    return type;
}

/* Returns max memory in kb, 0 if error */
static unsigned long long
qemuDomainGetMaxMemory(virDomainPtr dom)
{
    virDomainObjPtr vm;
    unsigned long long ret = 0;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetMaxMemoryEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = virDomainDefGetMemoryTotal(vm->def);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainSetMemoryFlags(virDomainPtr dom, unsigned long newmem,
                                    unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuDomainObjPrivatePtr priv;
    virDomainObjPtr vm;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1, r;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_MEM_MAXIMUM, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetMemoryFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;


    if (flags & VIR_DOMAIN_MEM_MAXIMUM) {
        /* resize the maximum memory */

        if (def) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot resize the maximum memory on an "
                             "active domain"));
            goto endjob;
        }

        if (persistentDef) {
            /* resizing memory with NUMA nodes specified doesn't work as there
             * is no way to change the individual node sizes with this API */
            if (virDomainNumaGetNodeCount(persistentDef->numa) > 0) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("initial memory size of a domain with NUMA "
                                 "nodes cannot be modified with this API"));
                goto endjob;
            }

            if (persistentDef->mem.max_memory &&
                persistentDef->mem.max_memory < newmem) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("cannot set initial memory size greater than "
                                 "the maximum memory size"));
                goto endjob;
            }

            virDomainDefSetMemoryTotal(persistentDef, newmem);

            if (persistentDef->mem.cur_balloon > newmem)
                persistentDef->mem.cur_balloon = newmem;
            ret = virDomainDefSave(persistentDef, driver->xmlopt,
                                   cfg->configDir);
            goto endjob;
        }

    } else {
        /* resize the current memory */
        unsigned long oldmax = 0;

        if (def)
            oldmax = virDomainDefGetMemoryTotal(def);
        if (persistentDef) {
            if (!oldmax || oldmax > virDomainDefGetMemoryTotal(persistentDef))
                oldmax = virDomainDefGetMemoryTotal(persistentDef);
        }

        if (newmem > oldmax) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("cannot set memory higher than max memory"));
            goto endjob;
        }

        if (def) {
            priv = vm->privateData;
            qemuDomainObjEnterMonitor(driver, vm);
            r = qemuMonitorSetBalloon(priv->mon, newmem);
            if (qemuDomainObjExitMonitor(driver, vm) < 0 || r < 0)
                goto endjob;

            /* Lack of balloon support is a fatal error */
            if (r == 0) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("Unable to change memory of active domain without "
                                 "the balloon device and guest OS balloon driver"));
                goto endjob;
            }
        }

        if (persistentDef) {
            persistentDef->mem.cur_balloon = newmem;
            ret = virDomainDefSave(persistentDef, driver->xmlopt,
                                   cfg->configDir);
            goto endjob;
        }
    }

    ret = 0;
 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainSetMemory(virDomainPtr dom, unsigned long newmem)
{
    return qemuDomainSetMemoryFlags(dom, newmem, VIR_DOMAIN_AFFECT_LIVE);
}

static int qemuDomainSetMaxMemory(virDomainPtr dom, unsigned long memory)
{
    return qemuDomainSetMemoryFlags(dom, memory, VIR_DOMAIN_MEM_MAXIMUM);
}

static int qemuDomainSetMemoryStatsPeriod(virDomainPtr dom, int period,
                                          unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuDomainObjPrivatePtr priv;
    virDomainObjPtr vm;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1, r;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetMemoryStatsPeriodEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    /* Set the balloon driver collection interval */
    priv = vm->privateData;

    if (def) {
        if (!virDomainDefHasMemballoon(def)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No memory balloon device configured, "
                             "can not set the collection period"));
            goto endjob;
        }

        qemuDomainObjEnterMonitor(driver, vm);
        r = qemuMonitorSetMemoryStatsPeriod(priv->mon, def->memballoon, period);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto endjob;
        if (r < 0) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("unable to set balloon driver collection period"));
            goto endjob;
        }

        def->memballoon->period = period;
        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        if (!virDomainDefHasMemballoon(persistentDef)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No memory balloon device configured, "
                             "can not set the collection period"));
            goto endjob;
        }
        persistentDef->memballoon->period = period;
        ret = virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir);
        goto endjob;
    }

    ret = 0;
 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainInjectNMI(virDomainPtr domain, unsigned int flags)
{
    virQEMUDriverPtr driver = domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainInjectNMIEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    priv = vm->privateData;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorInjectNMI(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainSendKey(virDomainPtr domain,
                             unsigned int codeset,
                             unsigned int holdtime,
                             unsigned int *keycodes,
                             int nkeycodes,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(0, -1);

    /* translate the keycode to QNUM for qemu driver */
    if (codeset != VIR_KEYCODE_SET_QNUM) {
        size_t i;
        int keycode;

        for (i = 0; i < nkeycodes; i++) {
            keycode = virKeycodeValueTranslate(codeset, VIR_KEYCODE_SET_QNUM,
                                               keycodes[i]);
            if (keycode < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot translate keycode %u of %s codeset to qnum keycode"),
                               keycodes[i],
                               virKeycodeSetTypeToString(codeset));
                return -1;
            }
            keycodes[i] = keycode;
        }
    }

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainSendKeyEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorSendKey(priv->mon, holdtime, keycodes, nkeycodes);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainGetInfo(virDomainPtr dom,
                  virDomainInfoPtr info)
{
    unsigned long long maxmem;
    virDomainObjPtr vm;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    qemuDomainUpdateCurrentMemorySize(vm);

    memset(info, 0, sizeof(*info));

    info->state = virDomainObjGetState(vm, NULL);

    maxmem = virDomainDefGetMemoryTotal(vm->def);
    if (VIR_ASSIGN_IS_OVERFLOW(info->maxMem, maxmem)) {
        virReportError(VIR_ERR_OVERFLOW, "%s",
                       _("Initial memory size too large"));
        goto cleanup;
    }

    if (VIR_ASSIGN_IS_OVERFLOW(info->memory, vm->def->mem.cur_balloon)) {
        virReportError(VIR_ERR_OVERFLOW, "%s",
                       _("Current memory size too large"));
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        if (qemuGetProcessInfo(&(info->cpuTime), NULL, NULL, vm->pid, 0) < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("cannot read cputime for domain"));
            goto cleanup;
        }
    }

    if (VIR_ASSIGN_IS_OVERFLOW(info->nrVirtCpu, virDomainDefGetVcpus(vm->def))) {
        virReportError(VIR_ERR_OVERFLOW, "%s", _("cpu count too large"));
        goto cleanup;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetState(virDomainPtr dom,
                   int *state,
                   int *reason,
                   unsigned int flags)
{
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetStateEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    *state = virDomainObjGetState(vm, reason);
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetControlInfo(virDomainPtr dom,
                          virDomainControlInfoPtr info,
                          unsigned int flags)
{
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetControlInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    priv = vm->privateData;

    memset(info, 0, sizeof(*info));

    if (priv->monError) {
        info->state = VIR_DOMAIN_CONTROL_ERROR;
        info->details = VIR_DOMAIN_CONTROL_ERROR_REASON_MONITOR;
    } else if (priv->job.active) {
        if (virTimeMillisNow(&info->stateTime) < 0)
            goto cleanup;
        if (priv->job.current) {
            info->state = VIR_DOMAIN_CONTROL_JOB;
            info->stateTime -= priv->job.current->started;
        } else {
            if (priv->monStart > 0) {
                info->state = VIR_DOMAIN_CONTROL_OCCUPIED;
                info->stateTime -= priv->monStart;
            } else {
                /* At this point the domain has an active job, but monitor was
                 * not entered and the domain object lock is not held thus we
                 * are stuck in the job forever due to a programming error.
                 */
                info->state = VIR_DOMAIN_CONTROL_ERROR;
                info->details = VIR_DOMAIN_CONTROL_ERROR_REASON_INTERNAL;
                info->stateTime = 0;
            }
        }
    } else {
        info->state = VIR_DOMAIN_CONTROL_OK;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/* It would be nice to replace 'Qemud' with 'Qemu' but
 * this magic string is ABI, so it can't be changed
 */
#define QEMU_SAVE_MAGIC   "LibvirtQemudSave"
#define QEMU_SAVE_PARTIAL "LibvirtQemudPart"
#define QEMU_SAVE_VERSION 2

verify(sizeof(QEMU_SAVE_MAGIC) == sizeof(QEMU_SAVE_PARTIAL));

typedef enum {
    QEMU_SAVE_FORMAT_RAW = 0,
    QEMU_SAVE_FORMAT_GZIP = 1,
    QEMU_SAVE_FORMAT_BZIP2 = 2,
    /*
     * Deprecated by xz and never used as part of a release
     * QEMU_SAVE_FORMAT_LZMA
     */
    QEMU_SAVE_FORMAT_XZ = 3,
    QEMU_SAVE_FORMAT_LZOP = 4,
    /* Note: add new members only at the end.
       These values are used in the on-disk format.
       Do not change or re-use numbers. */

    QEMU_SAVE_FORMAT_LAST
} virQEMUSaveFormat;

VIR_ENUM_DECL(qemuSaveCompression);
VIR_ENUM_IMPL(qemuSaveCompression,
              QEMU_SAVE_FORMAT_LAST,
              "raw",
              "gzip",
              "bzip2",
              "xz",
              "lzop",
);

VIR_ENUM_DECL(qemuDumpFormat);
VIR_ENUM_IMPL(qemuDumpFormat,
              VIR_DOMAIN_CORE_DUMP_FORMAT_LAST,
              "elf",
              "kdump-zlib",
              "kdump-lzo",
              "kdump-snappy",
);

typedef struct _virQEMUSaveHeader virQEMUSaveHeader;
typedef virQEMUSaveHeader *virQEMUSaveHeaderPtr;
struct _virQEMUSaveHeader {
    char magic[sizeof(QEMU_SAVE_MAGIC)-1];
    uint32_t version;
    uint32_t data_len;
    uint32_t was_running;
    uint32_t compressed;
    uint32_t cookieOffset;
    uint32_t unused[14];
};

typedef struct _virQEMUSaveData virQEMUSaveData;
typedef virQEMUSaveData *virQEMUSaveDataPtr;
struct _virQEMUSaveData {
    virQEMUSaveHeader header;
    char *xml;
    char *cookie;
};


static inline void
bswap_header(virQEMUSaveHeaderPtr hdr)
{
    hdr->version = GUINT32_SWAP_LE_BE(hdr->version);
    hdr->data_len = GUINT32_SWAP_LE_BE(hdr->data_len);
    hdr->was_running = GUINT32_SWAP_LE_BE(hdr->was_running);
    hdr->compressed = GUINT32_SWAP_LE_BE(hdr->compressed);
    hdr->cookieOffset = GUINT32_SWAP_LE_BE(hdr->cookieOffset);
}


static void
virQEMUSaveDataFree(virQEMUSaveDataPtr data)
{
    if (!data)
        return;

    VIR_FREE(data->xml);
    VIR_FREE(data->cookie);
    VIR_FREE(data);
}


/**
 * This function steals @domXML on success.
 */
static virQEMUSaveDataPtr
virQEMUSaveDataNew(char *domXML,
                   qemuDomainSaveCookiePtr cookieObj,
                   bool running,
                   int compressed,
                   virDomainXMLOptionPtr xmlopt)
{
    virQEMUSaveDataPtr data = NULL;
    virQEMUSaveHeaderPtr header;

    if (VIR_ALLOC(data) < 0)
        return NULL;

    data->xml = g_steal_pointer(&domXML);

    if (cookieObj &&
        !(data->cookie = virSaveCookieFormat((virObjectPtr) cookieObj,
                                             virDomainXMLOptionGetSaveCookie(xmlopt))))
        goto error;

    header = &data->header;
    memcpy(header->magic, QEMU_SAVE_PARTIAL, sizeof(header->magic));
    header->version = QEMU_SAVE_VERSION;
    header->was_running = running ? 1 : 0;
    header->compressed = compressed;

    return data;

 error:
    virQEMUSaveDataFree(data);
    return NULL;
}


/* virQEMUSaveDataWrite:
 *
 * Writes libvirt's header (including domain XML) into a saved image of a
 * running domain. If @header has data_len filled in (because it was previously
 * read from the file), the function will make sure the new data will fit
 * within data_len.
 *
 * Returns -1 on failure, or 0 on success.
 */
static int
virQEMUSaveDataWrite(virQEMUSaveDataPtr data,
                     int fd,
                     const char *path)
{
    virQEMUSaveHeaderPtr header = &data->header;
    size_t len;
    size_t xml_len;
    size_t cookie_len = 0;
    size_t zerosLen = 0;
    g_autofree char *zeros = NULL;

    xml_len = strlen(data->xml) + 1;
    if (data->cookie)
        cookie_len = strlen(data->cookie) + 1;

    len = xml_len + cookie_len;

    if (header->data_len > 0) {
        if (len > header->data_len) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("new xml too large to fit in file"));
            return -1;
        }

        zerosLen = header->data_len - len;
        if (VIR_ALLOC_N(zeros, zerosLen) < 0)
            return -1;
    } else {
        header->data_len = len;
    }

    if (data->cookie)
        header->cookieOffset = xml_len;

    if (safewrite(fd, header, sizeof(*header)) != sizeof(*header)) {
        virReportSystemError(errno,
                             _("failed to write header to domain save file '%s'"),
                             path);
        return -1;
    }

    if (safewrite(fd, data->xml, xml_len) != xml_len) {
        virReportSystemError(errno,
                             _("failed to write domain xml to '%s'"),
                             path);
        return -1;
    }

    if (data->cookie &&
        safewrite(fd, data->cookie, cookie_len) != cookie_len) {
        virReportSystemError(errno,
                             _("failed to write cookie to '%s'"),
                             path);
        return -1;
    }

    if (safewrite(fd, zeros, zerosLen) != zerosLen) {
        virReportSystemError(errno,
                             _("failed to write padding to '%s'"),
                             path);
        return -1;
    }

    return 0;
}


static int
virQEMUSaveDataFinish(virQEMUSaveDataPtr data,
                      int *fd,
                      const char *path)
{
    virQEMUSaveHeaderPtr header = &data->header;

    memcpy(header->magic, QEMU_SAVE_MAGIC, sizeof(header->magic));

    if (safewrite(*fd, header, sizeof(*header)) != sizeof(*header) ||
        VIR_CLOSE(*fd) < 0) {
        virReportSystemError(errno,
                             _("failed to write header to domain save file '%s'"),
                             path);
        return -1;
    }

    return 0;
}


static virCommandPtr
qemuCompressGetCommand(virQEMUSaveFormat compression)
{
    virCommandPtr ret = NULL;
    const char *prog = qemuSaveCompressionTypeToString(compression);

    if (!prog) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Invalid compressed save format %d"),
                       compression);
        return NULL;
    }

    ret = virCommandNew(prog);
    virCommandAddArg(ret, "-dc");

    if (compression == QEMU_SAVE_FORMAT_LZOP)
        virCommandAddArg(ret, "--ignore-warn");

    return ret;
}

/**
 * qemuOpenFile:
 * @driver: driver object
 * @vm: domain object
 * @path: path to file to open
 * @oflags: flags for opening/creation of the file
 * @needUnlink: set to true if file was created by this function
 *
 * Internal function to properly create or open existing files, with
 * ownership affected by qemu driver setup and domain DAC label.
 *
 * Returns the file descriptor on success and negative errno on failure.
 *
 * This function should not be used on storage sources. Use
 * qemuDomainStorageFileInit and storage driver APIs if possible.
 **/
static int
qemuOpenFile(virQEMUDriverPtr driver,
             virDomainObjPtr vm,
             const char *path,
             int oflags,
             bool *needUnlink)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    uid_t user = cfg->user;
    gid_t group = cfg->group;
    bool dynamicOwnership = cfg->dynamicOwnership;
    virSecurityLabelDefPtr seclabel;

    /* TODO: Take imagelabel into account? */
    if (vm &&
        (seclabel = virDomainDefGetSecurityLabelDef(vm->def, "dac")) != NULL &&
        seclabel->label != NULL &&
        (virParseOwnershipIds(seclabel->label, &user, &group) < 0))
        return -1;

    return qemuOpenFileAs(user, group, dynamicOwnership,
                          path, oflags, needUnlink);
}

static int
qemuOpenFileAs(uid_t fallback_uid, gid_t fallback_gid,
               bool dynamicOwnership,
               const char *path, int oflags,
               bool *needUnlink)
{
    struct stat sb;
    bool is_reg = true;
    bool need_unlink = false;
    unsigned int vfoflags = 0;
    int fd = -1;
    int path_shared = virFileIsSharedFS(path);
    uid_t uid = geteuid();
    gid_t gid = getegid();

    /* path might be a pre-existing block dev, in which case
     * we need to skip the create step, and also avoid unlink
     * in the failure case */
    if (oflags & O_CREAT) {
        need_unlink = true;

        /* Don't force chown on network-shared FS
         * as it is likely to fail. */
        if (path_shared <= 0 || dynamicOwnership)
            vfoflags |= VIR_FILE_OPEN_FORCE_OWNER;

        if (stat(path, &sb) == 0) {
            /* It already exists, we don't want to delete it on error */
            need_unlink = false;

            is_reg = !!S_ISREG(sb.st_mode);
            /* If the path is regular file which exists
             * already and dynamic_ownership is off, we don't
             * want to change its ownership, just open it as-is */
            if (is_reg && !dynamicOwnership) {
                uid = sb.st_uid;
                gid = sb.st_gid;
            }
        }
    }

    /* First try creating the file as root */
    if (!is_reg) {
        if ((fd = open(path, oflags & ~O_CREAT)) < 0) {
            fd = -errno;
            goto error;
        }
    } else {
        if ((fd = virFileOpenAs(path, oflags, S_IRUSR | S_IWUSR, uid, gid,
                                vfoflags | VIR_FILE_OPEN_NOFORK)) < 0) {
            /* If we failed as root, and the error was permission-denied
               (EACCES or EPERM), assume it's on a network-connected share
               where root access is restricted (eg, root-squashed NFS). If the
               qemu user is non-root, just set a flag to
               bypass security driver shenanigans, and retry the operation
               after doing setuid to qemu user */
            if ((fd != -EACCES && fd != -EPERM) || fallback_uid == geteuid())
                goto error;

            /* On Linux we can also verify the FS-type of the directory. */
            switch (path_shared) {
                case 1:
                    /* it was on a network share, so we'll continue
                     * as outlined above
                     */
                    break;

                case -1:
                    virReportSystemError(-fd, oflags & O_CREAT
                                         ? _("Failed to create file "
                                             "'%s': couldn't determine fs type")
                                         : _("Failed to open file "
                                             "'%s': couldn't determine fs type"),
                                         path);
                    goto cleanup;

                case 0:
                default:
                    /* local file - log the error returned by virFileOpenAs */
                    goto error;
            }

            /* If we created the file above, then we need to remove it;
             * otherwise, the next attempt to create will fail. If the
             * file had already existed before we got here, then we also
             * don't want to delete it and allow the following to succeed
             * or fail based on existing protections
             */
            if (need_unlink)
                unlink(path);

            /* Retry creating the file as qemu user */

            /* Since we're passing different modes... */
            vfoflags |= VIR_FILE_OPEN_FORCE_MODE;

            if ((fd = virFileOpenAs(path, oflags,
                                    S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP,
                                    fallback_uid, fallback_gid,
                                    vfoflags | VIR_FILE_OPEN_FORK)) < 0) {
                virReportSystemError(-fd, oflags & O_CREAT
                                     ? _("Error from child process creating '%s'")
                                     : _("Error from child process opening '%s'"),
                                     path);
                goto cleanup;
            }
        }
    }
 cleanup:
    if (needUnlink)
        *needUnlink = need_unlink;
    return fd;

 error:
    virReportSystemError(-fd, oflags & O_CREAT
                         ? _("Failed to create file '%s'")
                         : _("Failed to open file '%s'"),
                         path);
    goto cleanup;
}


static int
qemuFileWrapperFDClose(virDomainObjPtr vm,
                       virFileWrapperFdPtr fd)
{
    int ret;

    /* virFileWrapperFd uses iohelper to write data onto disk.
     * However, iohelper calls fdatasync() which may take ages to
     * finish. Therefore, we shouldn't be waiting with the domain
     * object locked. */

    /* XXX Currently, this function is intended for *Save() only
     * as restore needs some reworking before it's ready for
     * this. */

    virObjectUnlock(vm);
    ret = virFileWrapperFdClose(fd);
    virObjectLock(vm);
    if (!virDomainObjIsActive(vm)) {
        if (virGetLastErrorCode() == VIR_ERR_OK)
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("domain is no longer running"));
        ret = -1;
    }
    return ret;
}


/* Helper function to execute a migration to file with a correct save header
 * the caller needs to make sure that the processors are stopped and do all other
 * actions besides saving memory */
static int
qemuDomainSaveMemory(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     const char *path,
                     virQEMUSaveDataPtr data,
                     virCommandPtr compressor,
                     unsigned int flags,
                     qemuDomainAsyncJob asyncJob)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    bool needUnlink = false;
    int ret = -1;
    int fd = -1;
    int directFlag = 0;
    virFileWrapperFdPtr wrapperFd = NULL;
    unsigned int wrapperFlags = VIR_FILE_WRAPPER_NON_BLOCKING;

    /* Obtain the file handle.  */
    if ((flags & VIR_DOMAIN_SAVE_BYPASS_CACHE)) {
        wrapperFlags |= VIR_FILE_WRAPPER_BYPASS_CACHE;
        directFlag = virFileDirectFdFlag();
        if (directFlag < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("bypass cache unsupported by this system"));
            goto cleanup;
        }
    }

    fd = qemuOpenFileAs(cfg->user, cfg->group, false, path,
                        O_WRONLY | O_TRUNC | O_CREAT | directFlag,
                        &needUnlink);
    if (fd < 0)
        goto cleanup;

    if (qemuSecuritySetImageFDLabel(driver->securityManager, vm->def, fd) < 0)
        goto cleanup;

    if (!(wrapperFd = virFileWrapperFdNew(&fd, path, wrapperFlags)))
        goto cleanup;

    if (virQEMUSaveDataWrite(data, fd, path) < 0)
        goto cleanup;

    /* Perform the migration */
    if (qemuMigrationSrcToFile(driver, vm, fd, compressor, asyncJob) < 0)
        goto cleanup;

    /* Touch up file header to mark image complete. */

    /* Reopen the file to touch up the header, since we aren't set
     * up to seek backwards on wrapperFd.  The reopened fd will
     * trigger a single page of file system cache pollution, but
     * that's acceptable.  */
    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno, _("unable to close %s"), path);
        goto cleanup;
    }

    if (qemuFileWrapperFDClose(vm, wrapperFd) < 0)
        goto cleanup;

    if ((fd = qemuOpenFile(driver, vm, path, O_WRONLY, NULL)) < 0 ||
        virQEMUSaveDataFinish(data, &fd, path) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    if (qemuFileWrapperFDClose(vm, wrapperFd) < 0)
        ret = -1;
    virFileWrapperFdFree(wrapperFd);

    if (ret < 0 && needUnlink)
        unlink(path);

    return ret;
}

/* The vm must be active + locked. Vm will be unlocked and
 * potentially free'd after this returns (eg transient VMs are freed
 * shutdown). So 'vm' must not be referenced by the caller after
 * this returns (whether returning success or failure).
 */
static int
qemuDomainSaveInternal(virQEMUDriverPtr driver,
                       virDomainObjPtr vm, const char *path,
                       int compressed, virCommandPtr compressor,
                       const char *xmlin, unsigned int flags)
{
    g_autofree char *xml = NULL;
    bool was_running = false;
    int ret = -1;
    virObjectEventPtr event = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUSaveDataPtr data = NULL;
    qemuDomainSaveCookiePtr cookie = NULL;

    if (!qemuMigrationSrcIsAllowed(driver, vm, false, 0))
        goto cleanup;

    if (qemuDomainObjBeginAsyncJob(driver, vm, QEMU_ASYNC_JOB_SAVE,
                                   VIR_DOMAIN_JOB_OPERATION_SAVE, flags) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("guest unexpectedly quit"));
        goto endjob;
    }

    priv->job.current->statsType = QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP;

    /* Pause */
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        was_running = true;
        if (qemuProcessStopCPUs(driver, vm, VIR_DOMAIN_PAUSED_SAVE,
                                QEMU_ASYNC_JOB_SAVE) < 0)
            goto endjob;

        if (!virDomainObjIsActive(vm)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("guest unexpectedly quit"));
            goto endjob;
        }
    }

   /* libvirt-domain.c already guaranteed these two flags are exclusive.  */
    if (flags & VIR_DOMAIN_SAVE_RUNNING)
        was_running = true;
    else if (flags & VIR_DOMAIN_SAVE_PAUSED)
        was_running = false;

    /* Get XML for the domain.  Restore needs only the inactive xml,
     * including secure.  We should get the same result whether xmlin
     * is NULL or whether it was the live xml of the domain moments
     * before.  */
    if (xmlin) {
        virDomainDefPtr def = NULL;

        if (!(def = virDomainDefParseString(xmlin, driver->xmlopt,
                                            priv->qemuCaps,
                                            VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                            VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE))) {
            goto endjob;
        }
        if (!qemuDomainCheckABIStability(driver, vm, def)) {
            virDomainDefFree(def);
            goto endjob;
        }
        xml = qemuDomainDefFormatLive(driver, priv->qemuCaps, def, NULL, true, true);
    } else {
        xml = qemuDomainDefFormatLive(driver, priv->qemuCaps, vm->def,
                                      priv->origCPU, true, true);
    }
    if (!xml) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("failed to get domain xml"));
        goto endjob;
    }

    if (!(cookie = qemuDomainSaveCookieNew(vm)))
        goto endjob;

    if (!(data = virQEMUSaveDataNew(xml, cookie, was_running, compressed,
                                    driver->xmlopt)))
        goto endjob;
    xml = NULL;

    ret = qemuDomainSaveMemory(driver, vm, path, data, compressor,
                               flags, QEMU_ASYNC_JOB_SAVE);
    if (ret < 0)
        goto endjob;

    /* Shut it down */
    qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_SAVED,
                    QEMU_ASYNC_JOB_SAVE, 0);
    virDomainAuditStop(vm, "saved");
    event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                              VIR_DOMAIN_EVENT_STOPPED_SAVED);
 endjob:
    if (ret < 0) {
        if (was_running && virDomainObjIsActive(vm)) {
            virErrorPtr save_err;
            virErrorPreserveLast(&save_err);
            if (qemuProcessStartCPUs(driver, vm,
                                     VIR_DOMAIN_RUNNING_SAVE_CANCELED,
                                     QEMU_ASYNC_JOB_SAVE) < 0) {
                VIR_WARN("Unable to resume guest CPUs after save failure");
                virObjectEventStateQueue(driver->domainEventState,
                                     virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_API_ERROR));
            }
            virErrorRestore(&save_err);
        }
    }
    qemuDomainObjEndAsyncJob(driver, vm);
    if (ret == 0)
        qemuDomainRemoveInactiveJob(driver, vm);

 cleanup:
    virObjectUnref(cookie);
    virQEMUSaveDataFree(data);
    virObjectEventStateQueue(driver->domainEventState, event);
    return ret;
}


/* qemuGetCompressionProgram:
 * @imageFormat: String representation from qemu.conf for the compression
 *               image format being used (dump, save, or snapshot).
 * @compresspath: Pointer to a character string to store the fully qualified
 *                path from virFindFileInPath.
 * @styleFormat: String representing the style of format (dump, save, snapshot)
 * @use_raw_on_fail: Boolean indicating how to handle the error path. For
 *                   callers that are OK with invalid data or inability to
 *                   find the compression program, just return a raw format
 *                   and let the path remain as NULL.
 *
 * Returns:
 *    virQEMUSaveFormat    - Integer representation of the compression
 *                           program to be used for particular style
 *                           (e.g. dump, save, or snapshot).
 *    QEMU_SAVE_FORMAT_RAW - If there is no qemu.conf imageFormat value or
 *                           no there was an error, then just return RAW
 *                           indicating none.
 */
static int ATTRIBUTE_NONNULL(2)
qemuGetCompressionProgram(const char *imageFormat,
                          virCommandPtr *compressor,
                          const char *styleFormat,
                          bool use_raw_on_fail)
{
    int ret;
    const char *prog;

    *compressor = NULL;

    if (!imageFormat)
        return QEMU_SAVE_FORMAT_RAW;

    if ((ret = qemuSaveCompressionTypeFromString(imageFormat)) < 0)
        goto error;

    if (ret == QEMU_SAVE_FORMAT_RAW)
        return QEMU_SAVE_FORMAT_RAW;

    if (!(prog = virFindFileInPath(imageFormat)))
        goto error;

    *compressor = virCommandNew(prog);
    virCommandAddArg(*compressor, "-c");
    if (ret == QEMU_SAVE_FORMAT_XZ)
        virCommandAddArg(*compressor, "-3");

    return ret;

 error:
    if (ret < 0) {
        if (use_raw_on_fail)
            VIR_WARN("Invalid %s image format specified in "
                     "configuration file, using raw",
                     styleFormat);
        else
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Invalid %s image format specified "
                             "in configuration file"),
                           styleFormat);
    } else {
        if (use_raw_on_fail)
            VIR_WARN("Compression program for %s image format in "
                     "configuration file isn't available, using raw",
                     styleFormat);
        else
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Compression program for %s image format "
                             "in configuration file isn't available"),
                           styleFormat);
    }

    /* Use "raw" as the format if the specified format is not valid,
     * or the compress program is not available. */
    if (use_raw_on_fail)
        return QEMU_SAVE_FORMAT_RAW;

    return -1;
}


static int
qemuDomainSaveFlags(virDomainPtr dom, const char *path, const char *dxml,
                    unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    int compressed;
    g_autoptr(virCommand) compressor = NULL;
    int ret = -1;
    virDomainObjPtr vm = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_SAVE_BYPASS_CACHE |
                  VIR_DOMAIN_SAVE_RUNNING |
                  VIR_DOMAIN_SAVE_PAUSED, -1);

    cfg = virQEMUDriverGetConfig(driver);
    if ((compressed = qemuGetCompressionProgram(cfg->saveImageFormat,
                                                &compressor,
                                                "save", false)) < 0)
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSaveFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    ret = qemuDomainSaveInternal(driver, vm, path, compressed,
                                 compressor, dxml, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainSave(virDomainPtr dom, const char *path)
{
    return qemuDomainSaveFlags(dom, path, NULL, 0);
}

static char *
qemuDomainManagedSavePath(virQEMUDriverPtr driver, virDomainObjPtr vm)
{
    char *ret;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);

    if (!(ret = g_strdup_printf("%s/%s.save", cfg->saveDir, vm->def->name)))
        return NULL;

    return ret;
}

static int
qemuDomainManagedSave(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    int compressed;
    g_autoptr(virCommand) compressor = NULL;
    virDomainObjPtr vm;
    g_autofree char *name = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_SAVE_BYPASS_CACHE |
                  VIR_DOMAIN_SAVE_RUNNING |
                  VIR_DOMAIN_SAVE_PAUSED, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainManagedSaveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot do managed save for transient domain"));
        goto cleanup;
    }

    cfg = virQEMUDriverGetConfig(driver);
    if ((compressed = qemuGetCompressionProgram(cfg->saveImageFormat,
                                                &compressor,
                                                "save", false)) < 0)
        goto cleanup;

    if (!(name = qemuDomainManagedSavePath(driver, vm)))
        goto cleanup;

    VIR_INFO("Saving state of domain '%s' to '%s'", vm->def->name, name);

    ret = qemuDomainSaveInternal(driver, vm, name, compressed,
                                 compressor, NULL, flags);
    if (ret == 0)
        vm->hasManagedSave = true;

 cleanup:
    virDomainObjEndAPI(&vm);

    return ret;
}

static int
qemuDomainManagedSaveLoad(virDomainObjPtr vm,
                          void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    g_autofree char *name = NULL;
    int ret = -1;

    virObjectLock(vm);

    if (!(name = qemuDomainManagedSavePath(driver, vm)))
        goto cleanup;

    vm->hasManagedSave = virFileExists(name);

    ret = 0;
 cleanup:
    virObjectUnlock(vm);
    return ret;
}


static int
qemuDomainHasManagedSaveImage(virDomainPtr dom, unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainHasManagedSaveImageEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = vm->hasManagedSave;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainManagedSaveRemove(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    g_autofree char *name = NULL;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainManagedSaveRemoveEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(name = qemuDomainManagedSavePath(driver, vm)))
        goto cleanup;

    if (unlink(name) < 0) {
        virReportSystemError(errno,
                             _("Failed to remove managed save file '%s'"),
                             name);
        goto cleanup;
    }

    vm->hasManagedSave = false;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/**
 * qemuDumpWaitForCompletion:
 * @vm: domain object
 *
 * If the query dump capability exists, then it's possible to start a
 * guest memory dump operation using a thread via a 'detach' qualifier
 * to the dump guest memory command. This allows the async check if the
 * dump is done.
 *
 * Returns 0 on success, -1 on failure
 */
static int
qemuDumpWaitForCompletion(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    VIR_DEBUG("Waiting for dump completion");
    while (!priv->job.dumpCompleted && !priv->job.abortJob) {
        if (virDomainObjWait(vm) < 0)
            return -1;
    }

    if (priv->job.current->stats.dump.status == QEMU_MONITOR_DUMP_STATUS_FAILED) {
        if (priv->job.error)
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("memory-only dump failed: %s"),
                           priv->job.error);
        else
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("memory-only dump failed for unknown reason"));

        return -1;
    }
    qemuDomainJobInfoUpdateTime(priv->job.current);

    return 0;
}


static int
qemuDumpToFd(virQEMUDriverPtr driver,
             virDomainObjPtr vm,
             int fd,
             qemuDomainAsyncJob asyncJob,
             const char *dumpformat)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool detach = false;
    int ret = -1;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DUMP_GUEST_MEMORY)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("dump-guest-memory is not supported"));
        return -1;
    }

    detach = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DUMP_COMPLETED);

    if (qemuSecuritySetImageFDLabel(driver->securityManager, vm->def, fd) < 0)
        return -1;

    if (detach)
        priv->job.current->statsType = QEMU_DOMAIN_JOB_STATS_TYPE_MEMDUMP;
    else
        VIR_FREE(priv->job.current);

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        return -1;

    if (dumpformat) {
        ret = qemuMonitorGetDumpGuestMemoryCapability(priv->mon, dumpformat);

        if (ret <= 0) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("unsupported dumpformat '%s' "
                             "for this QEMU binary"),
                           dumpformat);
            ignore_value(qemuDomainObjExitMonitor(driver, vm));
            return -1;
        }
    }

    ret = qemuMonitorDumpToFd(priv->mon, fd, dumpformat, detach);

    if ((qemuDomainObjExitMonitor(driver, vm) < 0) || ret < 0)
        return -1;

    if (detach)
        ret = qemuDumpWaitForCompletion(vm);

    return ret;
}


static int
doCoreDump(virQEMUDriverPtr driver,
           virDomainObjPtr vm,
           const char *path,
           unsigned int dump_flags,
           unsigned int dumpformat)
{
    int fd = -1;
    int ret = -1;
    virFileWrapperFdPtr wrapperFd = NULL;
    int directFlag = 0;
    unsigned int flags = VIR_FILE_WRAPPER_NON_BLOCKING;
    const char *memory_dump_format = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    g_autoptr(virCommand) compressor = NULL;

    /* We reuse "save" flag for "dump" here. Then, we can support the same
     * format in "save" and "dump". This path doesn't need the compression
     * program to exist and can ignore the return value - it only cares to
     * get the compressor */
    ignore_value(qemuGetCompressionProgram(cfg->dumpImageFormat,
                                           &compressor,
                                           "dump", true));

    /* Create an empty file with appropriate ownership.  */
    if (dump_flags & VIR_DUMP_BYPASS_CACHE) {
        flags |= VIR_FILE_WRAPPER_BYPASS_CACHE;
        directFlag = virFileDirectFdFlag();
        if (directFlag < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("bypass cache unsupported by this system"));
            goto cleanup;
        }
    }
    /* Core dumps usually imply last-ditch analysis efforts are
     * desired, so we intentionally do not unlink even if a file was
     * created.  */
    if ((fd = qemuOpenFileAs(cfg->user, cfg->group, false, path,
                             O_CREAT | O_TRUNC | O_WRONLY | directFlag,
                             NULL)) < 0)
        goto cleanup;

    if (!(wrapperFd = virFileWrapperFdNew(&fd, path, flags)))
        goto cleanup;

    if (dump_flags & VIR_DUMP_MEMORY_ONLY) {
        if (!(memory_dump_format = qemuDumpFormatTypeToString(dumpformat))) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("unknown dumpformat '%d'"), dumpformat);
            goto cleanup;
        }

        /* qemu dumps in "elf" without dumpformat set */
        if (STREQ(memory_dump_format, "elf"))
            memory_dump_format = NULL;

        ret = qemuDumpToFd(driver, vm, fd, QEMU_ASYNC_JOB_DUMP,
                           memory_dump_format);
    } else {
        if (dumpformat != VIR_DOMAIN_CORE_DUMP_FORMAT_RAW) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("kdump-compressed format is only supported with "
                             "memory-only dump"));
            goto cleanup;
        }

        if (!qemuMigrationSrcIsAllowed(driver, vm, false, 0))
            goto cleanup;

        ret = qemuMigrationSrcToFile(driver, vm, fd, compressor,
                                     QEMU_ASYNC_JOB_DUMP);
    }

    if (ret < 0)
        goto cleanup;

    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno,
                             _("unable to close file %s"),
                             path);
        goto cleanup;
    }
    if (qemuFileWrapperFDClose(vm, wrapperFd) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    if (qemuFileWrapperFDClose(vm, wrapperFd) < 0)
        ret = -1;
    virFileWrapperFdFree(wrapperFd);
    if (ret != 0)
        unlink(path);
    return ret;
}


static int
qemuDomainCoreDumpWithFormat(virDomainPtr dom,
                             const char *path,
                             unsigned int dumpformat,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv = NULL;
    bool resume = false, paused = false;
    int ret = -1;
    virObjectEventPtr event = NULL;

    virCheckFlags(VIR_DUMP_LIVE | VIR_DUMP_CRASH |
                  VIR_DUMP_BYPASS_CACHE | VIR_DUMP_RESET |
                  VIR_DUMP_MEMORY_ONLY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainCoreDumpWithFormatEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAsyncJob(driver, vm,
                                   QEMU_ASYNC_JOB_DUMP,
                                   VIR_DOMAIN_JOB_OPERATION_DUMP,
                                   flags) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;
    priv->job.current->statsType = QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP;

    /* Migrate will always stop the VM, so the resume condition is
       independent of whether the stop command is issued.  */
    resume = virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING;

    /* Pause domain for non-live dump */
    if (!(flags & VIR_DUMP_LIVE) &&
        virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        if (qemuProcessStopCPUs(driver, vm, VIR_DOMAIN_PAUSED_DUMP,
                                QEMU_ASYNC_JOB_DUMP) < 0)
            goto endjob;
        paused = true;

        if (!virDomainObjIsActive(vm)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("guest unexpectedly quit"));
            goto endjob;
        }
    }

    if ((ret = doCoreDump(driver, vm, path, flags, dumpformat)) < 0)
        goto endjob;

    paused = true;

 endjob:
    if ((ret == 0) && (flags & VIR_DUMP_CRASH)) {
        qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_CRASHED,
                        QEMU_ASYNC_JOB_DUMP, 0);
        virDomainAuditStop(vm, "crashed");
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STOPPED,
                                         VIR_DOMAIN_EVENT_STOPPED_CRASHED);
    } else if (((resume && paused) || (flags & VIR_DUMP_RESET)) &&
               virDomainObjIsActive(vm)) {
        if ((ret == 0) && (flags & VIR_DUMP_RESET)) {
            qemuDomainObjEnterMonitor(driver, vm);
            ret = qemuMonitorSystemReset(priv->mon);
            if (qemuDomainObjExitMonitor(driver, vm) < 0)
                ret = -1;
        }

        if (resume && virDomainObjIsActive(vm)) {
            if (qemuProcessStartCPUs(driver, vm,
                                     VIR_DOMAIN_RUNNING_UNPAUSED,
                                     QEMU_ASYNC_JOB_DUMP) < 0) {
                event = virDomainEventLifecycleNewFromObj(vm,
                                                          VIR_DOMAIN_EVENT_SUSPENDED,
                                                          VIR_DOMAIN_EVENT_SUSPENDED_API_ERROR);
                if (virGetLastErrorCode() == VIR_ERR_OK)
                    virReportError(VIR_ERR_OPERATION_FAILED,
                                   "%s", _("resuming after dump failed"));
            }
        }
    }

    qemuDomainObjEndAsyncJob(driver, vm);
    if (ret == 0 && flags & VIR_DUMP_CRASH)
        qemuDomainRemoveInactiveJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);
    return ret;
}


static int
qemuDomainCoreDump(virDomainPtr dom,
                   const char *path,
                   unsigned int flags)
{
    return qemuDomainCoreDumpWithFormat(dom, path,
                                        VIR_DOMAIN_CORE_DUMP_FORMAT_RAW,
                                        flags);
}


static char *
qemuDomainScreenshot(virDomainPtr dom,
                     virStreamPtr st,
                     unsigned int screen,
                     unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    g_autofree char *tmp = NULL;
    int tmp_fd = -1;
    size_t i;
    const char *videoAlias = NULL;
    char *ret = NULL;
    bool unlink_tmp = false;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainScreenshotEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!vm->def->nvideos) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                      _("no screens to take screenshot from"));
        goto endjob;
    }

    if (screen) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_SCREENDUMP_DEVICE)) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("qemu does not allow specifying screen ID"));
            goto endjob;
        }

        for (i = 0; i < vm->def->nvideos; i++) {
            const virDomainVideoDef *video = vm->def->videos[i];

            if (screen < video->heads) {
                videoAlias = video->info.alias;
                break;
            }

            screen -= video->heads;
        }

        if (i == vm->def->nvideos) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("no such screen ID"));
            goto endjob;
        }
    }

    if (!(tmp = g_strdup_printf("%s/qemu.screendump.XXXXXX", cfg->cacheDir)))
        goto endjob;

    if ((tmp_fd = g_mkstemp_full(tmp, O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR)) == -1) {
        virReportSystemError(errno, _("g_mkstemp(\"%s\") failed"), tmp);
        goto endjob;
    }
    unlink_tmp = true;

    qemuSecuritySetSavedStateLabel(driver, vm, tmp);

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorScreendump(priv->mon, videoAlias, screen, tmp) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto endjob;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto endjob;

    if (VIR_CLOSE(tmp_fd) < 0) {
        virReportSystemError(errno, _("unable to close %s"), tmp);
        goto endjob;
    }

    if (virFDStreamOpenFile(st, tmp, 0, 0, O_RDONLY) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("unable to open stream"));
        goto endjob;
    }

    ret = g_strdup("image/x-portable-pixmap");

 endjob:
    VIR_FORCE_CLOSE(tmp_fd);
    if (unlink_tmp)
        unlink(tmp);

    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static char *
getAutoDumpPath(virQEMUDriverPtr driver,
                virDomainObjPtr vm)
{
    g_autofree char *domname = virDomainDefGetShortName(vm->def);
    char timestr[100];
    struct tm time_info;
    time_t curtime = time(NULL);
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    if (!domname)
        return NULL;

    cfg = virQEMUDriverGetConfig(driver);

    localtime_r(&curtime, &time_info);
    strftime(timestr, sizeof(timestr), "%Y-%m-%d-%H:%M:%S", &time_info);

    return g_strdup_printf("%s/%s-%s", cfg->autoDumpPath, domname, timestr);
}

static void
processWatchdogEvent(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     int action)
{
    int ret;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    g_autofree char *dumpfile = getAutoDumpPath(driver, vm);
    unsigned int flags = VIR_DUMP_MEMORY_ONLY;

    if (!dumpfile)
        return;

    switch (action) {
    case VIR_DOMAIN_WATCHDOG_ACTION_DUMP:
        if (qemuDomainObjBeginAsyncJob(driver, vm,
                                       QEMU_ASYNC_JOB_DUMP,
                                       VIR_DOMAIN_JOB_OPERATION_DUMP,
                                       flags) < 0) {
            return;
        }

        if (virDomainObjCheckActive(vm) < 0)
            goto endjob;

        flags |= cfg->autoDumpBypassCache ? VIR_DUMP_BYPASS_CACHE: 0;
        if ((ret = doCoreDump(driver, vm, dumpfile, flags,
                              VIR_DOMAIN_CORE_DUMP_FORMAT_RAW)) < 0)
            virReportError(VIR_ERR_OPERATION_FAILED,
                           "%s", _("Dump failed"));

        ret = qemuProcessStartCPUs(driver, vm,
                                   VIR_DOMAIN_RUNNING_UNPAUSED,
                                   QEMU_ASYNC_JOB_DUMP);

        if (ret < 0)
            virReportError(VIR_ERR_OPERATION_FAILED,
                           "%s", _("Resuming after dump failed"));
        break;
    default:
        return;
    }

 endjob:
    qemuDomainObjEndAsyncJob(driver, vm);
}

static int
doCoreDumpToAutoDumpPath(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         unsigned int flags)
{
    int ret = -1;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    g_autofree char *dumpfile = getAutoDumpPath(driver, vm);

    if (!dumpfile)
        return -1;

    flags |= cfg->autoDumpBypassCache ? VIR_DUMP_BYPASS_CACHE: 0;
    if ((ret = doCoreDump(driver, vm, dumpfile, flags,
                          VIR_DOMAIN_CORE_DUMP_FORMAT_RAW)) < 0)
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("Dump failed"));
    return ret;
}


static void
qemuProcessGuestPanicEventInfo(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               qemuMonitorEventPanicInfoPtr info)
{
    g_autofree char *msg = qemuMonitorGuestPanicEventInfoFormatMsg(info);
    g_autofree char *timestamp = virTimeStringNow();

    if (msg && timestamp)
        qemuDomainLogAppendMessage(driver, vm, "%s: panic %s\n", timestamp, msg);
}


static void
processGuestPanicEvent(virQEMUDriverPtr driver,
                       virDomainObjPtr vm,
                       int action,
                       qemuMonitorEventPanicInfoPtr info)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    bool removeInactive = false;
    unsigned long flags = VIR_DUMP_MEMORY_ONLY;

    if (qemuDomainObjBeginAsyncJob(driver, vm, QEMU_ASYNC_JOB_DUMP,
                                   VIR_DOMAIN_JOB_OPERATION_DUMP, flags) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Ignoring GUEST_PANICKED event from inactive domain %s",
                  vm->def->name);
        goto endjob;
    }

    if (info)
        qemuProcessGuestPanicEventInfo(driver, vm, info);

    virDomainObjSetState(vm, VIR_DOMAIN_CRASHED, VIR_DOMAIN_CRASHED_PANICKED);

    event = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_CRASHED,
                                              VIR_DOMAIN_EVENT_CRASHED_PANICKED);

    virObjectEventStateQueue(driver->domainEventState, event);

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
        VIR_WARN("Unable to save status on vm %s after state change",
                 vm->def->name);
    }

    if (virDomainLockProcessPause(driver->lockManager, vm, &priv->lockState) < 0)
        VIR_WARN("Unable to release lease on %s", vm->def->name);
    VIR_DEBUG("Preserving lock state '%s'", NULLSTR(priv->lockState));

    switch (action) {
    case VIR_DOMAIN_LIFECYCLE_ACTION_COREDUMP_DESTROY:
        if (doCoreDumpToAutoDumpPath(driver, vm, flags) < 0)
            goto endjob;
        G_GNUC_FALLTHROUGH;

    case VIR_DOMAIN_LIFECYCLE_ACTION_DESTROY:
        qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_CRASHED,
                        QEMU_ASYNC_JOB_DUMP, 0);
        event = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_STOPPED,
                                                  VIR_DOMAIN_EVENT_STOPPED_CRASHED);

        virObjectEventStateQueue(driver->domainEventState, event);
        virDomainAuditStop(vm, "destroyed");
        removeInactive = true;
        break;

    case VIR_DOMAIN_LIFECYCLE_ACTION_COREDUMP_RESTART:
        if (doCoreDumpToAutoDumpPath(driver, vm, flags) < 0)
            goto endjob;
        G_GNUC_FALLTHROUGH;

    case VIR_DOMAIN_LIFECYCLE_ACTION_RESTART:
        qemuDomainSetFakeReboot(driver, vm, true);
        qemuProcessShutdownOrReboot(driver, vm);
        break;

    case VIR_DOMAIN_LIFECYCLE_ACTION_PRESERVE:
        break;

    default:
        break;
    }

 endjob:
    qemuDomainObjEndAsyncJob(driver, vm);
    if (removeInactive)
        qemuDomainRemoveInactiveJob(driver, vm);
}


static void
processDeviceDeletedEvent(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          const char *devAlias)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    virDomainDeviceDef dev;

    VIR_DEBUG("Removing device %s from domain %p %s",
              devAlias, vm, vm->def->name);

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain is not running");
        goto endjob;
    }

    if (STRPREFIX(devAlias, "vcpu")) {
        qemuDomainRemoveVcpuAlias(driver, vm, devAlias);
    } else {
        if (virDomainDefFindDevice(vm->def, devAlias, &dev, true) < 0)
            goto endjob;

        if (qemuDomainRemoveDevice(driver, vm, &dev) < 0)
            goto endjob;
    }

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        VIR_WARN("unable to save domain status after removing device %s",
                 devAlias);

 endjob:
    qemuDomainObjEndJob(driver, vm);
}


static void
syncNicRxFilterMacAddr(char *ifname, virNetDevRxFilterPtr guestFilter,
                       virNetDevRxFilterPtr hostFilter)
{
    char newMacStr[VIR_MAC_STRING_BUFLEN];

    if (virMacAddrCmp(&hostFilter->mac, &guestFilter->mac)) {
        virMacAddrFormat(&guestFilter->mac, newMacStr);

        /* set new MAC address from guest to associated macvtap device */
        if (virNetDevSetMAC(ifname, &guestFilter->mac) < 0) {
            VIR_WARN("Couldn't set new MAC address %s to device %s "
                     "while responding to NIC_RX_FILTER_CHANGED",
                     newMacStr, ifname);
        } else {
            VIR_DEBUG("device %s MAC address set to %s", ifname, newMacStr);
        }
    }
}


static void
syncNicRxFilterGuestMulticast(char *ifname, virNetDevRxFilterPtr guestFilter,
                              virNetDevRxFilterPtr hostFilter)
{
    size_t i, j;
    bool found;
    char macstr[VIR_MAC_STRING_BUFLEN];

    for (i = 0; i < guestFilter->multicast.nTable; i++) {
        found = false;

        for (j = 0; j < hostFilter->multicast.nTable; j++) {
            if (virMacAddrCmp(&guestFilter->multicast.table[i],
                              &hostFilter->multicast.table[j]) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            virMacAddrFormat(&guestFilter->multicast.table[i], macstr);

            if (virNetDevAddMulti(ifname, &guestFilter->multicast.table[i]) < 0) {
                VIR_WARN("Couldn't add new multicast MAC address %s to "
                         "device %s while responding to NIC_RX_FILTER_CHANGED",
                         macstr, ifname);
            } else {
                VIR_DEBUG("Added multicast MAC %s to %s interface",
                          macstr, ifname);
            }
        }
    }
}


static void
syncNicRxFilterHostMulticast(char *ifname, virNetDevRxFilterPtr guestFilter,
                             virNetDevRxFilterPtr hostFilter)
{
    size_t i, j;
    bool found;
    char macstr[VIR_MAC_STRING_BUFLEN];

    for (i = 0; i < hostFilter->multicast.nTable; i++) {
        found = false;

        for (j = 0; j < guestFilter->multicast.nTable; j++) {
            if (virMacAddrCmp(&hostFilter->multicast.table[i],
                              &guestFilter->multicast.table[j]) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            virMacAddrFormat(&hostFilter->multicast.table[i], macstr);

            if (virNetDevDelMulti(ifname, &hostFilter->multicast.table[i]) < 0) {
                VIR_WARN("Couldn't delete multicast MAC address %s from "
                         "device %s while responding to NIC_RX_FILTER_CHANGED",
                         macstr, ifname);
            } else {
                VIR_DEBUG("Deleted multicast MAC %s from %s interface",
                          macstr, ifname);
            }
        }
    }
}


static void
syncNicRxFilterPromiscMode(char *ifname,
                           virNetDevRxFilterPtr guestFilter,
                           virNetDevRxFilterPtr hostFilter)
{
    bool promisc;
    bool setpromisc = false;

    /* Set macvtap promisc mode to true if the guest has vlans defined */
    /* or synchronize the macvtap promisc mode if different from guest */
    if (guestFilter->vlan.nTable > 0) {
        if (!hostFilter->promiscuous) {
            setpromisc = true;
            promisc = true;
        }
    } else if (hostFilter->promiscuous != guestFilter->promiscuous) {
        setpromisc = true;
        promisc = guestFilter->promiscuous;
    }

    if (setpromisc) {
        if (virNetDevSetPromiscuous(ifname, promisc) < 0) {
            VIR_WARN("Couldn't set PROMISC flag to %s for device %s "
                     "while responding to NIC_RX_FILTER_CHANGED",
                     promisc ? "true" : "false", ifname);
        }
    }
}


static void
syncNicRxFilterMultiMode(char *ifname, virNetDevRxFilterPtr guestFilter,
                         virNetDevRxFilterPtr hostFilter)
{
    if (hostFilter->multicast.mode != guestFilter->multicast.mode ||
        (guestFilter->multicast.overflow &&
         guestFilter->multicast.mode == VIR_NETDEV_RX_FILTER_MODE_NORMAL)) {
        switch (guestFilter->multicast.mode) {
        case VIR_NETDEV_RX_FILTER_MODE_ALL:
            if (virNetDevSetRcvAllMulti(ifname, true) < 0) {
                VIR_WARN("Couldn't set allmulticast flag to 'on' for "
                         "device %s while responding to "
                         "NIC_RX_FILTER_CHANGED", ifname);
            }
            break;

        case VIR_NETDEV_RX_FILTER_MODE_NORMAL:
            if (guestFilter->multicast.overflow &&
                (hostFilter->multicast.mode == VIR_NETDEV_RX_FILTER_MODE_ALL)) {
                break;
            }

            if (virNetDevSetRcvMulti(ifname, true) < 0) {
                VIR_WARN("Couldn't set multicast flag to 'on' for "
                         "device %s while responding to "
                         "NIC_RX_FILTER_CHANGED", ifname);
            }

            if (virNetDevSetRcvAllMulti(ifname,
                                        guestFilter->multicast.overflow) < 0) {
                VIR_WARN("Couldn't set allmulticast flag to '%s' for "
                         "device %s while responding to "
                         "NIC_RX_FILTER_CHANGED",
                         virTristateSwitchTypeToString(virTristateSwitchFromBool(guestFilter->multicast.overflow)),
                         ifname);
            }
            break;

        case VIR_NETDEV_RX_FILTER_MODE_NONE:
            if (virNetDevSetRcvAllMulti(ifname, false) < 0) {
                VIR_WARN("Couldn't set allmulticast flag to 'off' for "
                         "device %s while responding to "
                         "NIC_RX_FILTER_CHANGED", ifname);
            }

            if (virNetDevSetRcvMulti(ifname, false) < 0) {
                VIR_WARN("Couldn't set multicast flag to 'off' for "
                         "device %s while responding to "
                         "NIC_RX_FILTER_CHANGED",
                         ifname);
            }
            break;
        }
    }
}


static void
syncNicRxFilterDeviceOptions(char *ifname, virNetDevRxFilterPtr guestFilter,
                           virNetDevRxFilterPtr hostFilter)
{
    syncNicRxFilterPromiscMode(ifname, guestFilter, hostFilter);
    syncNicRxFilterMultiMode(ifname, guestFilter, hostFilter);
}


static void
syncNicRxFilterMulticast(char *ifname,
                         virNetDevRxFilterPtr guestFilter,
                         virNetDevRxFilterPtr hostFilter)
{
    syncNicRxFilterGuestMulticast(ifname, guestFilter, hostFilter);
    syncNicRxFilterHostMulticast(ifname, guestFilter, hostFilter);
}

static void
processNicRxFilterChangedEvent(virQEMUDriverPtr driver,
                               virDomainObjPtr vm,
                               const char *devAlias)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDeviceDef dev;
    virDomainNetDefPtr def;
    virNetDevRxFilterPtr guestFilter = NULL;
    virNetDevRxFilterPtr hostFilter = NULL;
    int ret;

    VIR_DEBUG("Received NIC_RX_FILTER_CHANGED event for device %s "
              "from domain %p %s",
              devAlias, vm, vm->def->name);

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain is not running");
        goto endjob;
    }

    if (virDomainDefFindDevice(vm->def, devAlias, &dev, true) < 0) {
        VIR_WARN("NIC_RX_FILTER_CHANGED event received for "
                 "non-existent device %s in domain %s",
                 devAlias, vm->def->name);
        goto endjob;
    }
    if (dev.type != VIR_DOMAIN_DEVICE_NET) {
        VIR_WARN("NIC_RX_FILTER_CHANGED event received for "
                 "non-network device %s in domain %s",
                 devAlias, vm->def->name);
        goto endjob;
    }
    def = dev.data.net;

    if (!virDomainNetGetActualTrustGuestRxFilters(def)) {
        VIR_DEBUG("ignore NIC_RX_FILTER_CHANGED event for network "
                  "device %s in domain %s",
                  def->info.alias, vm->def->name);
        /* not sending "query-rx-filter" will also suppress any
         * further NIC_RX_FILTER_CHANGED events for this device
         */
        goto endjob;
    }

    /* handle the event - send query-rx-filter and respond to it. */

    VIR_DEBUG("process NIC_RX_FILTER_CHANGED event for network "
              "device %s in domain %s", def->info.alias, vm->def->name);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorQueryRxFilter(priv->mon, devAlias, &guestFilter);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    if (ret < 0)
        goto endjob;

    if (virDomainNetGetActualType(def) == VIR_DOMAIN_NET_TYPE_DIRECT) {

        if (virNetDevGetRxFilter(def->ifname, &hostFilter)) {
            VIR_WARN("Couldn't get current RX filter for device %s "
                     "while responding to NIC_RX_FILTER_CHANGED",
                     def->ifname);
            goto endjob;
        }

        /* For macvtap connections, set the following macvtap network device
         * attributes to match those of the guest network device:
         * - MAC address
         * - Multicast MAC address table
         * - Device options:
         *   - PROMISC
         *   - MULTICAST
         *   - ALLMULTI
         */
        syncNicRxFilterMacAddr(def->ifname, guestFilter, hostFilter);
        syncNicRxFilterMulticast(def->ifname, guestFilter, hostFilter);
        syncNicRxFilterDeviceOptions(def->ifname, guestFilter, hostFilter);
    }

    if (virDomainNetGetActualType(def) == VIR_DOMAIN_NET_TYPE_NETWORK) {
        const char *brname = virDomainNetGetActualBridgeName(def);

        /* For libivrt network connections, set the following TUN/TAP network
         * device attributes to match those of the guest network device:
         * - QoS filters (which are based on MAC address)
         */
        if (virDomainNetGetActualBandwidth(def) &&
            def->data.network.actual &&
            virNetDevBandwidthUpdateFilter(brname, &guestFilter->mac,
                                           def->data.network.actual->class_id) < 0)
            goto endjob;
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virNetDevRxFilterFree(hostFilter);
    virNetDevRxFilterFree(guestFilter);
}


static void
processSerialChangedEvent(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          const char *devAlias,
                          bool connected)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    virDomainChrDeviceState newstate;
    virObjectEventPtr event = NULL;
    virDomainDeviceDef dev;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (connected)
        newstate = VIR_DOMAIN_CHR_DEVICE_STATE_CONNECTED;
    else
        newstate = VIR_DOMAIN_CHR_DEVICE_STATE_DISCONNECTED;

    VIR_DEBUG("Changing serial port state %s in domain %p %s",
              devAlias, vm, vm->def->name);

    if (newstate == VIR_DOMAIN_CHR_DEVICE_STATE_DISCONNECTED &&
        virDomainObjIsActive(vm) && priv->agent) {
        /* peek into the domain definition to find the channel */
        if (virDomainDefFindDevice(vm->def, devAlias, &dev, true) == 0 &&
            dev.type == VIR_DOMAIN_DEVICE_CHR &&
            dev.data.chr->deviceType == VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL &&
            dev.data.chr->targetType == VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO &&
            STREQ_NULLABLE(dev.data.chr->target.name, "org.qemu.guest_agent.0"))
            /* Close agent monitor early, so that other threads
             * waiting for the agent to reply can finish and our
             * job we acquire below can succeed. */
            qemuAgentNotifyClose(priv->agent);

        /* now discard the data, since it may possibly change once we unlock
         * while entering the job */
        memset(&dev, 0, sizeof(dev));
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain is not running");
        goto endjob;
    }

    if (virDomainDefFindDevice(vm->def, devAlias, &dev, true) < 0)
        goto endjob;

    /* we care only about certain devices */
    if (dev.type != VIR_DOMAIN_DEVICE_CHR ||
        dev.data.chr->deviceType != VIR_DOMAIN_CHR_DEVICE_TYPE_CHANNEL ||
        dev.data.chr->targetType != VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO)
        goto endjob;

    dev.data.chr->state = newstate;

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        VIR_WARN("unable to save status of domain %s after updating state of "
                 "channel %s", vm->def->name, devAlias);

    if (STREQ_NULLABLE(dev.data.chr->target.name, "org.qemu.guest_agent.0")) {
        if (newstate == VIR_DOMAIN_CHR_DEVICE_STATE_CONNECTED) {
            if (qemuConnectAgent(driver, vm) < 0)
                goto endjob;
        } else {
            if (priv->agent) {
                qemuAgentClose(priv->agent);
                priv->agent = NULL;
            }
            priv->agentError = false;
        }

        event = virDomainEventAgentLifecycleNewFromObj(vm, newstate,
                                                       VIR_CONNECT_DOMAIN_EVENT_AGENT_LIFECYCLE_REASON_CHANNEL);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);
}


static void
processBlockJobEvent(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     const char *diskAlias,
                     int type,
                     int status)
{
    virDomainDiskDefPtr disk;
    g_autoptr(qemuBlockJobData) job = NULL;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain is not running");
        goto endjob;
    }

    if (!(disk = qemuProcessFindDomainDiskByAliasOrQOM(vm, diskAlias, NULL))) {
        VIR_DEBUG("disk %s not found", diskAlias);
        goto endjob;
    }

    if (!(job = qemuBlockJobDiskGetJob(disk))) {
        VIR_DEBUG("creating new block job object for '%s'", diskAlias);
        if (!(job = qemuBlockJobDiskNew(vm, disk, type, diskAlias)))
            goto endjob;
        job->state = QEMU_BLOCKJOB_STATE_RUNNING;
    }

    job->newstate = status;

    qemuBlockJobUpdate(vm, job, QEMU_ASYNC_JOB_NONE);

 endjob:
    qemuDomainObjEndJob(driver, vm);
}


static void
processJobStatusChangeEvent(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            qemuBlockJobDataPtr job)
{
    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain is not running");
        goto endjob;
    }

    qemuBlockJobUpdate(vm, job, QEMU_ASYNC_JOB_NONE);

 endjob:
    qemuDomainObjEndJob(driver, vm);
}


static void
processMonitorEOFEvent(virQEMUDriverPtr driver,
                       virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int eventReason = VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN;
    int stopReason = VIR_DOMAIN_SHUTOFF_SHUTDOWN;
    const char *auditReason = "shutdown";
    unsigned int stopFlags = 0;
    virObjectEventPtr event = NULL;

    if (qemuProcessBeginStopJob(driver, vm, QEMU_JOB_DESTROY, true) < 0)
        return;

    if (!virDomainObjIsActive(vm)) {
        VIR_DEBUG("Domain %p '%s' is not active, ignoring EOF",
                  vm, vm->def->name);
        goto endjob;
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_SHUTDOWN) {
        VIR_DEBUG("Monitor connection to '%s' closed without SHUTDOWN event; "
                  "assuming the domain crashed", vm->def->name);
        eventReason = VIR_DOMAIN_EVENT_STOPPED_FAILED;
        stopReason = VIR_DOMAIN_SHUTOFF_CRASHED;
        auditReason = "failed";
    }

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_IN) {
        stopFlags |= VIR_QEMU_PROCESS_STOP_MIGRATED;
        qemuMigrationDstErrorSave(driver, vm->def->name,
                                  qemuMonitorLastError(priv->mon));
    }

    event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                              eventReason);
    qemuProcessStop(driver, vm, stopReason, QEMU_ASYNC_JOB_NONE, stopFlags);
    virDomainAuditStop(vm, auditReason);
    virObjectEventStateQueue(driver->domainEventState, event);

 endjob:
    qemuDomainRemoveInactive(driver, vm);
    qemuDomainObjEndJob(driver, vm);
}


static void
processPRDisconnectEvent(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!virDomainObjIsActive(vm))
        return;

    if (!priv->prDaemonRunning &&
        qemuDomainDefHasManagedPR(vm))
        qemuProcessStartManagedPRDaemon(vm);
}


static void
processRdmaGidStatusChangedEvent(virDomainObjPtr vm,
                                 qemuMonitorRdmaGidStatusPtr info)
{
    unsigned int prefix_len;
    virSocketAddr addr;
    g_autofree char *addrStr = NULL;
    int rc;

    if (!virDomainObjIsActive(vm))
        return;

    VIR_DEBUG("netdev=%s, gid_status=%d, subnet_prefix=0x%llx, interface_id=0x%llx",
              info->netdev, info->gid_status, info->subnet_prefix,
              info->interface_id);

    if (info->subnet_prefix) {
        uint32_t ipv6[4] = {0};

        prefix_len = 64;
        memcpy(&ipv6[0], &info->subnet_prefix, sizeof(info->subnet_prefix));
        memcpy(&ipv6[2], &info->interface_id, sizeof(info->interface_id));
        virSocketAddrSetIPv6AddrNetOrder(&addr, ipv6);
    } else {
        prefix_len = 24;
        virSocketAddrSetIPv4AddrNetOrder(&addr, info->interface_id >> 32);
    }

    if (!(addrStr = virSocketAddrFormat(&addr)))
        return;

    if (info->gid_status) {
        VIR_DEBUG("Adding %s to %s", addrStr, info->netdev);
        rc = virNetDevIPAddrAdd(info->netdev, &addr, NULL, prefix_len);
    } else {
        VIR_DEBUG("Removing %s from %s", addrStr, info->netdev);
        rc = virNetDevIPAddrDel(info->netdev, &addr, prefix_len);
    }

    if (rc < 0)
        VIR_WARN("Fail to update address %s to %s", addrStr, info->netdev);
}


static void qemuProcessEventHandler(void *data, void *opaque)
{
    struct qemuProcessEvent *processEvent = data;
    virDomainObjPtr vm = processEvent->vm;
    virQEMUDriverPtr driver = opaque;

    VIR_DEBUG("vm=%p, event=%d", vm, processEvent->eventType);

    virObjectLock(vm);

    switch (processEvent->eventType) {
    case QEMU_PROCESS_EVENT_WATCHDOG:
        processWatchdogEvent(driver, vm, processEvent->action);
        break;
    case QEMU_PROCESS_EVENT_GUESTPANIC:
        processGuestPanicEvent(driver, vm, processEvent->action,
                               processEvent->data);
        break;
    case QEMU_PROCESS_EVENT_DEVICE_DELETED:
        processDeviceDeletedEvent(driver, vm, processEvent->data);
        break;
    case QEMU_PROCESS_EVENT_NIC_RX_FILTER_CHANGED:
        processNicRxFilterChangedEvent(driver, vm, processEvent->data);
        break;
    case QEMU_PROCESS_EVENT_SERIAL_CHANGED:
        processSerialChangedEvent(driver, vm, processEvent->data,
                                  processEvent->action);
        break;
    case QEMU_PROCESS_EVENT_BLOCK_JOB:
        processBlockJobEvent(driver, vm,
                             processEvent->data,
                             processEvent->action,
                             processEvent->status);
        break;
    case QEMU_PROCESS_EVENT_JOB_STATUS_CHANGE:
        processJobStatusChangeEvent(driver, vm, processEvent->data);
        break;
    case QEMU_PROCESS_EVENT_MONITOR_EOF:
        processMonitorEOFEvent(driver, vm);
        break;
    case QEMU_PROCESS_EVENT_PR_DISCONNECT:
        processPRDisconnectEvent(vm);
        break;
    case QEMU_PROCESS_EVENT_RDMA_GID_STATUS_CHANGED:
        processRdmaGidStatusChangedEvent(vm, processEvent->data);
        break;
    case QEMU_PROCESS_EVENT_LAST:
        break;
    }

    virDomainObjEndAPI(&vm);
    qemuProcessEventFree(processEvent);
}


static int
qemuDomainSetVcpusAgent(virDomainObjPtr vm,
                        unsigned int nvcpus)
{
    qemuAgentCPUInfoPtr cpuinfo = NULL;
    qemuAgentPtr agent;
    int ncpuinfo;
    int ret = -1;

    if (!qemuDomainAgentAvailable(vm, true))
        goto cleanup;

    if (nvcpus > virDomainDefGetVcpus(vm->def)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("requested vcpu count is greater than the count "
                         "of enabled vcpus in the domain: %d > %d"),
                       nvcpus, virDomainDefGetVcpus(vm->def));
        goto cleanup;
    }

    agent = qemuDomainObjEnterAgent(vm);
    ncpuinfo = qemuAgentGetVCPUs(agent, &cpuinfo);
    qemuDomainObjExitAgent(vm, agent);
    agent = NULL;

    if (ncpuinfo < 0)
        goto cleanup;

    if (qemuAgentUpdateCPUInfo(nvcpus, cpuinfo, ncpuinfo) < 0)
        goto cleanup;

    if (!qemuDomainAgentAvailable(vm, true))
        goto cleanup;

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentSetVCPUs(agent, cpuinfo, ncpuinfo);
    qemuDomainObjExitAgent(vm, agent);

 cleanup:
    VIR_FREE(cpuinfo);

    return ret;
}


static int
qemuDomainSetVcpusMax(virQEMUDriverPtr driver,
                      virDomainDefPtr def,
                      virDomainDefPtr persistentDef,
                      unsigned int nvcpus)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    unsigned int topologycpus;

    if (def) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("maximum vcpu count of a live domain can't be modified"));
        return -1;
    }

    if (virDomainNumaGetCPUCountTotal(persistentDef->numa) > nvcpus) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Number of CPUs in <numa> exceeds the desired "
                         "maximum vcpu count"));
        return -1;
    }

    if (virDomainDefGetVcpusTopology(persistentDef, &topologycpus) == 0 &&
        nvcpus != topologycpus) {
        /* allow setting a valid vcpu count for the topology so an invalid
         * setting may be corrected via this API */
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("CPU topology doesn't match the desired vcpu count"));
        return -1;
    }

    /* ordering information may become invalid, thus clear it */
    virDomainDefVcpuOrderClear(persistentDef);

    if (virDomainDefSetVcpusMax(persistentDef, nvcpus, driver->xmlopt) < 0)
        return -1;

    if (virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
        return -1;

    return 0;
}


static int
qemuDomainSetVcpusFlags(virDomainPtr dom,
                        unsigned int nvcpus,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    bool hotpluggable = !!(flags & VIR_DOMAIN_VCPU_HOTPLUGGABLE);
    bool useAgent = !!(flags & VIR_DOMAIN_VCPU_GUEST);
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM |
                  VIR_DOMAIN_VCPU_GUEST |
                  VIR_DOMAIN_VCPU_HOTPLUGGABLE, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSetVcpusFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;


    if (useAgent) {
        if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
            goto cleanup;
    } else {
        if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
            goto cleanup;
    }

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (useAgent)
        ret = qemuDomainSetVcpusAgent(vm, nvcpus);
    else if (flags & VIR_DOMAIN_VCPU_MAXIMUM)
        ret = qemuDomainSetVcpusMax(driver, def, persistentDef, nvcpus);
    else
        ret = qemuDomainSetVcpusInternal(driver, vm, def, persistentDef,
                                         nvcpus, hotpluggable);

 endjob:
    if (useAgent)
        qemuDomainObjEndAgentJob(vm);
    else
        qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSetVcpus(virDomainPtr dom, unsigned int nvcpus)
{
    return qemuDomainSetVcpusFlags(dom, nvcpus, VIR_DOMAIN_AFFECT_LIVE);
}


static int
qemuDomainPinVcpuLive(virDomainObjPtr vm,
                      virDomainDefPtr def,
                      int vcpu,
                      virQEMUDriverPtr driver,
                      virQEMUDriverConfigPtr cfg,
                      virBitmapPtr cpumap)
{
    virBitmapPtr tmpmap = NULL;
    virDomainVcpuDefPtr vcpuinfo;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virCgroupPtr cgroup_vcpu = NULL;
    g_autofree char *str = NULL;
    virObjectEventPtr event = NULL;
    char paramField[VIR_TYPED_PARAM_FIELD_LENGTH] = "";
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;
    int ret = -1;

    if (!qemuDomainHasVcpuPids(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cpu affinity is not supported"));
        goto cleanup;
    }

    if (!(vcpuinfo = virDomainDefGetVcpu(def, vcpu))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("vcpu %d is out of range of live cpu count %d"),
                       vcpu, virDomainDefGetVcpusMax(def));
        goto cleanup;
    }

    if (!(tmpmap = virBitmapNewCopy(cpumap)))
        goto cleanup;

    if (!(str = virBitmapFormat(cpumap)))
        goto cleanup;

    if (vcpuinfo->online) {
        /* Configure the corresponding cpuset cgroup before set affinity. */
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, vcpu,
                                   false, &cgroup_vcpu) < 0)
                goto cleanup;
            if (qemuSetupCgroupCpusetCpus(cgroup_vcpu, cpumap) < 0)
                goto cleanup;
        }

        if (virProcessSetAffinity(qemuDomainGetVcpuPid(vm, vcpu), cpumap) < 0)
            goto cleanup;
    }

    virBitmapFree(vcpuinfo->cpumask);
    vcpuinfo->cpumask = tmpmap;
    tmpmap = NULL;

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto cleanup;

    if (g_snprintf(paramField, VIR_TYPED_PARAM_FIELD_LENGTH,
                   VIR_DOMAIN_TUNABLE_CPU_VCPUPIN, vcpu) < 0) {
        goto cleanup;
    }

    if (virTypedParamsAddString(&eventParams, &eventNparams,
                                &eventMaxparams, paramField, str) < 0)
        goto cleanup;

    event = virDomainEventTunableNewFromObj(vm, eventParams, eventNparams);

    ret = 0;

 cleanup:
    virBitmapFree(tmpmap);
    virCgroupFree(&cgroup_vcpu);
    virObjectEventStateQueue(driver->domainEventState, event);
    return ret;
}


static int
qemuDomainPinVcpuFlags(virDomainPtr dom,
                       unsigned int vcpu,
                       unsigned char *cpumap,
                       int maplen,
                       unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1;
    virBitmapPtr pcpumap = NULL;
    virDomainVcpuDefPtr vcpuinfo = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virQEMUDriverGetConfig(driver);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPinVcpuFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (persistentDef &&
        !(vcpuinfo = virDomainDefGetVcpu(persistentDef, vcpu))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("vcpu %d is out of range of persistent cpu count %d"),
                       vcpu, virDomainDefGetVcpus(persistentDef));
        goto endjob;
    }

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty cpu list for pinning"));
        goto endjob;
    }

    if (def &&
        qemuDomainPinVcpuLive(vm, def, vcpu, driver, cfg, pcpumap) < 0)
        goto endjob;

    if (persistentDef) {
        virBitmapFree(vcpuinfo->cpumask);
        vcpuinfo->cpumask = pcpumap;
        pcpumap = NULL;

        ret = virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir);
        goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virBitmapFree(pcpumap);
    return ret;
}

static int
qemuDomainPinVcpu(virDomainPtr dom,
                   unsigned int vcpu,
                   unsigned char *cpumap,
                   int maplen)
{
    return qemuDomainPinVcpuFlags(dom, vcpu, cpumap, maplen,
                                  VIR_DOMAIN_AFFECT_LIVE);
}

static int
qemuDomainGetVcpuPinInfo(virDomainPtr dom,
                         int ncpumaps,
                         unsigned char *cpumaps,
                         int maplen,
                         unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    bool live;
    int ret = -1;
    virBitmapPtr autoCpuset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetVcpuPinInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (live)
        autoCpuset = QEMU_DOMAIN_PRIVATE(vm)->autoCpuset;

    ret = virDomainDefGetVcpuPinInfoHelper(def, maplen, ncpumaps, cpumaps,
                                           virHostCPUGetCount(), autoCpuset);
 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainPinEmulator(virDomainPtr dom,
                      unsigned char *cpumap,
                      int maplen,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virCgroupPtr cgroup_emulator = NULL;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    virBitmapPtr pcpumap = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virObjectEventPtr event = NULL;
    g_autofree char *str = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virQEMUDriverGetConfig(driver);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPinEmulatorEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    priv = vm->privateData;

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty cpu list for pinning"));
        goto endjob;
    }

    if (def) {
        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_EMULATOR,
                                   0, false, &cgroup_emulator) < 0)
                goto endjob;

            if (qemuSetupCgroupCpusetCpus(cgroup_emulator, pcpumap) < 0) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("failed to set cpuset.cpus in cgroup"
                                 " for emulator threads"));
                goto endjob;
            }
        }

        if (virProcessSetAffinity(vm->pid, pcpumap) < 0)
            goto endjob;

        virBitmapFree(def->cputune.emulatorpin);
        def->cputune.emulatorpin = NULL;

        if (!(def->cputune.emulatorpin = virBitmapNewCopy(pcpumap)))
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;

        str = virBitmapFormat(pcpumap);
        if (virTypedParamsAddString(&eventParams, &eventNparams,
                                    &eventMaxparams,
                                    VIR_DOMAIN_TUNABLE_CPU_EMULATORPIN,
                                    str) < 0)
            goto endjob;

        event = virDomainEventTunableNewFromDom(dom, eventParams, eventNparams);
    }

    if (persistentDef) {
        virBitmapFree(persistentDef->cputune.emulatorpin);
        persistentDef->cputune.emulatorpin = NULL;

        if (!(persistentDef->cputune.emulatorpin = virBitmapNewCopy(pcpumap)))
            goto endjob;

        ret = virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir);
        goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    if (cgroup_emulator)
        virCgroupFree(&cgroup_emulator);
    virObjectEventStateQueue(driver->domainEventState, event);
    virBitmapFree(pcpumap);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetEmulatorPinInfo(virDomainPtr dom,
                             unsigned char *cpumaps,
                             int maplen,
                             unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    bool live;
    int ret = -1;
    int hostcpus;
    virBitmapPtr cpumask = NULL;
    virBitmapPtr bitmap = NULL;
    virBitmapPtr autoCpuset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetEmulatorPinInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if ((hostcpus = virHostCPUGetCount()) < 0)
        goto cleanup;

    if (live)
        autoCpuset = QEMU_DOMAIN_PRIVATE(vm)->autoCpuset;

    if (def->cputune.emulatorpin) {
        cpumask = def->cputune.emulatorpin;
    } else if (def->cpumask) {
        cpumask = def->cpumask;
    } else if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO &&
               autoCpuset) {
        cpumask = autoCpuset;
    } else {
        if (!(bitmap = virBitmapNew(hostcpus)))
            goto cleanup;
        virBitmapSetAll(bitmap);
        cpumask = bitmap;
    }

    virBitmapToDataBuf(cpumask, cpumaps, maplen);

    ret = 1;

 cleanup:
    virDomainObjEndAPI(&vm);
    virBitmapFree(bitmap);
    return ret;
}

static int
qemuDomainGetVcpus(virDomainPtr dom,
                   virVcpuInfoPtr info,
                   int maxinfo,
                   unsigned char *cpumaps,
                   int maplen)
{
    virDomainObjPtr vm;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetVcpusEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot retrieve vcpu information for inactive domain"));
        goto cleanup;
    }

    ret = qemuDomainHelperGetVcpus(vm, info, NULL, maxinfo, cpumaps, maplen);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainGetVcpusFlags(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainDefPtr def;
    int ret = -1;
    qemuAgentCPUInfoPtr cpuinfo = NULL;
    qemuAgentPtr agent;
    int ncpuinfo = -1;
    size_t i;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_VCPU_MAXIMUM |
                  VIR_DOMAIN_VCPU_GUEST, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetVcpusFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDef(vm, flags)))
        goto cleanup;

    if (flags & VIR_DOMAIN_VCPU_GUEST) {
        if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_QUERY) < 0)
            goto cleanup;

        if (!virDomainObjIsActive(vm)) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("vCPU count provided by the guest agent can only be "
                             "requested for live domains"));
            goto endjob;
        }

        if (!qemuDomainAgentAvailable(vm, true))
            goto endjob;

        agent = qemuDomainObjEnterAgent(vm);
        ncpuinfo = qemuAgentGetVCPUs(agent, &cpuinfo);
        qemuDomainObjExitAgent(vm, agent);

 endjob:
        qemuDomainObjEndAgentJob(vm);

        if (ncpuinfo < 0)
            goto cleanup;

        if (flags & VIR_DOMAIN_VCPU_MAXIMUM) {
            ret = ncpuinfo;
            goto cleanup;
        }

        /* count the online vcpus */
        ret = 0;
        for (i = 0; i < ncpuinfo; i++) {
            if (cpuinfo[i].online)
                ret++;
        }
    } else {
        if (flags & VIR_DOMAIN_VCPU_MAXIMUM)
            ret = virDomainDefGetVcpusMax(def);
        else
            ret = virDomainDefGetVcpus(def);
    }


 cleanup:
    virDomainObjEndAPI(&vm);
    VIR_FREE(cpuinfo);
    return ret;
}

static int
qemuDomainGetMaxVcpus(virDomainPtr dom)
{
    return qemuDomainGetVcpusFlags(dom, (VIR_DOMAIN_AFFECT_LIVE |
                                         VIR_DOMAIN_VCPU_MAXIMUM));
}


static int
qemuDomainGetIOThreadsMon(virQEMUDriverPtr driver,
                          virDomainObjPtr vm,
                          qemuMonitorIOThreadInfoPtr **iothreads)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int niothreads = 0;

    qemuDomainObjEnterMonitor(driver, vm);
    niothreads = qemuMonitorGetIOThreads(priv->mon, iothreads);
    if (qemuDomainObjExitMonitor(driver, vm) < 0 || niothreads < 0)
        return -1;

    return niothreads;
}


static int
qemuDomainGetIOThreadsLive(virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           virDomainIOThreadInfoPtr **info)
{
    qemuDomainObjPrivatePtr priv;
    qemuMonitorIOThreadInfoPtr *iothreads = NULL;
    virDomainIOThreadInfoPtr *info_ret = NULL;
    int niothreads = 0;
    size_t i;
    int ret = -1;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot list IOThreads for an inactive domain"));
        goto endjob;
    }

    priv = vm->privateData;
    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_IOTHREAD)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("IOThreads not supported with this binary"));
        goto endjob;
    }

    if ((niothreads = qemuDomainGetIOThreadsMon(driver, vm, &iothreads)) < 0)
        goto endjob;

    /* Nothing to do */
    if (niothreads == 0) {
        ret = 0;
        goto endjob;
    }

    if (VIR_ALLOC_N(info_ret, niothreads) < 0)
        goto endjob;

    for (i = 0; i < niothreads; i++) {
        virBitmapPtr map = NULL;

        if (VIR_ALLOC(info_ret[i]) < 0)
            goto endjob;
        info_ret[i]->iothread_id = iothreads[i]->iothread_id;

        if (!(map = virProcessGetAffinity(iothreads[i]->thread_id)))
            goto endjob;

        if (virBitmapToData(map, &info_ret[i]->cpumap,
                            &info_ret[i]->cpumaplen) < 0) {
            virBitmapFree(map);
            goto endjob;
        }
        virBitmapFree(map);
    }

    *info = g_steal_pointer(&info_ret);
    ret = niothreads;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    if (info_ret) {
        for (i = 0; i < niothreads; i++)
            virDomainIOThreadInfoFree(info_ret[i]);
        VIR_FREE(info_ret);
    }
    if (iothreads) {
        for (i = 0; i < niothreads; i++)
            VIR_FREE(iothreads[i]);
        VIR_FREE(iothreads);
    }

    return ret;
}

static int
qemuDomainGetIOThreadsConfig(virDomainDefPtr targetDef,
                             virDomainIOThreadInfoPtr **info)
{
    virDomainIOThreadInfoPtr *info_ret = NULL;
    virBitmapPtr bitmap = NULL;
    virBitmapPtr cpumask = NULL;
    int hostcpus;
    size_t i;
    int ret = -1;

    if (targetDef->niothreadids == 0)
        return 0;

    if ((hostcpus = virHostCPUGetCount()) < 0)
        goto cleanup;

    if (VIR_ALLOC_N(info_ret, targetDef->niothreadids) < 0)
        goto cleanup;

    for (i = 0; i < targetDef->niothreadids; i++) {
        if (VIR_ALLOC(info_ret[i]) < 0)
            goto cleanup;

        /* IOThread ID's are taken from the iothreadids list */
        info_ret[i]->iothread_id = targetDef->iothreadids[i]->iothread_id;

        cpumask = targetDef->iothreadids[i]->cpumask;
        if (!cpumask) {
            if (targetDef->cpumask) {
                cpumask = targetDef->cpumask;
            } else {
                if (!(bitmap = virBitmapNew(hostcpus)))
                    goto cleanup;
                virBitmapSetAll(bitmap);
                cpumask = bitmap;
            }
        }
        if (virBitmapToData(cpumask, &info_ret[i]->cpumap,
                            &info_ret[i]->cpumaplen) < 0)
            goto cleanup;
        virBitmapFree(bitmap);
        bitmap = NULL;
    }

    *info = info_ret;
    info_ret = NULL;
    ret = targetDef->niothreadids;

 cleanup:
    if (info_ret) {
        for (i = 0; i < targetDef->niothreadids; i++)
            virDomainIOThreadInfoFree(info_ret[i]);
        VIR_FREE(info_ret);
    }
    virBitmapFree(bitmap);

    return ret;
}

static int
qemuDomainGetIOThreadInfo(virDomainPtr dom,
                          virDomainIOThreadInfoPtr **info,
                          unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainDefPtr targetDef = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetIOThreadInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, NULL, &targetDef) < 0)
        goto cleanup;

    if (!targetDef)
        ret = qemuDomainGetIOThreadsLive(driver, vm, info);
    else
        ret = qemuDomainGetIOThreadsConfig(targetDef, info);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainPinIOThread(virDomainPtr dom,
                      unsigned int iothread_id,
                      unsigned char *cpumap,
                      int maplen,
                      unsigned int flags)
{
    int ret = -1;
    virQEMUDriverPtr driver = dom->conn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virDomainObjPtr vm;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    virBitmapPtr pcpumap = NULL;
    qemuDomainObjPrivatePtr priv;
    virCgroupPtr cgroup_iothread = NULL;
    virObjectEventPtr event = NULL;
    char paramField[VIR_TYPED_PARAM_FIELD_LENGTH] = "";
    g_autofree char *str = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virQEMUDriverGetConfig(driver);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;
    priv = vm->privateData;

    if (virDomainPinIOThreadEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (!(pcpumap = virBitmapNewData(cpumap, maplen)))
        goto endjob;

    if (virBitmapIsAllClear(pcpumap)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Empty iothread cpumap list for pinning"));
        goto endjob;
    }

    if (def) {
        virDomainIOThreadIDDefPtr iothrid;
        virBitmapPtr cpumask;

        if (!(iothrid = virDomainIOThreadIDFind(def, iothread_id))) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("iothread %d not found"), iothread_id);
            goto endjob;
        }

        if (!(cpumask = virBitmapNewData(cpumap, maplen)))
            goto endjob;

        virBitmapFree(iothrid->cpumask);
        iothrid->cpumask = cpumask;
        iothrid->autofill = false;

        /* Configure the corresponding cpuset cgroup before set affinity. */
        if (virCgroupHasController(priv->cgroup,
                                   VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                                   iothread_id, false, &cgroup_iothread) < 0)
                goto endjob;
            if (qemuSetupCgroupCpusetCpus(cgroup_iothread, pcpumap) < 0) {
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("failed to set cpuset.cpus in cgroup"
                                 " for iothread %d"), iothread_id);
                goto endjob;
            }
        }

        if (virProcessSetAffinity(iothrid->thread_id, pcpumap) < 0)
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;

        if (g_snprintf(paramField, VIR_TYPED_PARAM_FIELD_LENGTH,
                       VIR_DOMAIN_TUNABLE_CPU_IOTHREADSPIN, iothread_id) < 0) {
            goto endjob;
        }

        str = virBitmapFormat(pcpumap);
        if (virTypedParamsAddString(&eventParams, &eventNparams,
                                    &eventMaxparams, paramField, str) < 0)
            goto endjob;

        event = virDomainEventTunableNewFromDom(dom, eventParams, eventNparams);
    }

    if (persistentDef) {
        virDomainIOThreadIDDefPtr iothrid;
        virBitmapPtr cpumask;

        if (!(iothrid = virDomainIOThreadIDFind(persistentDef, iothread_id))) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("iothreadid %d not found"), iothread_id);
            goto endjob;
        }

        if (!(cpumask = virBitmapNewData(cpumap, maplen)))
            goto endjob;

        virBitmapFree(iothrid->cpumask);
        iothrid->cpumask = cpumask;
        iothrid->autofill = false;

        ret = virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir);
        goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    if (cgroup_iothread)
        virCgroupFree(&cgroup_iothread);
    virObjectEventStateQueue(driver->domainEventState, event);
    virBitmapFree(pcpumap);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainHotplugAddIOThread(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             unsigned int iothread_id)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autofree char *alias = NULL;
    size_t idx;
    int ret = -1;
    unsigned int orig_niothreads = vm->def->niothreadids;
    unsigned int exp_niothreads = vm->def->niothreadids;
    int new_niothreads = 0;
    qemuMonitorIOThreadInfoPtr *new_iothreads = NULL;
    virDomainIOThreadIDDefPtr iothrid;
    virJSONValuePtr props = NULL;

    if (!(alias = g_strdup_printf("iothread%u", iothread_id)))
        return -1;

    if (qemuMonitorCreateObjectProps(&props, "iothread", alias, NULL) < 0)
        goto cleanup;

    qemuDomainObjEnterMonitor(driver, vm);

    if (qemuMonitorAddObject(priv->mon, &props, NULL) < 0)
        goto exit_monitor;

    exp_niothreads++;

    /* After hotplugging the IOThreads we need to re-detect the
     * IOThreads thread_id's, adjust the cgroups, thread affinity,
     * and add the thread_id to the vm->def->iothreadids list.
     */
    if ((new_niothreads = qemuMonitorGetIOThreads(priv->mon,
                                                  &new_iothreads)) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (new_niothreads != exp_niothreads) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("got wrong number of IOThread ids from QEMU monitor. "
                         "got %d, wanted %d"),
                       new_niothreads, exp_niothreads);
        goto cleanup;
    }

    /*
     * If we've successfully added an IOThread, find out where we added it
     * in the QEMU IOThread list, so we can add it to our iothreadids list
     */
    for (idx = 0; idx < new_niothreads; idx++) {
        if (new_iothreads[idx]->iothread_id == iothread_id)
            break;
    }

    if (idx == new_niothreads) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find new IOThread '%u' in QEMU monitor."),
                       iothread_id);
        goto cleanup;
    }

    if (!(iothrid = virDomainIOThreadIDAdd(vm->def, iothread_id)))
        goto cleanup;

    iothrid->thread_id = new_iothreads[idx]->thread_id;

    if (qemuProcessSetupIOThread(vm, iothrid) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (new_iothreads) {
        for (idx = 0; idx < new_niothreads; idx++)
            VIR_FREE(new_iothreads[idx]);
        VIR_FREE(new_iothreads);
    }
    virDomainAuditIOThread(vm, orig_niothreads, new_niothreads,
                           "update", ret == 0);
    virJSONValueFree(props);
    return ret;

 exit_monitor:
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    goto cleanup;
}


static int
qemuDomainHotplugModIOThread(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             qemuMonitorIOThreadInfo iothread)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int rc;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_IOTHREAD_POLLING)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("IOThreads polling is not supported for this QEMU"));
        return -1;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    rc = qemuMonitorSetIOThread(priv->mon, &iothread);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    if (rc < 0)
        return -1;

    return 0;
}


static int
qemuDomainHotplugDelIOThread(virQEMUDriverPtr driver,
                             virDomainObjPtr vm,
                             unsigned int iothread_id)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    size_t idx;
    g_autofree char *alias = NULL;
    int rc = -1;
    int ret = -1;
    unsigned int orig_niothreads = vm->def->niothreadids;
    unsigned int exp_niothreads = vm->def->niothreadids;
    int new_niothreads = 0;
    qemuMonitorIOThreadInfoPtr *new_iothreads = NULL;

    if (!(alias = g_strdup_printf("iothread%u", iothread_id)))
        return -1;

    qemuDomainObjEnterMonitor(driver, vm);

    rc = qemuMonitorDelObject(priv->mon, alias);
    exp_niothreads--;
    if (rc < 0)
        goto exit_monitor;

    if ((new_niothreads = qemuMonitorGetIOThreads(priv->mon,
                                                  &new_iothreads)) < 0)
        goto exit_monitor;

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto cleanup;

    if (new_niothreads != exp_niothreads) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("got wrong number of IOThread ids from QEMU monitor. "
                         "got %d, wanted %d"),
                       new_niothreads, exp_niothreads);
        goto cleanup;
    }

    virDomainIOThreadIDDel(vm->def, iothread_id);

    if (virCgroupDelThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                           iothread_id) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    if (new_iothreads) {
        for (idx = 0; idx < new_niothreads; idx++)
            VIR_FREE(new_iothreads[idx]);
        VIR_FREE(new_iothreads);
    }
    virDomainAuditIOThread(vm, orig_niothreads, new_niothreads,
                           "update", rc == 0);
    return ret;

 exit_monitor:
    ignore_value(qemuDomainObjExitMonitor(driver, vm));
    goto cleanup;
}


static int
qemuDomainAddIOThreadCheck(virDomainDefPtr def,
                           unsigned int iothread_id)
{
    if (virDomainIOThreadIDFind(def, iothread_id)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("an IOThread is already using iothread_id '%u'"),
                       iothread_id);
        return -1;
    }

    return 0;
}


static int
qemuDomainDelIOThreadCheck(virDomainDefPtr def,
                           unsigned int iothread_id)
{
    size_t i;

    if (!virDomainIOThreadIDFind(def, iothread_id)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("cannot find IOThread '%u' in iothreadids list"),
                       iothread_id);
        return -1;
    }

    for (i = 0; i < def->ndisks; i++) {
        if (def->disks[i]->iothread == iothread_id) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("cannot remove IOThread %u since it "
                             "is being used by disk '%s'"),
                           iothread_id, def->disks[i]->dst);
            return -1;
        }
    }

    for (i = 0; i < def->ncontrollers; i++) {
        if (def->controllers[i]->iothread == iothread_id) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("cannot remove IOThread '%u' since it "
                             "is being used by controller"),
                           iothread_id);
            return -1;
        }
    }

    return 0;
}


/**
 * @params: Pointer to params list
 * @nparams: Number of params to be parsed
 * @iothread: Buffer to store the values
 *
 * The following is a description of each value parsed:
 *
 *  - "poll-max-ns" for each IOThread is the maximum time in nanoseconds
 *    to allow each polling interval to occur. A polling interval is a
 *    period of time allowed for a thread to process data before it returns
 *    the CPU quantum back to the host. A value set too small will not allow
 *    the IOThread to run long enough on a CPU to process data. A value set
 *    too high will consume too much CPU time per IOThread failing to allow
 *    other threads running on the CPU to get time. A value of 0 (zero) will
 *    disable the polling.
 *
 * - "poll-grow" - factor to grow the current polling time when deemed
 *   necessary. If a 0 (zero) value is provided, QEMU currently doubles
 *   its polling interval unless the current value is greater than the
 *   poll-max-ns.
 *
 * - "poll-shrink" - divisor to reduced the current polling time when deemed
 *   necessary. If a 0 (zero) value is provided, QEMU resets the polling
 *   interval to 0 (zero) allowing the poll-grow to manipulate the time.
 *
 * QEMU keeps track of the polling time elapsed and may grow or shrink the
 * its polling interval based upon its heuristic algorithm. It is possible
 * that calculations determine that it has found a "sweet spot" and no
 * adjustments are made. The polling time value is not available.
 *
 * Returns 0 on success, -1 on failure with error set.
 */
static int
qemuDomainIOThreadParseParams(virTypedParameterPtr params,
                              int nparams,
                              qemuMonitorIOThreadInfoPtr iothread)
{
    int rc;

    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_IOTHREAD_POLL_MAX_NS,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_IOTHREAD_POLL_GROW,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_IOTHREAD_POLL_SHRINK,
                               VIR_TYPED_PARAM_UINT,
                               NULL) < 0)
        return -1;

    if ((rc = virTypedParamsGetULLong(params, nparams,
                                      VIR_DOMAIN_IOTHREAD_POLL_MAX_NS,
                                      &iothread->poll_max_ns)) < 0)
        return -1;
    if (rc == 1)
        iothread->set_poll_max_ns = true;

    if ((rc = virTypedParamsGetUInt(params, nparams,
                                    VIR_DOMAIN_IOTHREAD_POLL_GROW,
                                    &iothread->poll_grow)) < 0)
        return -1;
    if (rc == 1)
        iothread->set_poll_grow = true;

    if ((rc = virTypedParamsGetUInt(params, nparams,
                                    VIR_DOMAIN_IOTHREAD_POLL_SHRINK,
                                    &iothread->poll_shrink)) < 0)
        return -1;
    if (rc == 1)
        iothread->set_poll_shrink = true;

    if (iothread->set_poll_max_ns && iothread->poll_max_ns > INT_MAX) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("poll-max-ns (%llu) must be less than or equal to %d"),
                       iothread->poll_max_ns, INT_MAX);
        return -1;
    }

    if (iothread->set_poll_grow && iothread->poll_grow > INT_MAX) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("poll-grow (%u) must be less than or equal to %d"),
                         iothread->poll_grow, INT_MAX);
        return -1;
    }

    if (iothread->set_poll_shrink && iothread->poll_shrink > INT_MAX) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("poll-shrink (%u) must be less than or equal to %d"),
                       iothread->poll_shrink, INT_MAX);
        return -1;
    }

    return 0;
}


typedef enum {
    VIR_DOMAIN_IOTHREAD_ACTION_ADD,
    VIR_DOMAIN_IOTHREAD_ACTION_DEL,
    VIR_DOMAIN_IOTHREAD_ACTION_MOD,
} virDomainIOThreadAction;

static int
qemuDomainChgIOThread(virQEMUDriverPtr driver,
                      virDomainObjPtr vm,
                      qemuMonitorIOThreadInfo iothread,
                      virDomainIOThreadAction action,
                      unsigned int flags)
{
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuDomainObjPrivatePtr priv;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1;

    cfg = virQEMUDriverGetConfig(driver);

    priv = vm->privateData;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return -1;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_IOTHREAD)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("IOThreads not supported with this binary"));
            goto endjob;
        }

        switch (action) {
        case VIR_DOMAIN_IOTHREAD_ACTION_ADD:
            if (qemuDomainAddIOThreadCheck(def, iothread.iothread_id) < 0)
                goto endjob;

            if (qemuDomainHotplugAddIOThread(driver, vm, iothread.iothread_id) < 0)
                goto endjob;

            break;

        case VIR_DOMAIN_IOTHREAD_ACTION_DEL:
            if (qemuDomainDelIOThreadCheck(def, iothread.iothread_id) < 0)
                goto endjob;

            if (qemuDomainHotplugDelIOThread(driver, vm, iothread.iothread_id) < 0)
                goto endjob;

            break;

        case VIR_DOMAIN_IOTHREAD_ACTION_MOD:
            if (!(virDomainIOThreadIDFind(def, iothread.iothread_id))) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("cannot find IOThread '%u' in iothreadids"),
                               iothread.iothread_id);
                goto endjob;
            }

            if (qemuDomainHotplugModIOThread(driver, vm, iothread) < 0)
                goto endjob;

            break;

        }

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        switch (action) {
        case VIR_DOMAIN_IOTHREAD_ACTION_ADD:
            if (qemuDomainAddIOThreadCheck(persistentDef, iothread.iothread_id) < 0)
                goto endjob;

            if (!virDomainIOThreadIDAdd(persistentDef, iothread.iothread_id))
                goto endjob;

            break;

        case VIR_DOMAIN_IOTHREAD_ACTION_DEL:
            if (qemuDomainDelIOThreadCheck(persistentDef, iothread.iothread_id) < 0)
                goto endjob;

            virDomainIOThreadIDDel(persistentDef, iothread.iothread_id);

            break;

        case VIR_DOMAIN_IOTHREAD_ACTION_MOD:
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("configuring persistent polling values is "
                             "not supported"));
            goto endjob;

            break;
        }

        if (virDomainDefSave(persistentDef, driver->xmlopt,
                             cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

    return ret;
}

static int
qemuDomainAddIOThread(virDomainPtr dom,
                      unsigned int iothread_id,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuMonitorIOThreadInfo iothread = {0};
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (iothread_id == 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid value of 0 for iothread_id"));
        return -1;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainAddIOThreadEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    iothread.iothread_id = iothread_id;
    ret = qemuDomainChgIOThread(driver, vm, iothread,
                                VIR_DOMAIN_IOTHREAD_ACTION_ADD, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainDelIOThread(virDomainPtr dom,
                      unsigned int iothread_id,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuMonitorIOThreadInfo iothread = {0};
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (iothread_id == 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid value of 0 for iothread_id"));
        return -1;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainDelIOThreadEnsureACL(dom->conn, vm->def, flags) < 0)
           goto cleanup;

    iothread.iothread_id = iothread_id;
    ret = qemuDomainChgIOThread(driver, vm, iothread,
                                VIR_DOMAIN_IOTHREAD_ACTION_DEL, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/**
 * @dom: Domain to set IOThread params
 * @iothread_id: IOThread 'id' that will be modified
 * @params: List of parameters to change
 * @nparams: Number of parameters in the list
 * @flags: Flags for the set (only supports live alteration)
 *
 * Alter the specified @iothread_id with the values provided.
 *
 * Returs 0 on success, -1 on failure
 */
static int
qemuDomainSetIOThreadParams(virDomainPtr dom,
                            unsigned int iothread_id,
                            virTypedParameterPtr params,
                            int nparams,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuMonitorIOThreadInfo iothread = {0};
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE, -1);

    if (iothread_id == 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("invalid value of 0 for iothread_id"));
        goto cleanup;
    }

    iothread.iothread_id = iothread_id;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSetIOThreadParamsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainIOThreadParseParams(params, nparams, &iothread) < 0)
        goto cleanup;

    ret = qemuDomainChgIOThread(driver, vm, iothread,
                                VIR_DOMAIN_IOTHREAD_ACTION_MOD, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int qemuDomainGetSecurityLabel(virDomainPtr dom, virSecurityLabelPtr seclabel)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    memset(seclabel, 0, sizeof(*seclabel));

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetSecurityLabelEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    /*
     * Theoretically, the pid can be replaced during this operation and
     * return the label of a different process.  If atomicity is needed,
     * further validation will be required.
     *
     * Comment from Dan Berrange:
     *
     *   Well the PID as stored in the virDomainObjPtr can't be changed
     *   because you've got a locked object.  The OS level PID could have
     *   exited, though and in extreme circumstances have cycled through all
     *   PIDs back to ours. We could sanity check that our PID still exists
     *   after reading the label, by checking that our FD connecting to the
     *   QEMU monitor hasn't seen SIGHUP/ERR on poll().
     */
    if (virDomainObjIsActive(vm)) {
        if (qemuSecurityGetProcessLabel(driver->securityManager,
                                        vm->def, vm->pid, seclabel) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainGetSecurityLabelList(virDomainPtr dom,
                                          virSecurityLabelPtr* seclabels)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    size_t i;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetSecurityLabelListEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    /*
     * Check the comment in qemuDomainGetSecurityLabel function.
     */
    if (!virDomainObjIsActive(vm)) {
        /* No seclabels */
        *seclabels = NULL;
        ret = 0;
    } else {
        int len = 0;
        virSecurityManagerPtr* mgrs = qemuSecurityGetNested(driver->securityManager);
        if (!mgrs)
            goto cleanup;

        /* Allocate seclabels array */
        for (i = 0; mgrs[i]; i++)
            len++;

        if (VIR_ALLOC_N((*seclabels), len) < 0) {
            VIR_FREE(mgrs);
            goto cleanup;
        }
        memset(*seclabels, 0, sizeof(**seclabels) * len);

        /* Fill the array */
        for (i = 0; i < len; i++) {
            if (qemuSecurityGetProcessLabel(mgrs[i], vm->def, vm->pid,
                                            &(*seclabels)[i]) < 0) {
                VIR_FREE(mgrs);
                VIR_FREE(*seclabels);
                goto cleanup;
            }
        }
        ret = len;
        VIR_FREE(mgrs);
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int qemuNodeGetSecurityModel(virConnectPtr conn,
                                    virSecurityModelPtr secmodel)
{
    virQEMUDriverPtr driver = conn->privateData;
    char *p;
    g_autoptr(virCaps) caps = NULL;

    memset(secmodel, 0, sizeof(*secmodel));

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        return 0;

    if (virNodeGetSecurityModelEnsureACL(conn) < 0)
        return 0;

    /* We treat no driver as success, but simply return no data in *secmodel */
    if (caps->host.nsecModels == 0 ||
        caps->host.secModels[0].model == NULL)
        return 0;

    p = caps->host.secModels[0].model;
    if (strlen(p) >= VIR_SECURITY_MODEL_BUFLEN-1) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("security model string exceeds max %d bytes"),
                       VIR_SECURITY_MODEL_BUFLEN-1);
        return -1;
    }
    strcpy(secmodel->model, p);

    p = caps->host.secModels[0].doi;
    if (strlen(p) >= VIR_SECURITY_DOI_BUFLEN-1) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("security DOI string exceeds max %d bytes"),
                       VIR_SECURITY_DOI_BUFLEN-1);
        return -1;
    }
    strcpy(secmodel->doi, p);

    return 0;
}


/**
 * qemuDomainSaveImageUpdateDef:
 * @driver: qemu driver data
 * @def: def of the domain from the save image
 * @newxml: user provided replacement XML
 *
 * Returns the new domain definition in case @newxml is ABI compatible with the
 * guest.
 */
static virDomainDefPtr
qemuDomainSaveImageUpdateDef(virQEMUDriverPtr driver,
                             virDomainDefPtr def,
                             const char *newxml)
{
    virDomainDefPtr ret = NULL;
    virDomainDefPtr newdef_migr = NULL;
    virDomainDefPtr newdef = NULL;

    if (!(newdef = virDomainDefParseString(newxml, driver->xmlopt, NULL,
                                           VIR_DOMAIN_DEF_PARSE_INACTIVE)))
        goto cleanup;

    if (!(newdef_migr = qemuDomainDefCopy(driver, NULL,
                                          newdef,
                                          QEMU_DOMAIN_FORMAT_LIVE_FLAGS |
                                          VIR_DOMAIN_XML_MIGRATABLE)))
        goto cleanup;

    if (!virDomainDefCheckABIStability(def, newdef_migr, driver->xmlopt)) {
        virErrorPtr save_err;

        virErrorPreserveLast(&save_err);

        /* Due to a bug in older version of external snapshot creation
         * code, the XML saved in the save image was not a migratable
         * XML. To ensure backwards compatibility with the change of the
         * saved XML type, we need to check the ABI compatibility against
         * the user provided XML if the check against the migratable XML
         * fails. Snapshots created prior to v1.1.3 have this issue. */
        if (!virDomainDefCheckABIStability(def, newdef, driver->xmlopt)) {
            virErrorRestore(&save_err);
            goto cleanup;
        }
        virFreeError(save_err);

        /* use the user provided XML */
        ret = g_steal_pointer(&newdef);
    } else {
        ret = g_steal_pointer(&newdef_migr);
    }

 cleanup:
    virDomainDefFree(newdef);
    virDomainDefFree(newdef_migr);

    return ret;
}


/**
 * qemuDomainSaveImageOpen:
 * @driver: qemu driver data
 * @qemuCaps: pointer to qemuCaps if the domain is running or NULL
 * @path: path of the save image
 * @ret_def: returns domain definition created from the XML stored in the image
 * @ret_data: returns structure filled with data from the image header
 * @bypass_cache: bypass cache when opening the file
 * @wrapperFd: returns the file wrapper structure
 * @open_write: open the file for writing (for updates)
 * @unlink_corrupt: remove the image file if it is corrupted
 *
 * Returns the opened fd of the save image file and fills the appropriate fields
 * on success. On error returns -1 on most failures, -3 if corrupt image was
 * unlinked (no error raised).
 */
static int ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4)
qemuDomainSaveImageOpen(virQEMUDriverPtr driver,
                        virQEMUCapsPtr qemuCaps,
                        const char *path,
                        virDomainDefPtr *ret_def,
                        virQEMUSaveDataPtr *ret_data,
                        bool bypass_cache,
                        virFileWrapperFdPtr *wrapperFd,
                        bool open_write,
                        bool unlink_corrupt)
{
    int fd = -1;
    virQEMUSaveDataPtr data = NULL;
    virQEMUSaveHeaderPtr header;
    virDomainDefPtr def = NULL;
    int oflags = open_write ? O_RDWR : O_RDONLY;
    size_t xml_len;
    size_t cookie_len;

    if (bypass_cache) {
        int directFlag = virFileDirectFdFlag();
        if (directFlag < 0) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("bypass cache unsupported by this system"));
            goto error;
        }
        oflags |= directFlag;
    }

    if ((fd = qemuOpenFile(driver, NULL, path, oflags, NULL)) < 0)
        goto error;
    if (bypass_cache &&
        !(*wrapperFd = virFileWrapperFdNew(&fd, path,
                                           VIR_FILE_WRAPPER_BYPASS_CACHE)))
        goto error;

    if (VIR_ALLOC(data) < 0)
        goto error;

    header = &data->header;
    if (saferead(fd, header, sizeof(*header)) != sizeof(*header)) {
        if (unlink_corrupt) {
            if (VIR_CLOSE(fd) < 0 || unlink(path) < 0) {
                virReportSystemError(errno,
                                     _("cannot remove corrupt file: %s"),
                                     path);
            } else {
                fd = -3;
            }
        } else {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           "%s", _("failed to read qemu header"));
        }
        goto error;
    }

    if (memcmp(header->magic, QEMU_SAVE_MAGIC, sizeof(header->magic)) != 0) {
        const char *msg = _("image magic is incorrect");

        if (memcmp(header->magic, QEMU_SAVE_PARTIAL,
                   sizeof(header->magic)) == 0) {
            msg = _("save image is incomplete");
            if (unlink_corrupt) {
                if (VIR_CLOSE(fd) < 0 || unlink(path) < 0) {
                    virReportSystemError(errno,
                                         _("cannot remove corrupt file: %s"),
                                         path);
                } else {
                    fd = -3;
                }
                goto error;
            }
        }
        virReportError(VIR_ERR_OPERATION_FAILED, "%s", msg);
        goto error;
    }

    if (header->version > QEMU_SAVE_VERSION) {
        /* convert endianess and try again */
        bswap_header(header);
    }

    if (header->version > QEMU_SAVE_VERSION) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("image version is not supported (%d > %d)"),
                       header->version, QEMU_SAVE_VERSION);
        goto error;
    }

    if (header->data_len <= 0) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("invalid header data length: %d"), header->data_len);
        goto error;
    }

    if (header->cookieOffset)
        xml_len = header->cookieOffset;
    else
        xml_len = header->data_len;

    cookie_len = header->data_len - xml_len;

    if (VIR_ALLOC_N(data->xml, xml_len) < 0)
        goto error;

    if (saferead(fd, data->xml, xml_len) != xml_len) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       "%s", _("failed to read domain XML"));
        goto error;
    }

    if (cookie_len > 0) {
        if (VIR_ALLOC_N(data->cookie, cookie_len) < 0)
            goto error;

        if (saferead(fd, data->cookie, cookie_len) != cookie_len) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("failed to read cookie"));
            goto error;
        }
    }

    /* Create a domain from this XML */
    if (!(def = virDomainDefParseString(data->xml, driver->xmlopt, qemuCaps,
                                        VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                        VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE)))
        goto error;

    *ret_def = def;
    *ret_data = data;

    return fd;

 error:
    virDomainDefFree(def);
    virQEMUSaveDataFree(data);
    VIR_FORCE_CLOSE(fd);
    return -1;
}

static int ATTRIBUTE_NONNULL(4) ATTRIBUTE_NONNULL(5) ATTRIBUTE_NONNULL(6)
qemuDomainSaveImageStartVM(virConnectPtr conn,
                           virQEMUDriverPtr driver,
                           virDomainObjPtr vm,
                           int *fd,
                           virQEMUSaveDataPtr data,
                           const char *path,
                           bool start_paused,
                           qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;
    bool restored = false;
    virObjectEventPtr event;
    int intermediatefd = -1;
    virCommandPtr cmd = NULL;
    g_autofree char *errbuf = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    virQEMUSaveHeaderPtr header = &data->header;
    qemuDomainSaveCookiePtr cookie = NULL;

    if (virSaveCookieParseString(data->cookie, (virObjectPtr *)&cookie,
                                 virDomainXMLOptionGetSaveCookie(driver->xmlopt)) < 0)
        goto cleanup;

    if ((header->version == 2) &&
        (header->compressed != QEMU_SAVE_FORMAT_RAW)) {
        if (!(cmd = qemuCompressGetCommand(header->compressed)))
            goto cleanup;

        intermediatefd = *fd;
        *fd = -1;

        virCommandSetInputFD(cmd, intermediatefd);
        virCommandSetOutputFD(cmd, fd);
        virCommandSetErrorBuffer(cmd, &errbuf);
        virCommandDoAsyncIO(cmd);

        if (virCommandRunAsync(cmd, NULL) < 0) {
            *fd = intermediatefd;
            goto cleanup;
        }
    }

    /* No cookie means libvirt which saved the domain was too old to mess up
     * the CPU definitions.
     */
    if (cookie &&
        qemuDomainFixupCPUs(vm, &cookie->cpu) < 0)
        goto cleanup;

    if (cookie && !cookie->slirpHelper)
        priv->disableSlirp = true;

    if (qemuProcessStart(conn, driver, vm, cookie ? cookie->cpu : NULL,
                         asyncJob, "stdio", *fd, path, NULL,
                         VIR_NETDEV_VPORT_PROFILE_OP_RESTORE,
                         VIR_QEMU_PROCESS_START_PAUSED |
                         VIR_QEMU_PROCESS_START_GEN_VMID) == 0)
        restored = true;

    if (intermediatefd != -1) {
        virErrorPtr orig_err = NULL;

        if (!restored) {
            /* if there was an error setting up qemu, the intermediate
             * process will wait forever to write to stdout, so we
             * must manually kill it and ignore any error related to
             * the process
             */
            virErrorPreserveLast(&orig_err);
            VIR_FORCE_CLOSE(intermediatefd);
            VIR_FORCE_CLOSE(*fd);
        }

        if (virCommandWait(cmd, NULL) < 0) {
            qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED, asyncJob, 0);
            restored = false;
        }
        VIR_DEBUG("Decompression binary stderr: %s", NULLSTR(errbuf));

        virErrorRestore(&orig_err);
    }
    VIR_FORCE_CLOSE(intermediatefd);

    if (VIR_CLOSE(*fd) < 0) {
        virReportSystemError(errno, _("cannot close file: %s"), path);
        restored = false;
    }

    virDomainAuditStart(vm, "restored", restored);
    if (!restored)
        goto cleanup;

    /* qemuProcessStart doesn't unset the qemu error reporting infrastructure
     * in case of migration (which is used in this case) so we need to reset it
     * so that the handle to virtlogd is not held open unnecessarily */
    qemuMonitorSetDomainLog(qemuDomainGetMonitor(vm), NULL, NULL, NULL);

    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STARTED,
                                     VIR_DOMAIN_EVENT_STARTED_RESTORED);
    virObjectEventStateQueue(driver->domainEventState, event);


    /* If it was running before, resume it now unless caller requested pause. */
    if (header->was_running && !start_paused) {
        if (qemuProcessStartCPUs(driver, vm,
                                 VIR_DOMAIN_RUNNING_RESTORED,
                                 asyncJob) < 0) {
            if (virGetLastErrorCode() == VIR_ERR_OK)
                virReportError(VIR_ERR_OPERATION_FAILED,
                               "%s", _("failed to resume domain"));
            goto cleanup;
        }
        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
            VIR_WARN("Failed to save status on vm %s", vm->def->name);
            goto cleanup;
        }
    } else {
        int detail = (start_paused ? VIR_DOMAIN_EVENT_SUSPENDED_PAUSED :
                      VIR_DOMAIN_EVENT_SUSPENDED_RESTORED);
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         detail);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    ret = 0;

 cleanup:
    virObjectUnref(cookie);
    virCommandFree(cmd);
    if (qemuSecurityRestoreSavedStateLabel(driver, vm, path) < 0)
        VIR_WARN("failed to restore save state label on %s", path);
    return ret;
}

static int
qemuDomainRestoreFlags(virConnectPtr conn,
                       const char *path,
                       const char *dxml,
                       unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    qemuDomainObjPrivatePtr priv = NULL;
    virDomainDefPtr def = NULL;
    virDomainObjPtr vm = NULL;
    g_autofree char *xmlout = NULL;
    const char *newxml = dxml;
    int fd = -1;
    int ret = -1;
    virQEMUSaveDataPtr data = NULL;
    virFileWrapperFdPtr wrapperFd = NULL;
    bool hook_taint = false;

    virCheckFlags(VIR_DOMAIN_SAVE_BYPASS_CACHE |
                  VIR_DOMAIN_SAVE_RUNNING |
                  VIR_DOMAIN_SAVE_PAUSED, -1);


    virNWFilterReadLockFilterUpdates();

    fd = qemuDomainSaveImageOpen(driver, NULL, path, &def, &data,
                                 (flags & VIR_DOMAIN_SAVE_BYPASS_CACHE) != 0,
                                 &wrapperFd, false, false);
    if (fd < 0)
        goto cleanup;

    if (virDomainRestoreFlagsEnsureACL(conn, def) < 0)
        goto cleanup;

    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        int hookret;

        if ((hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, def->name,
                                   VIR_HOOK_QEMU_OP_RESTORE,
                                   VIR_HOOK_SUBOP_BEGIN,
                                   NULL,
                                   dxml ? dxml : data->xml,
                                   &xmlout)) < 0)
            goto cleanup;

        if (hookret == 0 && !virStringIsEmpty(xmlout)) {
            VIR_DEBUG("Using hook-filtered domain XML: %s", xmlout);
            hook_taint = true;
            newxml = xmlout;
        }
    }

    if (newxml) {
        virDomainDefPtr tmp;
        if (!(tmp = qemuDomainSaveImageUpdateDef(driver, def, newxml)))
            goto cleanup;

        virDomainDefFree(def);
        def = tmp;
    }

    if (!(vm = virDomainObjListAdd(driver->domains, def,
                                   driver->xmlopt,
                                   VIR_DOMAIN_OBJ_LIST_ADD_LIVE |
                                   VIR_DOMAIN_OBJ_LIST_ADD_CHECK_LIVE,
                                   NULL)))
        goto cleanup;
    def = NULL;

    if (flags & VIR_DOMAIN_SAVE_RUNNING)
        data->header.was_running = 1;
    else if (flags & VIR_DOMAIN_SAVE_PAUSED)
        data->header.was_running = 0;

    if (hook_taint) {
        priv = vm->privateData;
        priv->hookRun = true;
    }

    if (qemuProcessBeginJob(driver, vm, VIR_DOMAIN_JOB_OPERATION_RESTORE,
                            flags) < 0)
        goto cleanup;

    ret = qemuDomainSaveImageStartVM(conn, driver, vm, &fd, data, path,
                                     false, QEMU_ASYNC_JOB_START);

    qemuProcessEndJob(driver, vm);

 cleanup:
    virDomainDefFree(def);
    VIR_FORCE_CLOSE(fd);
    if (virFileWrapperFdClose(wrapperFd) < 0)
        ret = -1;
    virFileWrapperFdFree(wrapperFd);
    virQEMUSaveDataFree(data);
    if (vm && ret < 0)
        qemuDomainRemoveInactiveJob(driver, vm);
    virDomainObjEndAPI(&vm);
    virNWFilterUnlockFilterUpdates();
    return ret;
}

static int
qemuDomainRestore(virConnectPtr conn,
                  const char *path)
{
    return qemuDomainRestoreFlags(conn, path, NULL, 0);
}

static char *
qemuDomainSaveImageGetXMLDesc(virConnectPtr conn, const char *path,
                              unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    char *ret = NULL;
    virDomainDefPtr def = NULL;
    int fd = -1;
    virQEMUSaveDataPtr data = NULL;

    virCheckFlags(VIR_DOMAIN_SAVE_IMAGE_XML_SECURE, NULL);

    fd = qemuDomainSaveImageOpen(driver, NULL, path, &def, &data,
                                 false, NULL, false, false);

    if (fd < 0)
        goto cleanup;

    if (virDomainSaveImageGetXMLDescEnsureACL(conn, def) < 0)
        goto cleanup;

    ret = qemuDomainDefFormatXML(driver, NULL, def, flags);

 cleanup:
    virQEMUSaveDataFree(data);
    virDomainDefFree(def);
    VIR_FORCE_CLOSE(fd);
    return ret;
}

static int
qemuDomainSaveImageDefineXML(virConnectPtr conn, const char *path,
                             const char *dxml, unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    int ret = -1;
    virDomainDefPtr def = NULL;
    virDomainDefPtr newdef = NULL;
    int fd = -1;
    virQEMUSaveDataPtr data = NULL;
    int state = -1;

    virCheckFlags(VIR_DOMAIN_SAVE_RUNNING |
                  VIR_DOMAIN_SAVE_PAUSED, -1);

    if (flags & VIR_DOMAIN_SAVE_RUNNING)
        state = 1;
    else if (flags & VIR_DOMAIN_SAVE_PAUSED)
        state = 0;

    fd = qemuDomainSaveImageOpen(driver, NULL, path, &def, &data,
                                 false, NULL, true, false);

    if (fd < 0)
        goto cleanup;

    if (virDomainSaveImageDefineXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    if (STREQ(data->xml, dxml) &&
        (state < 0 || state == data->header.was_running)) {
        /* no change to the XML */
        ret = 0;
        goto cleanup;
    }

    if (state >= 0)
        data->header.was_running = state;

    if (!(newdef = qemuDomainSaveImageUpdateDef(driver, def, dxml)))
        goto cleanup;

    VIR_FREE(data->xml);

    if (!(data->xml = qemuDomainDefFormatXML(driver, NULL, newdef,
                                             VIR_DOMAIN_XML_INACTIVE |
                                             VIR_DOMAIN_XML_SECURE |
                                             VIR_DOMAIN_XML_MIGRATABLE)))
        goto cleanup;

    if (lseek(fd, 0, SEEK_SET) != 0) {
        virReportSystemError(errno, _("cannot seek in '%s'"), path);
        goto cleanup;
    }

    if (virQEMUSaveDataWrite(data, fd, path) < 0)
        goto cleanup;

    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno, _("failed to write header data to '%s'"), path);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    virDomainDefFree(def);
    virDomainDefFree(newdef);
    VIR_FORCE_CLOSE(fd);
    virQEMUSaveDataFree(data);
    return ret;
}

static char *
qemuDomainManagedSaveGetXMLDesc(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    g_autofree char *path = NULL;
    char *ret = NULL;
    virDomainDefPtr def = NULL;
    int fd = -1;
    virQEMUSaveDataPtr data = NULL;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_SAVE_IMAGE_XML_SECURE, NULL);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return ret;

    priv = vm->privateData;

    if (virDomainManagedSaveGetXMLDescEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!(path = qemuDomainManagedSavePath(driver, vm)))
        goto cleanup;

    if (!virFileExists(path)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain does not have managed save image"));
        goto cleanup;
    }

    if ((fd = qemuDomainSaveImageOpen(driver, priv->qemuCaps, path, &def, &data,
                                      false, NULL, false, false)) < 0)
        goto cleanup;

    ret = qemuDomainDefFormatXML(driver, priv->qemuCaps, def, flags);

 cleanup:
    virQEMUSaveDataFree(data);
    virDomainDefFree(def);
    VIR_FORCE_CLOSE(fd);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainManagedSaveDefineXML(virDomainPtr dom, const char *dxml,
                               unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virConnectPtr conn = dom->conn;
    virDomainObjPtr vm;
    g_autofree char *path = NULL;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainManagedSaveDefineXMLEnsureACL(conn, vm->def) < 0)
        goto cleanup;

    if (!(path = qemuDomainManagedSavePath(driver, vm)))
        goto cleanup;

    if (!virFileExists(path)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain does not have managed save image"));
        goto cleanup;
    }

    ret = qemuDomainSaveImageDefineXML(conn, path, dxml, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/* Return 0 on success, 1 if incomplete saved image was silently unlinked,
 * and -1 on failure with error raised.  */
static int
qemuDomainObjRestore(virConnectPtr conn,
                     virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     const char *path,
                     bool start_paused,
                     bool bypass_cache,
                     qemuDomainAsyncJob asyncJob)
{
    virDomainDefPtr def = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int fd = -1;
    int ret = -1;
    g_autofree char *xmlout = NULL;
    virQEMUSaveDataPtr data = NULL;
    virFileWrapperFdPtr wrapperFd = NULL;

    fd = qemuDomainSaveImageOpen(driver, NULL, path, &def, &data,
                                 bypass_cache, &wrapperFd, false, true);
    if (fd < 0) {
        if (fd == -3)
            ret = 1;
        goto cleanup;
    }

    if (virHookPresent(VIR_HOOK_DRIVER_QEMU)) {
        int hookret;

        if ((hookret = virHookCall(VIR_HOOK_DRIVER_QEMU, def->name,
                                   VIR_HOOK_QEMU_OP_RESTORE,
                                   VIR_HOOK_SUBOP_BEGIN,
                                   NULL, data->xml, &xmlout)) < 0)
            goto cleanup;

        if (hookret == 0 && !virStringIsEmpty(xmlout)) {
            virDomainDefPtr tmp;

            VIR_DEBUG("Using hook-filtered domain XML: %s", xmlout);

            if (!(tmp = qemuDomainSaveImageUpdateDef(driver, def, xmlout)))
                goto cleanup;

            virDomainDefFree(def);
            def = tmp;
            priv->hookRun = true;
        }
    }

    if (STRNEQ(vm->def->name, def->name) ||
        memcmp(vm->def->uuid, def->uuid, VIR_UUID_BUFLEN)) {
        char vm_uuidstr[VIR_UUID_STRING_BUFLEN];
        char def_uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(vm->def->uuid, vm_uuidstr);
        virUUIDFormat(def->uuid, def_uuidstr);
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("cannot restore domain '%s' uuid %s from a file"
                         " which belongs to domain '%s' uuid %s"),
                       vm->def->name, vm_uuidstr,
                       def->name, def_uuidstr);
        goto cleanup;
    }

    virDomainObjAssignDef(vm, def, true, NULL);
    def = NULL;

    ret = qemuDomainSaveImageStartVM(conn, driver, vm, &fd, data, path,
                                     start_paused, asyncJob);

 cleanup:
    virQEMUSaveDataFree(data);
    virDomainDefFree(def);
    VIR_FORCE_CLOSE(fd);
    if (virFileWrapperFdClose(wrapperFd) < 0)
        ret = -1;
    virFileWrapperFdFree(wrapperFd);
    return ret;
}


static char
*qemuDomainGetXMLDesc(virDomainPtr dom,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    virCheckFlags(VIR_DOMAIN_XML_COMMON_FLAGS | VIR_DOMAIN_XML_UPDATE_CPU,
                  NULL);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetXMLDescEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    qemuDomainUpdateCurrentMemorySize(vm);

    if ((flags & VIR_DOMAIN_XML_MIGRATABLE))
        flags |= QEMU_DOMAIN_FORMAT_LIVE_FLAGS;

    /* The CPU is already updated in the domain's live definition, we need to
     * ignore the VIR_DOMAIN_XML_UPDATE_CPU flag.
     */
    if (virDomainObjIsActive(vm) &&
        !(flags & VIR_DOMAIN_XML_INACTIVE))
        flags &= ~VIR_DOMAIN_XML_UPDATE_CPU;

    ret = qemuDomainFormatXML(driver, vm, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static char *qemuConnectDomainXMLToNative(virConnectPtr conn,
                                          const char *format,
                                          const char *xmlData,
                                          unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    virDomainObjPtr vm = NULL;
    virCommandPtr cmd = NULL;
    char *ret = NULL;
    size_t i;

    virCheckFlags(0, NULL);

    if (virConnectDomainXMLToNativeEnsureACL(conn) < 0)
        goto cleanup;

    if (STRNEQ(format, QEMU_CONFIG_FORMAT_ARGV)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unsupported config type %s"), format);
        goto cleanup;
    }

    if (!(vm = virDomainObjNew(driver->xmlopt)))
        goto cleanup;

    if (!(vm->def = virDomainDefParseString(xmlData, driver->xmlopt, NULL,
                                            VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                            VIR_DOMAIN_DEF_PARSE_ABI_UPDATE)))
        goto cleanup;

    /* Since we're just exporting args, we can't do bridge/network/direct
     * setups, since libvirt will normally create TAP/macvtap devices
     * directly. We convert those configs into generic 'ethernet'
     * config and assume the user has suitable 'ifup-qemu' scripts
     */
    for (i = 0; i < vm->def->nnets; i++) {
        virDomainNetDefPtr net = vm->def->nets[i];
        unsigned int bootIndex = net->info.bootIndex;
        g_autofree char *model = NULL;
        virMacAddr mac = net->mac;
        char *script = net->script;

        model = g_strdup(virDomainNetGetModelString(net));

        net->script = NULL;

        virDomainNetDefClear(net);

        net->type = VIR_DOMAIN_NET_TYPE_ETHERNET;
        net->info.bootIndex = bootIndex;
        net->mac = mac;
        net->script = script;

        if (virDomainNetSetModelString(net, model) < 0)
            goto cleanup;
    }

    if (!(cmd = qemuProcessCreatePretendCmd(driver, vm, NULL,
                                            qemuCheckFips(), true,
                                            VIR_QEMU_PROCESS_START_COLD)))
        goto cleanup;

    ret = virCommandToString(cmd, false);

 cleanup:
    virCommandFree(cmd);
    virObjectUnref(vm);
    return ret;
}


static int qemuConnectListDefinedDomains(virConnectPtr conn,
                                         char **const names, int nnames) {
    virQEMUDriverPtr driver = conn->privateData;
    int ret = -1;

    if (virConnectListDefinedDomainsEnsureACL(conn) < 0)
        return -1;

    ret = virDomainObjListGetInactiveNames(driver->domains, names, nnames,
                                           virConnectListDefinedDomainsCheckACL,
                                           conn);

    return ret;
}

static int qemuConnectNumOfDefinedDomains(virConnectPtr conn)
{
    virQEMUDriverPtr driver = conn->privateData;
    int ret = -1;

    if (virConnectNumOfDefinedDomainsEnsureACL(conn) < 0)
        return -1;

    ret = virDomainObjListNumOfDomains(driver->domains, false,
                                       virConnectNumOfDefinedDomainsCheckACL,
                                       conn);

    return ret;
}


static int
qemuDomainObjStart(virConnectPtr conn,
                   virQEMUDriverPtr driver,
                   virDomainObjPtr vm,
                   unsigned int flags,
                   qemuDomainAsyncJob asyncJob)
{
    int ret = -1;
    g_autofree char *managed_save = NULL;
    bool start_paused = (flags & VIR_DOMAIN_START_PAUSED) != 0;
    bool autodestroy = (flags & VIR_DOMAIN_START_AUTODESTROY) != 0;
    bool bypass_cache = (flags & VIR_DOMAIN_START_BYPASS_CACHE) != 0;
    bool force_boot = (flags & VIR_DOMAIN_START_FORCE_BOOT) != 0;
    unsigned int start_flags = VIR_QEMU_PROCESS_START_COLD;
    qemuDomainObjPrivatePtr priv = vm->privateData;

    start_flags |= start_paused ? VIR_QEMU_PROCESS_START_PAUSED : 0;
    start_flags |= autodestroy ? VIR_QEMU_PROCESS_START_AUTODESTROY : 0;

    /*
     * If there is a managed saved state restore it instead of starting
     * from scratch. The old state is removed once the restoring succeeded.
     */
    managed_save = qemuDomainManagedSavePath(driver, vm);

    if (!managed_save)
        return ret;

    if (virFileExists(managed_save)) {
        if (force_boot) {
            if (unlink(managed_save) < 0) {
                virReportSystemError(errno,
                                     _("cannot remove managed save file %s"),
                                     managed_save);
                return ret;
            }
            vm->hasManagedSave = false;
        } else {
            virDomainJobOperation op = priv->job.current->operation;
            priv->job.current->operation = VIR_DOMAIN_JOB_OPERATION_RESTORE;

            ret = qemuDomainObjRestore(conn, driver, vm, managed_save,
                                       start_paused, bypass_cache, asyncJob);

            if (ret == 0) {
                if (unlink(managed_save) < 0)
                    VIR_WARN("Failed to remove the managed state %s", managed_save);
                else
                    vm->hasManagedSave = false;

                return ret;
            } else if (ret < 0) {
                VIR_WARN("Unable to restore from managed state %s. "
                         "Maybe the file is corrupted?", managed_save);
                return ret;
            } else {
                VIR_WARN("Ignoring incomplete managed state %s", managed_save);
                priv->job.current->operation = op;
                vm->hasManagedSave = false;
            }
        }
    }

    ret = qemuProcessStart(conn, driver, vm, NULL, asyncJob,
                           NULL, -1, NULL, NULL,
                           VIR_NETDEV_VPORT_PROFILE_OP_CREATE, start_flags);
    virDomainAuditStart(vm, "booted", ret >= 0);
    if (ret >= 0) {
        virObjectEventPtr event =
            virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STARTED,
                                     VIR_DOMAIN_EVENT_STARTED_BOOTED);
        virObjectEventStateQueue(driver->domainEventState, event);
        if (start_paused) {
            event = virDomainEventLifecycleNewFromObj(vm,
                                             VIR_DOMAIN_EVENT_SUSPENDED,
                                             VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);
            virObjectEventStateQueue(driver->domainEventState, event);
        }
    }

    return ret;
}

static int
qemuDomainCreateWithFlags(virDomainPtr dom, unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_START_PAUSED |
                  VIR_DOMAIN_START_AUTODESTROY |
                  VIR_DOMAIN_START_BYPASS_CACHE |
                  VIR_DOMAIN_START_FORCE_BOOT, -1);

    virNWFilterReadLockFilterUpdates();

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainCreateWithFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuProcessBeginJob(driver, vm, VIR_DOMAIN_JOB_OPERATION_START,
                            flags) < 0)
        goto cleanup;

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain is already running"));
        goto endjob;
    }

    if (qemuDomainObjStart(dom->conn, driver, vm, flags,
                           QEMU_ASYNC_JOB_START) < 0)
        goto endjob;

    dom->id = vm->def->id;
    ret = 0;

 endjob:
    qemuProcessEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virNWFilterUnlockFilterUpdates();
    return ret;
}

static int
qemuDomainCreate(virDomainPtr dom)
{
    return qemuDomainCreateWithFlags(dom, 0);
}

static virDomainPtr
qemuDomainDefineXMLFlags(virConnectPtr conn,
                         const char *xml,
                         unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainDefPtr oldDef = NULL;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;
    virObjectEventPtr event = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE |
                               VIR_DOMAIN_DEF_PARSE_ABI_UPDATE;

    virCheckFlags(VIR_DOMAIN_DEFINE_VALIDATE, NULL);

    if (flags & VIR_DOMAIN_DEFINE_VALIDATE)
        parse_flags |= VIR_DOMAIN_DEF_PARSE_VALIDATE_SCHEMA;

    if (!(def = virDomainDefParseString(xml, driver->xmlopt,
                                        NULL, parse_flags)))
        goto cleanup;

    if (virXMLCheckIllegalChars("name", def->name, "\n") < 0)
        goto cleanup;

    if (virDomainDefineXMLFlagsEnsureACL(conn, def) < 0)
        goto cleanup;

    if (!(vm = virDomainObjListAdd(driver->domains, def,
                                   driver->xmlopt,
                                   0, &oldDef)))
        goto cleanup;
    def = NULL;

    vm->persistent = 1;

    if (virDomainDefSave(vm->newDef ? vm->newDef : vm->def,
                         driver->xmlopt, cfg->configDir) < 0) {
        if (oldDef) {
            /* There is backup so this VM was defined before.
             * Just restore the backup. */
            VIR_INFO("Restoring domain '%s' definition", vm->def->name);
            if (virDomainObjIsActive(vm))
                vm->newDef = oldDef;
            else
                vm->def = oldDef;
            oldDef = NULL;
        } else {
            /* Brand new domain. Remove it */
            VIR_INFO("Deleting domain '%s'", vm->def->name);
            vm->persistent = 0;
            qemuDomainRemoveInactiveJob(driver, vm);
        }
        goto cleanup;
    }

    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     !oldDef ?
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED :
                                     VIR_DOMAIN_EVENT_DEFINED_UPDATED);

    VIR_INFO("Creating domain '%s'", vm->def->name);
    dom = virGetDomain(conn, vm->def->name, vm->def->uuid, vm->def->id);

 cleanup:
    virDomainDefFree(oldDef);
    virDomainDefFree(def);
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);
    return dom;
}

static virDomainPtr
qemuDomainDefineXML(virConnectPtr conn, const char *xml)
{
    return qemuDomainDefineXMLFlags(conn, xml, 0);
}

static int
qemuDomainUndefineFlags(virDomainPtr dom,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virObjectEventPtr event = NULL;
    g_autofree char *name = NULL;
    int ret = -1;
    int nsnapshots;
    int ncheckpoints;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    g_autofree char *nvram_path = NULL;

    virCheckFlags(VIR_DOMAIN_UNDEFINE_MANAGED_SAVE |
                  VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA |
                  VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA |
                  VIR_DOMAIN_UNDEFINE_NVRAM |
                  VIR_DOMAIN_UNDEFINE_KEEP_NVRAM, -1);

    if ((flags & VIR_DOMAIN_UNDEFINE_NVRAM) &&
        (flags & VIR_DOMAIN_UNDEFINE_KEEP_NVRAM)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot both keep and delete nvram"));
        return -1;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainUndefineFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot undefine transient domain"));
        goto endjob;
    }

    if (!virDomainObjIsActive(vm) &&
        (nsnapshots = virDomainSnapshotObjListNum(vm->snapshots, NULL, 0))) {
        if (!(flags & VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA)) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("cannot delete inactive domain with %d "
                             "snapshots"),
                           nsnapshots);
            goto endjob;
        }
        if (qemuDomainSnapshotDiscardAllMetadata(driver, vm) < 0)
            goto endjob;
    }
    if (!virDomainObjIsActive(vm) &&
        (ncheckpoints = virDomainListCheckpoints(vm->checkpoints, NULL, dom,
                                                 NULL, flags)) > 0) {
        if (!(flags & VIR_DOMAIN_UNDEFINE_CHECKPOINTS_METADATA)) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("cannot delete inactive domain with %d "
                             "checkpoints"),
                           ncheckpoints);
            goto endjob;
        }
        if (qemuCheckpointDiscardAllMetadata(driver, vm) < 0)
            goto endjob;
    }

    name = qemuDomainManagedSavePath(driver, vm);
    if (name == NULL)
        goto endjob;

    if (virFileExists(name)) {
        if (flags & VIR_DOMAIN_UNDEFINE_MANAGED_SAVE) {
            if (unlink(name) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Failed to remove domain managed "
                                 "save image"));
                goto endjob;
            }
        } else {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("Refusing to undefine while domain managed "
                             "save image exists"));
            goto endjob;
        }
    }

    if (vm->def->os.firmware == VIR_DOMAIN_OS_DEF_FIRMWARE_EFI) {
        qemuDomainNVRAMPathFormat(cfg, vm->def, &nvram_path);
    } else {
        if (vm->def->os.loader)
            nvram_path = g_strdup(vm->def->os.loader->nvram);
    }

    if (nvram_path && virFileExists(nvram_path)) {
        if ((flags & VIR_DOMAIN_UNDEFINE_NVRAM)) {
            if (unlink(nvram_path) < 0) {
                virReportSystemError(errno,
                                     _("failed to remove nvram: %s"),
                                     nvram_path);
                goto endjob;
            }
        } else if (!(flags & VIR_DOMAIN_UNDEFINE_KEEP_NVRAM)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot undefine domain with nvram"));
            goto endjob;
        }
    }

    if (virDomainDeleteConfig(cfg->configDir, cfg->autostartDir, vm) < 0)
        goto endjob;

    event = virDomainEventLifecycleNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_UNDEFINED,
                                     VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);

    VIR_INFO("Undefining domain '%s'", vm->def->name);

    /* If the domain is active, keep it running but set it as transient.
     * domainDestroy and domainShutdown will take care of removing the
     * domain obj from the hash table.
     */
    vm->persistent = 0;
    if (!virDomainObjIsActive(vm))
        qemuDomainRemoveInactive(driver, vm);

    ret = 0;
 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virObjectEventStateQueue(driver->domainEventState, event);
    return ret;
}

static int
qemuDomainUndefine(virDomainPtr dom)
{
    return qemuDomainUndefineFlags(dom, 0);
}

static int
qemuDomainAttachDeviceLive(virDomainObjPtr vm,
                           virDomainDeviceDefPtr dev,
                           virQEMUDriverPtr driver)
{
    int ret = -1;
    const char *alias = NULL;

    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        qemuDomainObjCheckDiskTaint(driver, vm, dev->data.disk, NULL);
        ret = qemuDomainAttachDeviceDiskLive(driver, vm, dev);
        if (!ret) {
            alias = dev->data.disk->info.alias;
            dev->data.disk = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_CONTROLLER:
        ret = qemuDomainAttachControllerDevice(driver, vm, dev->data.controller);
        if (!ret) {
            alias = dev->data.controller->info.alias;
            dev->data.controller = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_LEASE:
        ret = qemuDomainAttachLease(driver, vm,
                                    dev->data.lease);
        if (ret == 0)
            dev->data.lease = NULL;
        break;

    case VIR_DOMAIN_DEVICE_NET:
        qemuDomainObjCheckNetTaint(driver, vm, dev->data.net, NULL);
        ret = qemuDomainAttachNetDevice(driver, vm, dev->data.net);
        if (!ret) {
            alias = dev->data.net->info.alias;
            dev->data.net = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_HOSTDEV:
        qemuDomainObjCheckHostdevTaint(driver, vm, dev->data.hostdev, NULL);
        ret = qemuDomainAttachHostDevice(driver, vm,
                                         dev->data.hostdev);
        if (!ret) {
            alias = dev->data.hostdev->info->alias;
            dev->data.hostdev = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_REDIRDEV:
        ret = qemuDomainAttachRedirdevDevice(driver, vm,
                                             dev->data.redirdev);
        if (!ret) {
            alias = dev->data.redirdev->info.alias;
            dev->data.redirdev = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_CHR:
        ret = qemuDomainAttachChrDevice(driver, vm,
                                        dev->data.chr);
        if (!ret) {
            alias = dev->data.chr->info.alias;
            dev->data.chr = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_RNG:
        ret = qemuDomainAttachRNGDevice(driver, vm,
                                        dev->data.rng);
        if (!ret) {
            alias = dev->data.rng->info.alias;
            dev->data.rng = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_MEMORY:
        /* note that qemuDomainAttachMemory always consumes dev->data.memory
         * and dispatches DeviceAdded event on success */
        ret = qemuDomainAttachMemory(driver, vm,
                                     dev->data.memory);
        dev->data.memory = NULL;
        break;

    case VIR_DOMAIN_DEVICE_SHMEM:
        ret = qemuDomainAttachShmemDevice(driver, vm,
                                          dev->data.shmem);
        if (!ret) {
            alias = dev->data.shmem->info.alias;
            dev->data.shmem = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_WATCHDOG:
        ret = qemuDomainAttachWatchdog(driver, vm,
                                       dev->data.watchdog);
        if (!ret) {
            alias = dev->data.watchdog->info.alias;
            dev->data.watchdog = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_INPUT:
        ret = qemuDomainAttachInputDevice(driver, vm, dev->data.input);
        if (ret == 0) {
            alias = dev->data.input->info.alias;
            dev->data.input = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_VSOCK:
        ret = qemuDomainAttachVsockDevice(driver, vm, dev->data.vsock);
        if (ret == 0) {
            alias = dev->data.vsock->info.alias;
            dev->data.vsock = NULL;
        }
        break;

    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("live attach of device '%s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    if (alias) {
        /* queue the event before the alias has a chance to get freed
         * if the domain disappears while qemuDomainUpdateDeviceList
         * is in monitor */
        virObjectEventPtr event;
        event = virDomainEventDeviceAddedNewFromObj(vm, alias);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    if (ret == 0)
        ret = qemuDomainUpdateDeviceList(driver, vm, QEMU_ASYNC_JOB_NONE);

    return ret;
}


static int
qemuDomainChangeDiskLive(virDomainObjPtr vm,
                         virDomainDeviceDefPtr dev,
                         virQEMUDriverPtr driver,
                         bool force)
{
    virDomainDiskDefPtr disk = dev->data.disk;
    virDomainDiskDefPtr orig_disk = NULL;
    virDomainDeviceDef oldDev = { .type = dev->type };

    if (!(orig_disk = virDomainDiskByTarget(vm->def, disk->dst))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("disk '%s' not found"), disk->dst);
        return -1;
    }

    oldDev.data.disk = orig_disk;
    if (virDomainDefCompatibleDevice(vm->def, dev, &oldDev,
                                     VIR_DOMAIN_DEVICE_ACTION_UPDATE,
                                     true) < 0)
        return -1;

    if (!qemuDomainDiskChangeSupported(disk, orig_disk))
        return -1;

    if (!virStorageSourceIsSameLocation(disk->src, orig_disk->src)) {
        /* Disk source can be changed only for removable devices */
        if (disk->device != VIR_DOMAIN_DISK_DEVICE_CDROM &&
            disk->device != VIR_DOMAIN_DISK_DEVICE_FLOPPY) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("disk source can be changed only in removable "
                             "drives"));
            return -1;
        }

        if (qemuDomainChangeEjectableMedia(driver, vm, orig_disk,
                                           dev->data.disk->src, force) < 0)
            return -1;

        dev->data.disk->src = NULL;
    }

    orig_disk->startupPolicy = dev->data.disk->startupPolicy;
    orig_disk->snapshot = dev->data.disk->snapshot;

    return 0;
}

static int
qemuDomainUpdateDeviceLive(virDomainObjPtr vm,
                           virDomainDeviceDefPtr dev,
                           virDomainPtr dom,
                           bool force)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainDeviceDef oldDev = { .type = dev->type };
    int ret = -1;
    int idx;

    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        qemuDomainObjCheckDiskTaint(driver, vm, dev->data.disk, NULL);
        ret = qemuDomainChangeDiskLive(vm, dev, driver, force);
        break;

    case VIR_DOMAIN_DEVICE_GRAPHICS:
        if ((idx = qemuDomainFindGraphicsIndex(vm->def, dev->data.graphics)) >= 0) {
            oldDev.data.graphics = vm->def->graphics[idx];
            if (virDomainDefCompatibleDevice(vm->def, dev, &oldDev,
                                             VIR_DOMAIN_DEVICE_ACTION_UPDATE,
                                             true) < 0)
                return -1;
        }

        ret = qemuDomainChangeGraphics(driver, vm, dev->data.graphics);
        break;

    case VIR_DOMAIN_DEVICE_NET:
        if ((idx = virDomainNetFindIdx(vm->def, dev->data.net)) >= 0) {
            oldDev.data.net = vm->def->nets[idx];
            if (virDomainDefCompatibleDevice(vm->def, dev, &oldDev,
                                             VIR_DOMAIN_DEVICE_ACTION_UPDATE,
                                             true) < 0)
                return -1;
        }

        ret = qemuDomainChangeNet(driver, vm, dev);
        break;

    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("live update of device '%s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        break;
    }

    return ret;
}


static int
qemuCheckDiskConfigAgainstDomain(const virDomainDef *def,
                                 const virDomainDiskDef *disk)
{
    if (disk->bus == VIR_DOMAIN_DISK_BUS_SCSI &&
        virDomainSCSIDriveAddressIsUsed(def, &disk->info.addr.drive)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Domain already contains a disk with that address"));
        return -1;
    }

    return 0;
}


static int
qemuDomainAttachDeviceConfig(virDomainDefPtr vmdef,
                             virDomainDeviceDefPtr dev,
                             virQEMUCapsPtr qemuCaps,
                             unsigned int parse_flags,
                             virDomainXMLOptionPtr xmlopt)
{
    virDomainDiskDefPtr disk;
    virDomainNetDefPtr net;
    virDomainSoundDefPtr sound;
    virDomainHostdevDefPtr hostdev;
    virDomainLeaseDefPtr lease;
    virDomainControllerDefPtr controller;
    virDomainFSDefPtr fs;
    virDomainRedirdevDefPtr redirdev;
    virDomainShmemDefPtr shmem;

    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        disk = dev->data.disk;
        if (virDomainDiskIndexByName(vmdef, disk->dst, true) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("target %s already exists"), disk->dst);
            return -1;
        }
        if (virDomainDiskTranslateSourcePool(disk) < 0)
            return -1;
        if (qemuCheckDiskConfig(disk, NULL) < 0)
            return -1;
        if (qemuCheckDiskConfigAgainstDomain(vmdef, disk) < 0)
            return -1;
        if (virDomainDiskInsert(vmdef, disk) < 0)
            return -1;
        /* vmdef has the pointer. Generic codes for vmdef will do all jobs */
        dev->data.disk = NULL;
        break;

    case VIR_DOMAIN_DEVICE_NET:
        net = dev->data.net;
        if (virDomainNetInsert(vmdef, net))
            return -1;
        dev->data.net = NULL;
        break;

    case VIR_DOMAIN_DEVICE_SOUND:
        sound = dev->data.sound;
        if (VIR_APPEND_ELEMENT(vmdef->sounds, vmdef->nsounds, sound) < 0)
            return -1;
        dev->data.sound = NULL;
        break;

    case VIR_DOMAIN_DEVICE_HOSTDEV:
        hostdev = dev->data.hostdev;
        if (virDomainHostdevFind(vmdef, hostdev, NULL) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("device is already in the domain configuration"));
            return -1;
        }
        if (virDomainHostdevInsert(vmdef, hostdev))
            return -1;
        dev->data.hostdev = NULL;
        break;

    case VIR_DOMAIN_DEVICE_LEASE:
        lease = dev->data.lease;
        if (virDomainLeaseIndex(vmdef, lease) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("Lease %s in lockspace %s already exists"),
                           lease->key, NULLSTR(lease->lockspace));
            return -1;
        }
        if (virDomainLeaseInsert(vmdef, lease) < 0)
            return -1;

        /* vmdef has the pointer. Generic codes for vmdef will do all jobs */
        dev->data.lease = NULL;
        break;

    case VIR_DOMAIN_DEVICE_CONTROLLER:
        controller = dev->data.controller;
        if (controller->idx != -1 &&
            virDomainControllerFind(vmdef, controller->type,
                                    controller->idx) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("controller index='%d' already exists"),
                           controller->idx);
            return -1;
        }

        if (virDomainControllerInsert(vmdef, controller) < 0)
            return -1;
        dev->data.controller = NULL;

        break;

    case VIR_DOMAIN_DEVICE_CHR:
        if (qemuDomainChrInsert(vmdef, dev->data.chr) < 0)
            return -1;
        dev->data.chr = NULL;
        break;

    case VIR_DOMAIN_DEVICE_FS:
        fs = dev->data.fs;
        if (virDomainFSIndexByName(vmdef, fs->dst) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                         "%s", _("Target already exists"));
            return -1;
        }

        if (virDomainFSInsert(vmdef, fs) < 0)
            return -1;
        dev->data.fs = NULL;
        break;

    case VIR_DOMAIN_DEVICE_RNG:
        if (dev->data.rng->info.type != VIR_DOMAIN_DEVICE_ADDRESS_TYPE_NONE &&
            virDomainDefHasDeviceAddress(vmdef, &dev->data.rng->info)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("a device with the same address already exists "));
            return -1;
        }

        if (VIR_APPEND_ELEMENT(vmdef->rngs, vmdef->nrngs, dev->data.rng) < 0)
            return -1;
        dev->data.rng = NULL;

        break;

    case VIR_DOMAIN_DEVICE_MEMORY:
        if (vmdef->nmems == vmdef->mem.memory_slots) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("no free memory device slot available"));
            return -1;
        }

        vmdef->mem.cur_balloon += dev->data.memory->size;

        if (virDomainMemoryInsert(vmdef, dev->data.memory) < 0)
            return -1;
        dev->data.memory = NULL;
        break;

    case VIR_DOMAIN_DEVICE_REDIRDEV:
        redirdev = dev->data.redirdev;

        if (VIR_APPEND_ELEMENT(vmdef->redirdevs, vmdef->nredirdevs, redirdev) < 0)
            return -1;
        dev->data.redirdev = NULL;
        break;

    case VIR_DOMAIN_DEVICE_SHMEM:
        shmem = dev->data.shmem;
        if (virDomainShmemDefFind(vmdef, shmem) >= 0) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("device is already in the domain configuration"));
            return -1;
        }
        if (virDomainShmemDefInsert(vmdef, shmem) < 0)
            return -1;
        dev->data.shmem = NULL;
        break;

    case VIR_DOMAIN_DEVICE_WATCHDOG:
        if (vmdef->watchdog) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("domain already has a watchdog"));
            return -1;
        }
        vmdef->watchdog = g_steal_pointer(&dev->data.watchdog);
        break;

    case VIR_DOMAIN_DEVICE_INPUT:
        if (VIR_APPEND_ELEMENT(vmdef->inputs, vmdef->ninputs, dev->data.input) < 0)
            return -1;
        break;

    case VIR_DOMAIN_DEVICE_VSOCK:
        if (vmdef->vsock) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("domain already has a vsock device"));
            return -1;
        }
        vmdef->vsock = g_steal_pointer(&dev->data.vsock);
        break;

    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_LAST:
         virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                        _("persistent attach of device '%s' is not supported"),
                        virDomainDeviceTypeToString(dev->type));
         return -1;
    }

    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, qemuCaps) < 0)
        return -1;

    return 0;
}


static int
qemuDomainDetachDeviceConfig(virDomainDefPtr vmdef,
                             virDomainDeviceDefPtr dev,
                             virQEMUCapsPtr qemuCaps,
                             unsigned int parse_flags,
                             virDomainXMLOptionPtr xmlopt)
{
    virDomainDiskDefPtr disk, det_disk;
    virDomainNetDefPtr net;
    virDomainSoundDefPtr sound;
    virDomainHostdevDefPtr hostdev, det_hostdev;
    virDomainLeaseDefPtr lease, det_lease;
    virDomainControllerDefPtr cont, det_cont;
    virDomainChrDefPtr chr;
    virDomainFSDefPtr fs;
    virDomainMemoryDefPtr mem;
    int idx;

    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        disk = dev->data.disk;
        if (!(det_disk = virDomainDiskRemoveByName(vmdef, disk->dst))) {
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("no target device %s"), disk->dst);
            return -1;
        }
        virDomainDiskDefFree(det_disk);
        break;

    case VIR_DOMAIN_DEVICE_NET:
        net = dev->data.net;
        if ((idx = virDomainNetFindIdx(vmdef, net)) < 0)
            return -1;

        /* this is guaranteed to succeed */
        virDomainNetDefFree(virDomainNetRemove(vmdef, idx));
        break;

    case VIR_DOMAIN_DEVICE_SOUND:
        sound = dev->data.sound;
        if ((idx = virDomainSoundDefFind(vmdef, sound)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("device not present in domain configuration"));
            return -1;
        }
        virDomainSoundDefFree(virDomainSoundDefRemove(vmdef, idx));
        break;

    case VIR_DOMAIN_DEVICE_HOSTDEV: {
        hostdev = dev->data.hostdev;
        if ((idx = virDomainHostdevFind(vmdef, hostdev, &det_hostdev)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("device not present in domain configuration"));
            return -1;
        }
        virDomainHostdevRemove(vmdef, idx);
        virDomainHostdevDefFree(det_hostdev);
        break;
    }

    case VIR_DOMAIN_DEVICE_LEASE:
        lease = dev->data.lease;
        if (!(det_lease = virDomainLeaseRemove(vmdef, lease))) {
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("Lease %s in lockspace %s does not exist"),
                           lease->key, NULLSTR(lease->lockspace));
            return -1;
        }
        virDomainLeaseDefFree(det_lease);
        break;

    case VIR_DOMAIN_DEVICE_CONTROLLER:
        cont = dev->data.controller;
        if ((idx = virDomainControllerFind(vmdef, cont->type,
                                           cont->idx)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("device not present in domain configuration"));
            return -1;
        }
        det_cont = virDomainControllerRemove(vmdef, idx);
        virDomainControllerDefFree(det_cont);

        break;

    case VIR_DOMAIN_DEVICE_CHR:
        if (!(chr = qemuDomainChrRemove(vmdef, dev->data.chr)))
            return -1;

        virDomainChrDefFree(chr);
        break;

    case VIR_DOMAIN_DEVICE_FS:
        fs = dev->data.fs;
        idx = virDomainFSIndexByName(vmdef, fs->dst);
        if (idx < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("no matching filesystem device was found"));
            return -1;
        }

        fs = virDomainFSRemove(vmdef, idx);
        virDomainFSDefFree(fs);
        break;

    case VIR_DOMAIN_DEVICE_RNG:
        if ((idx = virDomainRNGFind(vmdef, dev->data.rng)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("no matching RNG device was found"));
            return -1;
        }

        virDomainRNGDefFree(virDomainRNGRemove(vmdef, idx));
        break;

    case VIR_DOMAIN_DEVICE_MEMORY:
        if ((idx = virDomainMemoryFindInactiveByDef(vmdef,
                                                    dev->data.memory)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("matching memory device was not found"));
            return -1;
        }
        mem = virDomainMemoryRemove(vmdef, idx);
        vmdef->mem.cur_balloon -= mem->size;
        virDomainMemoryDefFree(mem);
        break;

    case VIR_DOMAIN_DEVICE_REDIRDEV:
        if ((idx = virDomainRedirdevDefFind(vmdef,
                                            dev->data.redirdev)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("no matching redirdev was not found"));
            return -1;
        }

        virDomainRedirdevDefFree(virDomainRedirdevDefRemove(vmdef, idx));
        break;

    case VIR_DOMAIN_DEVICE_SHMEM:
        if ((idx = virDomainShmemDefFind(vmdef, dev->data.shmem)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("matching shmem device was not found"));
            return -1;
        }

        virDomainShmemDefFree(virDomainShmemDefRemove(vmdef, idx));
        break;


    case VIR_DOMAIN_DEVICE_WATCHDOG:
        if (!vmdef->watchdog) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("domain has no watchdog"));
            return -1;
        }
        virDomainWatchdogDefFree(vmdef->watchdog);
        vmdef->watchdog = NULL;
        break;

    case VIR_DOMAIN_DEVICE_INPUT:
        if ((idx = virDomainInputDefFind(vmdef, dev->data.input)) < 0) {
            virReportError(VIR_ERR_DEVICE_MISSING, "%s",
                           _("matching input device not found"));
            return -1;
        }
        VIR_DELETE_ELEMENT(vmdef->inputs, idx, vmdef->ninputs);
        break;

    case VIR_DOMAIN_DEVICE_VSOCK:
        if (!vmdef->vsock ||
            !virDomainVsockDefEquals(dev->data.vsock, vmdef->vsock)) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("matching vsock device not found"));
            return -1;
        }
        virDomainVsockDefFree(vmdef->vsock);
        vmdef->vsock = NULL;
        break;

    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("persistent detach of device '%s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;
    }

    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, qemuCaps) < 0)
        return -1;

    return 0;
}

static int
qemuDomainUpdateDeviceConfig(virDomainDefPtr vmdef,
                             virDomainDeviceDefPtr dev,
                             virQEMUCapsPtr qemuCaps,
                             unsigned int parse_flags,
                             virDomainXMLOptionPtr xmlopt)
{
    virDomainDiskDefPtr newDisk;
    virDomainGraphicsDefPtr newGraphics;
    virDomainNetDefPtr net;
    virDomainDeviceDef oldDev = { .type = dev->type };
    int pos;

    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
        newDisk = dev->data.disk;
        if ((pos = virDomainDiskIndexByName(vmdef, newDisk->dst, false)) < 0) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("target %s doesn't exist."), newDisk->dst);
            return -1;
        }

        oldDev.data.disk = vmdef->disks[pos];
        if (virDomainDefCompatibleDevice(vmdef, dev, &oldDev,
                                         VIR_DOMAIN_DEVICE_ACTION_UPDATE,
                                         false) < 0)
            return -1;

        virDomainDiskDefFree(vmdef->disks[pos]);
        vmdef->disks[pos] = newDisk;
        dev->data.disk = NULL;
        break;

    case VIR_DOMAIN_DEVICE_GRAPHICS:
        newGraphics = dev->data.graphics;
        pos = qemuDomainFindGraphicsIndex(vmdef, newGraphics);
        if (pos < 0) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("cannot find existing graphics type '%s' device to modify"),
                           virDomainGraphicsTypeToString(newGraphics->type));
            return -1;
        }

        oldDev.data.graphics = vmdef->graphics[pos];
        if (virDomainDefCompatibleDevice(vmdef, dev, &oldDev,
                                         VIR_DOMAIN_DEVICE_ACTION_UPDATE,
                                         false) < 0)
            return -1;

        virDomainGraphicsDefFree(vmdef->graphics[pos]);
        vmdef->graphics[pos] = newGraphics;
        dev->data.graphics = NULL;
        break;

    case VIR_DOMAIN_DEVICE_NET:
        net = dev->data.net;
        if ((pos = virDomainNetFindIdx(vmdef, net)) < 0)
            return -1;

        oldDev.data.net = vmdef->nets[pos];
        if (virDomainDefCompatibleDevice(vmdef, dev, &oldDev,
                                         VIR_DOMAIN_DEVICE_ACTION_UPDATE,
                                         false) < 0)
            return -1;

        if (virDomainNetUpdate(vmdef, pos, net))
            return -1;

        virDomainNetDefFree(oldDev.data.net);
        dev->data.net = NULL;
        break;

    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_CHR:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_NONE:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_LAST:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("persistent update of device '%s' is not supported"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;
    }

    if (virDomainDefPostParse(vmdef, parse_flags, xmlopt, qemuCaps) < 0)
        return -1;

    return 0;
}


static void
qemuDomainAttachDeviceLiveAndConfigHomogenize(const virDomainDeviceDef *devConf,
                                              virDomainDeviceDefPtr devLive)
{
    /*
     * Fixup anything that needs to be identical in the live and
     * config versions of DeviceDef, but might not be. Do this by
     * changing the contents of devLive. This is done after all
     * post-parse tweaks and validation, so be very careful about what
     * changes are made. (For example, it would be a very bad idea to
     * change assigned PCI, scsi, or sata addresses, as it could lead
     * to a conflict and there would be nothing to catch it except
     * qemu itself!)
     */

    /* MAC address should be identical in both DeviceDefs, but if it
     * wasn't specified in the XML, and was instead autogenerated, it
     * will be different for the two since they are each the result of
     * a separate parser call. If it *was* specified, it will already
     * be the same, so copying does no harm.
     */

    if (devConf->type == VIR_DOMAIN_DEVICE_NET)
        virMacAddrSet(&devLive->data.net->mac, &devConf->data.net->mac);

}


static int
qemuDomainAttachDeviceLiveAndConfig(virDomainObjPtr vm,
                                    virQEMUDriverPtr driver,
                                    const char *xml,
                                    unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDefPtr vmdef = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virDomainDeviceDefPtr devConf = NULL;
    virDomainDeviceDef devConfSave = { 0 };
    virDomainDeviceDefPtr devLive = NULL;
    int ret = -1;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_INACTIVE |
                               VIR_DOMAIN_DEF_PARSE_ABI_UPDATE;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virQEMUDriverGetConfig(driver);

    /* The config and live post processing address auto-generation algorithms
     * rely on the correct vm->def or vm->newDef being passed, so call the
     * device parse based on which definition is in use */
    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt,
                                              priv->qemuCaps);
        if (!vmdef)
            goto cleanup;

        if (!(devConf = virDomainDeviceDefParse(xml, vmdef,
                                                driver->xmlopt, priv->qemuCaps,
                                                parse_flags)))
            goto cleanup;

        /*
         * devConf will be NULLed out by
         * qemuDomainAttachDeviceConfig(), so save it for later use by
         * qemuDomainAttachDeviceLiveAndConfigHomogenize()
         */
        devConfSave = *devConf;

        if (virDomainDeviceValidateAliasForHotplug(vm, devConf,
                                                   VIR_DOMAIN_AFFECT_CONFIG) < 0)
            goto cleanup;

        if (virDomainDefCompatibleDevice(vmdef, devConf, NULL,
                                         VIR_DOMAIN_DEVICE_ACTION_ATTACH,
                                         false) < 0)
            goto cleanup;

        if (qemuDomainAttachDeviceConfig(vmdef, devConf, priv->qemuCaps,
                                         parse_flags,
                                         driver->xmlopt) < 0)
            goto cleanup;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        if (!(devLive = virDomainDeviceDefParse(xml, vm->def,
                                                driver->xmlopt, priv->qemuCaps,
                                                parse_flags)))
            goto cleanup;

        if (flags & VIR_DOMAIN_AFFECT_CONFIG)
            qemuDomainAttachDeviceLiveAndConfigHomogenize(&devConfSave, devLive);

        if (virDomainDeviceValidateAliasForHotplug(vm, devLive,
                                                   VIR_DOMAIN_AFFECT_LIVE) < 0)
            goto cleanup;

        if (virDomainDefCompatibleDevice(vm->def, devLive, NULL,
                                         VIR_DOMAIN_DEVICE_ACTION_ATTACH,
                                         true) < 0)
            goto cleanup;

        if (qemuDomainAttachDeviceLive(vm, devLive, driver) < 0)
            goto cleanup;
        /*
         * update domain status forcibly because the domain status may be
         * changed even if we failed to attach the device. For example,
         * a new controller may be created.
         */
        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto cleanup;
    }

    /* Finally, if no error until here, we can save config. */
    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
            goto cleanup;

        virDomainObjAssignDef(vm, vmdef, false, NULL);
        vmdef = NULL;
    }

    ret = 0;
 cleanup:
    virDomainDefFree(vmdef);
    virDomainDeviceDefFree(devConf);
    virDomainDeviceDefFree(devLive);

    return ret;
}

static int
qemuDomainAttachDeviceFlags(virDomainPtr dom,
                            const char *xml,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    virNWFilterReadLockFilterUpdates();

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainAttachDeviceFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjUpdateModificationImpact(vm, &flags) < 0)
        goto endjob;

    if (qemuDomainAttachDeviceLiveAndConfig(vm, driver, xml, flags) < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virNWFilterUnlockFilterUpdates();
    return ret;
}

static int qemuDomainAttachDevice(virDomainPtr dom, const char *xml)
{
    return qemuDomainAttachDeviceFlags(dom, xml,
                                       VIR_DOMAIN_AFFECT_LIVE);
}


static int qemuDomainUpdateDeviceFlags(virDomainPtr dom,
                                       const char *xml,
                                       unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuDomainObjPrivatePtr priv;
    virDomainDefPtr vmdef = NULL;
    virDomainDeviceDefPtr dev = NULL, dev_copy = NULL;
    bool force = (flags & VIR_DOMAIN_DEVICE_MODIFY_FORCE) != 0;
    int ret = -1;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    unsigned int parse_flags = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_DOMAIN_DEVICE_MODIFY_FORCE, -1);

    virNWFilterReadLockFilterUpdates();

    cfg = virQEMUDriverGetConfig(driver);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainUpdateDeviceFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjUpdateModificationImpact(vm, &flags) < 0)
        goto endjob;

    if ((flags & VIR_DOMAIN_AFFECT_CONFIG) &&
        !(flags & VIR_DOMAIN_AFFECT_LIVE))
        parse_flags |= VIR_DOMAIN_DEF_PARSE_INACTIVE;

    dev = dev_copy = virDomainDeviceDefParse(xml, vm->def,
                                             driver->xmlopt, priv->qemuCaps,
                                             parse_flags);
    if (dev == NULL)
        goto endjob;

    if (flags & VIR_DOMAIN_AFFECT_CONFIG &&
        flags & VIR_DOMAIN_AFFECT_LIVE) {
        /* If we are affecting both CONFIG and LIVE
         * create a deep copy of device as adding
         * to CONFIG takes one instance.
         */
        dev_copy = virDomainDeviceDefCopy(dev, vm->def,
                                          driver->xmlopt, priv->qemuCaps);
        if (!dev_copy)
            goto endjob;
    }

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        /* Make a copy for updated domain. */
        vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt,
                                              priv->qemuCaps);
        if (!vmdef)
            goto endjob;

        /* virDomainDefCompatibleDevice call is delayed until we know the
         * device we're going to update. */
        if ((ret = qemuDomainUpdateDeviceConfig(vmdef, dev, priv->qemuCaps,
                                                parse_flags,
                                                driver->xmlopt)) < 0)
            goto endjob;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        /* virDomainDefCompatibleDevice call is delayed until we know the
         * device we're going to update. */
        if ((ret = qemuDomainUpdateDeviceLive(vm, dev_copy, dom, force)) < 0)
            goto endjob;
        /*
         * update domain status forcibly because the domain status may be
         * changed even if we failed to attach the device. For example,
         * a new controller may be created.
         */
        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0) {
            ret = -1;
            goto endjob;
        }
    }

    /* Finally, if no error until here, we can save config. */
    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        ret = virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir);
        if (!ret) {
            virDomainObjAssignDef(vm, vmdef, false, NULL);
            vmdef = NULL;
        }
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainDefFree(vmdef);
    if (dev != dev_copy)
        virDomainDeviceDefFree(dev_copy);
    virDomainDeviceDefFree(dev);
    virDomainObjEndAPI(&vm);
    virNWFilterUnlockFilterUpdates();
    return ret;
}

static int
qemuDomainDetachDeviceLiveAndConfig(virQEMUDriverPtr driver,
                                    virDomainObjPtr vm,
                                    const char *xml,
                                    unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virDomainDeviceDefPtr dev = NULL, dev_copy = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE;
    virDomainDefPtr vmdef = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virQEMUDriverGetConfig(driver);

    if ((flags & VIR_DOMAIN_AFFECT_CONFIG) &&
        !(flags & VIR_DOMAIN_AFFECT_LIVE))
        parse_flags |= VIR_DOMAIN_DEF_PARSE_INACTIVE;

    dev = dev_copy = virDomainDeviceDefParse(xml, vm->def,
                                             driver->xmlopt, priv->qemuCaps,
                                             parse_flags);
    if (dev == NULL)
        goto cleanup;

    if (flags & VIR_DOMAIN_AFFECT_CONFIG &&
        flags & VIR_DOMAIN_AFFECT_LIVE) {
        /* If we are affecting both CONFIG and LIVE
         * create a deep copy of device as adding
         * to CONFIG takes one instance.
         */
        dev_copy = virDomainDeviceDefCopy(dev, vm->def,
                                          driver->xmlopt, priv->qemuCaps);
        if (!dev_copy)
            goto cleanup;
    }

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        /* Make a copy for updated domain. */
        vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt, priv->qemuCaps);
        if (!vmdef)
            goto cleanup;

        if (qemuDomainDetachDeviceConfig(vmdef, dev, priv->qemuCaps,
                                         parse_flags,
                                         driver->xmlopt) < 0)
            goto cleanup;
    }

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        int rc;

        if ((rc = qemuDomainDetachDeviceLive(vm, dev_copy, driver, false)) < 0)
            goto cleanup;

        if (rc == 0 && qemuDomainUpdateDeviceList(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
            goto cleanup;

        /*
         * update domain status forcibly because the domain status may be
         * changed even if we failed to attach the device. For example,
         * a new controller may be created.
         */
        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto cleanup;
    }

    /* Finally, if no error until here, we can save config. */
    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
            goto cleanup;

        virDomainObjAssignDef(vm, vmdef, false, NULL);
        vmdef = NULL;
    }

    ret = 0;

 cleanup:
    if (dev != dev_copy)
        virDomainDeviceDefFree(dev_copy);
    virDomainDeviceDefFree(dev);
    virDomainDefFree(vmdef);
    return ret;
}


static int
qemuDomainDetachDeviceAliasLiveAndConfig(virQEMUDriverPtr driver,
                                         virDomainObjPtr vm,
                                         const char *alias,
                                         unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    virDomainDefPtr vmdef = NULL;
    unsigned int parse_flags = VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    cfg = virQEMUDriverGetConfig(driver);

    if ((flags & VIR_DOMAIN_AFFECT_CONFIG) &&
        !(flags & VIR_DOMAIN_AFFECT_LIVE))
        parse_flags |= VIR_DOMAIN_DEF_PARSE_INACTIVE;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto cleanup;

    if (persistentDef) {
        virDomainDeviceDef dev;

        if (!(vmdef = virDomainObjCopyPersistentDef(vm, driver->xmlopt,
                                                    priv->qemuCaps)))
            goto cleanup;

        if (virDomainDefFindDevice(vmdef, alias, &dev, true) < 0)
            goto cleanup;

        if (qemuDomainDetachDeviceConfig(vmdef, &dev, priv->qemuCaps,
                                         parse_flags, driver->xmlopt) < 0)
            goto cleanup;
    }

    if (def) {
        virDomainDeviceDef dev;
        int rc;

        if (virDomainDefFindDevice(def, alias, &dev, true) < 0)
            goto cleanup;

        if ((rc = qemuDomainDetachDeviceLive(vm, &dev, driver, true)) < 0)
            goto cleanup;

        if (rc == 0 && qemuDomainUpdateDeviceList(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
            goto cleanup;
    }

    if (vmdef) {
        if (virDomainDefSave(vmdef, driver->xmlopt, cfg->configDir) < 0)
            goto cleanup;
        virDomainObjAssignDef(vm, vmdef, false, NULL);
        vmdef = NULL;
    }

    ret = 0;
 cleanup:
    virDomainDefFree(vmdef);
    return ret;
}


static int
qemuDomainDetachDeviceFlags(virDomainPtr dom,
                            const char *xml,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainDetachDeviceFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjUpdateModificationImpact(vm, &flags) < 0)
        goto endjob;

    if (qemuDomainDetachDeviceLiveAndConfig(driver, vm, xml, flags) < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainDetachDeviceAlias(virDomainPtr dom,
                            const char *alias,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainDetachDeviceAliasEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjUpdateModificationImpact(vm, &flags) < 0)
        goto endjob;

    if (qemuDomainDetachDeviceAliasLiveAndConfig(driver, vm, alias, flags) < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int qemuDomainDetachDevice(virDomainPtr dom, const char *xml)
{
    return qemuDomainDetachDeviceFlags(dom, xml,
                                       VIR_DOMAIN_AFFECT_LIVE);
}

static int qemuDomainGetAutostart(virDomainPtr dom,
                                  int *autostart)
{
    virDomainObjPtr vm;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetAutostartEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    *autostart = vm->autostart;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int qemuDomainSetAutostart(virDomainPtr dom,
                                  int autostart)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    g_autofree char *configFile = NULL;
    g_autofree char *autostartLink = NULL;
    int ret = -1;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetAutostartEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot set autostart for transient domain"));
        goto cleanup;
    }

    autostart = (autostart != 0);

    if (vm->autostart != autostart) {
        if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
            goto cleanup;

        if (!(configFile = virDomainConfigFile(cfg->configDir, vm->def->name)))
            goto endjob;

        if (!(autostartLink = virDomainConfigFile(cfg->autostartDir,
                                                  vm->def->name)))
            goto endjob;

        if (autostart) {
            if (virFileMakePath(cfg->autostartDir) < 0) {
                virReportSystemError(errno,
                                     _("cannot create autostart directory %s"),
                                     cfg->autostartDir);
                goto endjob;
            }

            if (symlink(configFile, autostartLink) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create symlink '%s to '%s'"),
                                     autostartLink, configFile);
                goto endjob;
            }
        } else {
            if (unlink(autostartLink) < 0 &&
                errno != ENOENT &&
                errno != ENOTDIR) {
                virReportSystemError(errno,
                                     _("Failed to delete symlink '%s'"),
                                     autostartLink);
                goto endjob;
            }
        }

        vm->autostart = autostart;

 endjob:
        qemuDomainObjEndJob(driver, vm);
    }
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static char *qemuDomainGetSchedulerType(virDomainPtr dom,
                                        int *nparams)
{
    char *ret = NULL;
    virDomainObjPtr vm = NULL;
    qemuDomainObjPrivatePtr priv;
    virQEMUDriverPtr driver = dom->conn->privateData;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainGetSchedulerTypeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("CPU tuning is not available in session mode"));
        goto cleanup;
    }

    /* Domain not running, thus no cgroups - return defaults */
    if (!virDomainObjIsActive(vm)) {
        if (nparams)
            *nparams = 9;
        ret = g_strdup("posix");
        goto cleanup;
    }

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cgroup CPU controller is not mounted"));
        goto cleanup;
    }

    if (nparams) {
        if (virCgroupSupportsCpuBW(priv->cgroup))
            *nparams = 9;
        else
            *nparams = 1;
    }

    ret = g_strdup("posix");

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/* blkioDeviceStr in the form of /device/path,weight,/device/path,weight
 * for example, /dev/disk/by-path/pci-0000:00:1f.2-scsi-0:0:0:0,800
 */
static int
qemuDomainParseBlkioDeviceStr(char *blkioDeviceStr, const char *type,
                              virBlkioDevicePtr *dev, size_t *size)
{
    char *temp;
    int ndevices = 0;
    int nsep = 0;
    size_t i;
    virBlkioDevicePtr result = NULL;

    *dev = NULL;
    *size = 0;

    if (STREQ(blkioDeviceStr, ""))
        return 0;

    temp = blkioDeviceStr;
    while (temp) {
        temp = strchr(temp, ',');
        if (temp) {
            temp++;
            nsep++;
        }
    }

    /* A valid string must have even number of fields, hence an odd
     * number of commas.  */
    if (!(nsep & 1))
        goto parse_error;

    ndevices = (nsep + 1) / 2;

    if (VIR_ALLOC_N(result, ndevices) < 0)
        return -1;

    i = 0;
    temp = blkioDeviceStr;
    while (temp) {
        char *p = temp;

        /* device path */
        p = strchr(p, ',');
        if (!p)
            goto parse_error;

        result[i].path = g_strndup(temp, p - temp);

        /* value */
        temp = p + 1;

        if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
            if (virStrToLong_uip(temp, &p, 10, &result[i].weight) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
            if (virStrToLong_uip(temp, &p, 10, &result[i].riops) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
            if (virStrToLong_uip(temp, &p, 10, &result[i].wiops) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
            if (virStrToLong_ullp(temp, &p, 10, &result[i].rbps) < 0)
                goto number_error;
        } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
            if (virStrToLong_ullp(temp, &p, 10, &result[i].wbps) < 0)
                goto number_error;
        } else {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("unknown parameter '%s'"), type);
            goto cleanup;
        }

        i++;

        if (*p == '\0')
            break;
        else if (*p != ',')
            goto parse_error;
        temp = p + 1;
    }

    if (!i)
        VIR_FREE(result);

    *dev = result;
    *size = i;

    return 0;

 parse_error:
    virReportError(VIR_ERR_INVALID_ARG,
                   _("unable to parse blkio device '%s' '%s'"),
                   type, blkioDeviceStr);
    goto cleanup;

 number_error:
    virReportError(VIR_ERR_INVALID_ARG,
                   _("invalid value '%s' for parameter '%s' of device '%s'"),
                   temp, type, result[i].path);

 cleanup:
    if (result) {
        virBlkioDeviceArrayClear(result, ndevices);
        VIR_FREE(result);
    }
    return -1;
}

/* Modify dest_array to reflect all blkio device changes described in
 * src_array.  */
static int
qemuDomainMergeBlkioDevice(virBlkioDevicePtr *dest_array,
                           size_t *dest_size,
                           virBlkioDevicePtr src_array,
                           size_t src_size,
                           const char *type)
{
    size_t i, j;
    virBlkioDevicePtr dest, src;

    for (i = 0; i < src_size; i++) {
        bool found = false;

        src = &src_array[i];
        for (j = 0; j < *dest_size; j++) {
            dest = &(*dest_array)[j];
            if (STREQ(src->path, dest->path)) {
                found = true;

                if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
                    dest->weight = src->weight;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
                    dest->riops = src->riops;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
                    dest->wiops = src->wiops;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
                    dest->rbps = src->rbps;
                } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                    dest->wbps = src->wbps;
                } else {
                    virReportError(VIR_ERR_INVALID_ARG, _("Unknown parameter %s"),
                                   type);
                    return -1;
                }
                break;
            }
        }
        if (!found) {
            if (!src->weight && !src->riops && !src->wiops && !src->rbps && !src->wbps)
                continue;
            if (VIR_EXPAND_N(*dest_array, *dest_size, 1) < 0)
                return -1;
            dest = &(*dest_array)[*dest_size - 1];

            if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
                dest->weight = src->weight;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
                dest->riops = src->riops;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
                dest->wiops = src->wiops;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
                dest->rbps = src->rbps;
            } else if (STREQ(type, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                dest->wbps = src->wbps;
            } else {
                *dest_size = *dest_size - 1;
                return -1;
            }

            dest->path = src->path;
            src->path = NULL;
        }
    }

    return 0;
}

static int
qemuDomainSetBlkioParameters(virDomainPtr dom,
                             virTypedParameterPtr params,
                             int nparams,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    size_t i;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);
    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_BLKIO_WEIGHT,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BLKIO_DEVICE_WEIGHT,
                               VIR_TYPED_PARAM_STRING,
                               VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS,
                               VIR_TYPED_PARAM_STRING,
                               VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS,
                               VIR_TYPED_PARAM_STRING,
                               VIR_DOMAIN_BLKIO_DEVICE_READ_BPS,
                               VIR_TYPED_PARAM_STRING,
                               VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS,
                               VIR_TYPED_PARAM_STRING,
                               NULL) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetBlkioParametersEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Block I/O tuning is not available in session mode"));
        goto cleanup;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (flags & VIR_DOMAIN_AFFECT_LIVE) {
        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_BLKIO)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("blkio cgroup isn't mounted"));
            goto endjob;
        }
    }

    ret = 0;
    if (def) {
        for (i = 0; i < nparams; i++) {
            virTypedParameterPtr param = &params[i];

            if (STREQ(param->field, VIR_DOMAIN_BLKIO_WEIGHT)) {
                if (virCgroupSetBlkioWeight(priv->cgroup, param->value.ui) < 0 ||
                    virCgroupGetBlkioWeight(priv->cgroup, &def->blkio.weight) < 0)
                    ret = -1;
            } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                size_t ndevices;
                virBlkioDevicePtr devices = NULL;
                size_t j;

                if (qemuDomainParseBlkioDeviceStr(param->value.s,
                                                  param->field,
                                                  &devices,
                                                  &ndevices) < 0) {
                    ret = -1;
                    continue;
                }

                if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT)) {
                    for (j = 0; j < ndevices; j++) {
                        if (virCgroupSetBlkioDeviceWeight(priv->cgroup,
                                                          devices[j].path,
                                                          devices[j].weight) < 0 ||
                            virCgroupGetBlkioDeviceWeight(priv->cgroup,
                                                          devices[j].path,
                                                          &devices[j].weight) < 0) {
                            ret = -1;
                            break;
                        }
                    }
                } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS)) {
                    for (j = 0; j < ndevices; j++) {
                        if (virCgroupSetBlkioDeviceReadIops(priv->cgroup,
                                                            devices[j].path,
                                                            devices[j].riops) < 0 ||
                            virCgroupGetBlkioDeviceReadIops(priv->cgroup,
                                                            devices[j].path,
                                                            &devices[j].riops) < 0) {
                            ret = -1;
                            break;
                        }
                    }
                } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS)) {
                    for (j = 0; j < ndevices; j++) {
                        if (virCgroupSetBlkioDeviceWriteIops(priv->cgroup,
                                                             devices[j].path,
                                                             devices[j].wiops) < 0 ||
                            virCgroupGetBlkioDeviceWriteIops(priv->cgroup,
                                                             devices[j].path,
                                                             &devices[j].wiops) < 0) {
                            ret = -1;
                            break;
                        }
                    }
                } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS)) {
                    for (j = 0; j < ndevices; j++) {
                        if (virCgroupSetBlkioDeviceReadBps(priv->cgroup,
                                                           devices[j].path,
                                                           devices[j].rbps) < 0 ||
                            virCgroupGetBlkioDeviceReadBps(priv->cgroup,
                                                           devices[j].path,
                                                           &devices[j].rbps) < 0) {
                            ret = -1;
                            break;
                        }
                    }
                } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                    for (j = 0; j < ndevices; j++) {
                        if (virCgroupSetBlkioDeviceWriteBps(priv->cgroup,
                                                            devices[j].path,
                                                            devices[j].wbps) < 0 ||
                            virCgroupGetBlkioDeviceWriteBps(priv->cgroup,
                                                            devices[j].path,
                                                            &devices[j].wbps) < 0) {
                            ret = -1;
                            break;
                        }
                    }
                } else {
                    virReportError(VIR_ERR_INVALID_ARG, _("Unknown blkio parameter %s"),
                                   param->field);
                    ret = -1;
                    virBlkioDeviceArrayClear(devices, ndevices);
                    VIR_FREE(devices);

                    continue;
                }

                if (j != ndevices ||
                    qemuDomainMergeBlkioDevice(&def->blkio.devices,
                                               &def->blkio.ndevices,
                                               devices, ndevices, param->field) < 0)
                    ret = -1;
                virBlkioDeviceArrayClear(devices, ndevices);
                VIR_FREE(devices);
            }
        }

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }
    if (ret < 0)
        goto endjob;
    if (persistentDef) {
        for (i = 0; i < nparams; i++) {
            virTypedParameterPtr param = &params[i];

            if (STREQ(param->field, VIR_DOMAIN_BLKIO_WEIGHT)) {
                persistentDef->blkio.weight = param->value.ui;
            } else if (STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WEIGHT) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_IOPS) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_IOPS) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_READ_BPS) ||
                       STREQ(param->field, VIR_DOMAIN_BLKIO_DEVICE_WRITE_BPS)) {
                virBlkioDevicePtr devices = NULL;
                size_t ndevices;

                if (qemuDomainParseBlkioDeviceStr(param->value.s,
                                                  param->field,
                                                  &devices,
                                                  &ndevices) < 0) {
                    ret = -1;
                    continue;
                }
                if (qemuDomainMergeBlkioDevice(&persistentDef->blkio.devices,
                                               &persistentDef->blkio.ndevices,
                                               devices, ndevices, param->field) < 0)
                    ret = -1;
                virBlkioDeviceArrayClear(devices, ndevices);
                VIR_FREE(devices);
            }
        }

        if (virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
            ret = -1;
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainGetBlkioParameters(virDomainPtr dom,
                             virTypedParameterPtr params,
                             int *nparams,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    int maxparams = QEMU_NB_BLKIO_PARAM;
    unsigned int val;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    /* We blindly return a string, and let libvirt.c and
     * remote_driver.c do the filtering on behalf of older clients
     * that can't parse it.  */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;

    if (virDomainGetBlkioParametersEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Block I/O tuning is not available in session mode"));
        goto cleanup;
    }

    if ((*nparams) == 0) {
        /* Current number of blkio parameters supported by cgroups */
        *nparams = QEMU_NB_BLKIO_PARAM;
        ret = 0;
        goto cleanup;
    } else if (*nparams < maxparams) {
        maxparams = *nparams;
    }

    *nparams = 0;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto cleanup;

    if (def) {
        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_BLKIO)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("blkio cgroup isn't mounted"));
            goto cleanup;
        }

        /* fill blkio weight here */
        if (virCgroupGetBlkioWeight(priv->cgroup, &val) < 0)
            goto cleanup;
        if (virTypedParameterAssign(&(params[(*nparams)++]),
                                    VIR_DOMAIN_BLKIO_WEIGHT,
                                    VIR_TYPED_PARAM_UINT, val) < 0)
            goto cleanup;

        if (virDomainGetBlkioParametersAssignFromDef(def, params, nparams,
                                                     maxparams) < 0)
            goto cleanup;

    } else if (persistentDef) {
        /* fill blkio weight here */
        if (virTypedParameterAssign(&(params[(*nparams)++]),
                                    VIR_DOMAIN_BLKIO_WEIGHT,
                                    VIR_TYPED_PARAM_UINT,
                                    persistentDef->blkio.weight) < 0)
            goto cleanup;

        if (virDomainGetBlkioParametersAssignFromDef(persistentDef, params,
                                                     nparams, maxparams) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainSetMemoryParameters(virDomainPtr dom,
                              virTypedParameterPtr params,
                              int nparams,
                              unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    virDomainObjPtr vm = NULL;
    unsigned long long swap_hard_limit;
    unsigned long long hard_limit = 0;
    unsigned long long soft_limit = 0;
    bool set_swap_hard_limit = false;
    bool set_hard_limit = false;
    bool set_soft_limit = false;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    int rc;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_MEMORY_HARD_LIMIT,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_MEMORY_SOFT_LIMIT,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_MEMORY_SWAP_HARD_LIMIT,
                               VIR_TYPED_PARAM_ULLONG,
                               NULL) < 0)
        return -1;


    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetMemoryParametersEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("Memory tuning is not available in session mode"));
        goto cleanup;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    /* QEMU and LXC implementation are identical */
    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_MEMORY)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cgroup memory controller is not mounted"));
        goto endjob;
    }

#define VIR_GET_LIMIT_PARAMETER(PARAM, VALUE) \
    if ((rc = virTypedParamsGetULLong(params, nparams, PARAM, &VALUE)) < 0) \
        goto endjob; \
 \
    if (rc == 1) \
        set_ ## VALUE = true;

    VIR_GET_LIMIT_PARAMETER(VIR_DOMAIN_MEMORY_SWAP_HARD_LIMIT, swap_hard_limit)
    VIR_GET_LIMIT_PARAMETER(VIR_DOMAIN_MEMORY_HARD_LIMIT, hard_limit)
    VIR_GET_LIMIT_PARAMETER(VIR_DOMAIN_MEMORY_SOFT_LIMIT, soft_limit)

#undef VIR_GET_LIMIT_PARAMETER

    /* Swap hard limit must be greater than hard limit. */
    if (set_swap_hard_limit || set_hard_limit) {
        unsigned long long mem_limit = vm->def->mem.hard_limit;
        unsigned long long swap_limit = vm->def->mem.swap_hard_limit;

        if (set_swap_hard_limit)
            swap_limit = swap_hard_limit;

        if (set_hard_limit)
            mem_limit = hard_limit;

        if (mem_limit > swap_limit) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("memory hard_limit tunable value must be lower "
                             "than or equal to swap_hard_limit"));
            goto endjob;
        }
    }

#define VIR_SET_MEM_PARAMETER(FUNC, VALUE) \
    if (set_ ## VALUE) { \
        if (def) { \
            if ((rc = FUNC(priv->cgroup, VALUE)) < 0) \
                goto endjob; \
            def->mem.VALUE = VALUE; \
        } \
 \
        if (persistentDef) \
            persistentDef->mem.VALUE = VALUE; \
    }

    /* Soft limit doesn't clash with the others */
    VIR_SET_MEM_PARAMETER(virCgroupSetMemorySoftLimit, soft_limit);

    /* set hard limit before swap hard limit if decreasing it */
    if (def && def->mem.hard_limit > hard_limit) {
        VIR_SET_MEM_PARAMETER(virCgroupSetMemoryHardLimit, hard_limit);
        /* inhibit changing the limit a second time */
        set_hard_limit = false;
    }

    VIR_SET_MEM_PARAMETER(virCgroupSetMemSwapHardLimit, swap_hard_limit);

    /* otherwise increase it after swap hard limit */
    VIR_SET_MEM_PARAMETER(virCgroupSetMemoryHardLimit, hard_limit);

#undef VIR_SET_MEM_PARAMETER

    if (def &&
        virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto endjob;

    if (persistentDef &&
        virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
        goto endjob;
    /* QEMU and LXC implementations are identical */

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


#define QEMU_ASSIGN_MEM_PARAM(index, name, value) \
    if (index < *nparams && \
        virTypedParameterAssign(&params[index], name, VIR_TYPED_PARAM_ULLONG, \
                                value) < 0) \
        goto cleanup

static int
qemuDomainGetMemoryParameters(virDomainPtr dom,
                              virTypedParameterPtr params,
                              int *nparams,
                              unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr persistentDef = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    unsigned long long swap_hard_limit, mem_hard_limit, mem_soft_limit;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;

    if (virDomainGetMemoryParametersEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("Memory tuning is not available in session mode"));
        goto cleanup;
    }

    if (virDomainObjGetDefs(vm, flags, NULL, &persistentDef) < 0)
        goto cleanup;

    if ((*nparams) == 0) {
        /* Current number of memory parameters supported by cgroups */
        *nparams = QEMU_NB_MEM_PARAM;
        ret = 0;
        goto cleanup;
    }

    if (persistentDef) {
        mem_hard_limit = persistentDef->mem.hard_limit;
        mem_soft_limit = persistentDef->mem.soft_limit;
        swap_hard_limit = persistentDef->mem.swap_hard_limit;
    } else {
        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_MEMORY)) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           "%s", _("cgroup memory controller is not mounted"));
            goto cleanup;
        }

        if (virCgroupGetMemoryHardLimit(priv->cgroup, &mem_hard_limit) < 0)
            goto cleanup;

        if (virCgroupGetMemorySoftLimit(priv->cgroup, &mem_soft_limit) < 0)
            goto cleanup;

        if (virCgroupGetMemSwapHardLimit(priv->cgroup, &swap_hard_limit) < 0) {
            if (!virLastErrorIsSystemErrno(ENOENT) &&
                !virLastErrorIsSystemErrno(EOPNOTSUPP))
                goto cleanup;
            swap_hard_limit = VIR_DOMAIN_MEMORY_PARAM_UNLIMITED;
        }
    }

    QEMU_ASSIGN_MEM_PARAM(0, VIR_DOMAIN_MEMORY_HARD_LIMIT, mem_hard_limit);
    QEMU_ASSIGN_MEM_PARAM(1, VIR_DOMAIN_MEMORY_SOFT_LIMIT, mem_soft_limit);
    QEMU_ASSIGN_MEM_PARAM(2, VIR_DOMAIN_MEMORY_SWAP_HARD_LIMIT, swap_hard_limit);

    if (QEMU_NB_MEM_PARAM < *nparams)
        *nparams = QEMU_NB_MEM_PARAM;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}
#undef QEMU_ASSIGN_MEM_PARAM

static int
qemuDomainSetNumaParamsLive(virDomainObjPtr vm,
                            virBitmapPtr nodeset)
{
    virCgroupPtr cgroup_temp = NULL;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autofree char *nodeset_str = NULL;
    virDomainNumatuneMemMode mode;
    size_t i = 0;
    int ret = -1;

    if (virDomainNumatuneGetMode(vm->def->numa, -1, &mode) == 0 &&
        mode != VIR_DOMAIN_NUMATUNE_MEM_STRICT) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("change of nodeset for running domain "
                         "requires strict numa mode"));
        goto cleanup;
    }

    if (!virNumaNodesetIsAvailable(nodeset))
        goto cleanup;

    /* Ensure the cpuset string is formatted before passing to cgroup */
    if (!(nodeset_str = virBitmapFormat(nodeset)))
        goto cleanup;

    if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_EMULATOR, 0,
                           false, &cgroup_temp) < 0 ||
        virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
        goto cleanup;
    virCgroupFree(&cgroup_temp);

    for (i = 0; i < virDomainDefGetVcpusMax(vm->def); i++) {
        virDomainVcpuDefPtr vcpu = virDomainDefGetVcpu(vm->def, i);

        if (!vcpu->online)
            continue;

        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, i,
                               false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
            goto cleanup;
        virCgroupFree(&cgroup_temp);
    }

    for (i = 0; i < vm->def->niothreadids; i++) {
        if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                               vm->def->iothreadids[i]->iothread_id,
                               false, &cgroup_temp) < 0 ||
            virCgroupSetCpusetMems(cgroup_temp, nodeset_str) < 0)
            goto cleanup;
        virCgroupFree(&cgroup_temp);
    }

    ret = 0;
 cleanup:
    virCgroupFree(&cgroup_temp);

    return ret;
}

static int
qemuDomainSetNumaParameters(virDomainPtr dom,
                            virTypedParameterPtr params,
                            int nparams,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    size_t i;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuDomainObjPrivatePtr priv;
    virBitmapPtr nodeset = NULL;
    virDomainNumatuneMemMode config_mode;
    int mode = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_NUMA_MODE,
                               VIR_TYPED_PARAM_INT,
                               VIR_DOMAIN_NUMA_NODESET,
                               VIR_TYPED_PARAM_STRING,
                               NULL) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetNumaParametersEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_DOMAIN_NUMA_MODE)) {
            mode = param->value.i;

            if (mode < 0 || mode >= VIR_DOMAIN_NUMATUNE_MEM_LAST) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("unsupported numatune mode: '%d'"), mode);
                goto cleanup;
            }

        } else if (STREQ(param->field, VIR_DOMAIN_NUMA_NODESET)) {
            if (virBitmapParse(param->value.s, &nodeset,
                               VIR_DOMAIN_CPUMASK_LEN) < 0)
                goto cleanup;

            if (virBitmapIsAllClear(nodeset)) {
                virReportError(VIR_ERR_OPERATION_INVALID,
                               _("Invalid nodeset of 'numatune': %s"),
                               param->value.s);
                goto cleanup;
            }
        }
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        if (!virQEMUDriverIsPrivileged(driver)) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("NUMA tuning is not available in session mode"));
            goto endjob;
        }

        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cgroup cpuset controller is not mounted"));
            goto endjob;
        }

        if (mode != -1 &&
            virDomainNumatuneGetMode(def->numa, -1, &config_mode) == 0 &&
            config_mode != mode) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("can't change numatune mode for running domain"));
            goto endjob;
        }

        if (nodeset &&
            qemuDomainSetNumaParamsLive(vm, nodeset) < 0)
            goto endjob;

        if (virDomainNumatuneSet(def->numa,
                                 def->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 -1, mode, nodeset) < 0)
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        if (virDomainNumatuneSet(persistentDef->numa,
                                 persistentDef->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 -1, mode, nodeset) < 0)
            goto endjob;

        if (virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virBitmapFree(nodeset);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetNumaParameters(virDomainPtr dom,
                            virTypedParameterPtr params,
                            int *nparams,
                            unsigned int flags)
{
    size_t i;
    virDomainObjPtr vm = NULL;
    virDomainNumatuneMemMode tmpmode = VIR_DOMAIN_NUMATUNE_MEM_STRICT;
    qemuDomainObjPrivatePtr priv;
    g_autofree char *nodeset = NULL;
    int ret = -1;
    virDomainDefPtr def = NULL;
    bool live = false;
    virBitmapPtr autoNodeset = NULL;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;
    priv = vm->privateData;

    if (virDomainGetNumaParametersEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDefState(vm, flags, &live)))
        goto cleanup;

    if (live)
        autoNodeset = priv->autoNodeset;

    if ((*nparams) == 0) {
        *nparams = QEMU_NB_NUMA_PARAM;
        ret = 0;
        goto cleanup;
    }

    for (i = 0; i < QEMU_NB_NUMA_PARAM && i < *nparams; i++) {
        virMemoryParameterPtr param = &params[i];

        switch (i) {
        case 0: /* fill numa mode here */
            ignore_value(virDomainNumatuneGetMode(def->numa, -1, &tmpmode));

            if (virTypedParameterAssign(param, VIR_DOMAIN_NUMA_MODE,
                                        VIR_TYPED_PARAM_INT, tmpmode) < 0)
                goto cleanup;

            break;

        case 1: /* fill numa nodeset here */
            nodeset = virDomainNumatuneFormatNodeset(def->numa, autoNodeset, -1);
            if (!nodeset ||
                virTypedParameterAssign(param, VIR_DOMAIN_NUMA_NODESET,
                                        VIR_TYPED_PARAM_STRING, nodeset) < 0)
                goto cleanup;

            nodeset = NULL;
            break;

        /* coverity[dead_error_begin] */
        default:
            break;
            /* should not hit here */
        }
    }

    if (*nparams > QEMU_NB_NUMA_PARAM)
        *nparams = QEMU_NB_NUMA_PARAM;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuSetGlobalBWLive(virCgroupPtr cgroup, unsigned long long period,
                    long long quota)
{
    if (period == 0 && quota == 0)
        return 0;

    if (qemuSetupCgroupVcpuBW(cgroup, period, quota) < 0)
        return -1;

    return 0;
}

static int
qemuDomainSetPerfEvents(virDomainPtr dom,
                        virTypedParameterPtr params,
                        int nparams,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    size_t i;
    virDomainObjPtr vm = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuDomainObjPrivatePtr priv;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1;
    virPerfEventType type;
    bool enabled;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (virTypedParamsValidate(params, nparams,
                               VIR_PERF_PARAM_CMT, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_MBMT, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_MBML, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_CPU_CYCLES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_INSTRUCTIONS, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_CACHE_REFERENCES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_CACHE_MISSES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_BRANCH_INSTRUCTIONS, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_BRANCH_MISSES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_BUS_CYCLES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_STALLED_CYCLES_FRONTEND, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_STALLED_CYCLES_BACKEND, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_REF_CPU_CYCLES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_CPU_CLOCK, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_TASK_CLOCK, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_PAGE_FAULTS, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_CONTEXT_SWITCHES, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_CPU_MIGRATIONS, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_PAGE_FAULTS_MIN, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_PAGE_FAULTS_MAJ, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_ALIGNMENT_FAULTS, VIR_TYPED_PARAM_BOOLEAN,
                               VIR_PERF_PARAM_EMULATION_FAULTS, VIR_TYPED_PARAM_BOOLEAN,
                               NULL) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);
    priv = vm->privateData;

    if (virDomainSetPerfEventsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        for (i = 0; i < nparams; i++) {
            virTypedParameterPtr param = &params[i];
            enabled = param->value.b;
            type = virPerfEventTypeFromString(param->field);

            if (!enabled && virPerfEventDisable(priv->perf, type) < 0)
                goto endjob;
            if (enabled && virPerfEventEnable(priv->perf, type, vm->pid) < 0)
                goto endjob;

            def->perf.events[type] = enabled ?
                VIR_TRISTATE_BOOL_YES : VIR_TRISTATE_BOOL_NO;
        }

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        for (i = 0; i < nparams; i++) {
            virTypedParameterPtr param = &params[i];
            enabled = param->value.b;
            type = virPerfEventTypeFromString(param->field);

            persistentDef->perf.events[type] = enabled ?
                VIR_TRISTATE_BOOL_YES : VIR_TRISTATE_BOOL_NO;
        }

        if (virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetPerfEvents(virDomainPtr dom,
                        virTypedParameterPtr *params,
                        int *nparams,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuDomainObjPrivatePtr priv;
    virDomainDefPtr def;
    virTypedParameterPtr par = NULL;
    int maxpar = 0;
    int npar = 0;
    size_t i;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetPerfEventsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDef(vm, flags)))
        goto endjob;

    priv = vm->privateData;

    for (i = 0; i < VIR_PERF_EVENT_LAST; i++) {
        bool perf_enabled;

        if (flags & VIR_DOMAIN_AFFECT_CONFIG)
            perf_enabled = def->perf.events[i] == VIR_TRISTATE_BOOL_YES;
        else
            perf_enabled = virPerfEventIsEnabled(priv->perf, i);

        if (virTypedParamsAddBoolean(&par, &npar, &maxpar,
                                     virPerfEventTypeToString(i),
                                     perf_enabled) < 0)
            goto endjob;
    }

    *params = par;
    *nparams = npar;
    par = NULL;
    npar = 0;
    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virTypedParamsFree(par, npar);
    return ret;
}

static int
qemuSetVcpusBWLive(virDomainObjPtr vm, virCgroupPtr cgroup,
                   unsigned long long period, long long quota)
{
    size_t i;
    virCgroupPtr cgroup_vcpu = NULL;

    if (period == 0 && quota == 0)
        return 0;

    if (!qemuDomainHasVcpuPids(vm))
        return 0;

    for (i = 0; i < virDomainDefGetVcpusMax(vm->def); i++) {
        virDomainVcpuDefPtr vcpu = virDomainDefGetVcpu(vm->def, i);

        if (!vcpu->online)
            continue;

        if (virCgroupNewThread(cgroup, VIR_CGROUP_THREAD_VCPU, i,
                               false, &cgroup_vcpu) < 0)
            goto cleanup;

        if (qemuSetupCgroupVcpuBW(cgroup_vcpu, period, quota) < 0)
            goto cleanup;

        virCgroupFree(&cgroup_vcpu);
    }

    return 0;

 cleanup:
    virCgroupFree(&cgroup_vcpu);
    return -1;
}

static int
qemuSetEmulatorBandwidthLive(virCgroupPtr cgroup,
                             unsigned long long period,
                             long long quota)
{
    virCgroupPtr cgroup_emulator = NULL;

    if (period == 0 && quota == 0)
        return 0;

    if (virCgroupNewThread(cgroup, VIR_CGROUP_THREAD_EMULATOR, 0,
                           false, &cgroup_emulator) < 0)
        goto cleanup;

    if (qemuSetupCgroupVcpuBW(cgroup_emulator, period, quota) < 0)
        goto cleanup;

    virCgroupFree(&cgroup_emulator);
    return 0;

 cleanup:
    virCgroupFree(&cgroup_emulator);
    return -1;
}


static int
qemuSetIOThreadsBWLive(virDomainObjPtr vm, virCgroupPtr cgroup,
                       unsigned long long period, long long quota)
{
    size_t i;
    virCgroupPtr cgroup_iothread = NULL;

    if (period == 0 && quota == 0)
        return 0;

    if (!vm->def->niothreadids)
        return 0;

    for (i = 0; i < vm->def->niothreadids; i++) {
        if (virCgroupNewThread(cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                               vm->def->iothreadids[i]->iothread_id,
                               false, &cgroup_iothread) < 0)
            goto cleanup;

        if (qemuSetupCgroupVcpuBW(cgroup_iothread, period, quota) < 0)
            goto cleanup;

        virCgroupFree(&cgroup_iothread);
    }

    return 0;

 cleanup:
    virCgroupFree(&cgroup_iothread);
    return -1;
}


#define SCHED_RANGE_CHECK(VAR, NAME, MIN, MAX) \
    if (((VAR) > 0 && (VAR) < (MIN)) || (VAR) > (MAX)) { \
        virReportError(VIR_ERR_INVALID_ARG, \
                       _("value of '%s' is out of range [%lld, %lld]"), \
                       NAME, MIN, MAX); \
        rc = -1; \
        goto endjob; \
    }

static int
qemuDomainSetSchedulerParametersFlags(virDomainPtr dom,
                                      virTypedParameterPtr params,
                                      int nparams,
                                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    size_t i;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    virDomainDefPtr persistentDefCopy = NULL;
    unsigned long long value_ul;
    long long value_l;
    int ret = -1;
    int rc;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuDomainObjPrivatePtr priv;
    virObjectEventPtr event = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxNparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);
    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_SCHEDULER_CPU_SHARES,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_SCHEDULER_VCPU_PERIOD,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_SCHEDULER_VCPU_QUOTA,
                               VIR_TYPED_PARAM_LLONG,
                               VIR_DOMAIN_SCHEDULER_GLOBAL_PERIOD,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_SCHEDULER_GLOBAL_QUOTA,
                               VIR_TYPED_PARAM_LLONG,
                               VIR_DOMAIN_SCHEDULER_EMULATOR_PERIOD,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_SCHEDULER_EMULATOR_QUOTA,
                               VIR_TYPED_PARAM_LLONG,
                               VIR_DOMAIN_SCHEDULER_IOTHREAD_PERIOD,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_SCHEDULER_IOTHREAD_QUOTA,
                               VIR_TYPED_PARAM_LLONG,
                               NULL) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetSchedulerParametersFlagsEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("CPU tuning is not available in session mode"));
        goto cleanup;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (persistentDef) {
        /* Make a copy for updated domain. */
        if (!(persistentDefCopy = virDomainObjCopyPersistentDef(vm,
                                                                driver->xmlopt,
                                                                priv->qemuCaps)))
            goto endjob;
    }

    if (def &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cgroup CPU controller is not mounted"));
        goto endjob;
    }

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];
        value_ul = param->value.ul;
        value_l = param->value.l;

        if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_CPU_SHARES)) {
            if (def) {
                unsigned long long val;
                if (virCgroupSetCpuShares(priv->cgroup, value_ul) < 0)
                    goto endjob;

                if (virCgroupGetCpuShares(priv->cgroup, &val) < 0)
                    goto endjob;

                def->cputune.shares = val;
                def->cputune.sharesSpecified = true;

                if (virTypedParamsAddULLong(&eventParams, &eventNparams,
                                            &eventMaxNparams,
                                            VIR_DOMAIN_TUNABLE_CPU_CPU_SHARES,
                                            val) < 0)
                    goto endjob;
            }

            if (persistentDef) {
                persistentDefCopy->cputune.shares = value_ul;
                persistentDefCopy->cputune.sharesSpecified = true;
            }


        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_VCPU_PERIOD)) {
            SCHED_RANGE_CHECK(value_ul, VIR_DOMAIN_SCHEDULER_VCPU_PERIOD,
                              QEMU_SCHED_MIN_PERIOD, QEMU_SCHED_MAX_PERIOD);

            if (def && value_ul) {
                if ((rc = qemuSetVcpusBWLive(vm, priv->cgroup, value_ul, 0)))
                    goto endjob;

                def->cputune.period = value_ul;

                if (virTypedParamsAddULLong(&eventParams, &eventNparams,
                                            &eventMaxNparams,
                                            VIR_DOMAIN_TUNABLE_CPU_VCPU_PERIOD,
                                            value_ul) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.period = value_ul;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_VCPU_QUOTA)) {
            SCHED_RANGE_CHECK(value_l, VIR_DOMAIN_SCHEDULER_VCPU_QUOTA,
                              QEMU_SCHED_MIN_QUOTA, QEMU_SCHED_MAX_QUOTA);

            if (def && value_l) {
                if ((rc = qemuSetVcpusBWLive(vm, priv->cgroup, 0, value_l)))
                    goto endjob;

                def->cputune.quota = value_l;

                if (virTypedParamsAddLLong(&eventParams, &eventNparams,
                                           &eventMaxNparams,
                                           VIR_DOMAIN_TUNABLE_CPU_VCPU_QUOTA,
                                           value_l) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.quota = value_l;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_GLOBAL_PERIOD)) {
            SCHED_RANGE_CHECK(value_ul, VIR_DOMAIN_SCHEDULER_GLOBAL_PERIOD,
                              QEMU_SCHED_MIN_PERIOD, QEMU_SCHED_MAX_PERIOD);

            if (def && value_ul) {
                if ((rc = qemuSetGlobalBWLive(priv->cgroup, value_ul, 0)))
                    goto endjob;

                def->cputune.global_period = value_ul;

                if (virTypedParamsAddULLong(&eventParams, &eventNparams,
                                            &eventMaxNparams,
                                            VIR_DOMAIN_TUNABLE_CPU_GLOBAL_PERIOD,
                                            value_ul) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.global_period = value_ul;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_GLOBAL_QUOTA)) {
            SCHED_RANGE_CHECK(value_l, VIR_DOMAIN_SCHEDULER_GLOBAL_QUOTA,
                              QEMU_SCHED_MIN_QUOTA, QEMU_SCHED_MAX_QUOTA);

            if (def && value_l) {
                if ((rc = qemuSetGlobalBWLive(priv->cgroup, 0, value_l)))
                    goto endjob;

                def->cputune.global_quota = value_l;

                if (virTypedParamsAddLLong(&eventParams, &eventNparams,
                                           &eventMaxNparams,
                                           VIR_DOMAIN_TUNABLE_CPU_GLOBAL_QUOTA,
                                           value_l) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.global_quota = value_l;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_EMULATOR_PERIOD)) {
            SCHED_RANGE_CHECK(value_ul, VIR_DOMAIN_SCHEDULER_EMULATOR_PERIOD,
                              QEMU_SCHED_MIN_PERIOD, QEMU_SCHED_MAX_PERIOD);

            if (def && value_ul) {
                if ((rc = qemuSetEmulatorBandwidthLive(priv->cgroup,
                                                       value_ul, 0)))
                    goto endjob;

                def->cputune.emulator_period = value_ul;

                if (virTypedParamsAddULLong(&eventParams, &eventNparams,
                                            &eventMaxNparams,
                                            VIR_DOMAIN_TUNABLE_CPU_EMULATOR_PERIOD,
                                            value_ul) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.emulator_period = value_ul;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_EMULATOR_QUOTA)) {
            SCHED_RANGE_CHECK(value_l, VIR_DOMAIN_SCHEDULER_EMULATOR_QUOTA,
                              QEMU_SCHED_MIN_QUOTA, QEMU_SCHED_MAX_QUOTA);

            if (def && value_l) {
                if ((rc = qemuSetEmulatorBandwidthLive(priv->cgroup,
                                                       0, value_l)))
                    goto endjob;

                def->cputune.emulator_quota = value_l;

                if (virTypedParamsAddLLong(&eventParams, &eventNparams,
                                           &eventMaxNparams,
                                           VIR_DOMAIN_TUNABLE_CPU_EMULATOR_QUOTA,
                                           value_l) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.emulator_quota = value_l;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_IOTHREAD_PERIOD)) {
            SCHED_RANGE_CHECK(value_ul, VIR_DOMAIN_SCHEDULER_IOTHREAD_PERIOD,
                              QEMU_SCHED_MIN_PERIOD, QEMU_SCHED_MAX_PERIOD);

            if (def && value_ul) {
                if ((rc = qemuSetIOThreadsBWLive(vm, priv->cgroup, value_ul, 0)))
                    goto endjob;

                def->cputune.iothread_period = value_ul;

                if (virTypedParamsAddULLong(&eventParams, &eventNparams,
                                            &eventMaxNparams,
                                            VIR_DOMAIN_TUNABLE_CPU_IOTHREAD_PERIOD,
                                            value_ul) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.iothread_period = value_ul;

        } else if (STREQ(param->field, VIR_DOMAIN_SCHEDULER_IOTHREAD_QUOTA)) {
            SCHED_RANGE_CHECK(value_l, VIR_DOMAIN_SCHEDULER_IOTHREAD_QUOTA,
                              QEMU_SCHED_MIN_QUOTA, QEMU_SCHED_MAX_QUOTA);

            if (def && value_l) {
                if ((rc = qemuSetIOThreadsBWLive(vm, priv->cgroup, 0, value_l)))
                    goto endjob;

                def->cputune.iothread_quota = value_l;

                if (virTypedParamsAddLLong(&eventParams, &eventNparams,
                                           &eventMaxNparams,
                                           VIR_DOMAIN_TUNABLE_CPU_IOTHREAD_QUOTA,
                                           value_l) < 0)
                    goto endjob;
            }

            if (persistentDef)
                persistentDefCopy->cputune.iothread_quota = value_l;
        }
    }

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto endjob;

    if (eventNparams) {
        event = virDomainEventTunableNewFromDom(dom, eventParams, eventNparams);
        eventNparams = 0;
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    if (persistentDef) {
        rc = virDomainDefSave(persistentDefCopy, driver->xmlopt,
                              cfg->configDir);
        if (rc < 0)
            goto endjob;

        virDomainObjAssignDef(vm, persistentDefCopy, false, NULL);
        persistentDefCopy = NULL;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainDefFree(persistentDefCopy);
    virDomainObjEndAPI(&vm);
    if (eventNparams)
        virTypedParamsFree(eventParams, eventNparams);
    return ret;
}
#undef SCHED_RANGE_CHECK

static int
qemuDomainSetSchedulerParameters(virDomainPtr dom,
                                 virTypedParameterPtr params,
                                 int nparams)
{
    return qemuDomainSetSchedulerParametersFlags(dom,
                                                 params,
                                                 nparams,
                                                 VIR_DOMAIN_AFFECT_CURRENT);
}

static int
qemuGetVcpuBWLive(virCgroupPtr cgroup, unsigned long long *period,
                  long long *quota)
{
    if (virCgroupGetCpuCfsPeriod(cgroup, period) < 0)
        return -1;

    if (virCgroupGetCpuCfsQuota(cgroup, quota) < 0)
        return -1;

    return 0;
}

static int
qemuGetVcpusBWLive(virDomainObjPtr vm,
                   unsigned long long *period, long long *quota)
{
    virCgroupPtr cgroup_vcpu = NULL;
    qemuDomainObjPrivatePtr priv = NULL;
    int rc;
    int ret = -1;

    priv = vm->privateData;
    if (!qemuDomainHasVcpuPids(vm)) {
        /* We do not create sub dir for each vcpu */
        rc = qemuGetVcpuBWLive(priv->cgroup, period, quota);
        if (rc < 0)
            goto cleanup;

        if (*quota > 0)
            *quota /= virDomainDefGetVcpus(vm->def);
        goto out;
    }

    /* get period and quota for vcpu0 */
    if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_VCPU, 0,
                           false, &cgroup_vcpu) < 0)
        goto cleanup;

    rc = qemuGetVcpuBWLive(cgroup_vcpu, period, quota);
    if (rc < 0)
        goto cleanup;

 out:
    ret = 0;

 cleanup:
    virCgroupFree(&cgroup_vcpu);
    return ret;
}

static int
qemuGetEmulatorBandwidthLive(virCgroupPtr cgroup,
                             unsigned long long *period,
                             long long *quota)
{
    virCgroupPtr cgroup_emulator = NULL;
    int ret = -1;

    /* get period and quota for emulator */
    if (virCgroupNewThread(cgroup, VIR_CGROUP_THREAD_EMULATOR, 0,
                           false, &cgroup_emulator) < 0)
        goto cleanup;

    if (qemuGetVcpuBWLive(cgroup_emulator, period, quota) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virCgroupFree(&cgroup_emulator);
    return ret;
}

static int
qemuGetIOThreadsBWLive(virDomainObjPtr vm,
                       unsigned long long *period, long long *quota)
{
    virCgroupPtr cgroup_iothread = NULL;
    qemuDomainObjPrivatePtr priv = NULL;
    int rc;
    int ret = -1;

    priv = vm->privateData;
    if (!vm->def->niothreadids) {
        /* We do not create sub dir for each iothread */
        if ((rc = qemuGetVcpuBWLive(priv->cgroup, period, quota)) < 0)
            goto cleanup;

        goto out;
    }

    /* get period and quota for the "first" IOThread */
    if (virCgroupNewThread(priv->cgroup, VIR_CGROUP_THREAD_IOTHREAD,
                           vm->def->iothreadids[0]->iothread_id,
                           false, &cgroup_iothread) < 0)
        goto cleanup;

    rc = qemuGetVcpuBWLive(cgroup_iothread, period, quota);
    if (rc < 0)
        goto cleanup;

 out:
    ret = 0;

 cleanup:
    virCgroupFree(&cgroup_iothread);
    return ret;
}


static int
qemuGetGlobalBWLive(virCgroupPtr cgroup, unsigned long long *period,
                    long long *quota)
{
    if (qemuGetVcpuBWLive(cgroup, period, quota) < 0)
        return -1;

    return 0;
}

static int
qemuDomainGetSchedulerParametersFlags(virDomainPtr dom,
                                      virTypedParameterPtr params,
                                      int *nparams,
                                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainCputune data = {0};
    int ret = -1;
    bool cpu_bw_status = true;
    virDomainDefPtr persistentDef;
    virDomainDefPtr def;
    qemuDomainObjPrivatePtr priv;
    int maxparams = *nparams;

    *nparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainGetSchedulerParametersFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!virQEMUDriverIsPrivileged(driver)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("CPU tuning is not available in session mode"));
        goto cleanup;
    }

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto cleanup;

    if (persistentDef) {
        data = persistentDef->cputune;
    } else if (def) {
        if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           "%s", _("cgroup CPU controller is not mounted"));
            goto cleanup;
        }

        if (virCgroupGetCpuShares(priv->cgroup, &data.shares) < 0)
            goto cleanup;

        if (virCgroupSupportsCpuBW(priv->cgroup)) {
            if (maxparams > 1 &&
                qemuGetVcpusBWLive(vm, &data.period, &data.quota) < 0)
                goto cleanup;

            if (maxparams > 3 &&
                qemuGetEmulatorBandwidthLive(priv->cgroup, &data.emulator_period,
                                             &data.emulator_quota) < 0)
                goto cleanup;

            if (maxparams > 5 &&
                qemuGetGlobalBWLive(priv->cgroup, &data.global_period,
                                    &data.global_quota) < 0)
                goto cleanup;

            if (maxparams > 7 &&
                qemuGetIOThreadsBWLive(vm, &data.iothread_period,
                                       &data.iothread_quota) < 0)
                goto cleanup;
        } else {
            cpu_bw_status = false;
        }
    }

#define QEMU_SCHED_ASSIGN(param, name, type) \
    if (*nparams < maxparams && \
        virTypedParameterAssign(&(params[(*nparams)++]), \
                                VIR_DOMAIN_SCHEDULER_ ## name, \
                                VIR_TYPED_PARAM_ ## type, \
                                data.param) < 0) \
            goto cleanup

    QEMU_SCHED_ASSIGN(shares, CPU_SHARES, ULLONG);

    if (cpu_bw_status) {
        QEMU_SCHED_ASSIGN(period, VCPU_PERIOD, ULLONG);
        QEMU_SCHED_ASSIGN(quota, VCPU_QUOTA, LLONG);

        QEMU_SCHED_ASSIGN(emulator_period, EMULATOR_PERIOD, ULLONG);
        QEMU_SCHED_ASSIGN(emulator_quota, EMULATOR_QUOTA, LLONG);

        QEMU_SCHED_ASSIGN(global_period, GLOBAL_PERIOD, ULLONG);
        QEMU_SCHED_ASSIGN(global_quota, GLOBAL_QUOTA, LLONG);

        QEMU_SCHED_ASSIGN(iothread_period, IOTHREAD_PERIOD, ULLONG);
        QEMU_SCHED_ASSIGN(iothread_quota, IOTHREAD_QUOTA, LLONG);
    }

#undef QEMU_SCHED_ASSIGN

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetSchedulerParameters(virDomainPtr dom,
                                 virTypedParameterPtr params,
                                 int *nparams)
{
    return qemuDomainGetSchedulerParametersFlags(dom, params, nparams,
                                                 VIR_DOMAIN_AFFECT_CURRENT);
}

/**
 * Resize a block device while a guest is running. Resize to a lower size
 * is supported, but should be used with extreme caution.  Note that it
 * only supports to resize image files, it can't resize block devices
 * like LVM volumes.
 */
static int
qemuDomainBlockResize(virDomainPtr dom,
                      const char *path,
                      unsigned long long size,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    int ret = -1;
    g_autofree char *device = NULL;
    const char *nodename = NULL;
    virDomainDiskDefPtr disk = NULL;

    virCheckFlags(VIR_DOMAIN_BLOCK_RESIZE_BYTES, -1);

    /* We prefer operating on bytes.  */
    if ((flags & VIR_DOMAIN_BLOCK_RESIZE_BYTES) == 0) {
        if (size > ULLONG_MAX / 1024) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("size must be less than %llu"),
                           ULLONG_MAX / 1024);
            return -1;
        }
        size *= 1024;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainBlockResizeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = virDomainDiskByName(vm->def, path, false))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("disk '%s' was not found in the domain config"), path);
        goto endjob;
    }

    /* qcow2 and qed must be sized on 512 byte blocks/sectors,
     * so adjust size if necessary to round up.
     */
    if (disk->src->format == VIR_STORAGE_FILE_QCOW2 ||
        disk->src->format == VIR_STORAGE_FILE_QED)
        size = VIR_ROUND_UP(size, 512);

    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV)) {
        if (virStorageSourceIsEmpty(disk->src) || disk->src->readonly) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                           _("can't resize empty or readonly disk '%s'"),
                           disk->dst);
            goto endjob;
        }

        nodename = disk->src->nodeformat;
    } else {
        if (!(device = qemuAliasDiskDriveFromDisk(disk)))
            goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    if (qemuMonitorBlockResize(priv->mon, device, nodename, size) < 0) {
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
        goto endjob;
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static void
qemuDomainBlockStatsGatherTotals(qemuBlockStatsPtr data,
                                 qemuBlockStatsPtr total)
{
    total->wr_bytes += data->wr_bytes;
    total->wr_req += data->wr_req;
    total->rd_bytes += data->rd_bytes;
    total->rd_req += data->rd_req;
    total->flush_req += data->flush_req;
    total->wr_total_times += data->wr_total_times;
    total->rd_total_times += data->rd_total_times;
    total->flush_total_times += data->flush_total_times;
}


/**
 * qemuDomainBlocksStatsGather:
 * @driver: driver object
 * @vm: domain object
 * @path: to gather the statistics for
 * @capacity: refresh capacity of the backing image
 * @retstats: returns pointer to structure holding the stats
 *
 * Gathers the block statistics for use in qemuDomainBlockStats* APIs.
 *
 * Returns -1 on error; number of filled block statistics on success.
 */
static int
qemuDomainBlocksStatsGather(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            const char *path,
                            bool capacity,
                            qemuBlockStatsPtr *retstats)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    virDomainDiskDefPtr disk = NULL;
    virHashTablePtr blockstats = NULL;
    qemuBlockStatsPtr stats;
    size_t i;
    int nstats;
    int rc = 0;
    const char *entryname = NULL;
    int ret = -1;

    if (*path) {
        if (!(disk = virDomainDiskByName(vm->def, path, false))) {
            virReportError(VIR_ERR_INVALID_ARG, _("invalid path: %s"), path);
            goto cleanup;
        }

        if (blockdev) {
            entryname = QEMU_DOMAIN_DISK_PRIVATE(disk)->qomName;
        } else {
            if (!disk->info.alias) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("missing disk device alias name for %s"), disk->dst);
                goto cleanup;
            }

            entryname = disk->info.alias;
        }
    }

    qemuDomainObjEnterMonitor(driver, vm);
    nstats = qemuMonitorGetAllBlockStatsInfo(priv->mon, &blockstats, false);

    if (capacity && nstats >= 0) {
        if (blockdev)
            rc = qemuMonitorBlockStatsUpdateCapacityBlockdev(priv->mon, blockstats);
        else
            rc = qemuMonitorBlockStatsUpdateCapacity(priv->mon, blockstats, false);
    }

    if (qemuDomainObjExitMonitor(driver, vm) < 0 || nstats < 0 || rc < 0)
        goto cleanup;

    if (VIR_ALLOC(*retstats) < 0)
        goto cleanup;

    if (entryname) {
        if (!(stats = virHashLookup(blockstats, entryname))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot find statistics for device '%s'"), entryname);
            goto cleanup;
        }

        **retstats = *stats;

        if (blockdev) {
            /* capacity are reported only per node-name so we need to transfer them */
            qemuBlockStatsPtr capstats;

            if (disk && disk->src &&
                (capstats = virHashLookup(blockstats, disk->src->nodeformat))) {
                (*retstats)->capacity = capstats->capacity;
                (*retstats)->physical = capstats->physical;
                (*retstats)->wr_highest_offset = capstats->wr_highest_offset;
                (*retstats)->wr_highest_offset_valid = capstats->wr_highest_offset_valid;
                (*retstats)->write_threshold = capstats->write_threshold;
            }
        }
    } else {
        for (i = 0; i < vm->def->ndisks; i++) {
            disk = vm->def->disks[i];
            entryname = disk->info.alias;

            if (blockdev)
                entryname = QEMU_DOMAIN_DISK_PRIVATE(disk)->qomName;

            if (!entryname)
                continue;

            if (!(stats = virHashLookup(blockstats, entryname))) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("cannot find statistics for device '%s'"), entryname);
                goto cleanup;
            }

            qemuDomainBlockStatsGatherTotals(stats, *retstats);
        }
    }

    ret = nstats;

 cleanup:
    virHashFree(blockstats);
    return ret;
}


static int
qemuDomainBlockStats(virDomainPtr dom,
                     const char *path,
                     virDomainBlockStatsPtr stats)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuBlockStatsPtr blockstats = NULL;
    int ret = -1;
    virDomainObjPtr vm;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainBlockStatsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (qemuDomainBlocksStatsGather(driver, vm, path, false, &blockstats) < 0)
        goto endjob;

    if (VIR_ASSIGN_IS_OVERFLOW(stats->rd_req, blockstats->rd_req) ||
        VIR_ASSIGN_IS_OVERFLOW(stats->rd_bytes, blockstats->rd_bytes) ||
        VIR_ASSIGN_IS_OVERFLOW(stats->wr_req, blockstats->wr_req) ||
        VIR_ASSIGN_IS_OVERFLOW(stats->wr_bytes, blockstats->wr_bytes)) {
        virReportError(VIR_ERR_OVERFLOW, "%s", _("statistic value too large"));
        goto endjob;
    }

    /* qemu doesn't report the error count */
    stats->errs = -1;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    VIR_FREE(blockstats);
    return ret;
}


static int
qemuDomainBlockStatsFlags(virDomainPtr dom,
                          const char *path,
                          virTypedParameterPtr params,
                          int *nparams,
                          unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuBlockStatsPtr blockstats = NULL;
    int nstats;
    int ret = -1;

    VIR_DEBUG("params=%p, flags=0x%x", params, flags);

    virCheckFlags(VIR_TYPED_PARAM_STRING_OKAY, -1);

    /* We don't return strings, and thus trivially support this flag.  */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainBlockStatsFlagsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if ((nstats = qemuDomainBlocksStatsGather(driver, vm, path, false,
                                              &blockstats)) < 0)
        goto endjob;

    /* return count of supported stats */
    if (*nparams == 0) {
        *nparams = nstats;
        ret = 0;
        goto endjob;
    }

    nstats = 0;

#define QEMU_BLOCK_STATS_ASSIGN_PARAM(VAR, NAME) \
    if (nstats < *nparams) { \
        long long tmp; \
        if (VIR_ASSIGN_IS_OVERFLOW(tmp, (blockstats->VAR))) { \
            virReportError(VIR_ERR_OVERFLOW, \
                           _("value of '%s' is too large"), NAME); \
            goto endjob; \
        } \
        if (virTypedParameterAssign(params + nstats, NAME, \
                                    VIR_TYPED_PARAM_LLONG, tmp) < 0) \
            goto endjob; \
        nstats++; \
    }

    QEMU_BLOCK_STATS_ASSIGN_PARAM(wr_bytes, VIR_DOMAIN_BLOCK_STATS_WRITE_BYTES);
    QEMU_BLOCK_STATS_ASSIGN_PARAM(wr_req, VIR_DOMAIN_BLOCK_STATS_WRITE_REQ);

    QEMU_BLOCK_STATS_ASSIGN_PARAM(rd_bytes, VIR_DOMAIN_BLOCK_STATS_READ_BYTES);
    QEMU_BLOCK_STATS_ASSIGN_PARAM(rd_req, VIR_DOMAIN_BLOCK_STATS_READ_REQ);

    QEMU_BLOCK_STATS_ASSIGN_PARAM(flush_req, VIR_DOMAIN_BLOCK_STATS_FLUSH_REQ);

    QEMU_BLOCK_STATS_ASSIGN_PARAM(wr_total_times,
                                  VIR_DOMAIN_BLOCK_STATS_WRITE_TOTAL_TIMES);
    QEMU_BLOCK_STATS_ASSIGN_PARAM(rd_total_times,
                                  VIR_DOMAIN_BLOCK_STATS_READ_TOTAL_TIMES);
    QEMU_BLOCK_STATS_ASSIGN_PARAM(flush_total_times,
                                  VIR_DOMAIN_BLOCK_STATS_FLUSH_TOTAL_TIMES);
#undef QEMU_BLOCK_STATS_ASSIGN_PARAM

    ret = 0;
    *nparams = nstats;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    VIR_FREE(blockstats);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainInterfaceStats(virDomainPtr dom,
                         const char *device,
                         virDomainInterfaceStatsPtr stats)
{
    virDomainObjPtr vm;
    virDomainNetDefPtr net = NULL;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainInterfaceStatsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    if (!(net = virDomainNetFind(vm->def, device)))
        goto cleanup;

    if (virDomainNetGetActualType(net) == VIR_DOMAIN_NET_TYPE_VHOSTUSER) {
        if (virNetDevOpenvswitchInterfaceStats(net->ifname, stats) < 0)
            goto cleanup;
    } else {
        if (virNetDevTapInterfaceStats(net->ifname, stats,
                                       !virDomainNetTypeSharesHostView(net)) < 0)
            goto cleanup;
    }

    ret = 0;
 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainSetInterfaceParameters(virDomainPtr dom,
                                 const char *device,
                                 virTypedParameterPtr params,
                                 int nparams,
                                 unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    size_t i;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    virDomainDefPtr persistentDef;
    int ret = -1;
    virDomainNetDefPtr net = NULL, persistentNet = NULL;
    virNetDevBandwidthPtr bandwidth = NULL, newBandwidth = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    bool inboundSpecified = false, outboundSpecified = false;
    int actualType;
    bool qosSupported = true;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);
    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_BANDWIDTH_IN_AVERAGE,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BANDWIDTH_IN_PEAK,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BANDWIDTH_IN_BURST,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BANDWIDTH_IN_FLOOR,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BANDWIDTH_OUT_AVERAGE,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BANDWIDTH_OUT_PEAK,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BANDWIDTH_OUT_BURST,
                               VIR_TYPED_PARAM_UINT,
                               NULL) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetInterfaceParametersEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def &&
        !(net = virDomainNetFind(vm->def, device)))
        goto endjob;

    if (persistentDef &&
        !(persistentNet = virDomainNetFind(persistentDef, device)))
        goto endjob;

    if (net) {
        actualType = virDomainNetGetActualType(net);
        qosSupported = virNetDevSupportBandwidth(actualType);
    }

    if (qosSupported && persistentNet) {
        actualType = virDomainNetGetActualType(persistentNet);
        qosSupported = virNetDevSupportBandwidth(actualType);
    }

    if (!qosSupported) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("setting bandwidth on interfaces of "
                         "type '%s' is not implemented yet"),
                       virDomainNetTypeToString(actualType));
        goto endjob;
    }

    if ((VIR_ALLOC(bandwidth) < 0) ||
        (VIR_ALLOC(bandwidth->in) < 0) ||
        (VIR_ALLOC(bandwidth->out) < 0))
        goto endjob;

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_IN_AVERAGE)) {
            bandwidth->in->average = param->value.ui;
            inboundSpecified = true;
        } else if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_IN_PEAK)) {
            bandwidth->in->peak = param->value.ui;
        } else if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_IN_BURST)) {
            bandwidth->in->burst = param->value.ui;
        } else if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_IN_FLOOR)) {
            bandwidth->in->floor = param->value.ui;
            inboundSpecified = true;
        } else if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_OUT_AVERAGE)) {
            bandwidth->out->average = param->value.ui;
            outboundSpecified = true;
        } else if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_OUT_PEAK)) {
            bandwidth->out->peak = param->value.ui;
        } else if (STREQ(param->field, VIR_DOMAIN_BANDWIDTH_OUT_BURST)) {
            bandwidth->out->burst = param->value.ui;
        }
    }

    /* average or floor are mandatory, peak and burst are optional.
     * So if no average or floor is given, we free inbound/outbound
     * here which causes inbound/outbound to not be set. */
    if (!bandwidth->in->average && !bandwidth->in->floor)
        VIR_FREE(bandwidth->in);
    if (!bandwidth->out->average)
        VIR_FREE(bandwidth->out);

    if (net) {
        if (VIR_ALLOC(newBandwidth) < 0)
            goto endjob;

        /* virNetDevBandwidthSet() will clear any previous value of
         * bandwidth parameters, so merge with old bandwidth parameters
         * here to prevent them from being lost. */
        if (bandwidth->in ||
            (!inboundSpecified && net->bandwidth && net->bandwidth->in)) {
            if (VIR_ALLOC(newBandwidth->in) < 0)
                goto endjob;

            memcpy(newBandwidth->in,
                   bandwidth->in ? bandwidth->in : net->bandwidth->in,
                   sizeof(*newBandwidth->in));
        }
        if (bandwidth->out ||
            (!outboundSpecified && net->bandwidth && net->bandwidth->out)) {
            if (VIR_ALLOC(newBandwidth->out) < 0)
                goto endjob;

            memcpy(newBandwidth->out,
                   bandwidth->out ? bandwidth->out : net->bandwidth->out,
                   sizeof(*newBandwidth->out));
        }

        if (net->type == VIR_DOMAIN_NET_TYPE_NETWORK &&
            virDomainNetBandwidthUpdate(net, newBandwidth) < 0)
            goto endjob;

        if (virNetDevBandwidthSet(net->ifname, newBandwidth, false,
                                  !virDomainNetTypeSharesHostView(net)) < 0) {
            ignore_value(virNetDevBandwidthSet(net->ifname,
                                               net->bandwidth,
                                               false,
                                               !virDomainNetTypeSharesHostView(net)));
            ignore_value(virDomainNetBandwidthUpdate(net,
                                                     net->bandwidth));
            goto endjob;
        }

        virNetDevBandwidthFree(net->bandwidth);
        if (newBandwidth->in || newBandwidth->out) {
            net->bandwidth = newBandwidth;
            newBandwidth = NULL;
        } else {
            net->bandwidth = NULL;
        }

        if (net->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
            virNetDevBandwidthFree(net->data.network.actual->bandwidth);
            if (virNetDevBandwidthCopy(&net->data.network.actual->bandwidth,
                                       net->bandwidth) < 0)
                goto endjob;
        }

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentNet) {
        if (!persistentNet->bandwidth) {
            persistentNet->bandwidth = bandwidth;
            bandwidth = NULL;
        } else {
            if (bandwidth->in) {
                VIR_FREE(persistentNet->bandwidth->in);
                persistentNet->bandwidth->in = bandwidth->in;
                bandwidth->in = NULL;
            } else  if (inboundSpecified) {
                VIR_FREE(persistentNet->bandwidth->in);
            }
            if (bandwidth->out) {
                VIR_FREE(persistentNet->bandwidth->out);
                persistentNet->bandwidth->out = bandwidth->out;
                bandwidth->out = NULL;
            } else if (outboundSpecified) {
                VIR_FREE(persistentNet->bandwidth->out);
            }
        }

        if (virDomainDefSave(persistentDef, driver->xmlopt, cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virNetDevBandwidthFree(bandwidth);
    virNetDevBandwidthFree(newBandwidth);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetInterfaceParameters(virDomainPtr dom,
                                 const char *device,
                                 virTypedParameterPtr params,
                                 int *nparams,
                                 unsigned int flags)
{
    size_t i;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def = NULL;
    virDomainNetDefPtr net = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetInterfaceParametersEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(def = virDomainObjGetOneDef(vm, flags)))
        goto cleanup;

    if ((*nparams) == 0) {
        *nparams = QEMU_NB_BANDWIDTH_PARAM;
        ret = 0;
        goto cleanup;
    }

    if (!(net = virDomainNetFind(def, device)))
        goto cleanup;

    for (i = 0; i < *nparams && i < QEMU_NB_BANDWIDTH_PARAM; i++) {
        switch (i) {
        case 0: /* inbound.average */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_IN_AVERAGE,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->in)
                params[i].value.ui = net->bandwidth->in->average;
            break;
        case 1: /* inbound.peak */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_IN_PEAK,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->in)
                params[i].value.ui = net->bandwidth->in->peak;
            break;
        case 2: /* inbound.burst */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_IN_BURST,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->in)
                params[i].value.ui = net->bandwidth->in->burst;
            break;
        case 3: /* inbound.floor */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_IN_FLOOR,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->in)
                params[i].value.ui = net->bandwidth->in->floor;
            break;
        case 4: /* outbound.average */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_OUT_AVERAGE,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->out)
                params[i].value.ui = net->bandwidth->out->average;
            break;
        case 5: /* outbound.peak */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_OUT_PEAK,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->out)
                params[i].value.ui = net->bandwidth->out->peak;
            break;
        case 6: /* outbound.burst */
            if (virTypedParameterAssign(&params[i],
                                        VIR_DOMAIN_BANDWIDTH_OUT_BURST,
                                        VIR_TYPED_PARAM_UINT, 0) < 0)
                goto cleanup;
            if (net->bandwidth && net->bandwidth->out)
                params[i].value.ui = net->bandwidth->out->burst;
            break;
        /* coverity[dead_error_begin] */
        default:
            break;
            /* should not hit here */
        }
    }

    if (*nparams > QEMU_NB_BANDWIDTH_PARAM)
        *nparams = QEMU_NB_BANDWIDTH_PARAM;
    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

/* This functions assumes that job QEMU_JOB_QUERY is started by a caller */
static int
qemuDomainMemoryStatsInternal(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainMemoryStatPtr stats,
                              unsigned int nr_stats)

{
    int ret = -1;
    long rss;

    if (virDomainObjCheckActive(vm) < 0)
        return -1;

    if (virDomainDefHasMemballoon(vm->def)) {
        qemuDomainObjEnterMonitor(driver, vm);
        ret = qemuMonitorGetMemoryStats(qemuDomainGetMonitor(vm),
                                        vm->def->memballoon, stats, nr_stats);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            ret = -1;

        if (ret < 0 || ret >= nr_stats)
            return ret;
    } else {
        ret = 0;
    }

    if (qemuGetProcessInfo(NULL, NULL, &rss, vm->pid, 0) < 0) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("cannot get RSS for domain"));
    } else {
        stats[ret].tag = VIR_DOMAIN_MEMORY_STAT_RSS;
        stats[ret].val = rss;
        ret++;
    }

    return ret;
}

static int
qemuDomainMemoryStats(virDomainPtr dom,
                      virDomainMemoryStatPtr stats,
                      unsigned int nr_stats,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMemoryStatsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    ret = qemuDomainMemoryStatsInternal(driver, vm, stats, nr_stats);

    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainBlockPeek(virDomainPtr dom,
                    const char *path,
                    unsigned long long offset, size_t size,
                    void *buffer,
                    unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainDiskDefPtr disk = NULL;
    virDomainObjPtr vm;
    g_autofree char *tmpbuf = NULL;
    ssize_t nread;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainBlockPeekEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto cleanup;

    if (disk->src->format != VIR_STORAGE_FILE_RAW) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("peeking is only supported for disk with 'raw' format not '%s'"),
                       virStorageFileFormatTypeToString(disk->src->format));
        goto cleanup;
    }

    if (qemuDomainStorageFileInit(driver, vm, disk->src, NULL) < 0)
        goto cleanup;

    if ((nread = virStorageFileRead(disk->src, offset, size, &tmpbuf)) < 0) {
        if (nread == -2) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("storage file reading is not supported for "
                             "storage type %s (protocol: %s)"),
                           virStorageTypeToString(disk->src->type),
                           virStorageNetProtocolTypeToString(disk->src->protocol));
        }
        goto cleanup;
    }

    if (nread < size) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("'%s' starting from %llu has only %zd bytes available"),
                       path, offset, nread);
        goto cleanup;
    }

    memcpy(buffer, tmpbuf, size);

    ret = 0;

 cleanup:
    if (disk)
        virStorageFileDeinit(disk->src);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainMemoryPeek(virDomainPtr dom,
                     unsigned long long offset, size_t size,
                     void *buffer,
                     unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    g_autofree char *tmp = NULL;
    int fd = -1, ret = -1;
    qemuDomainObjPrivatePtr priv;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(VIR_MEMORY_VIRTUAL | VIR_MEMORY_PHYSICAL, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainMemoryPeekEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (flags != VIR_MEMORY_VIRTUAL && flags != VIR_MEMORY_PHYSICAL) {
        virReportError(VIR_ERR_INVALID_ARG,
                       "%s", _("flags parameter must be VIR_MEMORY_VIRTUAL or VIR_MEMORY_PHYSICAL"));
        goto cleanup;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(tmp = g_strdup_printf("%s/qemu.mem.XXXXXX", cfg->cacheDir)))
        goto endjob;

    /* Create a temporary filename. */
    if ((fd = g_mkstemp_full(tmp, O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR)) == -1) {
        virReportSystemError(errno,
                             _("g_mkstemp(\"%s\") failed"), tmp);
        goto endjob;
    }

    qemuSecuritySetSavedStateLabel(driver, vm, tmp);

    priv = vm->privateData;
    qemuDomainObjEnterMonitor(driver, vm);
    if (flags == VIR_MEMORY_VIRTUAL) {
        if (qemuMonitorSaveVirtualMemory(priv->mon, offset, size, tmp) < 0) {
            ignore_value(qemuDomainObjExitMonitor(driver, vm));
            goto endjob;
        }
    } else {
        if (qemuMonitorSavePhysicalMemory(priv->mon, offset, size, tmp) < 0) {
            ignore_value(qemuDomainObjExitMonitor(driver, vm));
            goto endjob;
        }
    }
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto endjob;

    /* Read the memory file into buffer. */
    if (saferead(fd, buffer, size) == (ssize_t)-1) {
        virReportSystemError(errno,
                             _("failed to read temporary file "
                               "created with template %s"), tmp);
        goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    VIR_FORCE_CLOSE(fd);
    if (tmp)
        unlink(tmp);
    virDomainObjEndAPI(&vm);
    return ret;
}


/**
 * @driver: qemu driver data
 * @cfg: driver configuration data
 * @vm: domain object
 * @src: storage source data
 * @ret_fd: pointer to return open'd file descriptor
 * @ret_sb: pointer to return stat buffer (local or remote)
 * @skipInaccessible: Don't report error if files are not accessible
 *
 * For local storage, open the file using qemuOpenFile and then use
 * fstat() to grab the stat struct data for the caller.
 *
 * For remote storage, attempt to access the file and grab the stat
 * struct data if the remote connection supports it.
 *
 * Returns 1 if @src was successfully opened (@ret_fd and @ret_sb is populated),
 * 0 if @src can't be opened and @skipInaccessible is true (no errors are
 * reported) or -1 otherwise (errors are reported).
 */
static int
qemuDomainStorageOpenStat(virQEMUDriverPtr driver,
                          virQEMUDriverConfigPtr cfg,
                          virDomainObjPtr vm,
                          virStorageSourcePtr src,
                          int *ret_fd,
                          struct stat *ret_sb,
                          bool skipInaccessible)
{
    if (virStorageSourceIsLocalStorage(src)) {
        if (skipInaccessible && !virFileExists(src->path))
            return 0;

        if ((*ret_fd = qemuOpenFile(driver, vm, src->path, O_RDONLY,
                                    NULL)) < 0)
            return -1;

        if (fstat(*ret_fd, ret_sb) < 0) {
            virReportSystemError(errno, _("cannot stat file '%s'"), src->path);
            VIR_FORCE_CLOSE(*ret_fd);
            return -1;
        }
    } else {
        if (skipInaccessible && virStorageFileSupportsBackingChainTraversal(src) <= 0)
            return 0;

        if (virStorageFileInitAs(src, cfg->user, cfg->group) < 0)
            return -1;

        if (virStorageFileStat(src, ret_sb) < 0) {
            virStorageFileDeinit(src);
            virReportSystemError(errno, _("failed to stat remote file '%s'"),
                                 NULLSTR(src->path));
            return -1;
        }
    }

    return 1;
}


/**
 * @src: storage source data
 * @fd: file descriptor to close for local
 *
 * If local, then just close the file descriptor.
 * else remote, then tear down the storage driver backend connection.
 */
static void
qemuDomainStorageCloseStat(virStorageSourcePtr src,
                           int *fd)
{
    if (virStorageSourceIsLocalStorage(src))
        VIR_FORCE_CLOSE(*fd);
    else
        virStorageFileDeinit(src);
}


/**
 * qemuDomainStorageUpdatePhysical:
 * @driver: qemu driver
 * @cfg: qemu driver configuration object
 * @vm: domain object
 * @src: storage source to update
 *
 * Update the physical size of the disk by reading the actual size of the image
 * on disk.
 *
 * Returns 0 on successful update and -1 otherwise (some uncommon errors may be
 * reported but are reset (thus only logged)).
 */
static int
qemuDomainStorageUpdatePhysical(virQEMUDriverPtr driver,
                                virQEMUDriverConfigPtr cfg,
                                virDomainObjPtr vm,
                                virStorageSourcePtr src)
{
    int ret;
    int fd = -1;
    struct stat sb;

    if (virStorageSourceIsEmpty(src))
        return 0;

    if ((ret = qemuDomainStorageOpenStat(driver, cfg, vm, src, &fd, &sb, true)) <= 0) {
        if (ret < 0)
            virResetLastError();
        return -1;
    }

    ret = virStorageSourceUpdatePhysicalSize(src, fd, &sb);

    qemuDomainStorageCloseStat(src, &fd);

    return ret;
}


/**
 * @driver: qemu driver data
 * @cfg: driver configuration data
 * @vm: domain object
 * @src: storage source data
 * @skipInaccessible: Suppress reporting of common errors when accessing @src
 *
 * Refresh the capacity and allocation limits of a given storage source.
 *
 * Assumes that the caller has already obtained a domain job and only
 * called for an offline domain. Being offline is particularly important
 * since reading a file while qemu is writing it risks the reader seeing
 * bogus data or avoiding opening a file in order to get stat data.
 *
 * We always want to check current on-disk statistics (as users have been
 * known to change offline images behind our backs).
 *
 * For read-only disks, nothing should be changing unless the user has
 * requested a block-commit action.  For read-write disks, we know some
 * special cases: capacity should not change without a block-resize (where
 * capacity is the only stat that requires reading a file, and even then,
 * only for non-raw files); and physical size of a raw image or of a
 * block device should likewise not be changing without block-resize.
 * On the other hand, allocation of a raw file can change (if the file
 * is sparse, but the amount of sparseness changes due to writes or
 * punching holes), and physical size of a non-raw file can change.
 *
 * Returns 1 if @src was successfully updated, 0 if @src can't be opened and
 * @skipInaccessible is true (no errors are reported) or -1 otherwise (errors
 * are reported).
 */
static int
qemuStorageLimitsRefresh(virQEMUDriverPtr driver,
                         virQEMUDriverConfigPtr cfg,
                         virDomainObjPtr vm,
                         virStorageSourcePtr src,
                         bool skipInaccessible)
{
    int rc;
    int ret = -1;
    int fd = -1;
    struct stat sb;
    g_autofree char *buf = NULL;
    ssize_t len;

    if ((rc = qemuDomainStorageOpenStat(driver, cfg, vm, src, &fd, &sb,
                                        skipInaccessible)) <= 0)
        return rc;

    if (virStorageSourceIsLocalStorage(src)) {
        if ((len = virFileReadHeaderFD(fd, VIR_STORAGE_MAX_HEADER, &buf)) < 0) {
            virReportSystemError(errno, _("cannot read header '%s'"),
                                 src->path);
            goto cleanup;
        }
    } else {
        if ((len = virStorageFileRead(src, 0, VIR_STORAGE_MAX_HEADER, &buf)) < 0)
            goto cleanup;
    }

    if (virStorageSourceUpdateBackingSizes(src, fd, &sb) < 0)
        goto cleanup;

    if (virStorageSourceUpdateCapacity(src, buf, len, false) < 0)
        goto cleanup;

    /* If guest is not using raw disk format and is on a host block
     * device, then leave the value unspecified, so caller knows to
     * query the highest allocated extent from QEMU
     */
    if (virStorageSourceGetActualType(src) == VIR_STORAGE_TYPE_BLOCK &&
        src->format != VIR_STORAGE_FILE_RAW &&
        S_ISBLK(sb.st_mode))
        src->allocation = 0;

    ret = 1;

 cleanup:
    qemuDomainStorageCloseStat(src, &fd);
    return ret;
}


static int
qemuDomainGetBlockInfo(virDomainPtr dom,
                       const char *path,
                       virDomainBlockInfoPtr info,
                       unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    virDomainDiskDefPtr disk;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuBlockStatsPtr entry = NULL;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainGetBlockInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (!(disk = virDomainDiskByName(vm->def, path, false))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("invalid path %s not assigned to domain"), path);
        goto endjob;
    }

    if (virStorageSourceIsEmpty(disk->src)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("disk '%s' does not currently have a source assigned"),
                       path);
        goto endjob;
    }

    /* for inactive domains we have to peek into the files */
    if (!virDomainObjIsActive(vm)) {
        if ((qemuStorageLimitsRefresh(driver, cfg, vm, disk->src, false)) < 0)
            goto endjob;

        info->capacity = disk->src->capacity;
        info->allocation = disk->src->allocation;
        info->physical = disk->src->physical;

        ret = 0;
        goto endjob;
    }

    if (qemuDomainBlocksStatsGather(driver, vm, path, true, &entry) < 0)
        goto endjob;

    if (!entry->wr_highest_offset_valid) {
        info->allocation = entry->physical;
    } else {
        if (virStorageSourceGetActualType(disk->src) == VIR_STORAGE_TYPE_FILE &&
            disk->src->format == VIR_STORAGE_FILE_QCOW2)
            info->allocation = entry->physical;
        else
            info->allocation = entry->wr_highest_offset;
    }

    /* Unlike GetStatsBlock, this API has defined the expected return values
     * for allocation and physical slightly differently.
     *
     * Having a zero for either or if they're the same is an indication that
     * there's a sparse file backing this device. In this case, we'll force
     * the setting of physical based on the on disk file size.
     *
     * Additionally, if qemu hasn't written to the file yet, then set the
     * allocation to whatever qemu returned for physical (e.g. the "actual-
     * size" from the json query) as that will match the expected allocation
     * value for this API. NB: May still be 0 for block. */
    if (entry->physical == 0 || info->allocation == 0 ||
        info->allocation == entry->physical) {
        if (info->allocation == 0)
            info->allocation = entry->physical;

        if (qemuDomainStorageUpdatePhysical(driver, cfg, vm, disk->src) == 0) {
            info->physical = disk->src->physical;
        } else {
            info->physical = entry->physical;
        }
    } else {
        info->physical = entry->physical;
    }

    info->capacity = entry->capacity;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);
 cleanup:
    VIR_FREE(entry);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuConnectDomainEventRegister(virConnectPtr conn,
                               virConnectDomainEventCallback callback,
                               void *opaque,
                               virFreeCallback freecb)
{
    virQEMUDriverPtr driver = conn->privateData;

    if (virConnectDomainEventRegisterEnsureACL(conn) < 0)
        return -1;

    if (virDomainEventStateRegister(conn,
                                    driver->domainEventState,
                                    callback, opaque, freecb) < 0)
        return -1;

    return 0;
}


static int
qemuConnectDomainEventDeregister(virConnectPtr conn,
                                 virConnectDomainEventCallback callback)
{
    virQEMUDriverPtr driver = conn->privateData;

    if (virConnectDomainEventDeregisterEnsureACL(conn) < 0)
        return -1;

    if (virDomainEventStateDeregister(conn,
                                      driver->domainEventState,
                                      callback) < 0)
        return -1;

    return 0;
}


static int
qemuConnectDomainEventRegisterAny(virConnectPtr conn,
                                  virDomainPtr dom,
                                  int eventID,
                                  virConnectDomainEventGenericCallback callback,
                                  void *opaque,
                                  virFreeCallback freecb)
{
    virQEMUDriverPtr driver = conn->privateData;
    int ret = -1;

    if (virConnectDomainEventRegisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virDomainEventStateRegisterID(conn,
                                      driver->domainEventState,
                                      dom, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;

    return ret;
}


static int
qemuConnectDomainEventDeregisterAny(virConnectPtr conn,
                                    int callbackID)
{
    virQEMUDriverPtr driver = conn->privateData;

    if (virConnectDomainEventDeregisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virObjectEventStateDeregisterID(conn,
                                        driver->domainEventState,
                                        callbackID, true) < 0)
        return -1;

    return 0;
}


/*******************************************************************
 * Migration Protocol Version 2
 *******************************************************************/

/* Prepare is the first step, and it runs on the destination host.
 *
 * This version starts an empty VM listening on a localhost TCP port, and
 * sets up the corresponding virStream to handle the incoming data.
 */
static int
qemuDomainMigratePrepareTunnel(virConnectPtr dconn,
                               virStreamPtr st,
                               unsigned long flags,
                               const char *dname,
                               unsigned long resource G_GNUC_UNUSED,
                               const char *dom_xml)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainDefPtr def = NULL;
    g_autofree char *origname = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (!(flags & VIR_MIGRATE_TUNNELLED)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("PrepareTunnel called but no TUNNELLED flag set"));
        goto cleanup;
    }

    if (!(migParams = qemuMigrationParamsFromFlags(NULL, 0, flags,
                                                   QEMU_MIGRATION_DESTINATION)))
        goto cleanup;

    if (virLockManagerPluginUsesState(driver->lockManager)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot use migrate v2 protocol with lock manager %s"),
                       virLockManagerPluginGetName(driver->lockManager));
        goto cleanup;
    }

    if (!(def = qemuMigrationAnyPrepareDef(driver, NULL, dom_xml, dname, &origname)))
        goto cleanup;

    if (virDomainMigratePrepareTunnelEnsureACL(dconn, def) < 0)
        goto cleanup;

    ret = qemuMigrationDstPrepareTunnel(driver, dconn,
                                        NULL, 0, NULL, NULL, /* No cookies in v2 */
                                        st, &def, origname, migParams, flags);

 cleanup:
    qemuMigrationParamsFree(migParams);
    virDomainDefFree(def);
    return ret;
}

/* Prepare is the first step, and it runs on the destination host.
 *
 * This starts an empty VM listening on a TCP port.
 */
static int ATTRIBUTE_NONNULL(5)
qemuDomainMigratePrepare2(virConnectPtr dconn,
                          char **cookie G_GNUC_UNUSED,
                          int *cookielen G_GNUC_UNUSED,
                          const char *uri_in,
                          char **uri_out,
                          unsigned long flags,
                          const char *dname,
                          unsigned long resource G_GNUC_UNUSED,
                          const char *dom_xml)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainDefPtr def = NULL;
    g_autofree char *origname = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (flags & VIR_MIGRATE_TUNNELLED) {
        /* this is a logical error; we never should have gotten here with
         * VIR_MIGRATE_TUNNELLED set
         */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Tunnelled migration requested but invalid "
                         "RPC method called"));
        goto cleanup;
    }

    if (!(migParams = qemuMigrationParamsFromFlags(NULL, 0, flags,
                                                   QEMU_MIGRATION_DESTINATION)))
        goto cleanup;

    if (virLockManagerPluginUsesState(driver->lockManager)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot use migrate v2 protocol with lock manager %s"),
                       virLockManagerPluginGetName(driver->lockManager));
        goto cleanup;
    }

    if (!(def = qemuMigrationAnyPrepareDef(driver, NULL, dom_xml, dname, &origname)))
        goto cleanup;

    if (virDomainMigratePrepare2EnsureACL(dconn, def) < 0)
        goto cleanup;

    /* Do not use cookies in v2 protocol, since the cookie
     * length was not sufficiently large, causing failures
     * migrating between old & new libvirtd
     */
    ret = qemuMigrationDstPrepareDirect(driver, dconn,
                                        NULL, 0, NULL, NULL, /* No cookies */
                                        uri_in, uri_out,
                                        &def, origname, NULL, 0, NULL, 0,
                                        migParams, flags);

 cleanup:
    qemuMigrationParamsFree(migParams);
    virDomainDefFree(def);
    return ret;
}


/* Perform is the second step, and it runs on the source host. */
static int
qemuDomainMigratePerform(virDomainPtr dom,
                         const char *cookie,
                         int cookielen,
                         const char *uri,
                         unsigned long flags,
                         const char *dname,
                         unsigned long resource)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    const char *dconnuri = NULL;
    qemuMigrationParamsPtr migParams = NULL;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (virLockManagerPluginUsesState(driver->lockManager)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot use migrate v2 protocol with lock manager %s"),
                       virLockManagerPluginGetName(driver->lockManager));
        goto cleanup;
    }

    if (!(migParams = qemuMigrationParamsFromFlags(NULL, 0, flags,
                                                   QEMU_MIGRATION_SOURCE)))
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigratePerformEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (flags & VIR_MIGRATE_PEER2PEER)
        dconnuri = g_steal_pointer(&uri);

    /* Do not output cookies in v2 protocol, since the cookie
     * length was not sufficiently large, causing failures
     * migrating between old & new libvirtd.
     *
     * Consume any cookie we were able to decode though
     */
    ret = qemuMigrationSrcPerform(driver, dom->conn, vm, NULL,
                                  NULL, dconnuri, uri, NULL, NULL, 0, NULL, 0,
                                  migParams, cookie, cookielen,
                                  NULL, NULL, /* No output cookies in v2 */
                                  flags, dname, resource, false);

 cleanup:
    virDomainObjEndAPI(&vm);
    qemuMigrationParamsFree(migParams);
    return ret;
}


/* Finish is the third and final step, and it runs on the destination host. */
static virDomainPtr
qemuDomainMigrateFinish2(virConnectPtr dconn,
                         const char *dname,
                         const char *cookie G_GNUC_UNUSED,
                         int cookielen G_GNUC_UNUSED,
                         const char *uri G_GNUC_UNUSED,
                         unsigned long flags,
                         int retcode)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainObjPtr vm;

    virCheckFlags(QEMU_MIGRATION_FLAGS, NULL);

    vm = virDomainObjListFindByName(driver->domains, dname);
    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), dname);
        qemuMigrationDstErrorReport(driver, dname);
        return NULL;
    }

    if (virDomainMigrateFinish2EnsureACL(dconn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return NULL;
    }

    /* Do not use cookies in v2 protocol, since the cookie
     * length was not sufficiently large, causing failures
     * migrating between old & new libvirtd
     */
    return qemuMigrationDstFinish(driver, dconn, vm,
                                  NULL, 0, NULL, NULL, /* No cookies */
                                  flags, retcode, false);
}


/*******************************************************************
 * Migration Protocol Version 3
 *******************************************************************/

static char *
qemuDomainMigrateBegin3(virDomainPtr domain,
                        const char *xmlin,
                        char **cookieout,
                        int *cookieoutlen,
                        unsigned long flags,
                        const char *dname,
                        unsigned long resource G_GNUC_UNUSED)
{
    virDomainObjPtr vm;

    virCheckFlags(QEMU_MIGRATION_FLAGS, NULL);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return NULL;

    if (virDomainMigrateBegin3EnsureACL(domain->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return NULL;
    }

    return qemuMigrationSrcBegin(domain->conn, vm, xmlin, dname,
                                 cookieout, cookieoutlen, 0, NULL, flags);
}

static char *
qemuDomainMigrateBegin3Params(virDomainPtr domain,
                              virTypedParameterPtr params,
                              int nparams,
                              char **cookieout,
                              int *cookieoutlen,
                              unsigned int flags)
{
    const char *xmlin = NULL;
    const char *dname = NULL;
    const char **migrate_disks = NULL;
    int nmigrate_disks;
    char *ret = NULL;
    virDomainObjPtr vm;

    virCheckFlags(QEMU_MIGRATION_FLAGS, NULL);
    if (virTypedParamsValidate(params, nparams, QEMU_MIGRATION_PARAMETERS) < 0)
        goto cleanup;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &xmlin) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0)
        goto cleanup;

    nmigrate_disks = virTypedParamsGetStringList(params, nparams,
                                                 VIR_MIGRATE_PARAM_MIGRATE_DISKS,
                                                 &migrate_disks);

    if (nmigrate_disks < 0)
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainMigrateBegin3ParamsEnsureACL(domain->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        goto cleanup;
    }

    ret = qemuMigrationSrcBegin(domain->conn, vm, xmlin, dname,
                                cookieout, cookieoutlen,
                                nmigrate_disks, migrate_disks, flags);

 cleanup:
    VIR_FREE(migrate_disks);
    return ret;
}


static int
qemuDomainMigratePrepare3(virConnectPtr dconn,
                          const char *cookiein,
                          int cookieinlen,
                          char **cookieout,
                          int *cookieoutlen,
                          const char *uri_in,
                          char **uri_out,
                          unsigned long flags,
                          const char *dname,
                          unsigned long resource G_GNUC_UNUSED,
                          const char *dom_xml)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainDefPtr def = NULL;
    g_autofree char *origname = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (flags & VIR_MIGRATE_TUNNELLED) {
        /* this is a logical error; we never should have gotten here with
         * VIR_MIGRATE_TUNNELLED set
         */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Tunnelled migration requested but invalid "
                         "RPC method called"));
        goto cleanup;
    }

    if (!(migParams = qemuMigrationParamsFromFlags(NULL, 0, flags,
                                                   QEMU_MIGRATION_DESTINATION)))
        goto cleanup;

    if (!(def = qemuMigrationAnyPrepareDef(driver, NULL, dom_xml, dname, &origname)))
        goto cleanup;

    if (virDomainMigratePrepare3EnsureACL(dconn, def) < 0)
        goto cleanup;

    ret = qemuMigrationDstPrepareDirect(driver, dconn,
                                        cookiein, cookieinlen,
                                        cookieout, cookieoutlen,
                                        uri_in, uri_out,
                                        &def, origname, NULL, 0, NULL, 0,
                                        migParams, flags);

 cleanup:
    qemuMigrationParamsFree(migParams);
    virDomainDefFree(def);
    return ret;
}

static int
qemuDomainMigratePrepare3Params(virConnectPtr dconn,
                                virTypedParameterPtr params,
                                int nparams,
                                const char *cookiein,
                                int cookieinlen,
                                char **cookieout,
                                int *cookieoutlen,
                                char **uri_out,
                                unsigned int flags)
{
    virQEMUDriverPtr driver = dconn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    virDomainDefPtr def = NULL;
    const char *dom_xml = NULL;
    const char *dname = NULL;
    const char *uri_in = NULL;
    const char *listenAddress = cfg->migrationAddress;
    int nbdPort = 0;
    int nmigrate_disks;
    const char **migrate_disks = NULL;
    g_autofree char *origname = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlagsGoto(QEMU_MIGRATION_FLAGS, cleanup);
    if (virTypedParamsValidate(params, nparams, QEMU_MIGRATION_PARAMETERS) < 0)
        goto cleanup;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &dom_xml) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_URI,
                                &uri_in) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_LISTEN_ADDRESS,
                                &listenAddress) < 0 ||
        virTypedParamsGetInt(params, nparams,
                             VIR_MIGRATE_PARAM_DISKS_PORT,
                             &nbdPort) < 0)
        goto cleanup;

    nmigrate_disks = virTypedParamsGetStringList(params, nparams,
                                                 VIR_MIGRATE_PARAM_MIGRATE_DISKS,
                                                 &migrate_disks);

    if (nmigrate_disks < 0)
        goto cleanup;

    if (!(migParams = qemuMigrationParamsFromFlags(params, nparams, flags,
                                                   QEMU_MIGRATION_DESTINATION)))
        goto cleanup;

    if (flags & VIR_MIGRATE_TUNNELLED) {
        /* this is a logical error; we never should have gotten here with
         * VIR_MIGRATE_TUNNELLED set
         */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Tunnelled migration requested but invalid "
                         "RPC method called"));
        goto cleanup;
    }

    if (!(def = qemuMigrationAnyPrepareDef(driver, NULL, dom_xml, dname, &origname)))
        goto cleanup;

    if (virDomainMigratePrepare3ParamsEnsureACL(dconn, def) < 0)
        goto cleanup;

    ret = qemuMigrationDstPrepareDirect(driver, dconn,
                                        cookiein, cookieinlen,
                                        cookieout, cookieoutlen,
                                        uri_in, uri_out,
                                        &def, origname, listenAddress,
                                        nmigrate_disks, migrate_disks, nbdPort,
                                        migParams, flags);

 cleanup:
    qemuMigrationParamsFree(migParams);
    VIR_FREE(migrate_disks);
    virDomainDefFree(def);
    return ret;
}


static int
qemuDomainMigratePrepareTunnel3(virConnectPtr dconn,
                                virStreamPtr st,
                                const char *cookiein,
                                int cookieinlen,
                                char **cookieout,
                                int *cookieoutlen,
                                unsigned long flags,
                                const char *dname,
                                unsigned long resource G_GNUC_UNUSED,
                                const char *dom_xml)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainDefPtr def = NULL;
    g_autofree char *origname = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (!(flags & VIR_MIGRATE_TUNNELLED)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("PrepareTunnel called but no TUNNELLED flag set"));
        goto cleanup;
    }

    if (!(migParams = qemuMigrationParamsFromFlags(NULL, 0, flags,
                                                   QEMU_MIGRATION_DESTINATION)))
        goto cleanup;

    if (!(def = qemuMigrationAnyPrepareDef(driver, NULL, dom_xml, dname, &origname)))
        goto cleanup;

    if (virDomainMigratePrepareTunnel3EnsureACL(dconn, def) < 0)
        goto cleanup;

    ret = qemuMigrationDstPrepareTunnel(driver, dconn,
                                        cookiein, cookieinlen,
                                        cookieout, cookieoutlen,
                                        st, &def, origname, migParams, flags);

 cleanup:
    qemuMigrationParamsFree(migParams);
    virDomainDefFree(def);
    return ret;
}

static int
qemuDomainMigratePrepareTunnel3Params(virConnectPtr dconn,
                                      virStreamPtr st,
                                      virTypedParameterPtr params,
                                      int nparams,
                                      const char *cookiein,
                                      int cookieinlen,
                                      char **cookieout,
                                      int *cookieoutlen,
                                      unsigned int flags)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainDefPtr def = NULL;
    const char *dom_xml = NULL;
    const char *dname = NULL;
    g_autofree char *origname = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);
    if (virTypedParamsValidate(params, nparams, QEMU_MIGRATION_PARAMETERS) < 0)
        return -1;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &dom_xml) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0)
        return -1;

    if (!(flags & VIR_MIGRATE_TUNNELLED)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("PrepareTunnel called but no TUNNELLED flag set"));
        goto cleanup;
    }

    if (!(migParams = qemuMigrationParamsFromFlags(params, nparams, flags,
                                                   QEMU_MIGRATION_DESTINATION)))
        goto cleanup;

    if (!(def = qemuMigrationAnyPrepareDef(driver, NULL, dom_xml, dname, &origname)))
        goto cleanup;

    if (virDomainMigratePrepareTunnel3ParamsEnsureACL(dconn, def) < 0)
        goto cleanup;

    ret = qemuMigrationDstPrepareTunnel(driver, dconn,
                                        cookiein, cookieinlen,
                                        cookieout, cookieoutlen,
                                        st, &def, origname, migParams, flags);

 cleanup:
    qemuMigrationParamsFree(migParams);
    virDomainDefFree(def);
    return ret;
}


static int
qemuDomainMigratePerform3(virDomainPtr dom,
                          const char *xmlin,
                          const char *cookiein,
                          int cookieinlen,
                          char **cookieout,
                          int *cookieoutlen,
                          const char *dconnuri,
                          const char *uri,
                          unsigned long flags,
                          const char *dname,
                          unsigned long resource)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (!(migParams = qemuMigrationParamsFromFlags(NULL, 0, flags,
                                                   QEMU_MIGRATION_SOURCE)))
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigratePerform3EnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = qemuMigrationSrcPerform(driver, dom->conn, vm, xmlin, NULL,
                                  dconnuri, uri, NULL, NULL, 0, NULL, 0,
                                  migParams,
                                  cookiein, cookieinlen,
                                  cookieout, cookieoutlen,
                                  flags, dname, resource, true);

 cleanup:
    virDomainObjEndAPI(&vm);
    qemuMigrationParamsFree(migParams);
    return ret;
}

static int
qemuDomainMigratePerform3Params(virDomainPtr dom,
                                const char *dconnuri,
                                virTypedParameterPtr params,
                                int nparams,
                                const char *cookiein,
                                int cookieinlen,
                                char **cookieout,
                                int *cookieoutlen,
                                unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    const char *dom_xml = NULL;
    const char *persist_xml = NULL;
    const char *dname = NULL;
    const char *uri = NULL;
    const char *graphicsuri = NULL;
    const char *listenAddress = NULL;
    int nmigrate_disks;
    const char **migrate_disks = NULL;
    unsigned long long bandwidth = 0;
    int nbdPort = 0;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);
    if (virTypedParamsValidate(params, nparams, QEMU_MIGRATION_PARAMETERS) < 0)
        return ret;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_XML,
                                &dom_xml) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_URI,
                                &uri) < 0 ||
        virTypedParamsGetULLong(params, nparams,
                                VIR_MIGRATE_PARAM_BANDWIDTH,
                                &bandwidth) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_GRAPHICS_URI,
                                &graphicsuri) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_LISTEN_ADDRESS,
                                &listenAddress) < 0 ||
        virTypedParamsGetInt(params, nparams,
                             VIR_MIGRATE_PARAM_DISKS_PORT,
                             &nbdPort) < 0 ||
        virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_PERSIST_XML,
                                &persist_xml) < 0)
        goto cleanup;

    nmigrate_disks = virTypedParamsGetStringList(params, nparams,
                                                 VIR_MIGRATE_PARAM_MIGRATE_DISKS,
                                                 &migrate_disks);

    if (nmigrate_disks < 0)
        goto cleanup;

    if (!(migParams = qemuMigrationParamsFromFlags(params, nparams, flags,
                                                   QEMU_MIGRATION_SOURCE)))
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigratePerform3ParamsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = qemuMigrationSrcPerform(driver, dom->conn, vm, dom_xml, persist_xml,
                                  dconnuri, uri, graphicsuri, listenAddress,
                                  nmigrate_disks, migrate_disks, nbdPort,
                                  migParams,
                                  cookiein, cookieinlen, cookieout, cookieoutlen,
                                  flags, dname, bandwidth, true);
 cleanup:
    virDomainObjEndAPI(&vm);
    qemuMigrationParamsFree(migParams);
    VIR_FREE(migrate_disks);
    return ret;
}


static virDomainPtr
qemuDomainMigrateFinish3(virConnectPtr dconn,
                         const char *dname,
                         const char *cookiein,
                         int cookieinlen,
                         char **cookieout,
                         int *cookieoutlen,
                         const char *dconnuri G_GNUC_UNUSED,
                         const char *uri G_GNUC_UNUSED,
                         unsigned long flags,
                         int cancelled)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainObjPtr vm;

    virCheckFlags(QEMU_MIGRATION_FLAGS, NULL);

    if (!dname) {
        virReportError(VIR_ERR_NO_DOMAIN, "%s", _("missing domain name"));
        return NULL;
    }

    vm = virDomainObjListFindByName(driver->domains, dname);
    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), dname);
        qemuMigrationDstErrorReport(driver, dname);
        return NULL;
    }

    if (virDomainMigrateFinish3EnsureACL(dconn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return NULL;
    }

    return qemuMigrationDstFinish(driver, dconn, vm,
                                  cookiein, cookieinlen,
                                  cookieout, cookieoutlen,
                                  flags, cancelled, true);
}

static virDomainPtr
qemuDomainMigrateFinish3Params(virConnectPtr dconn,
                               virTypedParameterPtr params,
                               int nparams,
                               const char *cookiein,
                               int cookieinlen,
                               char **cookieout,
                               int *cookieoutlen,
                               unsigned int flags,
                               int cancelled)
{
    virQEMUDriverPtr driver = dconn->privateData;
    virDomainObjPtr vm;
    const char *dname = NULL;

    virCheckFlags(QEMU_MIGRATION_FLAGS, NULL);
    if (virTypedParamsValidate(params, nparams, QEMU_MIGRATION_PARAMETERS) < 0)
        return NULL;

    if (virTypedParamsGetString(params, nparams,
                                VIR_MIGRATE_PARAM_DEST_NAME,
                                &dname) < 0)
        return NULL;

    if (!dname) {
        virReportError(VIR_ERR_NO_DOMAIN, "%s", _("missing domain name"));
        return NULL;
    }

    vm = virDomainObjListFindByName(driver->domains, dname);
    if (!vm) {
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching name '%s'"), dname);
        qemuMigrationDstErrorReport(driver, dname);
        return NULL;
    }

    if (virDomainMigrateFinish3ParamsEnsureACL(dconn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return NULL;
    }

    return qemuMigrationDstFinish(driver, dconn, vm,
                                  cookiein, cookieinlen,
                                  cookieout, cookieoutlen,
                                  flags, cancelled, true);
}


static int
qemuDomainMigrateConfirm3(virDomainPtr domain,
                          const char *cookiein,
                          int cookieinlen,
                          unsigned long flags,
                          int cancelled)
{
    virDomainObjPtr vm;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainMigrateConfirm3EnsureACL(domain->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return -1;
    }

    return qemuMigrationSrcConfirm(domain->conn->privateData, vm, cookiein, cookieinlen,
                                   flags, cancelled);
}

static int
qemuDomainMigrateConfirm3Params(virDomainPtr domain,
                                virTypedParameterPtr params,
                                int nparams,
                                const char *cookiein,
                                int cookieinlen,
                                unsigned int flags,
                                int cancelled)
{
    virDomainObjPtr vm;

    virCheckFlags(QEMU_MIGRATION_FLAGS, -1);

    if (virTypedParamsValidate(params, nparams, QEMU_MIGRATION_PARAMETERS) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainMigrateConfirm3ParamsEnsureACL(domain->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return -1;
    }

    return qemuMigrationSrcConfirm(domain->conn->privateData, vm, cookiein, cookieinlen,
                                   flags, cancelled);
}


static int
qemuNodeDeviceGetPCIInfo(virNodeDeviceDefPtr def,
                         unsigned *domain,
                         unsigned *bus,
                         unsigned *slot,
                         unsigned *function)
{
    virNodeDevCapsDefPtr cap;

    cap = def->caps;
    while (cap) {
        if (cap->data.type == VIR_NODE_DEV_CAP_PCI_DEV) {
            *domain   = cap->data.pci_dev.domain;
            *bus      = cap->data.pci_dev.bus;
            *slot     = cap->data.pci_dev.slot;
            *function = cap->data.pci_dev.function;
            break;
        }

        cap = cap->next;
    }

    if (!cap) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("device %s is not a PCI device"), def->name);
        return -1;
    }

    return 0;
}

static int
qemuNodeDeviceDetachFlags(virNodeDevicePtr dev,
                          const char *driverName,
                          unsigned int flags)
{
    virQEMUDriverPtr driver = dev->conn->privateData;
    virPCIDevicePtr pci = NULL;
    unsigned domain = 0, bus = 0, slot = 0, function = 0;
    int ret = -1;
    virNodeDeviceDefPtr def = NULL;
    g_autofree char *xml = NULL;
    bool vfio = qemuHostdevHostSupportsPassthroughVFIO();
    virHostdevManagerPtr hostdev_mgr = driver->hostdevMgr;

    virCheckFlags(0, -1);

    xml = virNodeDeviceGetXMLDesc(dev, 0);
    if (!xml)
        goto cleanup;

    def = virNodeDeviceDefParseString(xml, EXISTING_DEVICE, NULL);
    if (!def)
        goto cleanup;

    if (virNodeDeviceDetachFlagsEnsureACL(dev->conn, def) < 0)
        goto cleanup;

    if (qemuNodeDeviceGetPCIInfo(def, &domain, &bus, &slot, &function) < 0)
        goto cleanup;

    pci = virPCIDeviceNew(domain, bus, slot, function);
    if (!pci)
        goto cleanup;

    if (!driverName)
        driverName = "vfio";

    if (STREQ(driverName, "vfio")) {
        if (!vfio) {
            virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                           _("VFIO device assignment is currently not "
                             "supported on this system"));
            goto cleanup;
        }
        virPCIDeviceSetStubDriver(pci, VIR_PCI_STUB_DRIVER_VFIO);
    } else if (STREQ(driverName, "kvm")) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("KVM device assignment is no longer "
                         "supported on this system"));
        goto cleanup;
    } else {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("unknown driver name '%s'"), driverName);
        goto cleanup;
    }

    ret = virHostdevPCINodeDeviceDetach(hostdev_mgr, pci);
 cleanup:
    virPCIDeviceFree(pci);
    virNodeDeviceDefFree(def);
    return ret;
}

static int
qemuNodeDeviceDettach(virNodeDevicePtr dev)
{
    return qemuNodeDeviceDetachFlags(dev, NULL, 0);
}

static int
qemuNodeDeviceReAttach(virNodeDevicePtr dev)
{
    virQEMUDriverPtr driver = dev->conn->privateData;
    virPCIDevicePtr pci = NULL;
    unsigned domain = 0, bus = 0, slot = 0, function = 0;
    int ret = -1;
    virNodeDeviceDefPtr def = NULL;
    g_autofree char *xml = NULL;
    virHostdevManagerPtr hostdev_mgr = driver->hostdevMgr;

    xml = virNodeDeviceGetXMLDesc(dev, 0);
    if (!xml)
        goto cleanup;

    def = virNodeDeviceDefParseString(xml, EXISTING_DEVICE, NULL);
    if (!def)
        goto cleanup;

    if (virNodeDeviceReAttachEnsureACL(dev->conn, def) < 0)
        goto cleanup;

    if (qemuNodeDeviceGetPCIInfo(def, &domain, &bus, &slot, &function) < 0)
        goto cleanup;

    pci = virPCIDeviceNew(domain, bus, slot, function);
    if (!pci)
        goto cleanup;

    ret = virHostdevPCINodeDeviceReAttach(hostdev_mgr, pci);

    virPCIDeviceFree(pci);
 cleanup:
    virNodeDeviceDefFree(def);
    return ret;
}

static int
qemuNodeDeviceReset(virNodeDevicePtr dev)
{
    virQEMUDriverPtr driver = dev->conn->privateData;
    virPCIDevicePtr pci;
    unsigned domain = 0, bus = 0, slot = 0, function = 0;
    int ret = -1;
    virNodeDeviceDefPtr def = NULL;
    g_autofree char *xml = NULL;
    virHostdevManagerPtr hostdev_mgr = driver->hostdevMgr;

    xml = virNodeDeviceGetXMLDesc(dev, 0);
    if (!xml)
        goto cleanup;

    def = virNodeDeviceDefParseString(xml, EXISTING_DEVICE, NULL);
    if (!def)
        goto cleanup;

    if (virNodeDeviceResetEnsureACL(dev->conn, def) < 0)
        goto cleanup;

    if (qemuNodeDeviceGetPCIInfo(def, &domain, &bus, &slot, &function) < 0)
        goto cleanup;

    pci = virPCIDeviceNew(domain, bus, slot, function);
    if (!pci)
        goto cleanup;

    ret = virHostdevPCINodeDeviceReset(hostdev_mgr, pci);

    virPCIDeviceFree(pci);
 cleanup:
    virNodeDeviceDefFree(def);
    return ret;
}

static int
qemuConnectCompareCPU(virConnectPtr conn,
                      const char *xmlDesc,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    g_autoptr(virCPUDef) cpu = NULL;
    bool failIncompatible;

    virCheckFlags(VIR_CONNECT_COMPARE_CPU_FAIL_INCOMPATIBLE,
                  VIR_CPU_COMPARE_ERROR);

    if (virConnectCompareCPUEnsureACL(conn) < 0)
        return VIR_CPU_COMPARE_ERROR;

    failIncompatible = !!(flags & VIR_CONNECT_COMPARE_CPU_FAIL_INCOMPATIBLE);

    if (!(cpu = virQEMUDriverGetHostCPU(driver)))
        return VIR_CPU_COMPARE_ERROR;

    return virCPUCompareXML(driver->hostarch, cpu,
                            xmlDesc, failIncompatible);
}


static virCPUCompareResult
qemuConnectCPUModelComparison(virQEMUCapsPtr qemuCaps,
                              const char *libDir,
                              uid_t runUid,
                              gid_t runGid,
                              virCPUDefPtr cpu_a,
                              virCPUDefPtr cpu_b,
                              bool failIncompatible)
{
    qemuProcessQMPPtr proc = NULL;
    char *result = NULL;
    int ret = VIR_CPU_COMPARE_ERROR;

    if (!(proc = qemuProcessQMPNew(virQEMUCapsGetBinary(qemuCaps),
                                   libDir, runUid, runGid, false)))
        goto cleanup;

    if (qemuProcessQMPStart(proc) < 0)
        goto cleanup;

    if (qemuMonitorGetCPUModelComparison(proc->mon, cpu_a, cpu_b, &result) < 0)
        goto cleanup;

    if (STREQ(result, "identical"))
        ret = VIR_CPU_COMPARE_IDENTICAL;
    else if (STREQ(result, "superset"))
        ret = VIR_CPU_COMPARE_SUPERSET;
    else if (failIncompatible)
        virReportError(VIR_ERR_CPU_INCOMPATIBLE, NULL);
    else
        ret = VIR_CPU_COMPARE_INCOMPATIBLE;

 cleanup:
    VIR_FREE(result);
    qemuProcessQMPFree(proc);
    return ret;
}


static int
qemuConnectCompareHypervisorCPU(virConnectPtr conn,
                                const char *emulator,
                                const char *archStr,
                                const char *machine,
                                const char *virttypeStr,
                                const char *xmlCPU,
                                unsigned int flags)
{
    int ret = VIR_CPU_COMPARE_ERROR;
    virQEMUDriverPtr driver = conn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    g_autoptr(virQEMUCaps) qemuCaps = NULL;
    bool failIncompatible;
    virCPUDefPtr hvCPU;
    virCPUDefPtr cpu = NULL;
    virArch arch;
    virDomainVirtType virttype;

    virCheckFlags(VIR_CONNECT_COMPARE_CPU_FAIL_INCOMPATIBLE,
                  VIR_CPU_COMPARE_ERROR);

    if (virConnectCompareHypervisorCPUEnsureACL(conn) < 0)
        goto cleanup;

    failIncompatible = !!(flags & VIR_CONNECT_COMPARE_CPU_FAIL_INCOMPATIBLE);

    qemuCaps = virQEMUCapsCacheLookupDefault(driver->qemuCapsCache,
                                             emulator,
                                             archStr,
                                             virttypeStr,
                                             machine,
                                             &arch, &virttype, NULL);
    if (!qemuCaps)
        goto cleanup;

    hvCPU = virQEMUCapsGetHostModel(qemuCaps, virttype,
                                    VIR_QEMU_CAPS_HOST_CPU_REPORTED);

    if (!hvCPU || hvCPU->fallback != VIR_CPU_FALLBACK_FORBID) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("QEMU '%s' does not support reporting CPU model for "
                         "virttype '%s'"),
                       virQEMUCapsGetBinary(qemuCaps),
                       virDomainVirtTypeToString(virttype));
        goto cleanup;
    }

    if (ARCH_IS_X86(arch)) {
        ret = virCPUCompareXML(arch, hvCPU, xmlCPU, failIncompatible);
    } else if (ARCH_IS_S390(arch) &&
               virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_COMPARISON)) {
        if (virCPUDefParseXMLString(xmlCPU, VIR_CPU_TYPE_AUTO, &cpu) < 0)
            goto cleanup;

        ret = qemuConnectCPUModelComparison(qemuCaps, cfg->libDir,
                                            cfg->user, cfg->group,
                                            hvCPU, cpu, failIncompatible);
    } else {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("comparing with the hypervisor CPU is not supported "
                         "for arch %s"), virArchToString(arch));
    }

 cleanup:
    virCPUDefFree(cpu);
    return ret;
}


static char *
qemuConnectBaselineCPU(virConnectPtr conn G_GNUC_UNUSED,
                       const char **xmlCPUs,
                       unsigned int ncpus,
                       unsigned int flags)
{
    virCPUDefPtr *cpus = NULL;
    virCPUDefPtr baseline = NULL;
    virCPUDefPtr cpu = NULL;
    char *cpustr = NULL;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES |
                  VIR_CONNECT_BASELINE_CPU_MIGRATABLE, NULL);

    if (virConnectBaselineCPUEnsureACL(conn) < 0)
        goto cleanup;

    if (!(cpus = virCPUDefListParse(xmlCPUs, ncpus, VIR_CPU_TYPE_HOST)))
        goto cleanup;

    if (!(baseline = virCPUBaseline(VIR_ARCH_NONE, cpus, ncpus, NULL, NULL,
                                    !!(flags & VIR_CONNECT_BASELINE_CPU_MIGRATABLE))))
        goto cleanup;

    if (!(cpu = virCPUDefCopyWithoutModel(baseline)))
        goto cleanup;

    if (virCPUDefCopyModelFilter(cpu, baseline, false,
                                 virQEMUCapsCPUFilterFeatures,
                                 &cpus[0]->arch) < 0)
        goto cleanup;

    if ((flags & VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES) &&
        virCPUExpandFeatures(cpus[0]->arch, cpu) < 0)
        goto cleanup;

    cpustr = virCPUDefFormat(cpu, NULL);

 cleanup:
    virCPUDefListFree(cpus);
    virCPUDefFree(baseline);
    virCPUDefFree(cpu);

    return cpustr;
}


/**
 * qemuConnectStealCPUModelFromInfo:
 *
 * Consumes @src and replaces the content of @dst with CPU model name and
 * features from @src. When this function returns (both with success or
 * failure), @src is freed.
 */
static int
qemuConnectStealCPUModelFromInfo(virCPUDefPtr dst,
                                 qemuMonitorCPUModelInfoPtr *src)
{
    qemuMonitorCPUModelInfoPtr info;
    size_t i;
    int ret = -1;

    virCPUDefFreeModel(dst);

    info = g_steal_pointer(&*src);
    dst->model = g_steal_pointer(&info->name);

    for (i = 0; i < info->nprops; i++) {
        char *name = info->props[i].name;

        if (info->props[i].type != QEMU_MONITOR_CPU_PROPERTY_BOOLEAN ||
            !info->props[i].value.boolean)
            continue;

        if (virCPUDefAddFeature(dst, name, VIR_CPU_FEATURE_REQUIRE) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    qemuMonitorCPUModelInfoFree(info);
    return ret;
}


static virCPUDefPtr
qemuConnectCPUModelBaseline(virQEMUCapsPtr qemuCaps,
                            const char *libDir,
                            uid_t runUid,
                            gid_t runGid,
                            bool expand_features,
                            virCPUDefPtr *cpus,
                            int ncpus)
{
    qemuProcessQMPPtr proc;
    virCPUDefPtr ret = NULL;
    virCPUDefPtr baseline = NULL;
    qemuMonitorCPUModelInfoPtr result = NULL;
    size_t i;

    if (!(proc = qemuProcessQMPNew(virQEMUCapsGetBinary(qemuCaps),
                                   libDir, runUid, runGid, false)))
        goto cleanup;

    if (qemuProcessQMPStart(proc) < 0)
        goto cleanup;

    if (VIR_ALLOC(baseline) < 0)
        goto cleanup;

    if (virCPUDefCopyModel(baseline, cpus[0], false))
        goto cleanup;

    for (i = 1; i < ncpus; i++) {
        if (qemuMonitorGetCPUModelBaseline(proc->mon, baseline,
                                           cpus[i], &result) < 0)
            goto cleanup;

        if (qemuConnectStealCPUModelFromInfo(baseline, &result) < 0)
            goto cleanup;
    }

    if (expand_features) {
        if (qemuMonitorGetCPUModelExpansion(proc->mon,
                                            QEMU_MONITOR_CPU_MODEL_EXPANSION_FULL,
                                            baseline, true, false, &result) < 0)
            goto cleanup;

        if (qemuConnectStealCPUModelFromInfo(baseline, &result) < 0)
            goto cleanup;
    }

    ret = g_steal_pointer(&baseline);

 cleanup:
    qemuProcessQMPFree(proc);
    virCPUDefFree(baseline);
    return ret;
}


static char *
qemuConnectBaselineHypervisorCPU(virConnectPtr conn,
                                 const char *emulator,
                                 const char *archStr,
                                 const char *machine,
                                 const char *virttypeStr,
                                 const char **xmlCPUs,
                                 unsigned int ncpus,
                                 unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    virCPUDefPtr *cpus = NULL;
    g_autoptr(virQEMUCaps) qemuCaps = NULL;
    virArch arch;
    virDomainVirtType virttype;
    g_autoptr(virDomainCapsCPUModels) cpuModels = NULL;
    bool migratable;
    virCPUDefPtr cpu = NULL;
    char *cpustr = NULL;
    char **features = NULL;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES |
                  VIR_CONNECT_BASELINE_CPU_MIGRATABLE, NULL);

    if (virConnectBaselineHypervisorCPUEnsureACL(conn) < 0)
        goto cleanup;

    migratable = !!(flags & VIR_CONNECT_BASELINE_CPU_MIGRATABLE);

    if (!(cpus = virCPUDefListParse(xmlCPUs, ncpus, VIR_CPU_TYPE_AUTO)))
        goto cleanup;

    qemuCaps = virQEMUCapsCacheLookupDefault(driver->qemuCapsCache,
                                             emulator,
                                             archStr,
                                             virttypeStr,
                                             machine,
                                             &arch, &virttype, NULL);
    if (!qemuCaps)
        goto cleanup;

    if (!(cpuModels = virQEMUCapsGetCPUModels(qemuCaps, virttype, NULL, NULL)) ||
        cpuModels->nmodels == 0) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("QEMU '%s' does not support any CPU models for "
                         "virttype '%s'"),
                       virQEMUCapsGetBinary(qemuCaps),
                       virDomainVirtTypeToString(virttype));
        goto cleanup;
    }

    if (ARCH_IS_X86(arch)) {
        int rc = virQEMUCapsGetCPUFeatures(qemuCaps, virttype,
                                           migratable, &features);
        if (rc < 0)
            goto cleanup;
        if (features && rc == 0) {
            /* We got only migratable features from QEMU if we asked for them,
             * no further filtering in virCPUBaseline is desired. */
            migratable = false;
        }

        if (!(cpu = virCPUBaseline(arch, cpus, ncpus, cpuModels,
                                   (const char **)features, migratable)))
            goto cleanup;
    } else if (ARCH_IS_S390(arch) &&
               virQEMUCapsGet(qemuCaps, QEMU_CAPS_QUERY_CPU_MODEL_BASELINE)) {
        bool expand_features = (flags & VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES);

        if (!(cpu = qemuConnectCPUModelBaseline(qemuCaps, cfg->libDir,
                                                cfg->user, cfg->group,
                                                expand_features, cpus, ncpus)))
            goto cleanup;
    } else {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("computing baseline hypervisor CPU is not supported "
                         "for arch %s"), virArchToString(arch));
        goto cleanup;
    }

    cpu->fallback = VIR_CPU_FALLBACK_FORBID;

    if ((flags & VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES) &&
        virCPUExpandFeatures(arch, cpu) < 0)
        goto cleanup;

    cpustr = virCPUDefFormat(cpu, NULL);

 cleanup:
    virCPUDefListFree(cpus);
    virCPUDefFree(cpu);
    virStringListFree(features);

    return cpustr;
}


static int
qemuDomainGetJobInfoMigrationStats(virQEMUDriverPtr driver,
                                   virDomainObjPtr vm,
                                   qemuDomainJobInfoPtr jobInfo)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool events = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_MIGRATION_EVENT);

    if (jobInfo->status == QEMU_DOMAIN_JOB_STATUS_ACTIVE ||
        jobInfo->status == QEMU_DOMAIN_JOB_STATUS_MIGRATING ||
        jobInfo->status == QEMU_DOMAIN_JOB_STATUS_QEMU_COMPLETED ||
        jobInfo->status == QEMU_DOMAIN_JOB_STATUS_POSTCOPY) {
        if (events &&
            jobInfo->status != QEMU_DOMAIN_JOB_STATUS_ACTIVE &&
            qemuMigrationAnyFetchStats(driver, vm, QEMU_ASYNC_JOB_NONE,
                                       jobInfo, NULL) < 0)
            return -1;

        if (jobInfo->status == QEMU_DOMAIN_JOB_STATUS_ACTIVE &&
            jobInfo->statsType == QEMU_DOMAIN_JOB_STATS_TYPE_MIGRATION &&
            qemuMigrationSrcFetchMirrorStats(driver, vm, QEMU_ASYNC_JOB_NONE,
                                             jobInfo) < 0)
            return -1;

        if (qemuDomainJobInfoUpdateTime(jobInfo) < 0)
            return -1;
    }

    return 0;
}


static int
qemuDomainGetJobInfoDumpStats(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              qemuDomainJobInfoPtr jobInfo)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuMonitorDumpStats stats = { 0 };
    int rc;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        return -1;

    rc = qemuMonitorQueryDump(priv->mon, &stats);

    if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
        return -1;

    jobInfo->stats.dump = stats;

    if (qemuDomainJobInfoUpdateTime(jobInfo) < 0)
        return -1;

    switch (jobInfo->stats.dump.status) {
    case QEMU_MONITOR_DUMP_STATUS_NONE:
    case QEMU_MONITOR_DUMP_STATUS_FAILED:
    case QEMU_MONITOR_DUMP_STATUS_LAST:
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("dump query failed, status=%d"),
                       jobInfo->stats.dump.status);
        return -1;
        break;

    case QEMU_MONITOR_DUMP_STATUS_ACTIVE:
        jobInfo->status = QEMU_DOMAIN_JOB_STATUS_ACTIVE;
        VIR_DEBUG("dump active, bytes written='%llu' remaining='%llu'",
                  jobInfo->stats.dump.completed,
                  jobInfo->stats.dump.total -
                  jobInfo->stats.dump.completed);
        break;

    case QEMU_MONITOR_DUMP_STATUS_COMPLETED:
        jobInfo->status = QEMU_DOMAIN_JOB_STATUS_COMPLETED;
        VIR_DEBUG("dump completed, bytes written='%llu'",
                  jobInfo->stats.dump.completed);
        break;
    }

    return 0;
}


static int
qemuDomainGetJobStatsInternal(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              bool completed,
                              qemuDomainJobInfoPtr jobInfo)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    if (completed) {
        if (priv->job.completed && !priv->job.current)
            *jobInfo = *priv->job.completed;
        else
            jobInfo->status = QEMU_DOMAIN_JOB_STATUS_NONE;

        return 0;
    }

    if (priv->job.asyncJob == QEMU_ASYNC_JOB_MIGRATION_IN) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
               _("migration statistics are available only on "
                 "the source host"));
        return -1;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        return -1;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    if (!priv->job.current) {
        jobInfo->status = QEMU_DOMAIN_JOB_STATUS_NONE;
        ret = 0;
        goto cleanup;
    }
    *jobInfo = *priv->job.current;

    switch (jobInfo->statsType) {
    case QEMU_DOMAIN_JOB_STATS_TYPE_MIGRATION:
    case QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP:
        if (qemuDomainGetJobInfoMigrationStats(driver, vm, jobInfo) < 0)
            goto cleanup;
        break;

    case QEMU_DOMAIN_JOB_STATS_TYPE_MEMDUMP:
        if (qemuDomainGetJobInfoDumpStats(driver, vm, jobInfo) < 0)
            goto cleanup;
        break;

    case QEMU_DOMAIN_JOB_STATS_TYPE_BACKUP:
        if (qemuBackupGetJobInfoStats(driver, vm, jobInfo) < 0)
            goto cleanup;
        break;

    case QEMU_DOMAIN_JOB_STATS_TYPE_NONE:
        break;
    }

    ret = 0;

 cleanup:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int
qemuDomainGetJobInfo(virDomainPtr dom,
                     virDomainJobInfoPtr info)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuDomainJobInfo jobInfo;
    virDomainObjPtr vm;
    int ret = -1;

    memset(info, 0, sizeof(*info));

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetJobInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainGetJobStatsInternal(driver, vm, false, &jobInfo) < 0)
        goto cleanup;

    if (jobInfo.status == QEMU_DOMAIN_JOB_STATUS_NONE) {
        ret = 0;
        goto cleanup;
    }

    ret = qemuDomainJobInfoToInfo(&jobInfo, info);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainGetJobStats(virDomainPtr dom,
                      int *type,
                      virTypedParameterPtr *params,
                      int *nparams,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    qemuDomainJobInfo jobInfo;
    bool completed = !!(flags & VIR_DOMAIN_JOB_STATS_COMPLETED);
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_JOB_STATS_COMPLETED |
                  VIR_DOMAIN_JOB_STATS_KEEP_COMPLETED, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetJobStatsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    priv = vm->privateData;
    if (qemuDomainGetJobStatsInternal(driver, vm, completed, &jobInfo) < 0)
        goto cleanup;

    if (jobInfo.status == QEMU_DOMAIN_JOB_STATUS_NONE) {
        *type = VIR_DOMAIN_JOB_NONE;
        *params = NULL;
        *nparams = 0;
        ret = 0;
        goto cleanup;
    }

    ret = qemuDomainJobInfoToParams(&jobInfo, type, params, nparams);

    if (completed && ret == 0 && !(flags & VIR_DOMAIN_JOB_STATS_KEEP_COMPLETED))
        VIR_FREE(priv->job.completed);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainAbortJobMigration(virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret;

    VIR_DEBUG("Cancelling migration job at client request");

    qemuDomainObjAbortAsyncJob(vm);
    qemuDomainObjEnterMonitor(priv->driver, vm);
    ret = qemuMonitorMigrateCancel(priv->mon);
    if (qemuDomainObjExitMonitor(priv->driver, vm) < 0)
        ret = -1;

    return ret;
}


static int qemuDomainAbortJob(virDomainPtr dom)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    int reason;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainAbortJobEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_ABORT) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    switch (priv->job.asyncJob) {
    case QEMU_ASYNC_JOB_NONE:
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("no job is active on the domain"));
        break;

    case QEMU_ASYNC_JOB_MIGRATION_IN:
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot abort incoming migration;"
                         " use virDomainDestroy instead"));
        break;

    case QEMU_ASYNC_JOB_START:
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot abort VM start;"
                         " use virDomainDestroy instead"));
        break;

    case QEMU_ASYNC_JOB_MIGRATION_OUT:
        if ((priv->job.current->status == QEMU_DOMAIN_JOB_STATUS_POSTCOPY ||
             (virDomainObjGetState(vm, &reason) == VIR_DOMAIN_PAUSED &&
              reason == VIR_DOMAIN_PAUSED_POSTCOPY))) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot abort migration in post-copy mode"));
            goto endjob;
        }

        ret = qemuDomainAbortJobMigration(vm);
        break;

    case QEMU_ASYNC_JOB_SAVE:
        ret = qemuDomainAbortJobMigration(vm);
        break;

    case QEMU_ASYNC_JOB_DUMP:
        if (priv->job.apiFlags & VIR_DUMP_MEMORY_ONLY) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("cannot abort memory-only dump"));
            goto endjob;
        }

        ret = qemuDomainAbortJobMigration(vm);
        break;

    case QEMU_ASYNC_JOB_SNAPSHOT:
        ret = qemuDomainAbortJobMigration(vm);
        break;

    case QEMU_ASYNC_JOB_BACKUP:
        qemuBackupJobCancelBlockjobs(vm, priv->backup, true, QEMU_ASYNC_JOB_NONE);
        ret = 0;
        break;

    case QEMU_ASYNC_JOB_LAST:
    default:
        virReportEnumRangeError(qemuDomainAsyncJob, priv->job.asyncJob);
        break;
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainMigrateSetMaxDowntime(virDomainPtr dom,
                                unsigned long long downtime,
                                unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigrateSetMaxDowntimeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MIGRATION_OP) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    VIR_DEBUG("Setting migration downtime to %llums", downtime);
    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorSetMigrationDowntime(priv->mon, downtime);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainMigrateGetMaxDowntime(virDomainPtr dom,
                                unsigned long long *downtime,
                                unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuMigrationParamsPtr migParams = NULL;
    int ret = -1;
    int rc;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainMigrateGetMaxDowntimeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (qemuMigrationParamsFetch(driver, vm, QEMU_ASYNC_JOB_NONE,
                                 &migParams) < 0)
        goto endjob;

    if ((rc = qemuMigrationParamsGetULL(migParams,
                                        QEMU_MIGRATION_PARAM_DOWNTIME_LIMIT,
                                        downtime)) < 0) {
        goto endjob;
    }

    if (rc == 1) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Querying migration downtime is not supported by "
                         "QEMU binary"));
        goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    qemuMigrationParamsFree(migParams);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainMigrateGetCompressionCache(virDomainPtr dom,
                                     unsigned long long *cacheSize,
                                     unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigrateGetCompressionCacheEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    if (!qemuMigrationCapsGet(vm, QEMU_MIGRATION_CAP_XBZRLE)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("Compressed migration is not supported by "
                         "QEMU binary"));
        goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    ret = qemuMonitorGetMigrationCacheSize(priv->mon, cacheSize);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainMigrateSetCompressionCache(virDomainPtr dom,
                                     unsigned long long cacheSize,
                                     unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigrateSetCompressionCacheEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MIGRATION_OP) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    if (!qemuMigrationCapsGet(vm, QEMU_MIGRATION_CAP_XBZRLE)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("Compressed migration is not supported by "
                         "QEMU binary"));
        goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    VIR_DEBUG("Setting compression cache to %llu B", cacheSize);
    ret = qemuMonitorSetMigrationCacheSize(priv->mon, cacheSize);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainMigrateSetMaxSpeed(virDomainPtr dom,
                             unsigned long bandwidth,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    bool postcopy = !!(flags & VIR_DOMAIN_MIGRATE_MAX_SPEED_POSTCOPY);
    g_autoptr(qemuMigrationParams) migParams = NULL;
    unsigned long long max;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_MIGRATE_MAX_SPEED_POSTCOPY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainMigrateSetMaxSpeedEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (postcopy)
        max = ULLONG_MAX / 1024 / 1024;
    else
        max = QEMU_DOMAIN_MIG_BANDWIDTH_MAX;

    if (bandwidth > max) {
        virReportError(VIR_ERR_OVERFLOW,
                       _("bandwidth must be less than %llu"), max + 1);
        goto cleanup;
    }

    if (!postcopy && !virDomainObjIsActive(vm)) {
        priv->migMaxBandwidth = bandwidth;
        ret = 0;
        goto cleanup;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MIGRATION_OP) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    VIR_DEBUG("Setting migration bandwidth to %luMbs", bandwidth);

    if (postcopy) {
        if (!(migParams = qemuMigrationParamsNew()))
            goto endjob;

        if (qemuMigrationParamsSetULL(migParams,
                                      QEMU_MIGRATION_PARAM_MAX_POSTCOPY_BANDWIDTH,
                                      bandwidth * 1024 * 1024) < 0)
            goto endjob;

        if (qemuMigrationParamsApply(driver, vm, QEMU_ASYNC_JOB_NONE,
                                     migParams) < 0)
            goto endjob;
    } else {
        int rc;

        qemuDomainObjEnterMonitor(driver, vm);
        rc = qemuMonitorSetMigrationSpeed(priv->mon, bandwidth);
        if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
            goto endjob;

        priv->migMaxBandwidth = bandwidth;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainMigrationGetPostcopyBandwidth(virQEMUDriverPtr driver,
                                        virDomainObjPtr vm,
                                        unsigned long *bandwidth)
{
    g_autoptr(qemuMigrationParams) migParams = NULL;
    unsigned long long bw;
    int rc;
    int ret = -1;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        return -1;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    if (qemuMigrationParamsFetch(driver, vm, QEMU_ASYNC_JOB_NONE,
                                 &migParams) < 0)
        goto cleanup;

    if ((rc = qemuMigrationParamsGetULL(migParams,
                                        QEMU_MIGRATION_PARAM_MAX_POSTCOPY_BANDWIDTH,
                                        &bw)) < 0)
        goto cleanup;

    if (rc == 1) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("querying maximum post-copy migration speed is "
                         "not supported by QEMU binary"));
        goto cleanup;
    }

    /* QEMU reports B/s while we use MiB/s */
    bw /= 1024 * 1024;

    if (bw > ULONG_MAX) {
        virReportError(VIR_ERR_OVERFLOW,
                       _("bandwidth %llu is greater than %lu which is the "
                         "maximum value supported by this API"),
                       bw, ULONG_MAX);
        goto cleanup;
    }

    *bandwidth = bw;
    ret = 0;

 cleanup:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int
qemuDomainMigrateGetMaxSpeed(virDomainPtr dom,
                             unsigned long *bandwidth,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    bool postcopy = !!(flags & VIR_DOMAIN_MIGRATE_MAX_SPEED_POSTCOPY);
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_MIGRATE_MAX_SPEED_POSTCOPY, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainMigrateGetMaxSpeedEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (postcopy) {
        if (qemuDomainMigrationGetPostcopyBandwidth(driver, vm, bandwidth) < 0)
            goto cleanup;
    } else {
        *bandwidth = priv->migMaxBandwidth;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainMigrateStartPostCopy(virDomainPtr dom,
                               unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainMigrateStartPostCopyEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MIGRATION_OP) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    if (priv->job.asyncJob != QEMU_ASYNC_JOB_MIGRATION_OUT) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("post-copy can only be started while "
                         "outgoing migration is in progress"));
        goto endjob;
    }

    if (!(priv->job.apiFlags & VIR_MIGRATE_POSTCOPY)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("switching to post-copy requires migration to be "
                         "started with VIR_MIGRATE_POSTCOPY flag"));
        goto endjob;
    }

    VIR_DEBUG("Starting post-copy");
    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorMigrateStartPostCopy(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/* Return -1 if request is not sent to agent due to misconfig, -2 if request
 * is sent but failed, and number of frozen filesystems on success. If -2 is
 * returned, FSThaw should be called revert the quiesced status. */
static int
qemuDomainSnapshotFSFreeze(virDomainObjPtr vm,
                           const char **mountpoints,
                           unsigned int nmountpoints)
{
    qemuAgentPtr agent;
    int frozen;

    if (!qemuDomainAgentAvailable(vm, true))
        return -1;

    agent = qemuDomainObjEnterAgent(vm);
    frozen = qemuAgentFSFreeze(agent, mountpoints, nmountpoints);
    qemuDomainObjExitAgent(vm, agent);
    return frozen < 0 ? -2 : frozen;
}


/* Return -1 on error, otherwise number of thawed filesystems. */
static int
qemuDomainSnapshotFSThaw(virDomainObjPtr vm,
                         bool report)
{
    qemuAgentPtr agent;
    int thawed;
    virErrorPtr err = NULL;

    if (!qemuDomainAgentAvailable(vm, report))
        return -1;

    agent = qemuDomainObjEnterAgent(vm);
    if (!report)
        virErrorPreserveLast(&err);
    thawed = qemuAgentFSThaw(agent);
    qemuDomainObjExitAgent(vm, agent);

    virErrorRestore(&err);

    return thawed;
}


/* The domain is expected to be locked and inactive. */
static int
qemuDomainSnapshotCreateInactiveInternal(virQEMUDriverPtr driver,
                                         virDomainObjPtr vm,
                                         virDomainMomentObjPtr snap)
{
    return qemuDomainSnapshotForEachQcow2(driver, vm, snap, "-c", false);
}


/* The domain is expected to be locked and inactive. */
static int
qemuDomainSnapshotCreateInactiveExternal(virQEMUDriverPtr driver,
                                         virDomainObjPtr vm,
                                         virDomainMomentObjPtr snap,
                                         bool reuse)
{
    size_t i;
    virDomainSnapshotDiskDefPtr snapdisk;
    virDomainDiskDefPtr defdisk;
    virCommandPtr cmd = NULL;
    const char *qemuImgPath;
    virBitmapPtr created = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    int ret = -1;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    virDomainSnapshotDefPtr snapdef = virDomainSnapshotObjGetDef(snap);

    if (!(qemuImgPath = qemuFindQemuImgBinary(driver)))
        goto cleanup;

    if (!(created = virBitmapNew(snapdef->ndisks)))
        goto cleanup;

    /* If reuse is true, then qemuDomainSnapshotPrepare already
     * ensured that the new files exist, and it was up to the user to
     * create them correctly.  */
    for (i = 0; i < snapdef->ndisks && !reuse; i++) {
        snapdisk = &(snapdef->disks[i]);
        defdisk = snapdef->parent.dom->disks[snapdisk->idx];
        if (snapdisk->snapshot != VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL)
            continue;

        if (!snapdisk->src->format)
            snapdisk->src->format = VIR_STORAGE_FILE_QCOW2;

        /* creates cmd line args: qemu-img create -f qcow2 -o */
        if (!(cmd = virCommandNewArgList(qemuImgPath,
                                         "create",
                                         "-f",
                                         virStorageFileFormatTypeToString(snapdisk->src->format),
                                         "-o",
                                         NULL)))
            goto cleanup;

        /* adds cmd line arg: backing_fmt=format,backing_file=/path/to/backing/file */
        virBufferAsprintf(&buf, "backing_fmt=%s,backing_file=",
                          virStorageFileFormatTypeToString(defdisk->src->format));
        virQEMUBuildBufferEscapeComma(&buf, defdisk->src->path);
        virCommandAddArgBuffer(cmd, &buf);

        /* adds cmd line args: /path/to/target/file */
        virQEMUBuildBufferEscapeComma(&buf, snapdisk->src->path);
        virCommandAddArgBuffer(cmd, &buf);

        /* If the target does not exist, we're going to create it possibly */
        if (!virFileExists(snapdisk->src->path))
            ignore_value(virBitmapSetBit(created, i));

        if (virCommandRun(cmd, NULL) < 0)
            goto cleanup;

        virCommandFree(cmd);
        cmd = NULL;
    }

    /* update disk definitions */
    for (i = 0; i < snapdef->ndisks; i++) {
        g_autoptr(virStorageSource) newsrc = NULL;

        snapdisk = &(snapdef->disks[i]);
        defdisk = vm->def->disks[snapdisk->idx];

        if (snapdisk->snapshot != VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL)
            continue;

        if (!(newsrc = virStorageSourceCopy(snapdisk->src, false)))
            goto cleanup;

        if (virStorageSourceInitChainElement(newsrc, defdisk->src, false) < 0)
            goto cleanup;

        if (virStorageSourceHasBacking(defdisk->src)) {
            defdisk->src->readonly = true;
            newsrc->backingStore = g_steal_pointer(&defdisk->src);
        } else {
            virObjectUnref(defdisk->src);
        }

        defdisk->src = g_steal_pointer(&newsrc);
    }

    if (virDomainDefSave(vm->def, driver->xmlopt, cfg->configDir) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virBufferFreeAndReset(&buf);
    virCommandFree(cmd);

    /* unlink images if creation has failed */
    if (ret < 0 && created) {
        ssize_t bit = -1;
        while ((bit = virBitmapNextSetBit(created, bit)) >= 0) {
            snapdisk = &(snapdef->disks[bit]);
            if (unlink(snapdisk->src->path) < 0)
                VIR_WARN("Failed to remove snapshot image '%s'",
                         snapdisk->src->path);
        }
    }
    virBitmapFree(created);

    return ret;
}


/* The domain is expected to be locked and active. */
static int
qemuDomainSnapshotCreateActiveInternal(virQEMUDriverPtr driver,
                                       virDomainObjPtr vm,
                                       virDomainMomentObjPtr snap,
                                       unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virObjectEventPtr event = NULL;
    bool resume = false;
    virDomainSnapshotDefPtr snapdef = virDomainSnapshotObjGetDef(snap);
    int ret = -1;

    if (!qemuMigrationSrcIsAllowed(driver, vm, false, 0))
        goto cleanup;

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        /* savevm monitor command pauses the domain emitting an event which
         * confuses libvirt since it's not notified when qemu resumes the
         * domain. Thus we stop and start CPUs ourselves.
         */
        if (qemuProcessStopCPUs(driver, vm, VIR_DOMAIN_PAUSED_SAVE,
                                QEMU_ASYNC_JOB_SNAPSHOT) < 0)
            goto cleanup;

        resume = true;
        if (!virDomainObjIsActive(vm)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("guest unexpectedly quit"));
            goto cleanup;
        }
    }

    if (qemuDomainObjEnterMonitorAsync(driver, vm,
                                       QEMU_ASYNC_JOB_SNAPSHOT) < 0) {
        resume = false;
        goto cleanup;
    }

    ret = qemuMonitorCreateSnapshot(priv->mon, snap->def->name);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    if (ret < 0)
        goto cleanup;

    if (!(snapdef->cookie = (virObjectPtr) qemuDomainSaveCookieNew(vm)))
        goto cleanup;

    if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_HALT) {
        event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                         VIR_DOMAIN_EVENT_STOPPED_FROM_SNAPSHOT);
        qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FROM_SNAPSHOT,
                        QEMU_ASYNC_JOB_SNAPSHOT, 0);
        virDomainAuditStop(vm, "from-snapshot");
        resume = false;
    }

 cleanup:
    if (resume && virDomainObjIsActive(vm) &&
        qemuProcessStartCPUs(driver, vm,
                             VIR_DOMAIN_RUNNING_UNPAUSED,
                             QEMU_ASYNC_JOB_SNAPSHOT) < 0) {
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_API_ERROR);
        if (virGetLastErrorCode() == VIR_ERR_OK) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("resuming after snapshot failed"));
        }
    }

    virObjectEventStateQueue(driver->domainEventState, event);

    return ret;
}


static int
qemuDomainSnapshotPrepareDiskShared(virDomainSnapshotDiskDefPtr snapdisk,
                                    virDomainDiskDefPtr domdisk)
{
    if (!domdisk->src->shared || domdisk->src->readonly)
        return 0;

    if (!qemuBlockStorageSourceSupportsConcurrentAccess(snapdisk->src)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("shared access for disk '%s' requires use of "
                         "supported storage format"), domdisk->dst);
        return -1;
    }

    return 0;
}


static int
qemuDomainSnapshotPrepareDiskExternalInactive(virDomainSnapshotDiskDefPtr snapdisk,
                                              virDomainDiskDefPtr domdisk)
{
    int domDiskType = virStorageSourceGetActualType(domdisk->src);
    int snapDiskType = virStorageSourceGetActualType(snapdisk->src);

    switch ((virStorageType)domDiskType) {
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_FILE:
        break;

    case VIR_STORAGE_TYPE_NETWORK:
        switch ((virStorageNetProtocol) domdisk->src->protocol) {
        case VIR_STORAGE_NET_PROTOCOL_NONE:
        case VIR_STORAGE_NET_PROTOCOL_NBD:
        case VIR_STORAGE_NET_PROTOCOL_RBD:
        case VIR_STORAGE_NET_PROTOCOL_SHEEPDOG:
        case VIR_STORAGE_NET_PROTOCOL_GLUSTER:
        case VIR_STORAGE_NET_PROTOCOL_ISCSI:
        case VIR_STORAGE_NET_PROTOCOL_HTTP:
        case VIR_STORAGE_NET_PROTOCOL_HTTPS:
        case VIR_STORAGE_NET_PROTOCOL_FTP:
        case VIR_STORAGE_NET_PROTOCOL_FTPS:
        case VIR_STORAGE_NET_PROTOCOL_TFTP:
        case VIR_STORAGE_NET_PROTOCOL_SSH:
        case VIR_STORAGE_NET_PROTOCOL_VXHS:
        case VIR_STORAGE_NET_PROTOCOL_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("external inactive snapshots are not supported on "
                             "'network' disks using '%s' protocol"),
                           virStorageNetProtocolTypeToString(domdisk->src->protocol));
            return -1;
        }
        break;

    case VIR_STORAGE_TYPE_DIR:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("external inactive snapshots are not supported on "
                         "'%s' disks"), virStorageTypeToString(domDiskType));
        return -1;
    }

    switch ((virStorageType)snapDiskType) {
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_FILE:
        break;

    case VIR_STORAGE_TYPE_NETWORK:
    case VIR_STORAGE_TYPE_DIR:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("external inactive snapshots are not supported on "
                         "'%s' disks"), virStorageTypeToString(snapDiskType));
        return -1;
    }

    if (qemuDomainSnapshotPrepareDiskShared(snapdisk, domdisk) < 0)
        return -1;

    return 0;
}


static int
qemuDomainSnapshotPrepareDiskExternalActive(virDomainSnapshotDiskDefPtr snapdisk,
                                            virDomainDiskDefPtr domdisk,
                                            bool blockdev)
{
    int actualType = virStorageSourceGetActualType(snapdisk->src);

    if (domdisk->device == VIR_DOMAIN_DISK_DEVICE_LUN) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("external active snapshots are not supported on scsi "
                         "passthrough devices"));
        return -1;
    }

    switch ((virStorageType)actualType) {
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_FILE:
        break;

    case VIR_STORAGE_TYPE_NETWORK:
        /* defer all of the checking to either qemu or libvirt's blockdev code */
        if (blockdev)
            break;

        switch ((virStorageNetProtocol) snapdisk->src->protocol) {
        case VIR_STORAGE_NET_PROTOCOL_GLUSTER:
            break;

        case VIR_STORAGE_NET_PROTOCOL_NONE:
        case VIR_STORAGE_NET_PROTOCOL_NBD:
        case VIR_STORAGE_NET_PROTOCOL_RBD:
        case VIR_STORAGE_NET_PROTOCOL_SHEEPDOG:
        case VIR_STORAGE_NET_PROTOCOL_ISCSI:
        case VIR_STORAGE_NET_PROTOCOL_HTTP:
        case VIR_STORAGE_NET_PROTOCOL_HTTPS:
        case VIR_STORAGE_NET_PROTOCOL_FTP:
        case VIR_STORAGE_NET_PROTOCOL_FTPS:
        case VIR_STORAGE_NET_PROTOCOL_TFTP:
        case VIR_STORAGE_NET_PROTOCOL_SSH:
        case VIR_STORAGE_NET_PROTOCOL_VXHS:
        case VIR_STORAGE_NET_PROTOCOL_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("external active snapshots are not supported on "
                             "'network' disks using '%s' protocol"),
                           virStorageNetProtocolTypeToString(snapdisk->src->protocol));
            return -1;

        }
        break;

    case VIR_STORAGE_TYPE_DIR:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("external active snapshots are not supported on "
                         "'%s' disks"), virStorageTypeToString(actualType));
        return -1;
    }

    if (qemuDomainSnapshotPrepareDiskShared(snapdisk, domdisk) < 0)
        return -1;

    return 0;
}


static int
qemuDomainSnapshotPrepareDiskExternal(virDomainDiskDefPtr disk,
                                      virDomainSnapshotDiskDefPtr snapdisk,
                                      bool active,
                                      bool reuse,
                                      bool blockdev)
{
    struct stat st;
    int err;
    int rc;

    if (disk->src->readonly) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("external snapshot for readonly disk %s "
                         "is not supported"), disk->dst);
        return -1;
    }

    if (qemuTranslateSnapshotDiskSourcePool(snapdisk) < 0)
        return -1;

    if (!active) {
        if (virDomainDiskTranslateSourcePool(disk) < 0)
            return -1;

        if (qemuDomainSnapshotPrepareDiskExternalInactive(snapdisk, disk) < 0)
            return -1;
    } else {
        if (qemuDomainSnapshotPrepareDiskExternalActive(snapdisk, disk, blockdev) < 0)
            return -1;
    }

    if (virStorageSourceIsLocalStorage(snapdisk->src)) {
        if (virStorageFileInit(snapdisk->src) < 0)
            return -1;

        rc = virStorageFileStat(snapdisk->src, &st);
        err = errno;

        virStorageFileDeinit(snapdisk->src);

        if (rc < 0) {
            if (err != ENOENT) {
                virReportSystemError(err,
                                     _("unable to stat for disk %s: %s"),
                                     snapdisk->name, snapdisk->src->path);
                return -1;
            } else if (reuse) {
                virReportSystemError(err,
                                     _("missing existing file for disk %s: %s"),
                                     snapdisk->name, snapdisk->src->path);
                return -1;
            }
        } else if (!S_ISBLK(st.st_mode) && st.st_size && !reuse) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("external snapshot file for disk %s already "
                             "exists and is not a block device: %s"),
                           snapdisk->name, snapdisk->src->path);
            return -1;
        }
    }

    return 0;
}


static int
qemuDomainSnapshotPrepareDiskInternal(virDomainDiskDefPtr disk,
                                      bool active)
{
    int actualType;

    /* active disks are handled by qemu itself so no need to worry about those */
    if (active)
        return 0;

    if (virDomainDiskTranslateSourcePool(disk) < 0)
        return -1;

    actualType = virStorageSourceGetActualType(disk->src);

    switch ((virStorageType)actualType) {
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_FILE:
        return 0;

    case VIR_STORAGE_TYPE_NETWORK:
        switch ((virStorageNetProtocol) disk->src->protocol) {
        case VIR_STORAGE_NET_PROTOCOL_NONE:
        case VIR_STORAGE_NET_PROTOCOL_NBD:
        case VIR_STORAGE_NET_PROTOCOL_RBD:
        case VIR_STORAGE_NET_PROTOCOL_SHEEPDOG:
        case VIR_STORAGE_NET_PROTOCOL_GLUSTER:
        case VIR_STORAGE_NET_PROTOCOL_ISCSI:
        case VIR_STORAGE_NET_PROTOCOL_HTTP:
        case VIR_STORAGE_NET_PROTOCOL_HTTPS:
        case VIR_STORAGE_NET_PROTOCOL_FTP:
        case VIR_STORAGE_NET_PROTOCOL_FTPS:
        case VIR_STORAGE_NET_PROTOCOL_TFTP:
        case VIR_STORAGE_NET_PROTOCOL_SSH:
        case VIR_STORAGE_NET_PROTOCOL_VXHS:
        case VIR_STORAGE_NET_PROTOCOL_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("internal inactive snapshots are not supported on "
                             "'network' disks using '%s' protocol"),
                           virStorageNetProtocolTypeToString(disk->src->protocol));
            return -1;
        }
        break;

    case VIR_STORAGE_TYPE_DIR:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("internal inactive snapshots are not supported on "
                         "'%s' disks"), virStorageTypeToString(actualType));
        return -1;
    }

    return 0;
}


static int
qemuDomainSnapshotPrepare(virDomainObjPtr vm,
                          virDomainSnapshotDefPtr def,
                          unsigned int *flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    size_t i;
    bool active = virDomainObjIsActive(vm);
    bool reuse = (*flags & VIR_DOMAIN_SNAPSHOT_CREATE_REUSE_EXT) != 0;
    bool found_internal = false;
    bool forbid_internal = false;
    int external = 0;

    for (i = 0; i < def->ndisks; i++) {
        virDomainSnapshotDiskDefPtr disk = &def->disks[i];
        virDomainDiskDefPtr dom_disk = vm->def->disks[i];

        if (disk->snapshot != VIR_DOMAIN_SNAPSHOT_LOCATION_NONE &&
            qemuDomainDiskBlockJobIsActive(dom_disk))
            return -1;

        switch ((virDomainSnapshotLocation) disk->snapshot) {
        case VIR_DOMAIN_SNAPSHOT_LOCATION_INTERNAL:
            found_internal = true;

            if (def->state == VIR_DOMAIN_SNAPSHOT_DISK_SNAPSHOT && active) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("active qemu domains require external disk "
                                 "snapshots; disk %s requested internal"),
                               disk->name);
                return -1;
            }

            if (qemuDomainSnapshotPrepareDiskInternal(dom_disk,
                                                      active) < 0)
                return -1;

            if (vm->def->disks[i]->src->format > 0 &&
                vm->def->disks[i]->src->format != VIR_STORAGE_FILE_QCOW2) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("internal snapshot for disk %s unsupported "
                                 "for storage type %s"),
                               disk->name,
                               virStorageFileFormatTypeToString(
                                   vm->def->disks[i]->src->format));
                return -1;
            }
            break;

        case VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL:
            if (!disk->src->format) {
                disk->src->format = VIR_STORAGE_FILE_QCOW2;
            } else if (disk->src->format != VIR_STORAGE_FILE_QCOW2 &&
                       disk->src->format != VIR_STORAGE_FILE_QED) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("external snapshot format for disk %s "
                                 "is unsupported: %s"),
                               disk->name,
                               virStorageFileFormatTypeToString(disk->src->format));
                return -1;
            }

            if (qemuDomainSnapshotPrepareDiskExternal(dom_disk, disk,
                                                      active, reuse, blockdev) < 0)
                return -1;

            external++;
            break;

        case VIR_DOMAIN_SNAPSHOT_LOCATION_NONE:
            /* Remember seeing a disk that has snapshot disabled */
            if (!virStorageSourceIsEmpty(dom_disk->src) &&
                !dom_disk->src->readonly)
                forbid_internal = true;
            break;

        case VIR_DOMAIN_SNAPSHOT_LOCATION_DEFAULT:
        case VIR_DOMAIN_SNAPSHOT_LOCATION_LAST:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unexpected code path"));
            return -1;
        }
    }

    if (!found_internal && !external &&
        def->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_NONE) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("nothing selected for snapshot"));
        return -1;
    }

    /* internal snapshot requires a disk image to store the memory image to, and
     * also disks can't be excluded from an internal snapshot*/
    if ((def->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_INTERNAL && !found_internal) ||
        (found_internal && forbid_internal)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("internal and full system snapshots require all "
                         "disks to be selected for snapshot"));
        return -1;
    }

    /* disk snapshot requires at least one disk */
    if (def->state == VIR_DOMAIN_SNAPSHOT_DISK_SNAPSHOT && !external) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("disk-only snapshots require at least "
                         "one disk to be selected for snapshot"));
        return -1;
    }

    /* For now, we don't allow mixing internal and external disks.
     * XXX technically, we could mix internal and external disks for
     * offline snapshots */
    if ((found_internal && external) ||
         (def->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_INTERNAL && external) ||
         (def->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL && found_internal)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("mixing internal and external targets for a snapshot "
                         "is not yet supported"));
        return -1;
    }

    /* internal snapshots + pflash based loader have the following problems:
     * - if the variable store is raw, the snapshot fails
     * - allowing a qcow2 image as the varstore would make it eligible to receive
     *   the vmstate dump, which would make it huge
     * - offline snapshot would not snapshot the varstore at all
     *
     * Avoid the issues by forbidding internal snapshot with pflash completely.
     */
    if (found_internal &&
        virDomainDefHasOldStyleUEFI(vm->def)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("internal snapshots of a VM with pflash based "
                         "firmware are not supported"));
        return -1;
    }

    /* Alter flags to let later users know what we learned.  */
    if (external && !active)
        *flags |= VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY;

    return 0;
}


struct _qemuDomainSnapshotDiskData {
    virStorageSourcePtr src;
    bool initialized; /* @src was initialized in the storage driver */
    bool created; /* @src was created by the snapshot code */
    bool prepared; /* @src was prepared using qemuDomainStorageSourceAccessAllow */
    virDomainDiskDefPtr disk;
    char *relPath; /* relative path component to fill into original disk */
    qemuBlockStorageSourceChainDataPtr crdata;
    bool blockdevadded;

    virStorageSourcePtr persistsrc;
    virDomainDiskDefPtr persistdisk;
};

typedef struct _qemuDomainSnapshotDiskData qemuDomainSnapshotDiskData;
typedef qemuDomainSnapshotDiskData *qemuDomainSnapshotDiskDataPtr;


static void
qemuDomainSnapshotDiskCleanup(qemuDomainSnapshotDiskDataPtr data,
                              size_t ndata,
                              virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              qemuDomainAsyncJob asyncJob)
{
    virErrorPtr orig_err;
    size_t i;

    if (!data)
        return;

    virErrorPreserveLast(&orig_err);

    for (i = 0; i < ndata; i++) {
        /* on success of the snapshot the 'src' and 'persistsrc' properties will
         * be set to NULL by qemuDomainSnapshotDiskUpdateSource */
        if (data[i].src) {
            if (data[i].blockdevadded) {
                if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) == 0) {

                    qemuBlockStorageSourceAttachRollback(qemuDomainGetMonitor(vm),
                                                         data[i].crdata->srcdata[0]);
                    ignore_value(qemuDomainObjExitMonitor(driver, vm));
                }
            }

            if (data[i].created &&
                virStorageFileUnlink(data[i].src) < 0) {
                VIR_WARN("Unable to remove just-created %s",
                         NULLSTR(data[i].src->path));
            }

            if (data[i].initialized)
                virStorageFileDeinit(data[i].src);

            if (data[i].prepared)
                qemuDomainStorageSourceAccessRevoke(driver, vm, data[i].src);

            virObjectUnref(data[i].src);
        }
        virObjectUnref(data[i].persistsrc);
        VIR_FREE(data[i].relPath);
        qemuBlockStorageSourceChainDataFree(data[i].crdata);
    }

    VIR_FREE(data);
    virErrorRestore(&orig_err);
}


/**
 * qemuDomainSnapshotDiskBitmapsPropagate:
 *
 * This function propagates any active persistent bitmap present in the original
 * image into the new snapshot. This is necessary to keep tracking the changed
 * blocks in the active bitmaps as the backing file will become read-only.
 * We leave the original bitmap active as in cases when the overlay is
 * discarded (snapshot revert with abandoning the history) everything works as
 * expected.
 */
static int
qemuDomainSnapshotDiskBitmapsPropagate(qemuDomainSnapshotDiskDataPtr dd,
                                       virJSONValuePtr actions,
                                       virHashTablePtr blockNamedNodeData)
{
    qemuBlockNamedNodeDataPtr entry;
    size_t i;

    if (!(entry = virHashLookup(blockNamedNodeData, dd->disk->src->nodeformat)))
        return 0;

    for (i = 0; i < entry->nbitmaps; i++) {
        qemuBlockNamedNodeDataBitmapPtr bitmap = entry->bitmaps[i];

        /* we don't care about temporary, inconsistent, or disabled bitmaps */
        if (!bitmap->persistent || !bitmap->recording || bitmap->inconsistent)
            continue;

        if (qemuMonitorTransactionBitmapAdd(actions, dd->src->nodeformat,
                                            bitmap->name, true, false,
                                            bitmap->granularity) < 0)
            return -1;
    }

    return 0;
}


static int
qemuDomainSnapshotDiskPrepareOneBlockdev(virQEMUDriverPtr driver,
                                         virDomainObjPtr vm,
                                         qemuDomainSnapshotDiskDataPtr dd,
                                         virQEMUDriverConfigPtr cfg,
                                         bool reuse,
                                         virHashTablePtr blockNamedNodeData,
                                         qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autoptr(virStorageSource) terminator = NULL;
    int rc;

    /* create a terminator for the snapshot disks so that qemu does not try
     * to open them at first */
    if (!(terminator = virStorageSourceNew()))
        return -1;

    if (qemuDomainPrepareStorageSourceBlockdev(dd->disk, dd->src,
                                               priv, cfg) < 0)
        return -1;

    if (!(dd->crdata = qemuBuildStorageSourceChainAttachPrepareBlockdevTop(dd->src,
                                                                           terminator,
                                                                           priv->qemuCaps)))
        return -1;

    if (reuse) {
        if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
            return -1;

        rc = qemuBlockStorageSourceAttachApply(qemuDomainGetMonitor(vm),
                                               dd->crdata->srcdata[0]);

        if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
            return -1;
    } else {
        if (qemuBlockStorageSourceCreateDetectSize(blockNamedNodeData,
                                                   dd->src, dd->disk->src) < 0)
            return -1;

        if (qemuBlockStorageSourceCreate(vm, dd->src, dd->disk->src,
                                         NULL, dd->crdata->srcdata[0],
                                         asyncJob) < 0)
            return -1;
    }

    dd->blockdevadded = true;
    return 0;
}


static int
qemuDomainSnapshotDiskPrepareOne(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virQEMUDriverConfigPtr cfg,
                                 virDomainDiskDefPtr disk,
                                 virDomainSnapshotDiskDefPtr snapdisk,
                                 qemuDomainSnapshotDiskDataPtr dd,
                                 virHashTablePtr blockNamedNodeData,
                                 bool reuse,
                                 bool blockdev,
                                 qemuDomainAsyncJob asyncJob,
                                 virJSONValuePtr actions)
{
    virDomainDiskDefPtr persistdisk;
    bool supportsCreate;
    bool supportsBacking;

    dd->disk = disk;

    if (qemuDomainStorageSourceValidateDepth(disk->src, 1, disk->dst) < 0)
        return -1;

    if (!(dd->src = virStorageSourceCopy(snapdisk->src, false)))
        return -1;

    if (virStorageSourceInitChainElement(dd->src, dd->disk->src, false) < 0)
        return -1;

    /* modify disk in persistent definition only when the source is the same */
    if (vm->newDef &&
        (persistdisk = virDomainDiskByTarget(vm->newDef, dd->disk->dst)) &&
        virStorageSourceIsSameLocation(dd->disk->src, persistdisk->src)) {

        dd->persistdisk = persistdisk;

        if (!(dd->persistsrc = virStorageSourceCopy(dd->src, false)))
            return -1;

        if (virStorageSourceInitChainElement(dd->persistsrc,
                                             dd->persistdisk->src, false) < 0)
            return -1;
    }

    supportsCreate = virStorageFileSupportsCreate(dd->src);
    supportsBacking = virStorageFileSupportsBackingChainTraversal(dd->src);

    if (supportsCreate || supportsBacking) {
        if (qemuDomainStorageFileInit(driver, vm, dd->src, NULL) < 0)
            return -1;

        dd->initialized = true;

        /* relative backing store paths need to be updated so that relative
         * block commit still works */
        if (reuse) {
            if (supportsBacking) {
                g_autofree char *backingStoreStr = NULL;

                if (virStorageFileGetBackingStoreStr(dd->src, &backingStoreStr) < 0)
                    return -1;
                if (backingStoreStr != NULL) {
                    if (virStorageIsRelative(backingStoreStr))
                        dd->relPath = g_steal_pointer(&backingStoreStr);
                }
            }
        } else {
            /* pre-create the image file so that we can label it before handing it to qemu */
            if (supportsCreate && dd->src->type != VIR_STORAGE_TYPE_BLOCK) {
                if (virStorageFileCreate(dd->src) < 0) {
                    virReportSystemError(errno, _("failed to create image file '%s'"),
                                         NULLSTR(dd->src->path));
                    return -1;
                }
                dd->created = true;
            }
        }
    }

    /* set correct security, cgroup and locking options on the new image */
    if (qemuDomainStorageSourceAccessAllow(driver, vm, dd->src, false, true) < 0)
        return -1;

    dd->prepared = true;

    if (blockdev) {
        if (qemuDomainSnapshotDiskPrepareOneBlockdev(driver, vm, dd, cfg, reuse,
                                                     blockNamedNodeData, asyncJob) < 0)
            return -1;

        if (qemuDomainSnapshotDiskBitmapsPropagate(dd, actions, blockNamedNodeData) < 0)
            return -1;

        if (qemuBlockSnapshotAddBlockdev(actions, dd->disk, dd->src) < 0)
            return -1;
    } else {
        if (qemuBlockSnapshotAddLegacy(actions, dd->disk, dd->src, reuse) < 0)
            return -1;
    }

    return 0;
}


/**
 * qemuDomainSnapshotDiskPrepare:
 *
 * Collects and prepares a list of structures that hold information about disks
 * that are selected for the snapshot.
 */
static int
qemuDomainSnapshotDiskPrepare(virQEMUDriverPtr driver,
                              virDomainObjPtr vm,
                              virDomainMomentObjPtr snap,
                              virQEMUDriverConfigPtr cfg,
                              bool reuse,
                              bool blockdev,
                              virHashTablePtr blockNamedNodeData,
                              qemuDomainAsyncJob asyncJob,
                              qemuDomainSnapshotDiskDataPtr *rdata,
                              size_t *rndata,
                              virJSONValuePtr actions)
{
    size_t i;
    qemuDomainSnapshotDiskDataPtr data;
    size_t ndata = 0;
    virDomainSnapshotDefPtr snapdef = virDomainSnapshotObjGetDef(snap);
    int ret = -1;

    if (VIR_ALLOC_N(data, snapdef->ndisks) < 0)
        return -1;

    for (i = 0; i < snapdef->ndisks; i++) {
        if (snapdef->disks[i].snapshot == VIR_DOMAIN_SNAPSHOT_LOCATION_NONE)
            continue;

        if (qemuDomainSnapshotDiskPrepareOne(driver, vm, cfg, vm->def->disks[i],
                                             snapdef->disks + i,
                                             data + ndata++,
                                             blockNamedNodeData,
                                             reuse, blockdev,
                                             asyncJob,
                                             actions) < 0)
            goto cleanup;
    }

    *rdata = g_steal_pointer(&data);
    *rndata = ndata;
    ret = 0;

 cleanup:
    qemuDomainSnapshotDiskCleanup(data, ndata, driver, vm, asyncJob);
    return ret;
}


static void
qemuDomainSnapshotDiskUpdateSourceRenumber(virStorageSourcePtr src)
{
    virStorageSourcePtr next;
    unsigned int idx = 1;

    for (next = src->backingStore; virStorageSourceIsBacking(next); next = next->backingStore)
        next->id = idx++;
}


/**
 * qemuDomainSnapshotDiskUpdateSource:
 * @driver: QEMU driver
 * @vm: domain object
 * @dd: snapshot disk data object
 * @blockdev: -blockdev is in use for the VM
 *
 * Updates disk definition after a successful snapshot.
 */
static void
qemuDomainSnapshotDiskUpdateSource(virQEMUDriverPtr driver,
                                   virDomainObjPtr vm,
                                   qemuDomainSnapshotDiskDataPtr dd,
                                   bool blockdev)
{
    /* storage driver access won'd be needed */
    if (dd->initialized)
        virStorageFileDeinit(dd->src);

    if (qemuSecurityMoveImageMetadata(driver, vm, dd->disk->src, dd->src) < 0)
        VIR_WARN("Unable to move disk metadata on vm %s", vm->def->name);

    /* unlock the write lock on the original image as qemu will no longer write to it */
    virDomainLockImageDetach(driver->lockManager, vm, dd->disk->src);

    /* unlock also the new image if the VM is paused to follow the locking semantics */
    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_RUNNING)
        virDomainLockImageDetach(driver->lockManager, vm, dd->src);

    /* the old disk image is now readonly */
    dd->disk->src->readonly = true;

    dd->disk->src->relPath = g_steal_pointer(&dd->relPath);
    dd->src->backingStore = g_steal_pointer(&dd->disk->src);
    dd->disk->src = g_steal_pointer(&dd->src);

    /* fix numbering of disks */
    if (!blockdev)
        qemuDomainSnapshotDiskUpdateSourceRenumber(dd->disk->src);

    if (dd->persistdisk) {
        dd->persistdisk->src->readonly = true;
        dd->persistsrc->backingStore = g_steal_pointer(&dd->persistdisk->src);
        dd->persistdisk->src = g_steal_pointer(&dd->persistsrc);
    }
}


/* The domain is expected to be locked and active. */
static int
qemuDomainSnapshotCreateDiskActive(virQEMUDriverPtr driver,
                                   virDomainObjPtr vm,
                                   virDomainMomentObjPtr snap,
                                   unsigned int flags,
                                   virQEMUDriverConfigPtr cfg,
                                   qemuDomainAsyncJob asyncJob)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autoptr(virJSONValue) actions = NULL;
    int rc;
    int ret = -1;
    size_t i;
    bool reuse = (flags & VIR_DOMAIN_SNAPSHOT_CREATE_REUSE_EXT) != 0;
    qemuDomainSnapshotDiskDataPtr diskdata = NULL;
    size_t ndiskdata = 0;
    bool blockdev =  virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    g_autoptr(virHashTable) blockNamedNodeData = NULL;

    if (virDomainObjCheckActive(vm) < 0)
        return -1;

    if (!(actions = virJSONValueNewArray()))
        return -1;

    if (blockdev) {
        if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
            return -1;

        blockNamedNodeData = qemuMonitorBlockGetNamedNodeData(priv->mon);

        if (qemuDomainObjExitMonitor(driver, vm) < 0 || !blockNamedNodeData)
            return -1;
    }

    /* prepare a list of objects to use in the vm definition so that we don't
     * have to roll back later */
    if (qemuDomainSnapshotDiskPrepare(driver, vm, snap, cfg, reuse, blockdev,
                                      blockNamedNodeData, asyncJob,
                                      &diskdata, &ndiskdata, actions) < 0)
        goto cleanup;

    /* check whether there's anything to do */
    if (ndiskdata == 0) {
        ret = 0;
        goto cleanup;
    }

    if (qemuDomainObjEnterMonitorAsync(driver, vm, asyncJob) < 0)
        goto cleanup;

    rc = qemuMonitorTransaction(priv->mon, &actions);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        rc = -1;

    for (i = 0; i < ndiskdata; i++) {
        qemuDomainSnapshotDiskDataPtr dd = &diskdata[i];

        virDomainAuditDisk(vm, dd->disk->src, dd->src, "snapshot", rc >= 0);

        if (rc == 0)
            qemuDomainSnapshotDiskUpdateSource(driver, vm, dd, blockdev);
    }

    if (rc < 0)
        goto cleanup;

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0 ||
        (vm->newDef && virDomainDefSave(vm->newDef, driver->xmlopt,
                                        cfg->configDir) < 0))
        goto cleanup;

    ret = 0;

 cleanup:
    qemuDomainSnapshotDiskCleanup(diskdata, ndiskdata, driver, vm, asyncJob);
    return ret;
}


static int
qemuDomainSnapshotCreateActiveExternal(virQEMUDriverPtr driver,
                                       virDomainObjPtr vm,
                                       virDomainMomentObjPtr snap,
                                       virQEMUDriverConfigPtr cfg,
                                       unsigned int flags)
{
    virObjectEventPtr event;
    bool resume = false;
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autofree char *xml = NULL;
    virDomainSnapshotDefPtr snapdef = virDomainSnapshotObjGetDef(snap);
    bool memory = snapdef->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL;
    bool memory_unlink = false;
    int thaw = 0; /* 1 if freeze succeeded, -1 if freeze failed */
    bool pmsuspended = false;
    int compressed;
    g_autoptr(virCommand) compressor = NULL;
    virQEMUSaveDataPtr data = NULL;

    /* If quiesce was requested, then issue a freeze command, and a
     * counterpart thaw command when it is actually sent to agent.
     * The command will fail if the guest is paused or the guest agent
     * is not running, or is already quiesced.  */
    if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_QUIESCE) {
        int freeze;

        if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
            goto cleanup;

        if (virDomainObjCheckActive(vm) < 0) {
            qemuDomainObjEndAgentJob(vm);
            goto cleanup;
        }

        freeze = qemuDomainSnapshotFSFreeze(vm, NULL, 0);
        qemuDomainObjEndAgentJob(vm);

        if (freeze < 0) {
            /* the helper reported the error */
            if (freeze == -2)
                thaw = -1; /* the command is sent but agent failed */
            goto cleanup;
        }
        thaw = 1;
    }

    /* We need to track what state the guest is in, since taking the
     * snapshot may alter that state and we must restore it later.  */
    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_PMSUSPENDED) {
        pmsuspended = true;
    } else if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
        /* For full system external snapshots (those with memory), the guest
         * must pause (either by libvirt up front, or by qemu after
         * _LIVE converges). */
        if (memory)
            resume = true;

        if (memory && !(flags & VIR_DOMAIN_SNAPSHOT_CREATE_LIVE)) {
            if (qemuProcessStopCPUs(driver, vm, VIR_DOMAIN_PAUSED_SNAPSHOT,
                                    QEMU_ASYNC_JOB_SNAPSHOT) < 0)
                goto cleanup;

            if (!virDomainObjIsActive(vm)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("guest unexpectedly quit"));
                goto cleanup;
            }

            resume = true;
        }
    }

    /* do the memory snapshot if necessary */
    if (memory) {
        /* check if migration is possible */
        if (!qemuMigrationSrcIsAllowed(driver, vm, false, 0))
            goto cleanup;

        priv->job.current->statsType = QEMU_DOMAIN_JOB_STATS_TYPE_SAVEDUMP;

        /* allow the migration job to be cancelled or the domain to be paused */
        qemuDomainObjSetAsyncJobMask(vm, (QEMU_JOB_DEFAULT_MASK |
                                          JOB_MASK(QEMU_JOB_SUSPEND) |
                                          JOB_MASK(QEMU_JOB_MIGRATION_OP)));

        if ((compressed = qemuGetCompressionProgram(cfg->snapshotImageFormat,
                                                    &compressor,
                                                    "snapshot", false)) < 0)
            goto cleanup;

        if (!(xml = qemuDomainDefFormatLive(driver, priv->qemuCaps,
                                            vm->def, priv->origCPU,
                                            true, true)) ||
            !(snapdef->cookie = (virObjectPtr) qemuDomainSaveCookieNew(vm)))
            goto cleanup;

        if (!(data = virQEMUSaveDataNew(xml,
                                        (qemuDomainSaveCookiePtr) snapdef->cookie,
                                        resume, compressed, driver->xmlopt)))
            goto cleanup;
        xml = NULL;

        if ((ret = qemuDomainSaveMemory(driver, vm, snapdef->file, data,
                                        compressor, 0,
                                        QEMU_ASYNC_JOB_SNAPSHOT)) < 0)
            goto cleanup;

        /* the memory image was created, remove it on errors */
        memory_unlink = true;

        /* forbid any further manipulation */
        qemuDomainObjSetAsyncJobMask(vm, QEMU_JOB_DEFAULT_MASK);
    }

    /* the domain is now paused if a memory snapshot was requested */

    if ((ret = qemuDomainSnapshotCreateDiskActive(driver, vm, snap, flags, cfg,
                                                  QEMU_ASYNC_JOB_SNAPSHOT)) < 0)
        goto cleanup;

    /* the snapshot is complete now */
    if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_HALT) {
        event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_STOPPED,
                                         VIR_DOMAIN_EVENT_STOPPED_FROM_SNAPSHOT);
        qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FROM_SNAPSHOT,
                        QEMU_ASYNC_JOB_SNAPSHOT, 0);
        virDomainAuditStop(vm, "from-snapshot");
        resume = false;
        thaw = 0;
        virObjectEventStateQueue(driver->domainEventState, event);
    } else if (memory && pmsuspended) {
        /* qemu 1.3 is unable to save a domain in pm-suspended (S3)
         * state; so we must emit an event stating that it was
         * converted to paused.  */
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED,
                             VIR_DOMAIN_PAUSED_FROM_SNAPSHOT);
        event = virDomainEventLifecycleNewFromObj(vm, VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_FROM_SNAPSHOT);
        virObjectEventStateQueue(driver->domainEventState, event);
    }

    ret = 0;

 cleanup:
    if (resume && virDomainObjIsActive(vm) &&
        qemuProcessStartCPUs(driver, vm,
                             VIR_DOMAIN_RUNNING_UNPAUSED,
                             QEMU_ASYNC_JOB_SNAPSHOT) < 0) {
        event = virDomainEventLifecycleNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_API_ERROR);
        virObjectEventStateQueue(driver->domainEventState, event);
        if (virGetLastErrorCode() == VIR_ERR_OK) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("resuming after snapshot failed"));
        }

        ret = -1;
    }

    if (thaw != 0 &&
        qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) >= 0 &&
        virDomainObjIsActive(vm)) {
        if (qemuDomainSnapshotFSThaw(vm, ret == 0 && thaw > 0) < 0) {
            /* helper reported the error, if it was needed */
            if (thaw > 0)
                ret = -1;
        }

        qemuDomainObjEndAgentJob(vm);
    }

    virQEMUSaveDataFree(data);
    if (memory_unlink && ret < 0)
        unlink(snapdef->file);

    return ret;
}


static virDomainSnapshotPtr
qemuDomainSnapshotCreateXML(virDomainPtr domain,
                            const char *xmlDesc,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    g_autofree char *xml = NULL;
    virDomainMomentObjPtr snap = NULL;
    virDomainSnapshotPtr snapshot = NULL;
    virDomainMomentObjPtr current = NULL;
    bool update_current = true;
    bool redefine = flags & VIR_DOMAIN_SNAPSHOT_CREATE_REDEFINE;
    unsigned int parse_flags = VIR_DOMAIN_SNAPSHOT_PARSE_DISKS;
    int align_location = VIR_DOMAIN_SNAPSHOT_LOCATION_INTERNAL;
    bool align_match = true;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    qemuDomainObjPrivatePtr priv;
    virDomainSnapshotState state;
    g_autoptr(virDomainSnapshotDef) def = NULL;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_CREATE_REDEFINE |
                  VIR_DOMAIN_SNAPSHOT_CREATE_CURRENT |
                  VIR_DOMAIN_SNAPSHOT_CREATE_NO_METADATA |
                  VIR_DOMAIN_SNAPSHOT_CREATE_HALT |
                  VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY |
                  VIR_DOMAIN_SNAPSHOT_CREATE_REUSE_EXT |
                  VIR_DOMAIN_SNAPSHOT_CREATE_QUIESCE |
                  VIR_DOMAIN_SNAPSHOT_CREATE_ATOMIC |
                  VIR_DOMAIN_SNAPSHOT_CREATE_LIVE |
                  VIR_DOMAIN_SNAPSHOT_CREATE_VALIDATE, NULL);

    VIR_REQUIRE_FLAG_RET(VIR_DOMAIN_SNAPSHOT_CREATE_QUIESCE,
                         VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY,
                         NULL);
    VIR_EXCLUSIVE_FLAGS_RET(VIR_DOMAIN_SNAPSHOT_CREATE_LIVE,
                            VIR_DOMAIN_SNAPSHOT_CREATE_REDEFINE,
                            NULL);

    if ((redefine && !(flags & VIR_DOMAIN_SNAPSHOT_CREATE_CURRENT)) ||
        (flags & VIR_DOMAIN_SNAPSHOT_CREATE_NO_METADATA))
        update_current = false;
    if (redefine)
        parse_flags |= VIR_DOMAIN_SNAPSHOT_PARSE_REDEFINE;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSnapshotCreateXMLEnsureACL(domain->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainSupportsCheckpointsBlockjobs(vm) < 0)
        goto cleanup;

    if (!vm->persistent && (flags & VIR_DOMAIN_SNAPSHOT_CREATE_HALT)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot halt after transient domain snapshot"));
        goto cleanup;
    }
    if ((flags & VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY) ||
        !virDomainObjIsActive(vm))
        parse_flags |= VIR_DOMAIN_SNAPSHOT_PARSE_OFFLINE;

    if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_VALIDATE)
        parse_flags |= VIR_DOMAIN_SNAPSHOT_PARSE_VALIDATE;

    if (!(def = virDomainSnapshotDefParseString(xmlDesc, driver->xmlopt,
                                                priv->qemuCaps, NULL, parse_flags)))
        goto cleanup;

    /* reject snapshot names containing slashes or starting with dot as
     * snapshot definitions are saved in files named by the snapshot name */
    if (!(flags & VIR_DOMAIN_SNAPSHOT_CREATE_NO_METADATA)) {
        if (strchr(def->parent.name, '/')) {
            virReportError(VIR_ERR_XML_DETAIL,
                           _("invalid snapshot name '%s': "
                             "name can't contain '/'"),
                           def->parent.name);
            goto cleanup;
        }

        if (def->parent.name[0] == '.') {
            virReportError(VIR_ERR_XML_DETAIL,
                           _("invalid snapshot name '%s': "
                             "name can't start with '.'"),
                           def->parent.name);
            goto cleanup;
        }
    }

    /* reject the VIR_DOMAIN_SNAPSHOT_CREATE_LIVE flag where not supported */
    if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_LIVE &&
        (!virDomainObjIsActive(vm) ||
         def->memory != VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("live snapshot creation is supported only "
                         "during full system snapshots"));
        goto cleanup;
    }

    /* allow snapshots only in certain states */
    state = redefine ? def->state : vm->state.state;
    switch (state) {
        /* valid states */
    case VIR_DOMAIN_SNAPSHOT_RUNNING:
    case VIR_DOMAIN_SNAPSHOT_PAUSED:
    case VIR_DOMAIN_SNAPSHOT_SHUTDOWN:
    case VIR_DOMAIN_SNAPSHOT_SHUTOFF:
    case VIR_DOMAIN_SNAPSHOT_CRASHED:
        break;

    case VIR_DOMAIN_SNAPSHOT_DISK_SNAPSHOT:
        if (!redefine) {
            virReportError(VIR_ERR_INTERNAL_ERROR, _("Invalid domain state %s"),
                           virDomainSnapshotStateTypeToString(state));
            goto cleanup;
        }
        break;

    case VIR_DOMAIN_SNAPSHOT_PMSUSPENDED:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("qemu doesn't support taking snapshots of "
                         "PMSUSPENDED guests"));
        goto cleanup;

        /* invalid states */
    case VIR_DOMAIN_SNAPSHOT_NOSTATE:
    case VIR_DOMAIN_SNAPSHOT_BLOCKED: /* invalid state, unused in qemu */
    case VIR_DOMAIN_SNAPSHOT_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Invalid domain state %s"),
                       virDomainSnapshotStateTypeToString(state));
        goto cleanup;
    }

    /* We are going to modify the domain below. Internal snapshots would use
     * a regular job, so we need to set the job mask to disallow query as
     * 'savevm' blocks the monitor. External snapshot will then modify the
     * job mask appropriately. */
    if (qemuDomainObjBeginAsyncJob(driver, vm, QEMU_ASYNC_JOB_SNAPSHOT,
                                   VIR_DOMAIN_JOB_OPERATION_SNAPSHOT, flags) < 0)
        goto cleanup;

    qemuDomainObjSetAsyncJobMask(vm, QEMU_JOB_NONE);

    if (redefine) {
        if (virDomainSnapshotRedefinePrep(vm, &def, &snap,
                                          driver->xmlopt,
                                          flags) < 0)
            goto endjob;
    } else {
        /* Easiest way to clone inactive portion of vm->def is via
         * conversion in and back out of xml.  */
        if (!(xml = qemuDomainDefFormatLive(driver, priv->qemuCaps,
                                            vm->def, priv->origCPU,
                                            true, true)) ||
            !(def->parent.dom = virDomainDefParseString(xml, driver->xmlopt,
                                                        priv->qemuCaps,
                                                        VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                                        VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE)))
            goto endjob;

        if (vm->newDef) {
            def->parent.inactiveDom = virDomainDefCopy(vm->newDef,
                                                       driver->xmlopt, priv->qemuCaps, true);
            if (!def->parent.inactiveDom)
                goto endjob;
        }

        if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY) {
            align_location = VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL;
            align_match = false;
            if (virDomainObjIsActive(vm))
                def->state = VIR_DOMAIN_SNAPSHOT_DISK_SNAPSHOT;
            else
                def->state = VIR_DOMAIN_SNAPSHOT_SHUTOFF;
            def->memory = VIR_DOMAIN_SNAPSHOT_LOCATION_NONE;
        } else if (def->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL) {
            def->state = virDomainObjGetState(vm, NULL);
            align_location = VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL;
            align_match = false;
        } else {
            def->state = virDomainObjGetState(vm, NULL);

            if (virDomainObjIsActive(vm) &&
                def->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_NONE) {
                virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                               _("internal snapshot of a running VM "
                                 "must include the memory state"));
                goto endjob;
            }

            def->memory = (def->state == VIR_DOMAIN_SNAPSHOT_SHUTOFF ?
                           VIR_DOMAIN_SNAPSHOT_LOCATION_NONE :
                           VIR_DOMAIN_SNAPSHOT_LOCATION_INTERNAL);
        }
        if (virDomainSnapshotAlignDisks(def, align_location,
                                        align_match) < 0 ||
            qemuDomainSnapshotPrepare(vm, def, &flags) < 0)
            goto endjob;
    }

    if (!snap) {
        if (!(snap = virDomainSnapshotAssignDef(vm->snapshots, def)))
            goto endjob;

        def = NULL;
    }

    current = virDomainSnapshotGetCurrent(vm->snapshots);
    if (current) {
        if (!redefine)
            snap->def->parent_name = g_strdup(current->def->name);
    }

    /* actually do the snapshot */
    if (redefine) {
        /* XXX Should we validate that the redefined snapshot even
         * makes sense, such as checking that qemu-img recognizes the
         * snapshot name in at least one of the domain's disks?  */
    } else if (virDomainObjIsActive(vm)) {
        if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY ||
            virDomainSnapshotObjGetDef(snap)->memory == VIR_DOMAIN_SNAPSHOT_LOCATION_EXTERNAL) {
            /* external full system or disk snapshot */
            if (qemuDomainSnapshotCreateActiveExternal(driver,
                                                       vm, snap, cfg, flags) < 0)
                goto endjob;
        } else {
            /* internal full system */
            if (qemuDomainSnapshotCreateActiveInternal(driver,
                                                       vm, snap, flags) < 0)
                goto endjob;
        }
    } else {
        /* inactive; qemuDomainSnapshotPrepare guaranteed that we
         * aren't mixing internal and external, and altered flags to
         * contain DISK_ONLY if there is an external disk.  */
        if (flags & VIR_DOMAIN_SNAPSHOT_CREATE_DISK_ONLY) {
            bool reuse = !!(flags & VIR_DOMAIN_SNAPSHOT_CREATE_REUSE_EXT);

            if (qemuDomainSnapshotCreateInactiveExternal(driver, vm, snap,
                                                         reuse) < 0)
                goto endjob;
        } else {
            if (qemuDomainSnapshotCreateInactiveInternal(driver, vm, snap) < 0)
                goto endjob;
        }
    }

    /* If we fail after this point, there's not a whole lot we can
     * do; we've successfully taken the snapshot, and we are now running
     * on it, so we have to go forward the best we can
     */
    snapshot = virGetDomainSnapshot(domain, snap->def->name);

 endjob:
    if (snapshot && !(flags & VIR_DOMAIN_SNAPSHOT_CREATE_NO_METADATA)) {
        if (update_current)
            virDomainSnapshotSetCurrent(vm->snapshots, snap);
        if (qemuDomainSnapshotWriteMetadata(vm, snap,
                                            driver->xmlopt,
                                            cfg->snapshotDir) < 0) {
            /* if writing of metadata fails, error out rather than trying
             * to silently carry on without completing the snapshot */
            virObjectUnref(snapshot);
            snapshot = NULL;
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unable to save metadata for snapshot %s"),
                           snap->def->name);
            virDomainSnapshotObjListRemove(vm->snapshots, snap);
        } else {
            virDomainSnapshotLinkParent(vm->snapshots, snap);
        }
    } else if (snap) {
        virDomainSnapshotObjListRemove(vm->snapshots, snap);
    }

    qemuDomainObjEndAsyncJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return snapshot;
}


static int
qemuDomainSnapshotListNames(virDomainPtr domain,
                            char **names,
                            int nameslen,
                            unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_ROOTS |
                  VIR_DOMAIN_SNAPSHOT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainSnapshotListNamesEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    n = virDomainSnapshotObjListGetNames(vm->snapshots, NULL, names, nameslen,
                                         flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static int
qemuDomainSnapshotNum(virDomainPtr domain,
                      unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_ROOTS |
                  VIR_DOMAIN_SNAPSHOT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainSnapshotNumEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    n = virDomainSnapshotObjListNum(vm->snapshots, NULL, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static int
qemuDomainListAllSnapshots(virDomainPtr domain,
                           virDomainSnapshotPtr **snaps,
                           unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_ROOTS |
                  VIR_DOMAIN_SNAPSHOT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainListAllSnapshotsEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    n = virDomainListSnapshots(vm->snapshots, NULL, domain, snaps, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static int
qemuDomainSnapshotListChildrenNames(virDomainSnapshotPtr snapshot,
                                    char **names,
                                    int nameslen,
                                    unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainMomentObjPtr snap = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS |
                  VIR_DOMAIN_SNAPSHOT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return -1;

    if (virDomainSnapshotListChildrenNamesEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    n = virDomainSnapshotObjListGetNames(vm->snapshots, snap, names, nameslen,
                                         flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static int
qemuDomainSnapshotNumChildren(virDomainSnapshotPtr snapshot,
                              unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainMomentObjPtr snap = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS |
                  VIR_DOMAIN_SNAPSHOT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return -1;

    if (virDomainSnapshotNumChildrenEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    n = virDomainSnapshotObjListNum(vm->snapshots, snap, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static int
qemuDomainSnapshotListAllChildren(virDomainSnapshotPtr snapshot,
                                  virDomainSnapshotPtr **snaps,
                                  unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainMomentObjPtr snap = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_LIST_DESCENDANTS |
                  VIR_DOMAIN_SNAPSHOT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_SNAPSHOT_FILTERS_ALL, -1);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return -1;

    if (virDomainSnapshotListAllChildrenEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    n = virDomainListSnapshots(vm->snapshots, snap, snapshot->domain, snaps,
                               flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static virDomainSnapshotPtr
qemuDomainSnapshotLookupByName(virDomainPtr domain,
                               const char *name,
                               unsigned int flags)
{
    virDomainObjPtr vm;
    virDomainMomentObjPtr snap = NULL;
    virDomainSnapshotPtr snapshot = NULL;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return NULL;

    if (virDomainSnapshotLookupByNameEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromName(vm, name)))
        goto cleanup;

    snapshot = virGetDomainSnapshot(domain, snap->def->name);

 cleanup:
    virDomainObjEndAPI(&vm);
    return snapshot;
}


static int
qemuDomainHasCurrentSnapshot(virDomainPtr domain,
                             unsigned int flags)
{
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainHasCurrentSnapshotEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    ret = (virDomainSnapshotGetCurrent(vm->snapshots) != NULL);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static virDomainSnapshotPtr
qemuDomainSnapshotGetParent(virDomainSnapshotPtr snapshot,
                            unsigned int flags)
{
    virDomainObjPtr vm;
    virDomainMomentObjPtr snap = NULL;
    virDomainSnapshotPtr parent = NULL;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return NULL;

    if (virDomainSnapshotGetParentEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    if (!snap->def->parent_name) {
        virReportError(VIR_ERR_NO_DOMAIN_SNAPSHOT,
                       _("snapshot '%s' does not have a parent"),
                       snap->def->name);
        goto cleanup;
    }

    parent = virGetDomainSnapshot(snapshot->domain, snap->def->parent_name);

 cleanup:
    virDomainObjEndAPI(&vm);
    return parent;
}


static virDomainSnapshotPtr
qemuDomainSnapshotCurrent(virDomainPtr domain,
                          unsigned int flags)
{
    virDomainObjPtr vm;
    virDomainSnapshotPtr snapshot = NULL;
    const char *name;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return NULL;

    if (virDomainSnapshotCurrentEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    name = virDomainSnapshotGetCurrentName(vm->snapshots);
    if (!name) {
        virReportError(VIR_ERR_NO_DOMAIN_SNAPSHOT, "%s",
                       _("the domain does not have a current snapshot"));
        goto cleanup;
    }

    snapshot = virGetDomainSnapshot(domain, name);

 cleanup:
    virDomainObjEndAPI(&vm);
    return snapshot;
}


static char *
qemuDomainSnapshotGetXMLDesc(virDomainSnapshotPtr snapshot,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = snapshot->domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    char *xml = NULL;
    virDomainMomentObjPtr snap = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_XML_SECURE, NULL);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return NULL;

    if (virDomainSnapshotGetXMLDescEnsureACL(snapshot->domain->conn, vm->def, flags) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    virUUIDFormat(snapshot->domain->uuid, uuidstr);

    xml = virDomainSnapshotDefFormat(uuidstr, virDomainSnapshotObjGetDef(snap),
                                     driver->xmlopt,
                                     virDomainSnapshotFormatConvertXMLFlags(flags));

 cleanup:
    virDomainObjEndAPI(&vm);
    return xml;
}


static int
qemuDomainSnapshotIsCurrent(virDomainSnapshotPtr snapshot,
                            unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;
    virDomainMomentObjPtr snap = NULL;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return -1;

    if (virDomainSnapshotIsCurrentEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    ret = snap == virDomainSnapshotGetCurrent(vm->snapshots);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSnapshotHasMetadata(virDomainSnapshotPtr snapshot,
                              unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;
    virDomainMomentObjPtr snap = NULL;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return -1;

    if (virDomainSnapshotHasMetadataEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto cleanup;

    /* XXX Someday, we should recognize internal snapshots in qcow2
     * images that are not tied to a libvirt snapshot; if we ever do
     * that, then we would have a reason to return 0 here.  */
    ret = 1;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/* The domain is expected to be locked and inactive. */
static int
qemuDomainSnapshotRevertInactive(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 virDomainMomentObjPtr snap)
{
    /* Try all disks, but report failure if we skipped any.  */
    int ret = qemuDomainSnapshotForEachQcow2(driver, vm, snap, "-a", true);
    return ret > 0 ? -1 : ret;
}


static int
qemuDomainRevertToSnapshot(virDomainSnapshotPtr snapshot,
                           unsigned int flags)
{
    virQEMUDriverPtr driver = snapshot->domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    virDomainMomentObjPtr snap = NULL;
    virDomainSnapshotDefPtr snapdef;
    virObjectEventPtr event = NULL;
    virObjectEventPtr event2 = NULL;
    int detail;
    qemuDomainObjPrivatePtr priv;
    int rc;
    virDomainDefPtr config = NULL;
    virDomainDefPtr inactiveConfig = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    bool was_stopped = false;
    qemuDomainSaveCookiePtr cookie;
    virCPUDefPtr origCPU = NULL;
    unsigned int start_flags = VIR_QEMU_PROCESS_START_GEN_VMID;
    qemuDomainAsyncJob jobType = QEMU_ASYNC_JOB_START;
    bool defined = false;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING |
                  VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED |
                  VIR_DOMAIN_SNAPSHOT_REVERT_FORCE, -1);

    /* We have the following transitions, which create the following events:
     * 1. inactive -> inactive: none
     * 2. inactive -> running:  EVENT_STARTED
     * 3. inactive -> paused:   EVENT_STARTED, EVENT_PAUSED
     * 4. running  -> inactive: EVENT_STOPPED
     * 5. running  -> running:  none
     * 6. running  -> paused:   EVENT_PAUSED
     * 7. paused   -> inactive: EVENT_STOPPED
     * 8. paused   -> running:  EVENT_RESUMED
     * 9. paused   -> paused:   none
     * Also, several transitions occur even if we fail partway through,
     * and use of FORCE can cause multiple transitions.
     */

    virNWFilterReadLockFilterUpdates();

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        goto cleanup;

    priv = vm->privateData;
    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainRevertToSnapshotEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainHasBlockjob(vm, false)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain has active block job"));
        goto cleanup;
    }

    if (qemuProcessBeginJob(driver, vm,
                            VIR_DOMAIN_JOB_OPERATION_SNAPSHOT_REVERT,
                            flags) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto endjob;
    snapdef = virDomainSnapshotObjGetDef(snap);

    if (!vm->persistent &&
        snapdef->state != VIR_DOMAIN_SNAPSHOT_RUNNING &&
        snapdef->state != VIR_DOMAIN_SNAPSHOT_PAUSED &&
        (flags & (VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING |
                  VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED)) == 0) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("transient domain needs to request run or pause "
                         "to revert to inactive snapshot"));
        goto endjob;
    }

    if (virDomainSnapshotIsExternal(snap)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("revert to external snapshot not supported yet"));
        goto endjob;
    }

    if (!(flags & VIR_DOMAIN_SNAPSHOT_REVERT_FORCE)) {
        if (!snap->def->dom) {
            virReportError(VIR_ERR_SNAPSHOT_REVERT_RISKY,
                           _("snapshot '%s' lacks domain '%s' rollback info"),
                           snap->def->name, vm->def->name);
            goto endjob;
        }
        if (virDomainObjIsActive(vm) &&
            !(snapdef->state == VIR_DOMAIN_SNAPSHOT_RUNNING ||
              snapdef->state == VIR_DOMAIN_SNAPSHOT_PAUSED) &&
            (flags & (VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING |
                      VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED))) {
            virReportError(VIR_ERR_SNAPSHOT_REVERT_RISKY, "%s",
                           _("must respawn qemu to start inactive snapshot"));
            goto endjob;
        }
        if (vm->hasManagedSave &&
            !(snapdef->state == VIR_DOMAIN_SNAPSHOT_RUNNING ||
              snapdef->state == VIR_DOMAIN_SNAPSHOT_PAUSED)) {
            virReportError(VIR_ERR_SNAPSHOT_REVERT_RISKY, "%s",
                           _("snapshot without memory state, removal of "
                             "existing managed saved state strongly "
                             "recommended to avoid corruption"));
            goto endjob;
        }
    }

    if (snap->def->dom) {
        config = virDomainDefCopy(snap->def->dom,
                                  driver->xmlopt, priv->qemuCaps, true);
        if (!config)
            goto endjob;
    }

    if (snap->def->inactiveDom) {
        inactiveConfig = virDomainDefCopy(snap->def->inactiveDom,
                                          driver->xmlopt, priv->qemuCaps, true);
        if (!inactiveConfig)
            goto endjob;
    } else {
        /* Inactive domain definition is missing:
         * - either this is an old active snapshot and we need to copy the
         *   active definition as an inactive one
         * - or this is an inactive snapshot which means config contains the
         *   inactive definition.
         */
        if (snapdef->state == VIR_DOMAIN_SNAPSHOT_RUNNING ||
            snapdef->state == VIR_DOMAIN_SNAPSHOT_PAUSED) {
            inactiveConfig = virDomainDefCopy(snap->def->dom,
                                              driver->xmlopt, priv->qemuCaps, true);
            if (!inactiveConfig)
                goto endjob;
        } else {
            inactiveConfig = g_steal_pointer(&config);
        }
    }

    cookie = (qemuDomainSaveCookiePtr) snapdef->cookie;

    switch ((virDomainSnapshotState) snapdef->state) {
    case VIR_DOMAIN_SNAPSHOT_RUNNING:
    case VIR_DOMAIN_SNAPSHOT_PAUSED:
        start_flags |= VIR_QEMU_PROCESS_START_PAUSED;

        /* Transitions 2, 3, 5, 6, 8, 9 */
        /* When using the loadvm monitor command, qemu does not know
         * whether to pause or run the reverted domain, and just stays
         * in the same state as before the monitor command, whether
         * that is paused or running.  We always pause before loadvm,
         * to have finer control.  */
        if (virDomainObjIsActive(vm)) {
            /* Transitions 5, 6, 8, 9 */
            /* Check for ABI compatibility. We need to do this check against
             * the migratable XML or it will always fail otherwise */
            if (config) {
                bool compatible;

                /* Replace the CPU in config and put the original one in priv
                 * once we're done. When we have the updated CPU def in the
                 * cookie, we don't want to replace the CPU in migratable def
                 * when doing ABI checks to make sure the current CPU exactly
                 * matches the one used at the time the snapshot was taken.
                 */
                if (cookie && cookie->cpu && config->cpu) {
                    origCPU = config->cpu;
                    if (!(config->cpu = virCPUDefCopy(cookie->cpu)))
                        goto endjob;

                    compatible = qemuDomainDefCheckABIStability(driver,
                                                                priv->qemuCaps,
                                                                vm->def,
                                                                config);
                } else {
                    compatible = qemuDomainCheckABIStability(driver, vm, config);
                }

                /* If using VM GenID, there is no way currently to change
                 * the genid for the running guest, so set an error,
                 * mark as incompatible, and don't allow change of genid
                 * if the revert force flag would start the guest again. */
                if (compatible && config->genidRequested) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("domain genid update requires restart"));
                    compatible = false;
                    start_flags &= ~VIR_QEMU_PROCESS_START_GEN_VMID;
                }

                if (!compatible) {
                    virErrorPtr err = virGetLastError();

                    if (!(flags & VIR_DOMAIN_SNAPSHOT_REVERT_FORCE)) {
                        /* Re-spawn error using correct category. */
                        if (err->code == VIR_ERR_CONFIG_UNSUPPORTED)
                            virReportError(VIR_ERR_SNAPSHOT_REVERT_RISKY, "%s",
                                           err->str2);
                        goto endjob;
                    }
                    virResetError(err);
                    qemuProcessStop(driver, vm,
                                    VIR_DOMAIN_SHUTOFF_FROM_SNAPSHOT,
                                    QEMU_ASYNC_JOB_START, 0);
                    virDomainAuditStop(vm, "from-snapshot");
                    detail = VIR_DOMAIN_EVENT_STOPPED_FROM_SNAPSHOT;
                    event = virDomainEventLifecycleNewFromObj(vm,
                                                     VIR_DOMAIN_EVENT_STOPPED,
                                                     detail);
                    virObjectEventStateQueue(driver->domainEventState, event);
                    /* Start after stop won't be an async start job, so
                     * reset to none */
                    jobType = QEMU_ASYNC_JOB_NONE;
                    goto load;
                }
            }

            if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_RUNNING) {
                /* Transitions 5, 6 */
                if (qemuProcessStopCPUs(driver, vm,
                                        VIR_DOMAIN_PAUSED_FROM_SNAPSHOT,
                                        QEMU_ASYNC_JOB_START) < 0)
                    goto endjob;
                if (!virDomainObjIsActive(vm)) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("guest unexpectedly quit"));
                    goto endjob;
                }
            }

            if (qemuDomainObjEnterMonitorAsync(driver, vm,
                                               QEMU_ASYNC_JOB_START) < 0)
                goto endjob;
            rc = qemuMonitorLoadSnapshot(priv->mon, snap->def->name);
            if (qemuDomainObjExitMonitor(driver, vm) < 0)
                goto endjob;
            if (rc < 0) {
                /* XXX resume domain if it was running before the
                 * failed loadvm attempt? */
                goto endjob;
            }
            if (config) {
                virCPUDefFree(priv->origCPU);
                priv->origCPU = g_steal_pointer(&origCPU);
            }

            if (cookie && !cookie->slirpHelper)
                priv->disableSlirp = true;

            if (inactiveConfig) {
                virDomainObjAssignDef(vm, inactiveConfig, false, NULL);
                inactiveConfig = NULL;
                defined = true;
            }
        } else {
            /* Transitions 2, 3 */
        load:
            was_stopped = true;

            if (inactiveConfig) {
                virDomainObjAssignDef(vm, inactiveConfig, false, NULL);
                inactiveConfig = NULL;
                defined = true;
            }

            if (config) {
                virDomainObjAssignDef(vm, config, true, NULL);
                config = NULL;
            }

            /* No cookie means libvirt which saved the domain was too old to
             * mess up the CPU definitions.
             */
            if (cookie &&
                qemuDomainFixupCPUs(vm, &cookie->cpu) < 0)
                goto cleanup;

            rc = qemuProcessStart(snapshot->domain->conn, driver, vm,
                                  cookie ? cookie->cpu : NULL,
                                  jobType, NULL, -1, NULL, snap,
                                  VIR_NETDEV_VPORT_PROFILE_OP_CREATE,
                                  start_flags);
            virDomainAuditStart(vm, "from-snapshot", rc >= 0);
            detail = VIR_DOMAIN_EVENT_STARTED_FROM_SNAPSHOT;
            event = virDomainEventLifecycleNewFromObj(vm,
                                             VIR_DOMAIN_EVENT_STARTED,
                                             detail);
            if (rc < 0)
                goto endjob;
        }

        /* Touch up domain state.  */
        if (!(flags & VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING) &&
            (snapdef->state == VIR_DOMAIN_SNAPSHOT_PAUSED ||
             (flags & VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED))) {
            /* Transitions 3, 6, 9 */
            virDomainObjSetState(vm, VIR_DOMAIN_PAUSED,
                                 VIR_DOMAIN_PAUSED_FROM_SNAPSHOT);
            if (was_stopped) {
                /* Transition 3, use event as-is and add event2 */
                detail = VIR_DOMAIN_EVENT_SUSPENDED_FROM_SNAPSHOT;
                event2 = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  detail);
            } /* else transition 6 and 9 use event as-is */
        } else {
            /* Transitions 2, 5, 8 */
            if (!virDomainObjIsActive(vm)) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("guest unexpectedly quit"));
                goto endjob;
            }
            rc = qemuProcessStartCPUs(driver, vm,
                                      VIR_DOMAIN_RUNNING_FROM_SNAPSHOT,
                                      jobType);
            if (rc < 0)
                goto endjob;
            virObjectUnref(event);
            event = NULL;
            if (was_stopped) {
                /* Transition 2 */
                detail = VIR_DOMAIN_EVENT_STARTED_FROM_SNAPSHOT;
                event = virDomainEventLifecycleNewFromObj(vm,
                                                 VIR_DOMAIN_EVENT_STARTED,
                                                 detail);
            }
        }
        break;

    case VIR_DOMAIN_SNAPSHOT_SHUTDOWN:
    case VIR_DOMAIN_SNAPSHOT_SHUTOFF:
    case VIR_DOMAIN_SNAPSHOT_CRASHED:
        /* Transitions 1, 4, 7 */
        /* Newer qemu -loadvm refuses to revert to the state of a snapshot
         * created by qemu-img snapshot -c.  If the domain is running, we
         * must take it offline; then do the revert using qemu-img.
         */

        if (virDomainObjIsActive(vm)) {
            /* Transitions 4, 7 */
            qemuProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FROM_SNAPSHOT,
                            QEMU_ASYNC_JOB_START, 0);
            virDomainAuditStop(vm, "from-snapshot");
            detail = VIR_DOMAIN_EVENT_STOPPED_FROM_SNAPSHOT;
            event = virDomainEventLifecycleNewFromObj(vm,
                                             VIR_DOMAIN_EVENT_STOPPED,
                                             detail);
        }

        if (qemuDomainSnapshotRevertInactive(driver, vm, snap) < 0) {
            qemuDomainRemoveInactive(driver, vm);
            qemuProcessEndJob(driver, vm);
            goto cleanup;
        }

        if (inactiveConfig) {
            virDomainObjAssignDef(vm, inactiveConfig, false, NULL);
            inactiveConfig = NULL;
            defined = true;
        }

        if (flags & (VIR_DOMAIN_SNAPSHOT_REVERT_RUNNING |
                     VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED)) {
            /* Flush first event, now do transition 2 or 3 */
            bool paused = (flags & VIR_DOMAIN_SNAPSHOT_REVERT_PAUSED) != 0;

            start_flags |= paused ? VIR_QEMU_PROCESS_START_PAUSED : 0;

            virObjectEventStateQueue(driver->domainEventState, event);
            rc = qemuProcessStart(snapshot->domain->conn, driver, vm, NULL,
                                  QEMU_ASYNC_JOB_START, NULL, -1, NULL, NULL,
                                  VIR_NETDEV_VPORT_PROFILE_OP_CREATE,
                                  start_flags);
            virDomainAuditStart(vm, "from-snapshot", rc >= 0);
            if (rc < 0) {
                qemuDomainRemoveInactive(driver, vm);
                qemuProcessEndJob(driver, vm);
                goto cleanup;
            }
            detail = VIR_DOMAIN_EVENT_STARTED_FROM_SNAPSHOT;
            event = virDomainEventLifecycleNewFromObj(vm,
                                             VIR_DOMAIN_EVENT_STARTED,
                                             detail);
            if (paused) {
                detail = VIR_DOMAIN_EVENT_SUSPENDED_FROM_SNAPSHOT;
                event2 = virDomainEventLifecycleNewFromObj(vm,
                                                  VIR_DOMAIN_EVENT_SUSPENDED,
                                                  detail);
            }
        }
        break;

    case VIR_DOMAIN_SNAPSHOT_PMSUSPENDED:
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("qemu doesn't support reversion of snapshot taken in "
                         "PMSUSPENDED state"));
        goto endjob;

    case VIR_DOMAIN_SNAPSHOT_DISK_SNAPSHOT:
        /* Rejected earlier as an external snapshot */
    case VIR_DOMAIN_SNAPSHOT_NOSTATE:
    case VIR_DOMAIN_SNAPSHOT_BLOCKED:
    case VIR_DOMAIN_SNAPSHOT_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invalid target domain state '%s'. Refusing "
                         "snapshot reversion"),
                       virDomainSnapshotStateTypeToString(snapdef->state));
        goto endjob;
    }

    ret = 0;

 endjob:
    qemuProcessEndJob(driver, vm);

 cleanup:
    if (ret == 0) {
        virDomainSnapshotSetCurrent(vm->snapshots, snap);
        if (qemuDomainSnapshotWriteMetadata(vm, snap,
                                            driver->xmlopt,
                                            cfg->snapshotDir) < 0) {
            virDomainSnapshotSetCurrent(vm->snapshots, NULL);
            ret = -1;
        }
    }
    if (ret == 0 && defined && vm->persistent &&
        !(ret = virDomainDefSave(vm->newDef ? vm->newDef : vm->def,
                                 driver->xmlopt, cfg->configDir))) {
        detail = VIR_DOMAIN_EVENT_DEFINED_FROM_SNAPSHOT;
        virObjectEventStateQueue(driver->domainEventState,
            virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_DEFINED,
                                              detail));
    }
    virObjectEventStateQueue(driver->domainEventState, event);
    virObjectEventStateQueue(driver->domainEventState, event2);
    virDomainObjEndAPI(&vm);
    virNWFilterUnlockFilterUpdates();
    virCPUDefFree(origCPU);
    virDomainDefFree(config);
    virDomainDefFree(inactiveConfig);

    return ret;
}


typedef struct _virQEMUMomentReparent virQEMUMomentReparent;
typedef virQEMUMomentReparent *virQEMUMomentReparentPtr;
struct _virQEMUMomentReparent {
    const char *dir;
    virDomainMomentObjPtr parent;
    virDomainObjPtr vm;
    virDomainXMLOptionPtr xmlopt;
    int err;
    int (*writeMetadata)(virDomainObjPtr, virDomainMomentObjPtr,
                         virDomainXMLOptionPtr, const char *);
};


static int
qemuDomainMomentReparentChildren(void *payload,
                                 const void *name G_GNUC_UNUSED,
                                 void *data)
{
    virDomainMomentObjPtr moment = payload;
    virQEMUMomentReparentPtr rep = data;

    if (rep->err < 0)
        return 0;

    VIR_FREE(moment->def->parent_name);

    if (rep->parent->def)
        moment->def->parent_name = g_strdup(rep->parent->def->name);

    rep->err = rep->writeMetadata(rep->vm, moment, rep->xmlopt,
                                  rep->dir);
    return 0;
}


static int
qemuDomainSnapshotDelete(virDomainSnapshotPtr snapshot,
                         unsigned int flags)
{
    virQEMUDriverPtr driver = snapshot->domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    virDomainMomentObjPtr snap = NULL;
    virQEMUMomentRemove rem;
    virQEMUMomentReparent rep;
    bool metadata_only = !!(flags & VIR_DOMAIN_SNAPSHOT_DELETE_METADATA_ONLY);
    int external = 0;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;

    virCheckFlags(VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN |
                  VIR_DOMAIN_SNAPSHOT_DELETE_METADATA_ONLY |
                  VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN_ONLY, -1);

    if (!(vm = qemuDomObjFromSnapshot(snapshot)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSnapshotDeleteEnsureACL(snapshot->domain->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (!(snap = qemuSnapObjFromSnapshot(vm, snapshot)))
        goto endjob;

    if (!metadata_only) {
        if (!(flags & VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN_ONLY) &&
            virDomainSnapshotIsExternal(snap))
            external++;
        if (flags & (VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN |
                     VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN_ONLY))
            virDomainMomentForEachDescendant(snap,
                                             qemuDomainSnapshotCountExternal,
                                             &external);
        if (external) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("deletion of %d external disk snapshots not "
                             "supported yet"), external);
            goto endjob;
        }
    }

    if (flags & (VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN |
                 VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN_ONLY)) {
        rem.driver = driver;
        rem.vm = vm;
        rem.metadata_only = metadata_only;
        rem.err = 0;
        rem.current = virDomainSnapshotGetCurrent(vm->snapshots);
        rem.found = false;
        rem.momentDiscard = qemuDomainSnapshotDiscard;
        virDomainMomentForEachDescendant(snap, qemuDomainMomentDiscardAll,
                                         &rem);
        if (rem.err < 0)
            goto endjob;
        if (rem.found) {
            virDomainSnapshotSetCurrent(vm->snapshots, snap);
            if (flags & VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN_ONLY) {
                if (qemuDomainSnapshotWriteMetadata(vm, snap,
                                                    driver->xmlopt,
                                                    cfg->snapshotDir) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("failed to set snapshot '%s' as current"),
                                   snap->def->name);
                    virDomainSnapshotSetCurrent(vm->snapshots, NULL);
                    goto endjob;
                }
            }
        }
    } else if (snap->nchildren) {
        rep.dir = cfg->snapshotDir;
        rep.parent = snap->parent;
        rep.vm = vm;
        rep.err = 0;
        rep.xmlopt = driver->xmlopt;
        rep.writeMetadata = qemuDomainSnapshotWriteMetadata;
        virDomainMomentForEachChild(snap,
                                    qemuDomainMomentReparentChildren,
                                    &rep);
        if (rep.err < 0)
            goto endjob;
        virDomainMomentMoveChildren(snap, snap->parent);
    }

    if (flags & VIR_DOMAIN_SNAPSHOT_DELETE_CHILDREN_ONLY) {
        virDomainMomentDropChildren(snap);
        ret = 0;
    } else {
        ret = qemuDomainSnapshotDiscard(driver, vm, snap, true, metadata_only);
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static virDomainCheckpointPtr
qemuDomainCheckpointCreateXML(virDomainPtr domain,
                              const char *xmlDesc,
                              unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainCheckpointPtr checkpoint = NULL;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainCheckpointCreateXMLEnsureACL(domain->conn, vm->def, flags) < 0)
        goto cleanup;

    checkpoint = qemuCheckpointCreateXML(domain, vm, xmlDesc, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return checkpoint;
}


static int
qemuDomainListAllCheckpoints(virDomainPtr domain,
                             virDomainCheckpointPtr **chks,
                             unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_CHECKPOINT_LIST_ROOTS |
                  VIR_DOMAIN_CHECKPOINT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_CHECKPOINT_FILTERS_ALL, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    if (virDomainListAllCheckpointsEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    n = virDomainListCheckpoints(vm->checkpoints, NULL, domain, chks, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static int
qemuDomainCheckpointListAllChildren(virDomainCheckpointPtr checkpoint,
                                    virDomainCheckpointPtr **chks,
                                    unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    virDomainMomentObjPtr chk = NULL;
    int n = -1;

    virCheckFlags(VIR_DOMAIN_CHECKPOINT_LIST_DESCENDANTS |
                  VIR_DOMAIN_CHECKPOINT_LIST_TOPOLOGICAL |
                  VIR_DOMAIN_CHECKPOINT_FILTERS_ALL, -1);

    if (!(vm = qemuDomObjFromCheckpoint(checkpoint)))
        return -1;

    if (virDomainCheckpointListAllChildrenEnsureACL(checkpoint->domain->conn,
                                                    vm->def) < 0)
        goto cleanup;

    if (!(chk = qemuCheckpointObjFromCheckpoint(vm, checkpoint)))
        goto cleanup;

    n = virDomainListCheckpoints(vm->checkpoints, chk, checkpoint->domain,
                                 chks, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return n;
}


static virDomainCheckpointPtr
qemuDomainCheckpointLookupByName(virDomainPtr domain,
                                 const char *name,
                                 unsigned int flags)
{
    virDomainObjPtr vm;
    virDomainMomentObjPtr chk = NULL;
    virDomainCheckpointPtr checkpoint = NULL;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return NULL;

    if (virDomainCheckpointLookupByNameEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(chk = qemuCheckpointObjFromName(vm, name)))
        goto cleanup;

    checkpoint = virGetDomainCheckpoint(domain, chk->def->name);

 cleanup:
    virDomainObjEndAPI(&vm);
    return checkpoint;
}


static virDomainCheckpointPtr
qemuDomainCheckpointGetParent(virDomainCheckpointPtr checkpoint,
                              unsigned int flags)
{
    virDomainObjPtr vm;
    virDomainMomentObjPtr chk = NULL;
    virDomainCheckpointPtr parent = NULL;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomObjFromCheckpoint(checkpoint)))
        return NULL;

    if (virDomainCheckpointGetParentEnsureACL(checkpoint->domain->conn, vm->def) < 0)
        goto cleanup;

    if (!(chk = qemuCheckpointObjFromCheckpoint(vm, checkpoint)))
        goto cleanup;

    if (!chk->def->parent_name) {
        virReportError(VIR_ERR_NO_DOMAIN_CHECKPOINT,
                       _("checkpoint '%s' does not have a parent"),
                       chk->def->name);
        goto cleanup;
    }

    parent = virGetDomainCheckpoint(checkpoint->domain, chk->def->parent_name);

 cleanup:
    virDomainObjEndAPI(&vm);
    return parent;
}


static char *
qemuDomainCheckpointGetXMLDesc(virDomainCheckpointPtr checkpoint,
                               unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    char *xml = NULL;

    if (!(vm = qemuDomObjFromCheckpoint(checkpoint)))
        return NULL;

    if (virDomainCheckpointGetXMLDescEnsureACL(checkpoint->domain->conn, vm->def, flags) < 0)
        goto cleanup;

    xml = qemuCheckpointGetXMLDesc(vm, checkpoint, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return xml;
}


static int
qemuDomainCheckpointDelete(virDomainCheckpointPtr checkpoint,
                           unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;

    if (!(vm = qemuDomObjFromCheckpoint(checkpoint)))
        return -1;

    if (virDomainCheckpointDeleteEnsureACL(checkpoint->domain->conn, vm->def) < 0)
        goto cleanup;

    ret = qemuCheckpointDelete(vm, checkpoint, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainBackupBegin(virDomainPtr domain,
                      const char *backupXML,
                      const char *checkpointXML,
                      unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainBackupBeginEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    ret = qemuBackupBegin(vm, backupXML, checkpointXML, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static char *
qemuDomainBackupGetXMLDesc(virDomainPtr domain,
                           unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    char *ret = NULL;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return NULL;

    if (virDomainBackupGetXMLDescEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    ret = qemuBackupGetXMLDesc(vm, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int qemuDomainQemuMonitorCommand(virDomainPtr domain, const char *cmd,
                                        char **result, unsigned int flags)
{
    virQEMUDriverPtr driver = domain->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    bool hmp;

    virCheckFlags(VIR_DOMAIN_QEMU_MONITOR_COMMAND_HMP, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainQemuMonitorCommandEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    qemuDomainObjTaint(driver, vm, VIR_DOMAIN_TAINT_CUSTOM_MONITOR, NULL);

    hmp = !!(flags & VIR_DOMAIN_QEMU_MONITOR_COMMAND_HMP);

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorArbitraryCommand(priv->mon, cmd, result, hmp);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainOpenConsole(virDomainPtr dom,
                      const char *dev_name,
                      virStreamPtr st,
                      unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;
    size_t i;
    virDomainChrDefPtr chr = NULL;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_CONSOLE_SAFE |
                  VIR_DOMAIN_CONSOLE_FORCE, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainOpenConsoleEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    priv = vm->privateData;

    if (dev_name) {
        for (i = 0; !chr && i < vm->def->nconsoles; i++) {
            if (vm->def->consoles[i]->info.alias &&
                STREQ(dev_name, vm->def->consoles[i]->info.alias))
                chr = vm->def->consoles[i];
        }
        for (i = 0; !chr && i < vm->def->nserials; i++) {
            if (STREQ(dev_name, vm->def->serials[i]->info.alias))
                chr = vm->def->serials[i];
        }
        for (i = 0; !chr && i < vm->def->nparallels; i++) {
            if (STREQ(dev_name, vm->def->parallels[i]->info.alias))
                chr = vm->def->parallels[i];
        }
    } else {
        if (vm->def->nconsoles)
            chr = vm->def->consoles[0];
        else if (vm->def->nserials)
            chr = vm->def->serials[0];
    }

    if (!chr) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find character device %s"),
                       NULLSTR(dev_name));
        goto cleanup;
    }

    if (chr->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("character device %s is not using a PTY"),
                       dev_name ? dev_name : NULLSTR(chr->info.alias));
        goto cleanup;
    }

    /* handle mutually exclusive access to console devices */
    ret = virChrdevOpen(priv->devs,
                        chr->source,
                        st,
                        (flags & VIR_DOMAIN_CONSOLE_FORCE) != 0);

    if (ret == 1) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Active console session exists for this domain"));
        ret = -1;
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainOpenChannel(virDomainPtr dom,
                      const char *name,
                      virStreamPtr st,
                      unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;
    size_t i;
    virDomainChrDefPtr chr = NULL;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(VIR_DOMAIN_CHANNEL_FORCE, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainOpenChannelEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    priv = vm->privateData;

    if (name) {
        for (i = 0; !chr && i < vm->def->nchannels; i++) {
            if (STREQ(name, vm->def->channels[i]->info.alias))
                chr = vm->def->channels[i];

            if (vm->def->channels[i]->targetType == \
                VIR_DOMAIN_CHR_CHANNEL_TARGET_TYPE_VIRTIO &&
                STREQ_NULLABLE(name, vm->def->channels[i]->target.name))
                chr = vm->def->channels[i];
        }
    } else {
        if (vm->def->nchannels)
            chr = vm->def->channels[0];
    }

    if (!chr) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find channel %s"),
                       NULLSTR(name));
        goto cleanup;
    }

    if (chr->source->type != VIR_DOMAIN_CHR_TYPE_UNIX) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("channel %s is not using a UNIX socket"),
                       name ? name : NULLSTR(chr->info.alias));
        goto cleanup;
    }

    /* handle mutually exclusive access to channel devices */
    ret = virChrdevOpen(priv->devs,
                        chr->source,
                        st,
                        (flags & VIR_DOMAIN_CHANNEL_FORCE) != 0);

    if (ret == 1) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Active channel stream exists for this domain"));
        ret = -1;
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


/* Called while holding the VM job lock, to implement a block job
 * abort with pivot; this updates the VM definition as appropriate, on
 * either success or failure.  */
static int
qemuDomainBlockPivot(virQEMUDriverPtr driver,
                     virDomainObjPtr vm,
                     qemuBlockJobDataPtr job,
                     virDomainDiskDefPtr disk)
{
    int ret = -1;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);

    switch ((qemuBlockJobType) job->type) {
    case QEMU_BLOCKJOB_TYPE_NONE:
    case QEMU_BLOCKJOB_TYPE_LAST:
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid job type '%d'"), job->type);
        return -1;

    case QEMU_BLOCKJOB_TYPE_PULL:
    case QEMU_BLOCKJOB_TYPE_COMMIT:
    case QEMU_BLOCKJOB_TYPE_BACKUP:
    case QEMU_BLOCKJOB_TYPE_INTERNAL:
    case QEMU_BLOCKJOB_TYPE_CREATE:
    case QEMU_BLOCKJOB_TYPE_BROKEN:
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("job type '%s' does not support pivot"),
                       qemuBlockjobTypeToString(job->type));
        return -1;

    case QEMU_BLOCKJOB_TYPE_COPY:
    case QEMU_BLOCKJOB_TYPE_ACTIVE_COMMIT:
        break;
    }

    if (job->state != QEMU_BLOCKJOB_STATE_READY) {
        virReportError(VIR_ERR_BLOCK_COPY_ACTIVE,
                       _("block job '%s' not ready for pivot yet"),
                       job->name);
        return -1;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    if (blockdev)
        ret = qemuMonitorJobComplete(priv->mon, job->name);
    else
        ret = qemuMonitorDrivePivot(priv->mon, job->name);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        return -1;

    /* The pivot failed. The block job in QEMU remains in the synchronised state */
    if (ret < 0)
        return -1;

    if (disk && disk->mirror)
        disk->mirrorState = VIR_DOMAIN_DISK_MIRROR_STATE_PIVOT;
    job->state = QEMU_BLOCKJOB_STATE_PIVOTING;

    return ret;
}


/* bandwidth in MiB/s per public API. Caller must lock vm beforehand,
 * and not access it afterwards.  */
static int
qemuDomainBlockPullCommon(virDomainObjPtr vm,
                          const char *path,
                          const char *base,
                          unsigned long bandwidth,
                          unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virQEMUDriverPtr driver = priv->driver;
    const char *device = NULL;
    const char *jobname = NULL;
    virDomainDiskDefPtr disk;
    virStorageSourcePtr baseSource = NULL;
    unsigned int baseIndex = 0;
    g_autofree char *basePath = NULL;
    g_autofree char *backingPath = NULL;
    unsigned long long speed = bandwidth;
    qemuBlockJobDataPtr job = NULL;
    bool persistjob = false;
    const char *nodebase = NULL;
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    int ret = -1;

    if (flags & VIR_DOMAIN_BLOCK_REBASE_RELATIVE && !base) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("flag VIR_DOMAIN_BLOCK_REBASE_RELATIVE is valid only "
                         "with non-null base"));
        goto cleanup;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto endjob;

    if (qemuDomainDiskBlockJobIsActive(disk))
        goto endjob;

    if (base &&
        (virStorageFileParseChainIndex(disk->dst, base, &baseIndex) < 0 ||
         !(baseSource = virStorageFileChainLookup(disk->src, disk->src,
                                                  base, baseIndex, NULL))))
        goto endjob;

    if (baseSource) {
        if (flags & VIR_DOMAIN_BLOCK_REBASE_RELATIVE) {
            if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_CHANGE_BACKING_FILE)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("this QEMU binary doesn't support relative "
                                 "block pull/rebase"));
                goto endjob;
            }

            if (virStorageFileGetRelativeBackingPath(disk->src->backingStore,
                                                     baseSource,
                                                     &backingPath) < 0)
                goto endjob;

            if (!backingPath) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                               _("can't keep relative backing relationship"));
                goto endjob;
            }
        }
    }

    /* Convert bandwidth MiB to bytes, if needed */
    if (!(flags & VIR_DOMAIN_BLOCK_PULL_BANDWIDTH_BYTES)) {
        if (speed > LLONG_MAX >> 20) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("bandwidth must be less than %llu"),
                           LLONG_MAX >> 20);
            goto endjob;
        }
        speed <<= 20;
    }

    if (!(job = qemuBlockJobDiskNewPull(vm, disk, baseSource)))
        goto endjob;

    if (blockdev) {
        jobname = job->name;
        persistjob = true;
        if (baseSource) {
            nodebase = baseSource->nodeformat;
            if (!backingPath &&
                !(backingPath = qemuBlockGetBackingStoreString(baseSource)))
                goto endjob;
        }
        device = disk->src->nodeformat;
    } else {
        device = job->name;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    if (!blockdev && baseSource)
        basePath = qemuMonitorDiskNameLookup(priv->mon, device, disk->src,
                                             baseSource);

    if (blockdev ||
        (!baseSource || basePath))
        ret = qemuMonitorBlockStream(priv->mon, device, jobname, persistjob, basePath,
                                     nodebase, backingPath, speed);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

    if (ret < 0)
        goto endjob;

    qemuBlockJobStarted(job, vm);

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    qemuBlockJobStartupFinalize(vm, job);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainBlockJobAbort(virDomainPtr dom,
                        const char *path,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainDiskDefPtr disk = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    bool pivot = !!(flags & VIR_DOMAIN_BLOCK_JOB_ABORT_PIVOT);
    bool async = !!(flags & VIR_DOMAIN_BLOCK_JOB_ABORT_ASYNC);
    g_autoptr(qemuBlockJobData) job = NULL;
    virDomainObjPtr vm;
    qemuDomainObjPrivatePtr priv = NULL;
    bool blockdev = false;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_BLOCK_JOB_ABORT_ASYNC |
                  VIR_DOMAIN_BLOCK_JOB_ABORT_PIVOT, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainBlockJobAbortEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto endjob;

    if (!(job = qemuBlockJobDiskGetJob(disk))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("disk %s does not have an active block job"), disk->dst);
        goto endjob;
    }

    priv = vm->privateData;
    blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);

    if (job->state == QEMU_BLOCKJOB_STATE_ABORTING ||
        job->state == QEMU_BLOCKJOB_STATE_PIVOTING) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("block job on disk '%s' is still being ended"),
                       disk->dst);
        goto endjob;
    }

    if (!async)
        qemuBlockJobSyncBegin(job);

    if (pivot) {
        if ((ret = qemuDomainBlockPivot(driver, vm, job, disk)) < 0)
            goto endjob;
    } else {
        qemuDomainObjEnterMonitor(driver, vm);
        if (blockdev)
            ret = qemuMonitorJobCancel(priv->mon, job->name, false);
        else
            ret = qemuMonitorBlockJobCancel(priv->mon, job->name);
        if (qemuDomainObjExitMonitor(driver, vm) < 0) {
            ret = -1;
            goto endjob;
        }

        if (ret < 0)
            goto endjob;

        if (disk->mirror)
            disk->mirrorState = VIR_DOMAIN_DISK_MIRROR_STATE_ABORT;
        job->state = QEMU_BLOCKJOB_STATE_ABORTING;
    }

    ignore_value(virDomainObjSave(vm, driver->xmlopt, cfg->stateDir));

    if (!async) {
        qemuBlockJobUpdate(vm, job, QEMU_ASYNC_JOB_NONE);
        while (qemuBlockJobIsRunning(job)) {
            if (virDomainObjWait(vm) < 0) {
                ret = -1;
                goto endjob;
            }
            qemuBlockJobUpdate(vm, job, QEMU_ASYNC_JOB_NONE);
        }

        if (pivot &&
            job->state == QEMU_BLOCKJOB_STATE_FAILED) {
            if (job->errmsg) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("block job '%s' failed while pivoting"),
                               job->name);
            } else {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("block job '%s' failed while pivoting: %s"),
                               job->name, job->errmsg);
            }

            ret = -1;
            goto endjob;
        }
    }

 endjob:
    if (job && !async)
        qemuBlockJobSyncEnd(vm, job, QEMU_ASYNC_JOB_NONE);
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuBlockJobInfoTranslate(qemuMonitorBlockJobInfoPtr rawInfo,
                          virDomainBlockJobInfoPtr info,
                          virDomainDiskDefPtr disk,
                          bool reportBytes)
{
    info->cur = rawInfo->cur;
    info->end = rawInfo->end;

    /* Fix job completeness reporting. If cur == end mgmt
     * applications think job is completed. Except when both cur
     * and end are zero, in which case qemu hasn't started the
     * job yet. */
    if (!info->cur && !info->end) {
        if (rawInfo->ready > 0) {
            info->cur = info->end = 1;
        } else if (!rawInfo->ready) {
            info->end = 1;
        }
    }

    /* If qemu reports that it's not ready yet don't make the job go to
     * cur == end as some apps wrote code polling this instead of waiting for
     * the ready event */
    if (rawInfo->ready == 0 &&
        info->cur == info->end &&
        info->cur > 0)
        info->cur -= 1;

    info->type = rawInfo->type;
    if (info->type == VIR_DOMAIN_BLOCK_JOB_TYPE_COMMIT &&
        disk->mirrorJob == VIR_DOMAIN_BLOCK_JOB_TYPE_ACTIVE_COMMIT)
        info->type = disk->mirrorJob;

    if (rawInfo->bandwidth && !reportBytes)
        rawInfo->bandwidth = VIR_DIV_UP(rawInfo->bandwidth, 1024 * 1024);
    info->bandwidth = rawInfo->bandwidth;
    if (info->bandwidth != rawInfo->bandwidth) {
        virReportError(VIR_ERR_OVERFLOW,
                       _("bandwidth %llu cannot be represented in result"),
                       rawInfo->bandwidth);
        return -1;
    }

    return 0;
}


static int
qemuDomainGetBlockJobInfo(virDomainPtr dom,
                          const char *path,
                          virDomainBlockJobInfoPtr info,
                          unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainDiskDefPtr disk;
    int ret = -1;
    qemuMonitorBlockJobInfo rawInfo;
    g_autoptr(qemuBlockJobData) job = NULL;

    virCheckFlags(VIR_DOMAIN_BLOCK_JOB_INFO_BANDWIDTH_BYTES, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainGetBlockJobInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;


    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto endjob;

    if (!(job = qemuBlockJobDiskGetJob(disk))) {
        ret = 0;
        goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorGetBlockJobInfo(qemuDomainGetMonitor(vm), job->name, &rawInfo);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    if (ret <= 0)
        goto endjob;

    if (qemuBlockJobInfoTranslate(&rawInfo, info, disk,
                                  flags & VIR_DOMAIN_BLOCK_JOB_INFO_BANDWIDTH_BYTES) < 0) {
        ret = -1;
        goto endjob;
    }

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainBlockJobSetSpeed(virDomainPtr dom,
                           const char *path,
                           unsigned long bandwidth,
                           unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainDiskDefPtr disk;
    int ret = -1;
    virDomainObjPtr vm;
    unsigned long long speed = bandwidth;
    g_autoptr(qemuBlockJobData) job = NULL;

    virCheckFlags(VIR_DOMAIN_BLOCK_JOB_SPEED_BANDWIDTH_BYTES, -1);

    /* Convert bandwidth MiB to bytes, if needed */
    if (!(flags & VIR_DOMAIN_BLOCK_JOB_SPEED_BANDWIDTH_BYTES)) {
        if (speed > LLONG_MAX >> 20) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("bandwidth must be less than %llu"),
                           LLONG_MAX >> 20);
            return -1;
        }
        speed <<= 20;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainBlockJobSetSpeedEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto endjob;

    if (!(job = qemuBlockJobDiskGetJob(disk))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("disk %s does not have an active block job"), disk->dst);
        goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorBlockJobSetSpeed(qemuDomainGetMonitor(vm),
                                      job->name,
                                      speed);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);

    return ret;
}


static int
qemuDomainBlockCopyValidateMirror(virStorageSourcePtr mirror,
                                  const char *dst,
                                  bool *reuse)
{
    int desttype = virStorageSourceGetActualType(mirror);
    struct stat st;

    if (!virStorageSourceIsLocalStorage(mirror))
        return 0;

    if (virStorageFileAccess(mirror, F_OK) < 0) {
        if (errno != ENOENT) {
            virReportSystemError(errno, "%s",
                                 _("unable to verify existence of "
                                   "block copy target"));
            return -1;
        }

        if (*reuse || desttype == VIR_STORAGE_TYPE_BLOCK) {
            virReportSystemError(errno,
                                 _("missing destination file for disk %s: %s"),
                                 dst, mirror->path);
            return -1;
        }
    } else {
        if (virStorageFileStat(mirror, &st) < 0) {
            virReportSystemError(errno,
                                 _("unable to stat block copy target '%s'"),
                                 mirror->path);
            return -1;
        }

        if (S_ISBLK(st.st_mode)) {
            /* if the target is a block device, assume that we are reusing it,
             * so there are no attempts to create it */
            *reuse = true;
        } else {
            if (st.st_size && !(*reuse)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("external destination file for disk %s already "
                                 "exists and is not a block device: %s"),
                               dst, mirror->path);
                return -1;
            }

            if (desttype == VIR_STORAGE_TYPE_BLOCK) {
                virReportError(VIR_ERR_INVALID_ARG,
                               _("blockdev flag requested for disk %s, but file "
                                 "'%s' is not a block device"),
                               dst, mirror->path);
                return -1;
            }
        }
    }

    return 0;
}


/**
 * qemuDomainBlockCopyCommonValidateUserMirrorBackingStore:
 * @mirror: target of the block copy
 * @flags: block copy API flags
 * @blockdev: true if blockdev is used for the VM
 *
 * Validates whether backingStore of @mirror makes sense according to @flags.
 * This makes sure that:
 * 1) mirror has a terminator if it isn't supposed to have backing chain
 * 2) if shallow copy is requested there is a chain or prepopulated image
 * 3) user specified chain is present only when blockdev is used
 * 4) if deep copy is requested, there's no chain
 */
static int
qemuDomainBlockCopyCommonValidateUserMirrorBackingStore(virStorageSourcePtr mirror,
                                                        bool shallow,
                                                        bool blockdev)
{
    if (!virStorageSourceHasBacking(mirror)) {
        /* for deep copy there won't be backing chain so we can terminate it */
        if (!mirror->backingStore &&
            !shallow &&
            !(mirror->backingStore = virStorageSourceNew()))
            return -1;

        /* When reusing an external image we document that the user must ensure
         * that the <mirror> image must expose data as the original image did
         * either by providing correct chain or prepopulating the image. This
         * means we can't validate this any more regardless of whether shallow
         * copy is requested.
         *
         * For a copy when we are not reusing external image requesting shallow
         * is okay and will inherit the original backing chain */
    } else {
        if (!blockdev) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("backingStore of mirror target is not supported by this qemu"));
            return -1;
        }

        if (!shallow) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("backingStore of mirror without VIR_DOMAIN_BLOCK_COPY_SHALLOW doesn't make sense"));
            return -1;
        }

        if (qemuDomainStorageSourceValidateDepth(mirror, 0, NULL) < 0)
            return -1;
    }

    return 0;
}


/* bandwidth in bytes/s.  Caller must lock vm beforehand, and not
 * access mirror afterwards.  */
static int
qemuDomainBlockCopyCommon(virDomainObjPtr vm,
                          virConnectPtr conn,
                          const char *path,
                          virStorageSourcePtr mirrorsrc,
                          unsigned long long bandwidth,
                          unsigned int granularity,
                          unsigned long long buf_size,
                          unsigned int flags,
                          bool keepParentLabel)
{
    virQEMUDriverPtr driver = conn->privateData;
    qemuDomainObjPrivatePtr priv = vm->privateData;
    virDomainDiskDefPtr disk = NULL;
    int ret = -1;
    bool need_unlink = false;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    const char *format = NULL;
    bool mirror_reuse = !!(flags & VIR_DOMAIN_BLOCK_COPY_REUSE_EXT);
    bool mirror_shallow = !!(flags & VIR_DOMAIN_BLOCK_COPY_SHALLOW);
    bool existing = mirror_reuse;
    qemuBlockJobDataPtr job = NULL;
    g_autoptr(virStorageSource) mirror = mirrorsrc;
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    bool supports_create = false;
    bool supports_access = false;
    bool supports_detect = false;
    g_autoptr(qemuBlockStorageSourceChainData) data = NULL;
    g_autoptr(qemuBlockStorageSourceChainData) crdata = NULL;
    virStorageSourcePtr n;
    virStorageSourcePtr mirrorBacking = NULL;
    g_autoptr(virHashTable) blockNamedNodeData = NULL;
    int rc = 0;

    /* Preliminaries: find the disk we are editing, sanity checks */
    virCheckFlags(VIR_DOMAIN_BLOCK_COPY_SHALLOW |
                  VIR_DOMAIN_BLOCK_COPY_REUSE_EXT |
                  VIR_DOMAIN_BLOCK_COPY_TRANSIENT_JOB, -1);

    if (virStorageSourceIsRelative(mirror)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("absolute path must be used as block copy target"));
        return -1;
    }

    if (bandwidth > LLONG_MAX) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("bandwidth must be less than "
                         "'%llu' bytes/s (%llu MiB/s)"),
                       LLONG_MAX, LLONG_MAX >> 20);
        return -1;
    }

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return -1;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto endjob;

    if (qemuDomainDiskBlockJobIsActive(disk))
        goto endjob;

    if (disk->device == VIR_DOMAIN_DISK_DEVICE_LUN &&
        qemuDomainDefValidateDiskLunSource(mirror) < 0)
        goto endjob;

    if (!(flags & VIR_DOMAIN_BLOCK_COPY_TRANSIENT_JOB) &&
        vm->persistent) {
        /* XXX if qemu ever lets us start a new domain with mirroring
         * already active, we can relax this; but for now, the risk of
         * 'managedsave' due to libvirt-guests means we can't risk
         * this on persistent domains.  */
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain is not transient"));
        goto endjob;
    }

    /* clear the _SHALLOW flag if there is only one layer */
    if (!virStorageSourceHasBacking(disk->src))
        mirror_shallow = false;

    if (qemuDomainBlockCopyCommonValidateUserMirrorBackingStore(mirror,
                                                                mirror_shallow,
                                                                blockdev) < 0)
        goto endjob;

    /* unless the user provides a pre-created file, shallow copy into a raw
     * file is not possible */
    if (mirror_shallow && !existing && mirror->format == VIR_STORAGE_FILE_RAW) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("shallow copy of disk '%s' into a raw file "
                         "is not possible"),
                       disk->dst);
        goto endjob;
    }

    /* Prepare the destination file.  */
    if (!blockdev &&
        !virStorageSourceIsLocalStorage(mirror)) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("non-file destination not supported yet"));
        goto endjob;
    }

    supports_access = virStorageFileSupportsAccess(mirror) == 1;
    supports_create = virStorageFileSupportsCreate(mirror) == 1;
    supports_detect = virStorageFileSupportsBackingChainTraversal(mirror) == 1;

    if (supports_access || supports_create || supports_detect) {
        if (qemuDomainStorageFileInit(driver, vm, mirror, NULL) < 0)
            goto endjob;
    }

    if (supports_access &&
        qemuDomainBlockCopyValidateMirror(mirror, disk->dst, &existing) < 0)
        goto endjob;

    if (!mirror->format) {
        if (!mirror_reuse) {
            mirror->format = disk->src->format;
        } else {
            /* If the user passed the REUSE_EXT flag, then either they
             * can also pass the RAW flag or use XML to tell us the format.
             * So if we get here, we assume it is safe for us to probe the
             * format from the file that we will be using.  */
            if (!supports_detect ||
                !virStorageSourceIsLocalStorage(mirror) ||
                (mirror->format = virStorageFileProbeFormat(mirror->path, cfg->user,
                                                            cfg->group)) < 0) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("reused mirror destination format must be specified"));
                goto endjob;
            }
        }
    }

    /* When copying a shareable disk we need to make sure that the disk can
     * be safely shared, since block copy may change the format. */
    if (disk->src->shared && !disk->src->readonly &&
        !qemuBlockStorageSourceSupportsConcurrentAccess(mirror)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("can't pivot a shared disk to a storage volume not "
                         "supporting sharing"));
        goto endjob;
    }

    /* pre-create the image file. In case when 'blockdev' is used this is
     * required so that libvirt can properly label the image for access by qemu */
    if (!existing) {
        if (supports_create) {
            if (virStorageFileCreate(mirror) < 0) {
                virReportSystemError(errno, "%s", _("failed to create copy target"));
                goto endjob;
            }

            need_unlink = true;
        }
    }

    if (mirror->format > 0)
        format = virStorageFileFormatTypeToString(mirror->format);

    if (virStorageSourceInitChainElement(mirror, disk->src,
                                         keepParentLabel) < 0)
        goto endjob;

    /* we must initialize XML-provided chain prior to detecting to keep semantics
     * with VM startup */
    if (blockdev) {
        for (n = mirror; virStorageSourceIsBacking(n); n = n->backingStore) {
            if (qemuDomainPrepareStorageSourceBlockdev(disk, n, priv, cfg) < 0)
                goto endjob;
        }
    }

    /* If reusing an external image that includes a backing file but the user
     * did not enumerate the chain in the XML we need to detect the chain */
    if (mirror_reuse &&
        mirror->format >= VIR_STORAGE_FILE_BACKING &&
        mirror->backingStore == NULL &&
        qemuDomainDetermineDiskChain(driver, vm, disk, mirror, true) < 0)
        goto endjob;

    if (qemuDomainStorageSourceChainAccessAllow(driver, vm, mirror) < 0)
        goto endjob;

    if (blockdev) {
        if (mirror_reuse) {
            if (!(data = qemuBuildStorageSourceChainAttachPrepareBlockdev(mirror,
                                                                          priv->qemuCaps)))
                goto endjob;
        } else {
            qemuDomainObjEnterMonitor(driver, vm);
            blockNamedNodeData = qemuMonitorBlockGetNamedNodeData(priv->mon);
            if (qemuDomainObjExitMonitor(driver, vm) < 0 || !blockNamedNodeData)
                goto endjob;

            if (qemuBlockStorageSourceCreateDetectSize(blockNamedNodeData,
                                                       mirror, disk->src))
                goto endjob;

            if (mirror_shallow) {
                /* if external backing store is populated we'll need to open it */
                if (virStorageSourceHasBacking(mirror)) {
                    if (!(data = qemuBuildStorageSourceChainAttachPrepareBlockdev(mirror->backingStore,
                                                                                  priv->qemuCaps)))
                        goto endjob;

                    mirrorBacking = mirror->backingStore;
                } else {
                    /* backing store of original image will be reused, but the
                     * new image must refer to it in the metadata */
                    mirrorBacking = disk->src->backingStore;
                }
            } else {
                mirrorBacking = mirror->backingStore;
            }

            if (!(crdata = qemuBuildStorageSourceChainAttachPrepareBlockdevTop(mirror,
                                                                               mirrorBacking,
                                                                               priv->qemuCaps)))
                goto endjob;
        }

        if (data) {
            qemuDomainObjEnterMonitor(driver, vm);
            rc = qemuBlockStorageSourceChainAttach(priv->mon, data);
            if (qemuDomainObjExitMonitor(driver, vm) < 0)
                goto endjob;

            if (rc < 0)
                goto endjob;
        }

        if (crdata &&
            qemuBlockStorageSourceCreate(vm, mirror, mirrorBacking, mirror->backingStore,
                                         crdata->srcdata[0], QEMU_ASYNC_JOB_NONE) < 0)
            goto endjob;
    }

    if (!(job = qemuBlockJobDiskNewCopy(vm, disk, mirror, mirror_shallow, mirror_reuse)))
        goto endjob;

    disk->mirrorState = VIR_DOMAIN_DISK_MIRROR_STATE_NONE;

    /* Actually start the mirroring */
    qemuDomainObjEnterMonitor(driver, vm);

    if (blockdev) {
        ret = qemuMonitorBlockdevMirror(priv->mon, job->name, true,
                                        disk->src->nodeformat,
                                        mirror->nodeformat, bandwidth,
                                        granularity, buf_size, mirror_shallow);
    } else {
        /* qemuMonitorDriveMirror needs to honor the REUSE_EXT flag as specified
         * by the user */
        ret = qemuMonitorDriveMirror(priv->mon, job->name, mirror->path, format,
                                     bandwidth, granularity, buf_size,
                                     mirror_shallow, mirror_reuse);
    }

    virDomainAuditDisk(vm, NULL, mirror, "mirror", ret >= 0);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    if (ret < 0) {
        qemuDomainStorageSourceChainAccessRevoke(driver, vm, mirror);
        goto endjob;
    }

    /* Update vm in place to match changes.  */
    need_unlink = false;
    virStorageFileDeinit(mirror);
    disk->mirror = g_steal_pointer(&mirror);
    disk->mirrorJob = VIR_DOMAIN_BLOCK_JOB_TYPE_COPY;
    qemuBlockJobStarted(job, vm);

 endjob:
    if (rc < 0 &&
        virDomainObjIsActive(vm) &&
        (data || crdata)) {
        qemuDomainObjEnterMonitor(driver, vm);
        if (data)
            qemuBlockStorageSourceChainDetach(priv->mon, data);
        if (crdata)
            qemuBlockStorageSourceAttachRollback(priv->mon, crdata->srcdata[0]);
        ignore_value(qemuDomainObjExitMonitor(driver, vm));
    }
    if (need_unlink && virStorageFileUnlink(mirror) < 0)
        VIR_WARN("%s", _("unable to remove just-created copy target"));
    virStorageFileDeinit(mirror);
    qemuDomainObjEndJob(driver, vm);
    qemuBlockJobStartupFinalize(vm, job);

    return ret;
}

static int
qemuDomainBlockRebase(virDomainPtr dom, const char *path, const char *base,
                      unsigned long bandwidth, unsigned int flags)
{
    virDomainObjPtr vm;
    int ret = -1;
    unsigned long long speed = bandwidth;
    g_autoptr(virStorageSource) dest = NULL;

    virCheckFlags(VIR_DOMAIN_BLOCK_REBASE_SHALLOW |
                  VIR_DOMAIN_BLOCK_REBASE_REUSE_EXT |
                  VIR_DOMAIN_BLOCK_REBASE_COPY |
                  VIR_DOMAIN_BLOCK_REBASE_COPY_RAW |
                  VIR_DOMAIN_BLOCK_REBASE_RELATIVE |
                  VIR_DOMAIN_BLOCK_REBASE_COPY_DEV |
                  VIR_DOMAIN_BLOCK_REBASE_BANDWIDTH_BYTES, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainBlockRebaseEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainSupportsCheckpointsBlockjobs(vm) < 0)
        goto cleanup;

    /* For normal rebase (enhanced blockpull), the common code handles
     * everything, including vm cleanup. */
    if (!(flags & VIR_DOMAIN_BLOCK_REBASE_COPY))
        return qemuDomainBlockPullCommon(vm, path, base, bandwidth, flags);

    /* If we got here, we are doing a block copy rebase. */
    if (!(dest = virStorageSourceNew()))
        goto cleanup;
    dest->type = (flags & VIR_DOMAIN_BLOCK_REBASE_COPY_DEV) ?
        VIR_STORAGE_TYPE_BLOCK : VIR_STORAGE_TYPE_FILE;
    dest->path = g_strdup(base);
    if (flags & VIR_DOMAIN_BLOCK_REBASE_COPY_RAW)
        dest->format = VIR_STORAGE_FILE_RAW;

    /* Convert bandwidth MiB to bytes, if necessary */
    if (!(flags & VIR_DOMAIN_BLOCK_REBASE_BANDWIDTH_BYTES)) {
        if (speed > LLONG_MAX >> 20) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("bandwidth must be less than %llu"),
                           LLONG_MAX >> 20);
            goto cleanup;
        }
        speed <<= 20;
    }

    /* XXX: If we are doing a shallow copy but not reusing an external
     * file, we should attempt to pre-create the destination with a
     * relative backing chain instead of qemu's default of absolute */
    if (flags & VIR_DOMAIN_BLOCK_REBASE_RELATIVE) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("Relative backing during copy not supported yet"));
        goto cleanup;
    }

    /* We rely on the fact that VIR_DOMAIN_BLOCK_REBASE_SHALLOW
     * and VIR_DOMAIN_BLOCK_REBASE_REUSE_EXT map to the same values
     * as for block copy. */
    flags &= (VIR_DOMAIN_BLOCK_REBASE_SHALLOW |
              VIR_DOMAIN_BLOCK_REBASE_REUSE_EXT);
    ret = qemuDomainBlockCopyCommon(vm, dom->conn, path, dest,
                                    speed, 0, 0, flags, true);
    dest = NULL;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainBlockCopy(virDomainPtr dom, const char *disk, const char *destxml,
                    virTypedParameterPtr params, int nparams,
                    unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    unsigned long long bandwidth = 0;
    unsigned int granularity = 0;
    unsigned long long buf_size = 0;
    virDomainDiskDefPtr diskdef = NULL;
    virStorageSourcePtr dest = NULL;
    size_t i;

    virCheckFlags(VIR_DOMAIN_BLOCK_COPY_SHALLOW |
                  VIR_DOMAIN_BLOCK_COPY_REUSE_EXT |
                  VIR_DOMAIN_BLOCK_COPY_TRANSIENT_JOB, -1);
    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_BLOCK_COPY_BANDWIDTH,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_COPY_GRANULARITY,
                               VIR_TYPED_PARAM_UINT,
                               VIR_DOMAIN_BLOCK_COPY_BUF_SIZE,
                               VIR_TYPED_PARAM_ULLONG,
                               NULL) < 0)
        return -1;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainBlockCopyEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainSupportsCheckpointsBlockjobs(vm) < 0)
        goto cleanup;

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        /* Typed params (wisely) refused to expose unsigned long, but
         * back-compat demands that we stick with a maximum of
         * unsigned long bandwidth in MiB/s, while our value is
         * unsigned long long in bytes/s.  Hence, we have to do
         * overflow detection if this is a 32-bit server handling a
         * 64-bit client.  */
        if (STREQ(param->field, VIR_DOMAIN_BLOCK_COPY_BANDWIDTH)) {
            if (sizeof(unsigned long)< sizeof(bandwidth) &&
                param->value.ul > ULONG_MAX * (1ULL << 20)) {
                virReportError(VIR_ERR_OVERFLOW,
                               _("bandwidth must be less than %llu bytes"),
                               ULONG_MAX * (1ULL << 20));
                goto cleanup;
            }
            bandwidth = param->value.ul;
        } else if (STREQ(param->field, VIR_DOMAIN_BLOCK_COPY_GRANULARITY)) {
            if (param->value.ui != VIR_ROUND_UP_POWER_OF_TWO(param->value.ui)) {
                virReportError(VIR_ERR_INVALID_ARG, "%s",
                               _("granularity must be power of 2"));
                goto cleanup;
            }
            granularity = param->value.ui;
        } else if (STREQ(param->field, VIR_DOMAIN_BLOCK_COPY_BUF_SIZE)) {
            buf_size = param->value.ul;
        }
    }

    if (!(diskdef = virDomainDiskDefParse(destxml, driver->xmlopt,
                                          VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                          VIR_DOMAIN_DEF_PARSE_DISK_SOURCE)))
        goto cleanup;

    dest = g_steal_pointer(&diskdef->src);

    ret = qemuDomainBlockCopyCommon(vm, dom->conn, disk, dest, bandwidth,
                                    granularity, buf_size, flags, false);

 cleanup:
    virDomainDiskDefFree(diskdef);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainBlockPull(virDomainPtr dom, const char *path, unsigned long bandwidth,
                    unsigned int flags)
{
    virDomainObjPtr vm;
    virCheckFlags(VIR_DOMAIN_BLOCK_PULL_BANDWIDTH_BYTES, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainBlockPullEnsureACL(dom->conn, vm->def) < 0) {
        virDomainObjEndAPI(&vm);
        return -1;
    }

    if (qemuDomainSupportsCheckpointsBlockjobs(vm) < 0) {
        virDomainObjEndAPI(&vm);
        return -1;
    }

    /* qemuDomainBlockPullCommon consumes the reference on @vm */
    return qemuDomainBlockPullCommon(vm, path, NULL, bandwidth, flags);
}


static int
qemuDomainBlockCommit(virDomainPtr dom,
                      const char *path,
                      const char *base,
                      const char *top,
                      unsigned long bandwidth,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuDomainObjPrivatePtr priv;
    virDomainObjPtr vm = NULL;
    const char *device = NULL;
    const char *jobname = NULL;
    int ret = -1;
    virDomainDiskDefPtr disk = NULL;
    virStorageSourcePtr topSource;
    unsigned int topIndex = 0;
    virStorageSourcePtr baseSource = NULL;
    unsigned int baseIndex = 0;
    virStorageSourcePtr top_parent = NULL;
    bool clean_access = false;
    g_autofree char *topPath = NULL;
    g_autofree char *basePath = NULL;
    g_autofree char *backingPath = NULL;
    unsigned long long speed = bandwidth;
    qemuBlockJobDataPtr job = NULL;
    g_autoptr(virStorageSource) mirror = NULL;
    const char *nodetop = NULL;
    const char *nodebase = NULL;
    bool persistjob = false;
    bool blockdev = false;

    virCheckFlags(VIR_DOMAIN_BLOCK_COMMIT_SHALLOW |
                  VIR_DOMAIN_BLOCK_COMMIT_ACTIVE |
                  VIR_DOMAIN_BLOCK_COMMIT_RELATIVE |
                  VIR_DOMAIN_BLOCK_COMMIT_DELETE |
                  VIR_DOMAIN_BLOCK_COMMIT_BANDWIDTH_BYTES, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;
    priv = vm->privateData;

    if (virDomainBlockCommitEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainSupportsCheckpointsBlockjobs(vm) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);

    if (!blockdev && (flags & VIR_DOMAIN_BLOCK_COMMIT_DELETE)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("deleting committed images is not supported by this VM"));
        goto endjob;
    }

    /* Convert bandwidth MiB to bytes, if necessary */
    if (!(flags & VIR_DOMAIN_BLOCK_COMMIT_BANDWIDTH_BYTES)) {
        if (speed > LLONG_MAX >> 20) {
            virReportError(VIR_ERR_OVERFLOW,
                           _("bandwidth must be less than %llu"),
                           LLONG_MAX >> 20);
            goto endjob;
        }
        speed <<= 20;
    }

    if (!(disk = qemuDomainDiskByName(vm->def, path)))
        goto endjob;

    if (virStorageSourceIsEmpty(disk->src)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("disk %s has no source file to be committed"),
                       disk->dst);
        goto endjob;
    }

    if (qemuDomainDiskBlockJobIsActive(disk))
        goto endjob;

    if (!top || STREQ(top, disk->dst))
        topSource = disk->src;
    else if (virStorageFileParseChainIndex(disk->dst, top, &topIndex) < 0 ||
             !(topSource = virStorageFileChainLookup(disk->src, NULL,
                                                     top, topIndex,
                                                     &top_parent)))
        goto endjob;

    if (topSource == disk->src) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_ACTIVE_COMMIT)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("active commit not supported with this QEMU binary"));
            goto endjob;
        }
        /* XXX Should we auto-pivot when COMMIT_ACTIVE is not specified? */
        if (!(flags & VIR_DOMAIN_BLOCK_COMMIT_ACTIVE)) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("commit of '%s' active layer requires active flag"),
                           disk->dst);
            goto endjob;
        }
    } else if (flags & VIR_DOMAIN_BLOCK_COMMIT_ACTIVE) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("active commit requested but '%s' is not active"),
                       topSource->path);
        goto endjob;
    }

    if (!virStorageSourceHasBacking(topSource)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("top '%s' in chain for '%s' has no backing file"),
                       topSource->path, path);
        goto endjob;
    }

    if (!base && (flags & VIR_DOMAIN_BLOCK_COMMIT_SHALLOW))
        baseSource = topSource->backingStore;
    else if (virStorageFileParseChainIndex(disk->dst, base, &baseIndex) < 0 ||
             !(baseSource = virStorageFileChainLookup(disk->src, topSource,
                                                      base, baseIndex, NULL)))
        goto endjob;

    if ((flags & VIR_DOMAIN_BLOCK_COMMIT_SHALLOW) &&
        baseSource != topSource->backingStore) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("base '%s' is not immediately below '%s' in chain "
                         "for '%s'"),
                       base, topSource->path, path);
        goto endjob;
    }

    /* For an active commit, clone enough of the base to act as the mirror */
    if (topSource == disk->src) {
        if (!(mirror = virStorageSourceCopy(baseSource, false)))
            goto endjob;
        if (virStorageSourceInitChainElement(mirror,
                                             disk->src,
                                             true) < 0)
            goto endjob;
    }

    if (flags & VIR_DOMAIN_BLOCK_COMMIT_RELATIVE &&
        topSource != disk->src) {
        if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_CHANGE_BACKING_FILE)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu doesn't support relative block commit"));
            goto endjob;
        }

        if (virStorageFileGetRelativeBackingPath(topSource, baseSource,
                                                 &backingPath) < 0)
            goto endjob;

        if (!backingPath) {
            virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                           _("can't keep relative backing relationship"));
            goto endjob;
        }
    }

    /* For the commit to succeed, we must allow qemu to open both the
     * 'base' image and the parent of 'top' as read/write; 'top' might
     * not have a parent, or might already be read-write.  XXX It
     * would also be nice to revert 'base' to read-only, as well as
     * revoke access to files removed from the chain, when the commit
     * operation succeeds, but doing that requires tracking the
     * operation in XML across libvirtd restarts.  */
    clean_access = true;
    if (qemuDomainStorageSourceAccessAllow(driver, vm, baseSource, false, false) < 0 ||
        (top_parent && top_parent != disk->src &&
         qemuDomainStorageSourceAccessAllow(driver, vm, top_parent, false, false) < 0))
        goto endjob;

    if (!(job = qemuBlockJobDiskNewCommit(vm, disk, top_parent, topSource,
                                          baseSource,
                                          flags & VIR_DOMAIN_BLOCK_COMMIT_DELETE)))
        goto endjob;

    disk->mirrorState = VIR_DOMAIN_DISK_MIRROR_STATE_NONE;

    /* Start the commit operation.  Pass the user's original spelling,
     * if any, through to qemu, since qemu may behave differently
     * depending on whether the input was specified as relative or
     * absolute (that is, our absolute top_canon may do the wrong
     * thing if the user specified a relative name).  */

    if (blockdev) {
        persistjob = true;
        jobname = job->name;
        nodetop = topSource->nodeformat;
        nodebase = baseSource->nodeformat;
        device = disk->src->nodeformat;
        if (!backingPath && top_parent &&
            !(backingPath = qemuBlockGetBackingStoreString(baseSource)))
            goto endjob;
    } else {
        device = job->name;
    }

    qemuDomainObjEnterMonitor(driver, vm);

    if (!blockdev) {
        basePath = qemuMonitorDiskNameLookup(priv->mon, device, disk->src,
                                             baseSource);
        topPath = qemuMonitorDiskNameLookup(priv->mon, device, disk->src,
                                            topSource);
    }

    if (blockdev || (basePath && topPath))
        ret = qemuMonitorBlockCommit(priv->mon, device, jobname, persistjob,
                                     topPath, nodetop, basePath, nodebase,
                                     backingPath, speed);

    if (qemuDomainObjExitMonitor(driver, vm) < 0 || ret < 0) {
        ret = -1;
        goto endjob;
    }

    if (mirror) {
        disk->mirror = g_steal_pointer(&mirror);
        disk->mirrorJob = VIR_DOMAIN_BLOCK_JOB_TYPE_ACTIVE_COMMIT;
    }
    qemuBlockJobStarted(job, vm);

 endjob:
    if (ret < 0 && clean_access) {
        virErrorPtr orig_err;
        virErrorPreserveLast(&orig_err);
        /* Revert access to read-only, if possible.  */
        qemuDomainStorageSourceAccessAllow(driver, vm, baseSource, true, false);
        if (top_parent && top_parent != disk->src)
            qemuDomainStorageSourceAccessAllow(driver, vm, top_parent, true, false);

        virErrorRestore(&orig_err);
    }
    qemuBlockJobStartupFinalize(vm, job);
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainOpenGraphics(virDomainPtr dom,
                       unsigned int idx,
                       int fd,
                       unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    const char *protocol;

    virCheckFlags(VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainOpenGraphicsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    if (idx >= vm->def->ngraphics) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No graphics backend with index %d"), idx);
        goto endjob;
    }
    switch (vm->def->graphics[idx]->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        protocol = "vnc";
        break;
    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        protocol = "spice";
        break;
    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
    case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Can only open VNC or SPICE graphics backends, not %s"),
                       virDomainGraphicsTypeToString(vm->def->graphics[idx]->type));
        goto endjob;
    case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainGraphicsType,
                                vm->def->graphics[idx]->type);
        goto endjob;
    }

    if (qemuSecuritySetImageFDLabel(driver->securityManager, vm->def, fd) < 0)
        goto endjob;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorOpenGraphics(priv->mon, protocol, fd, "graphicsfd",
                                  (flags & VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH) != 0);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainOpenGraphicsFD(virDomainPtr dom,
                         unsigned int idx,
                         unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    const char *protocol;
    int pair[2] = {-1, -1};

    virCheckFlags(VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainOpenGraphicsFdEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    priv = vm->privateData;

    if (idx >= vm->def->ngraphics) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No graphics backend with index %d"), idx);
        goto cleanup;
    }
    switch (vm->def->graphics[idx]->type) {
    case VIR_DOMAIN_GRAPHICS_TYPE_VNC:
        protocol = "vnc";
        break;
    case VIR_DOMAIN_GRAPHICS_TYPE_SPICE:
        protocol = "spice";
        break;
    case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
    case VIR_DOMAIN_GRAPHICS_TYPE_RDP:
    case VIR_DOMAIN_GRAPHICS_TYPE_DESKTOP:
    case VIR_DOMAIN_GRAPHICS_TYPE_EGL_HEADLESS:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Can only open VNC or SPICE graphics backends, not %s"),
                       virDomainGraphicsTypeToString(vm->def->graphics[idx]->type));
        goto cleanup;
    case VIR_DOMAIN_GRAPHICS_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainGraphicsType,
                                vm->def->graphics[idx]->type);
        goto cleanup;
    }

    if (qemuSecuritySetSocketLabel(driver->securityManager, vm->def) < 0)
        goto cleanup;

    if (socketpair(PF_UNIX, SOCK_STREAM, 0, pair) < 0)
        goto cleanup;

    if (qemuSecurityClearSocketLabel(driver->securityManager, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;
    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorOpenGraphics(priv->mon, protocol, pair[1], "graphicsfd",
                                  (flags & VIR_DOMAIN_OPEN_GRAPHICS_SKIPAUTH));
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;
    qemuDomainObjEndJob(driver, vm);
    if (ret < 0)
        goto cleanup;

    ret = pair[0];
    pair[0] = -1;

 cleanup:
    VIR_FORCE_CLOSE(pair[0]);
    VIR_FORCE_CLOSE(pair[1]);
    virDomainObjEndAPI(&vm);
    return ret;
}

typedef enum {
    QEMU_BLOCK_IOTUNE_SET_BYTES            = 1 << 0,
    QEMU_BLOCK_IOTUNE_SET_IOPS             = 1 << 1,
    QEMU_BLOCK_IOTUNE_SET_BYTES_MAX        = 1 << 2,
    QEMU_BLOCK_IOTUNE_SET_IOPS_MAX         = 1 << 3,
    QEMU_BLOCK_IOTUNE_SET_SIZE_IOPS        = 1 << 4,
    QEMU_BLOCK_IOTUNE_SET_GROUP_NAME       = 1 << 5,
    QEMU_BLOCK_IOTUNE_SET_BYTES_MAX_LENGTH = 1 << 6,
    QEMU_BLOCK_IOTUNE_SET_IOPS_MAX_LENGTH  = 1 << 7,
} qemuBlockIoTuneSetFlags;


/* If the user didn't specify bytes limits, inherit previous values;
 * likewise if the user didn't specify iops limits.  */
static int
qemuDomainSetBlockIoTuneDefaults(virDomainBlockIoTuneInfoPtr newinfo,
                                 virDomainBlockIoTuneInfoPtr oldinfo,
                                 qemuBlockIoTuneSetFlags set_fields)
{
#define SET_IOTUNE_DEFAULTS(BOOL, FIELD) \
    if (!(set_fields & QEMU_BLOCK_IOTUNE_SET_##BOOL)) { \
        newinfo->total_##FIELD = oldinfo->total_##FIELD; \
        newinfo->read_##FIELD = oldinfo->read_##FIELD; \
        newinfo->write_##FIELD = oldinfo->write_##FIELD; \
    }

    SET_IOTUNE_DEFAULTS(BYTES, bytes_sec);
    SET_IOTUNE_DEFAULTS(BYTES_MAX, bytes_sec_max);
    SET_IOTUNE_DEFAULTS(IOPS, iops_sec);
    SET_IOTUNE_DEFAULTS(IOPS_MAX, iops_sec_max);
#undef SET_IOTUNE_DEFAULTS

    if (!(set_fields & QEMU_BLOCK_IOTUNE_SET_SIZE_IOPS))
        newinfo->size_iops_sec = oldinfo->size_iops_sec;
    if (!(set_fields & QEMU_BLOCK_IOTUNE_SET_GROUP_NAME))
        newinfo->group_name = g_strdup(oldinfo->group_name);

    /* The length field is handled a bit differently. If not defined/set,
     * QEMU will default these to 0 or 1 depending on whether something in
     * the same family is set or not.
     *
     * Similar to other values, if nothing in the family is defined/set,
     * then take whatever is in the oldinfo.
     *
     * To clear an existing limit, a 0 is provided; however, passing that
     * 0 onto QEMU if there's a family value defined/set (or defaulted)
     * will cause an error. So, to mimic that, if our oldinfo was set and
     * our newinfo is clearing, then set max_length based on whether we
     * have a value in the family set/defined. */
#define SET_MAX_LENGTH(BOOL, FIELD) \
    if (!(set_fields & QEMU_BLOCK_IOTUNE_SET_##BOOL)) \
        newinfo->FIELD##_max_length = oldinfo->FIELD##_max_length; \
    else if ((set_fields & QEMU_BLOCK_IOTUNE_SET_##BOOL) && \
             oldinfo->FIELD##_max_length && \
             !newinfo->FIELD##_max_length) \
        newinfo->FIELD##_max_length = (newinfo->FIELD || \
                                       newinfo->FIELD##_max) ? 1 : 0;

    SET_MAX_LENGTH(BYTES_MAX_LENGTH, total_bytes_sec);
    SET_MAX_LENGTH(BYTES_MAX_LENGTH, read_bytes_sec);
    SET_MAX_LENGTH(BYTES_MAX_LENGTH, write_bytes_sec);
    SET_MAX_LENGTH(IOPS_MAX_LENGTH, total_iops_sec);
    SET_MAX_LENGTH(IOPS_MAX_LENGTH, read_iops_sec);
    SET_MAX_LENGTH(IOPS_MAX_LENGTH, write_iops_sec);

#undef SET_MAX_LENGTH

    return 0;
}


static int
qemuDomainSetBlockIoTune(virDomainPtr dom,
                         const char *path,
                         virTypedParameterPtr params,
                         int nparams,
                         unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuDomainObjPrivatePtr priv;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    virDomainBlockIoTuneInfo info;
    g_autofree char *drivealias = NULL;
    const char *qdevid = NULL;
    int ret = -1;
    size_t i;
    virDomainDiskDefPtr conf_disk = NULL;
    virDomainDiskDefPtr disk;
    qemuBlockIoTuneSetFlags set_fields = 0;
    bool supportMaxOptions = true;
    bool supportGroupNameOption = true;
    bool supportMaxLengthOptions = true;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virObjectEventPtr event = NULL;
    virTypedParameterPtr eventParams = NULL;
    int eventNparams = 0;
    int eventMaxparams = 0;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);
    if (virTypedParamsValidate(params, nparams,
                               VIR_DOMAIN_BLOCK_IOTUNE_TOTAL_BYTES_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_READ_BYTES_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_WRITE_BYTES_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_TOTAL_IOPS_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_READ_IOPS_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_WRITE_IOPS_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_TOTAL_BYTES_SEC_MAX,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_READ_BYTES_SEC_MAX,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_WRITE_BYTES_SEC_MAX,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_TOTAL_IOPS_SEC_MAX,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_READ_IOPS_SEC_MAX,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_WRITE_IOPS_SEC_MAX,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_SIZE_IOPS_SEC,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_GROUP_NAME,
                               VIR_TYPED_PARAM_STRING,
                               VIR_DOMAIN_BLOCK_IOTUNE_TOTAL_BYTES_SEC_MAX_LENGTH,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_READ_BYTES_SEC_MAX_LENGTH,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_WRITE_BYTES_SEC_MAX_LENGTH,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_TOTAL_IOPS_SEC_MAX_LENGTH,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_READ_IOPS_SEC_MAX_LENGTH,
                               VIR_TYPED_PARAM_ULLONG,
                               VIR_DOMAIN_BLOCK_IOTUNE_WRITE_IOPS_SEC_MAX_LENGTH,
                               VIR_TYPED_PARAM_ULLONG,
                               NULL) < 0)
        return -1;

    memset(&info, 0, sizeof(info));

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    if (virDomainSetBlockIoTuneEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    cfg = virQEMUDriverGetConfig(driver);

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    priv = vm->privateData;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (virTypedParamsAddString(&eventParams, &eventNparams, &eventMaxparams,
                                VIR_DOMAIN_TUNABLE_BLKDEV_DISK, path) < 0)
        goto endjob;

#define SET_IOTUNE_FIELD(FIELD, BOOL, CONST) \
    if (STREQ(param->field, VIR_DOMAIN_BLOCK_IOTUNE_##CONST)) { \
        info.FIELD = param->value.ul; \
        set_fields |= QEMU_BLOCK_IOTUNE_SET_##BOOL; \
        if (virTypedParamsAddULLong(&eventParams, &eventNparams, \
                                    &eventMaxparams, \
                                    VIR_DOMAIN_TUNABLE_BLKDEV_##CONST, \
                                    param->value.ul) < 0) \
            goto endjob; \
        continue; \
    }

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (param->value.ul > QEMU_BLOCK_IOTUNE_MAX) {
            virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                           _("block I/O throttle limit value must"
                             " be no more than %llu"), QEMU_BLOCK_IOTUNE_MAX);
            goto endjob;
        }

        SET_IOTUNE_FIELD(total_bytes_sec, BYTES, TOTAL_BYTES_SEC);
        SET_IOTUNE_FIELD(read_bytes_sec, BYTES, READ_BYTES_SEC);
        SET_IOTUNE_FIELD(write_bytes_sec, BYTES, WRITE_BYTES_SEC);
        SET_IOTUNE_FIELD(total_iops_sec, IOPS, TOTAL_IOPS_SEC);
        SET_IOTUNE_FIELD(read_iops_sec, IOPS, READ_IOPS_SEC);
        SET_IOTUNE_FIELD(write_iops_sec, IOPS, WRITE_IOPS_SEC);

        SET_IOTUNE_FIELD(total_bytes_sec_max, BYTES_MAX,
                         TOTAL_BYTES_SEC_MAX);
        SET_IOTUNE_FIELD(read_bytes_sec_max, BYTES_MAX,
                         READ_BYTES_SEC_MAX);
        SET_IOTUNE_FIELD(write_bytes_sec_max, BYTES_MAX,
                         WRITE_BYTES_SEC_MAX);
        SET_IOTUNE_FIELD(total_iops_sec_max, IOPS_MAX,
                         TOTAL_IOPS_SEC_MAX);
        SET_IOTUNE_FIELD(read_iops_sec_max, IOPS_MAX,
                         READ_IOPS_SEC_MAX);
        SET_IOTUNE_FIELD(write_iops_sec_max, IOPS_MAX,
                         WRITE_IOPS_SEC_MAX);
        SET_IOTUNE_FIELD(size_iops_sec, SIZE_IOPS, SIZE_IOPS_SEC);

        /* NB: Cannot use macro since this is a value.s not a value.ul */
        if (STREQ(param->field, VIR_DOMAIN_BLOCK_IOTUNE_GROUP_NAME)) {
            info.group_name = g_strdup(param->value.s);
            set_fields |= QEMU_BLOCK_IOTUNE_SET_GROUP_NAME;
            if (virTypedParamsAddString(&eventParams, &eventNparams,
                                        &eventMaxparams,
                                        VIR_DOMAIN_TUNABLE_BLKDEV_GROUP_NAME,
                                        param->value.s) < 0)
                goto endjob;
            continue;
        }

        SET_IOTUNE_FIELD(total_bytes_sec_max_length, BYTES_MAX_LENGTH,
                         TOTAL_BYTES_SEC_MAX_LENGTH);
        SET_IOTUNE_FIELD(read_bytes_sec_max_length, BYTES_MAX_LENGTH,
                         READ_BYTES_SEC_MAX_LENGTH);
        SET_IOTUNE_FIELD(write_bytes_sec_max_length, BYTES_MAX_LENGTH,
                         WRITE_BYTES_SEC_MAX_LENGTH);
        SET_IOTUNE_FIELD(total_iops_sec_max_length, IOPS_MAX_LENGTH,
                         TOTAL_IOPS_SEC_MAX_LENGTH);
        SET_IOTUNE_FIELD(read_iops_sec_max_length, IOPS_MAX_LENGTH,
                         READ_IOPS_SEC_MAX_LENGTH);
        SET_IOTUNE_FIELD(write_iops_sec_max_length, IOPS_MAX_LENGTH,
                         WRITE_IOPS_SEC_MAX_LENGTH);
    }

#undef SET_IOTUNE_FIELD

    if ((info.total_bytes_sec && info.read_bytes_sec) ||
        (info.total_bytes_sec && info.write_bytes_sec)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("total and read/write of bytes_sec "
                         "cannot be set at the same time"));
        goto endjob;
    }

    if ((info.total_iops_sec && info.read_iops_sec) ||
        (info.total_iops_sec && info.write_iops_sec)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("total and read/write of iops_sec "
                         "cannot be set at the same time"));
        goto endjob;
    }

    if ((info.total_bytes_sec_max && info.read_bytes_sec_max) ||
        (info.total_bytes_sec_max && info.write_bytes_sec_max)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("total and read/write of bytes_sec_max "
                         "cannot be set at the same time"));
        goto endjob;
    }

    if ((info.total_iops_sec_max && info.read_iops_sec_max) ||
        (info.total_iops_sec_max && info.write_iops_sec_max)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("total and read/write of iops_sec_max "
                         "cannot be set at the same time"));
        goto endjob;
    }

    if (def) {
        supportMaxOptions = virQEMUCapsGet(priv->qemuCaps,
                                           QEMU_CAPS_DRIVE_IOTUNE_MAX);
        supportGroupNameOption = virQEMUCapsGet(priv->qemuCaps,
                                                QEMU_CAPS_DRIVE_IOTUNE_GROUP);
        supportMaxLengthOptions =
            virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_MAX_LENGTH);

        if (!supportMaxOptions &&
            (set_fields & (QEMU_BLOCK_IOTUNE_SET_BYTES_MAX |
                           QEMU_BLOCK_IOTUNE_SET_IOPS_MAX |
                           QEMU_BLOCK_IOTUNE_SET_SIZE_IOPS))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("a block I/O throttling parameter is not "
                             "supported with this QEMU binary"));
             goto endjob;
        }

        if (!supportGroupNameOption &&
            (set_fields & QEMU_BLOCK_IOTUNE_SET_GROUP_NAME)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("the block I/O throttling group parameter is not "
                             "supported with this QEMU binary"));
             goto endjob;
        }

        if (!supportMaxLengthOptions &&
            (set_fields & (QEMU_BLOCK_IOTUNE_SET_BYTES_MAX_LENGTH |
                           QEMU_BLOCK_IOTUNE_SET_IOPS_MAX_LENGTH))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("a block I/O throttling length parameter is not "
                             "supported with this QEMU binary"));
             goto endjob;
        }

        if (!(disk = qemuDomainDiskByName(def, path)))
            goto endjob;

        if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV)) {
            qdevid = QEMU_DOMAIN_DISK_PRIVATE(disk)->qomName;
        } else {
            if (!(drivealias = qemuAliasDiskDriveFromDisk(disk)))
                goto endjob;
        }

        if (qemuDomainSetBlockIoTuneDefaults(&info, &disk->blkdeviotune,
                                             set_fields) < 0)
            goto endjob;

#define CHECK_MAX(val, _bool) \
        do { \
            if (info.val##_max) { \
                if (!info.val) { \
                    if (QEMU_BLOCK_IOTUNE_SET_##_bool) { \
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, \
                                       _("cannot reset '%s' when " \
                                         "'%s' is set"), \
                                       #val, #val "_max"); \
                    } else { \
                        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, \
                                       _("value '%s' cannot be set if " \
                                         "'%s' is not set"), \
                                       #val "_max", #val); \
                    } \
                    goto endjob; \
                } \
                if (info.val##_max < info.val) { \
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, \
                                   _("value '%s' cannot be " \
                                     "smaller than '%s'"), \
                                   #val "_max", #val); \
                    goto endjob; \
                } \
            } \
        } while (false)

        CHECK_MAX(total_bytes_sec, BYTES);
        CHECK_MAX(read_bytes_sec, BYTES);
        CHECK_MAX(write_bytes_sec, BYTES);
        CHECK_MAX(total_iops_sec, IOPS);
        CHECK_MAX(read_iops_sec, IOPS);
        CHECK_MAX(write_iops_sec, IOPS);

#undef CHECK_MAX

         /* NB: Let's let QEMU decide how to handle issues with _length
          * via the JSON error code from the block_set_io_throttle call */

        qemuDomainObjEnterMonitor(driver, vm);
        ret = qemuMonitorSetBlockIoThrottle(priv->mon, drivealias, qdevid,
                                            &info, supportMaxOptions,
                                            set_fields & QEMU_BLOCK_IOTUNE_SET_GROUP_NAME,
                                            supportMaxLengthOptions);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            ret = -1;
        if (ret < 0)
            goto endjob;
        ret = -1;

        if (virDomainDiskSetBlockIOTune(disk, &info) < 0)
            goto endjob;

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;

        if (eventNparams) {
            event = virDomainEventTunableNewFromDom(dom, eventParams, eventNparams);
            eventNparams = 0;
            virObjectEventStateQueue(driver->domainEventState, event);
        }
    }

    if (persistentDef) {
        if (!(conf_disk = virDomainDiskByName(persistentDef, path, true))) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("missing persistent configuration for disk '%s'"),
                           path);
            goto endjob;
        }

        if (qemuDomainSetBlockIoTuneDefaults(&info, &conf_disk->blkdeviotune,
                                             set_fields) < 0)
            goto endjob;

        if (virDomainDiskSetBlockIOTune(conf_disk, &info) < 0)
            goto endjob;

        if (virDomainDefSave(persistentDef, driver->xmlopt,
                             cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;
 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    VIR_FREE(info.group_name);
    virDomainObjEndAPI(&vm);
    if (eventNparams)
        virTypedParamsFree(eventParams, eventNparams);
    return ret;
}

static int
qemuDomainGetBlockIoTune(virDomainPtr dom,
                         const char *path,
                         virTypedParameterPtr params,
                         int *nparams,
                         unsigned int flags)
{
    virDomainDiskDefPtr disk;
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuDomainObjPrivatePtr priv = NULL;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    virDomainBlockIoTuneInfo reply = {0};
    g_autofree char *drivealias = NULL;
    const char *qdevid = NULL;
    int ret = -1;
    int maxparams;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG |
                  VIR_TYPED_PARAM_STRING_OKAY, -1);

    /* We don't return strings, and thus trivially support this flag.  */
    flags &= ~VIR_TYPED_PARAM_STRING_OKAY;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    priv = vm->privateData;

    if (virDomainGetBlockIoTuneEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    /* the API check guarantees that only one of the definitions will be set */
    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        /* If the VM is running, we can check if the current VM can use
         * optional parameters or not. */
        maxparams = QEMU_NB_BLOCK_IO_TUNE_BASE_PARAMS;
        if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_MAX))
            maxparams += QEMU_NB_BLOCK_IO_TUNE_MAX_PARAMS;
        if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_GROUP))
            maxparams += QEMU_NB_BLOCK_IO_TUNE_GROUP_PARAMS;
        if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_DRIVE_IOTUNE_MAX_LENGTH))
            maxparams += QEMU_NB_BLOCK_IO_TUNE_LENGTH_PARAMS;
    } else {
        maxparams = QEMU_NB_BLOCK_IO_TUNE_ALL_PARAMS;
    }

    if (*nparams == 0) {
        *nparams = maxparams;
        ret = 0;
        goto endjob;
    } else if (*nparams < maxparams) {
        maxparams = *nparams;
    }

    *nparams = 0;

    if (def) {
        if (!(disk = qemuDomainDiskByName(def, path)))
            goto endjob;

        if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV)) {
            qdevid = QEMU_DOMAIN_DISK_PRIVATE(disk)->qomName;
        } else {
            if (!(drivealias = qemuAliasDiskDriveFromDisk(disk)))
                goto endjob;
        }
        qemuDomainObjEnterMonitor(driver, vm);
        ret = qemuMonitorGetBlockIoThrottle(priv->mon, drivealias, qdevid, &reply);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto endjob;
        if (ret < 0)
            goto endjob;
    }

    if (persistentDef) {
        if (!(disk = virDomainDiskByName(persistentDef, path, true))) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("disk '%s' was not found in the domain config"),
                           path);
            goto endjob;
        }
        reply = disk->blkdeviotune;

        /* Group name needs to be copied since qemuMonitorGetBlockIoThrottle
         * allocates it as well */
        reply.group_name = g_strdup(disk->blkdeviotune.group_name);
    }

#define BLOCK_IOTUNE_ASSIGN(name, var) \
    if (*nparams < maxparams && \
        virTypedParameterAssign(&params[(*nparams)++], \
                                VIR_DOMAIN_BLOCK_IOTUNE_ ## name, \
                                VIR_TYPED_PARAM_ULLONG, \
                                reply.var) < 0) \
        goto endjob


    BLOCK_IOTUNE_ASSIGN(TOTAL_BYTES_SEC, total_bytes_sec);
    BLOCK_IOTUNE_ASSIGN(READ_BYTES_SEC, read_bytes_sec);
    BLOCK_IOTUNE_ASSIGN(WRITE_BYTES_SEC, write_bytes_sec);

    BLOCK_IOTUNE_ASSIGN(TOTAL_IOPS_SEC, total_iops_sec);
    BLOCK_IOTUNE_ASSIGN(READ_IOPS_SEC, read_iops_sec);
    BLOCK_IOTUNE_ASSIGN(WRITE_IOPS_SEC, write_iops_sec);

    BLOCK_IOTUNE_ASSIGN(TOTAL_BYTES_SEC_MAX, total_bytes_sec_max);
    BLOCK_IOTUNE_ASSIGN(READ_BYTES_SEC_MAX, read_bytes_sec_max);
    BLOCK_IOTUNE_ASSIGN(WRITE_BYTES_SEC_MAX, write_bytes_sec_max);

    BLOCK_IOTUNE_ASSIGN(TOTAL_IOPS_SEC_MAX, total_iops_sec_max);
    BLOCK_IOTUNE_ASSIGN(READ_IOPS_SEC_MAX, read_iops_sec_max);
    BLOCK_IOTUNE_ASSIGN(WRITE_IOPS_SEC_MAX, write_iops_sec_max);

    BLOCK_IOTUNE_ASSIGN(SIZE_IOPS_SEC, size_iops_sec);

    if (*nparams < maxparams) {
        if (virTypedParameterAssign(&params[(*nparams)++],
                                    VIR_DOMAIN_BLOCK_IOTUNE_GROUP_NAME,
                                    VIR_TYPED_PARAM_STRING,
                                    reply.group_name) < 0)
            goto endjob;

        reply.group_name = NULL;
    }

    BLOCK_IOTUNE_ASSIGN(TOTAL_BYTES_SEC_MAX_LENGTH, total_bytes_sec_max_length);
    BLOCK_IOTUNE_ASSIGN(READ_BYTES_SEC_MAX_LENGTH, read_bytes_sec_max_length);
    BLOCK_IOTUNE_ASSIGN(WRITE_BYTES_SEC_MAX_LENGTH, write_bytes_sec_max_length);

    BLOCK_IOTUNE_ASSIGN(TOTAL_IOPS_SEC_MAX_LENGTH, total_iops_sec_max_length);
    BLOCK_IOTUNE_ASSIGN(READ_IOPS_SEC_MAX_LENGTH, read_iops_sec_max_length);
    BLOCK_IOTUNE_ASSIGN(WRITE_IOPS_SEC_MAX_LENGTH, write_iops_sec_max_length);
#undef BLOCK_IOTUNE_ASSIGN

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    VIR_FREE(reply.group_name);
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainGetDiskErrors(virDomainPtr dom,
                        virDomainDiskErrorPtr errors,
                        unsigned int nerrors,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuDomainObjPrivatePtr priv;
    virHashTablePtr table = NULL;
    bool blockdev = false;
    int ret = -1;
    size_t i;
    int n = 0;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;
    blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);

    if (virDomainGetDiskErrorsEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!errors) {
        ret = vm->def->ndisks;
        goto endjob;
    }

    qemuDomainObjEnterMonitor(driver, vm);
    table = qemuMonitorGetBlockInfo(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto endjob;
    if (!table)
        goto endjob;

    for (i = n = 0; i < vm->def->ndisks; i++) {
        struct qemuDomainDiskInfo *info;
        virDomainDiskDefPtr disk = vm->def->disks[i];
        qemuDomainDiskPrivatePtr diskPriv = QEMU_DOMAIN_DISK_PRIVATE(disk);
        const char *entryname = disk->info.alias;

        if (blockdev)
            entryname = diskPriv->qomName;

        if ((info = virHashLookup(table, entryname)) &&
            info->io_status != VIR_DOMAIN_DISK_ERROR_NONE) {
            if (n == nerrors)
                break;

            errors[n].disk = g_strdup(disk->dst);
            errors[n].error = info->io_status;
            n++;
        }
    }

    ret = n;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    virHashFree(table);
    if (ret < 0) {
        for (i = 0; i < n; i++)
            VIR_FREE(errors[i].disk);
    }
    return ret;
}

static int
qemuDomainSetMetadata(virDomainPtr dom,
                      int type,
                      const char *metadata,
                      const char *key,
                      const char *uri,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainSetMetadataEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    ret = virDomainObjSetMetadata(vm, type, metadata, key, uri,
                                  driver->xmlopt, cfg->stateDir,
                                  cfg->configDir, flags);

    if (ret == 0) {
        virObjectEventPtr ev = NULL;
        ev = virDomainEventMetadataChangeNewFromObj(vm, type, uri);
        virObjectEventStateQueue(driver->domainEventState, ev);
    }

    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static char *
qemuDomainGetMetadata(virDomainPtr dom,
                      int type,
                      const char *uri,
                      unsigned int flags)
{
    virDomainObjPtr vm;
    char *ret = NULL;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return NULL;

    if (virDomainGetMetadataEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    ret = virDomainObjGetMetadata(vm, type, uri, flags);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainGetCPUStats(virDomainPtr domain,
                      virTypedParameterPtr params,
                      unsigned int nparams,
                      int start_cpu,
                      unsigned int ncpus,
                      unsigned int flags)
{
    virDomainObjPtr vm = NULL;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;
    virBitmapPtr guestvcpus = NULL;

    virCheckFlags(VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        return -1;

    priv = vm->privateData;

    if (virDomainGetCPUStatsEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    if (!virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUACCT)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cgroup CPUACCT controller is not mounted"));
        goto cleanup;
    }

    if (qemuDomainHasVcpuPids(vm) &&
        !(guestvcpus = virDomainDefGetOnlineVcpumap(vm->def)))
        goto cleanup;

    if (start_cpu == -1)
        ret = virCgroupGetDomainTotalCpuStats(priv->cgroup,
                                              params, nparams);
    else
        ret = virCgroupGetPercpuStats(priv->cgroup, params, nparams,
                                      start_cpu, ncpus, guestvcpus);
 cleanup:
    virBitmapFree(guestvcpus);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainProbeQMPCurrentMachine(virQEMUDriverPtr driver,
                                 virDomainObjPtr vm,
                                 bool *wakeupSupported)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    qemuMonitorCurrentMachineInfo info = { 0 };
    int rv;

    qemuDomainObjEnterMonitor(driver, vm);
    rv = qemuMonitorGetCurrentMachineInfo(priv->mon, &info);
    if (qemuDomainObjExitMonitor(driver, vm) < 0 ||
        rv < 0)
        return -1;

    *wakeupSupported = info.wakeupSuspendSupport;
    return 0;
}


/* returns -1 on error, or if query is not supported, 0 if query was successful */
static int
qemuDomainQueryWakeupSuspendSupport(virQEMUDriverPtr driver,
                                    virDomainObjPtr vm,
                                    bool *wakeupSupported)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    int ret = -1;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_QUERY_CURRENT_MACHINE))
        return -1;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        return -1;

    if ((ret = virDomainObjCheckActive(vm)) < 0)
        goto endjob;

    ret = qemuDomainProbeQMPCurrentMachine(driver, vm, wakeupSupported);

 endjob:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int
qemuDomainPMSuspendAgent(virQEMUDriverPtr driver,
                         virDomainObjPtr vm,
                         unsigned int target)
{
    qemuAgentPtr agent;
    int ret = -1;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        return -1;

    if ((ret = virDomainObjCheckActive(vm)) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentSuspend(agent, target);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndAgentJob(vm);
    return ret;
}


static int
qemuDomainPMSuspendForDuration(virDomainPtr dom,
                               unsigned int target,
                               unsigned long long duration,
                               unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    bool wakeupSupported;

    virCheckFlags(0, -1);

    if (duration) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("Duration not supported. Use 0 for now"));
        return -1;
    }

    if (!(target == VIR_NODE_SUSPEND_TARGET_MEM ||
          target == VIR_NODE_SUSPEND_TARGET_DISK ||
          target == VIR_NODE_SUSPEND_TARGET_HYBRID)) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Unknown suspend target: %u"),
                       target);
        return -1;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPMSuspendForDurationEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    /*
     * The case we want to handle here is when QEMU has the API (i.e.
     * QEMU_CAPS_QUERY_CURRENT_MACHINE is set). Otherwise, do not interfere
     * with the suspend process. This means that existing running domains,
     * that don't know about this cap, will keep their old behavior of
     * suspending 'in the dark'.
     */
    if (qemuDomainQueryWakeupSuspendSupport(driver, vm, &wakeupSupported) == 0) {
        if (!wakeupSupported) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("Domain does not have suspend support"));
            goto cleanup;
        }
    }

    if (vm->def->pm.s3 || vm->def->pm.s4) {
        if (vm->def->pm.s3 == VIR_TRISTATE_BOOL_NO &&
            (target == VIR_NODE_SUSPEND_TARGET_MEM ||
             target == VIR_NODE_SUSPEND_TARGET_HYBRID)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("S3 state is disabled for this domain"));
            goto cleanup;
        }

        if (vm->def->pm.s4 == VIR_TRISTATE_BOOL_NO &&
            target == VIR_NODE_SUSPEND_TARGET_DISK) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("S4 state is disabled for this domain"));
            goto cleanup;
        }
    }

    ret = qemuDomainPMSuspendAgent(driver, vm, target);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuDomainPMWakeup(virDomainPtr dom,
                   unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    qemuDomainObjPrivatePtr priv;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainPMWakeupEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    priv = vm->privateData;

    qemuDomainObjEnterMonitor(driver, vm);
    ret = qemuMonitorSystemWakeup(priv->mon);
    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        ret = -1;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static int
qemuConnectListAllDomains(virConnectPtr conn,
                          virDomainPtr **domains,
                          unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ALL, -1);

    if (virConnectListAllDomainsEnsureACL(conn) < 0)
        return -1;

    return virDomainObjListExport(driver->domains, conn, domains,
                                  virConnectListAllDomainsCheckACL, flags);
}

static char *
qemuDomainQemuAgentCommand(virDomainPtr domain,
                           const char *cmd,
                           int timeout,
                           unsigned int flags)
{
    virQEMUDriverPtr driver = domain->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;
    char *result = NULL;
    qemuAgentPtr agent;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainQemuAgentCommandEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    qemuDomainObjTaint(driver, vm, VIR_DOMAIN_TAINT_CUSTOM_GA_COMMAND, NULL);

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentArbitraryCommand(agent, cmd, &result, timeout);
    qemuDomainObjExitAgent(vm, agent);
    if (ret < 0)
        VIR_FREE(result);

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return result;
}


static int
qemuConnectDomainQemuMonitorEventRegister(virConnectPtr conn,
                                          virDomainPtr dom,
                                          const char *event,
                                          virConnectDomainQemuMonitorEventCallback callback,
                                          void *opaque,
                                          virFreeCallback freecb,
                                          unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    int ret = -1;

    if (virConnectDomainQemuMonitorEventRegisterEnsureACL(conn) < 0)
        return -1;

    if (virDomainQemuMonitorEventStateRegisterID(conn,
                                                 driver->domainEventState,
                                                 dom, event, callback,
                                                 opaque, freecb, flags,
                                                 &ret) < 0)
        ret = -1;

    return ret;
}


static int
qemuConnectDomainQemuMonitorEventDeregister(virConnectPtr conn,
                                            int callbackID)
{
    virQEMUDriverPtr driver = conn->privateData;

    if (virConnectDomainQemuMonitorEventDeregisterEnsureACL(conn) < 0)
        return -1;

    if (virObjectEventStateDeregisterID(conn, driver->domainEventState,
                                        callbackID, true) < 0)
        return -1;

    return 0;
}


static int
qemuDomainFSTrim(virDomainPtr dom,
                 const char *mountPoint,
                 unsigned long long minimum,
                 unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuAgentPtr agent;
    int ret = -1;

    virCheckFlags(0, -1);

    if (mountPoint) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("Specifying mount point "
                         "is not supported for now"));
        return -1;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainFSTrimEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        goto cleanup;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentFSTrim(agent, minimum);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuNodeGetInfo(virConnectPtr conn,
                virNodeInfoPtr nodeinfo)
{
    if (virNodeGetInfoEnsureACL(conn) < 0)
        return -1;

    return virCapabilitiesGetNodeInfo(nodeinfo);
}


static int
qemuNodeGetCPUStats(virConnectPtr conn,
                    int cpuNum,
                    virNodeCPUStatsPtr params,
                    int *nparams,
                    unsigned int flags)
{
    if (virNodeGetCPUStatsEnsureACL(conn) < 0)
        return -1;

    return virHostCPUGetStats(cpuNum, params, nparams, flags);
}


static int
qemuNodeGetMemoryStats(virConnectPtr conn,
                       int cellNum,
                       virNodeMemoryStatsPtr params,
                       int *nparams,
                       unsigned int flags)
{
    if (virNodeGetMemoryStatsEnsureACL(conn) < 0)
        return -1;

    return virHostMemGetStats(cellNum, params, nparams, flags);
}


static int
qemuNodeGetCellsFreeMemory(virConnectPtr conn,
                           unsigned long long *freeMems,
                           int startCell,
                           int maxCells)
{
    if (virNodeGetCellsFreeMemoryEnsureACL(conn) < 0)
        return -1;

    return virHostMemGetCellsFree(freeMems, startCell, maxCells);
}


static unsigned long long
qemuNodeGetFreeMemory(virConnectPtr conn)
{
    unsigned long long freeMem;

    if (virNodeGetFreeMemoryEnsureACL(conn) < 0)
        return 0;

    if (virHostMemGetInfo(NULL, &freeMem) < 0)
        return 0;

    return freeMem;
}


static int
qemuNodeGetMemoryParameters(virConnectPtr conn,
                            virTypedParameterPtr params,
                            int *nparams,
                            unsigned int flags)
{
    if (virNodeGetMemoryParametersEnsureACL(conn) < 0)
        return -1;

    return virHostMemGetParameters(params, nparams, flags);
}


static int
qemuNodeSetMemoryParameters(virConnectPtr conn,
                            virTypedParameterPtr params,
                            int nparams,
                            unsigned int flags)
{
    if (virNodeSetMemoryParametersEnsureACL(conn) < 0)
        return -1;

    return virHostMemSetParameters(params, nparams, flags);
}


static int
qemuNodeGetCPUMap(virConnectPtr conn,
                  unsigned char **cpumap,
                  unsigned int *online,
                  unsigned int flags)
{
    if (virNodeGetCPUMapEnsureACL(conn) < 0)
        return -1;

    return virHostCPUGetMap(cpumap, online, flags);
}


static int
qemuNodeSuspendForDuration(virConnectPtr conn,
                           unsigned int target,
                           unsigned long long duration,
                           unsigned int flags)
{
    if (virNodeSuspendForDurationEnsureACL(conn) < 0)
        return -1;

    return virNodeSuspend(target, duration, flags);
}

static int
qemuConnectGetCPUModelNames(virConnectPtr conn,
                            const char *archName,
                            char ***models,
                            unsigned int flags)
{
    virArch arch;

    virCheckFlags(0, -1);
    if (virConnectGetCPUModelNamesEnsureACL(conn) < 0)
        return -1;

    if (!(arch = virArchFromString(archName))) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("cannot find architecture %s"),
                       archName);
        return -1;
    }

    return virCPUGetModels(arch, models);
}


static char *
qemuDomainGetHostname(virDomainPtr dom,
                      unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuAgentPtr agent;
    char *hostname = NULL;

    virCheckFlags(0, NULL);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return NULL;

    if (virDomainGetHostnameEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ignore_value(qemuAgentGetHostname(agent, &hostname));
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return hostname;
}


static int
qemuDomainGetTime(virDomainPtr dom,
                  long long *seconds,
                  unsigned int *nseconds,
                  unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuAgentPtr agent;
    int ret = -1;
    int rv;

    virCheckFlags(0, ret);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return ret;

    if (virDomainGetTimeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    rv = qemuAgentGetTime(agent, seconds, nseconds);
    qemuDomainObjExitAgent(vm, agent);

    if (rv < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSetTimeAgent(virQEMUDriverPtr driver,
                       virDomainObjPtr vm,
                       long long seconds,
                       unsigned int nseconds,
                       bool rtcSync)
{
    qemuAgentPtr agent;
    int ret = -1;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        return -1;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentSetTime(agent, seconds, nseconds, rtcSync);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int
qemuDomainSetTime(virDomainPtr dom,
                  long long seconds,
                  unsigned int nseconds,
                  unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuDomainObjPrivatePtr priv;
    virDomainObjPtr vm;
    bool rtcSync = flags & VIR_DOMAIN_TIME_SYNC;
    int ret = -1;
    int rv;

    virCheckFlags(VIR_DOMAIN_TIME_SYNC, ret);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return ret;

    if (virDomainSetTimeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    priv = vm->privateData;

    /* On x86, the rtc-reset-reinjection QMP command must be called after
     * setting the time to avoid trouble down the line. If the command is
     * not available, don't set the time at all and report an error */
    if (ARCH_IS_X86(vm->def->os.arch) &&
        !virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_RTC_RESET_REINJECTION))
    {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("cannot set time: qemu doesn't support "
                         "rtc-reset-reinjection command"));
        goto cleanup;
    }

    if (qemuDomainSetTimeAgent(driver, vm, seconds, nseconds, rtcSync) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    /* Don't try to call rtc-reset-reinjection if it's not available */
    if (virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_RTC_RESET_REINJECTION)) {
        qemuDomainObjEnterMonitor(driver, vm);
        rv = qemuMonitorRTCResetReinjection(priv->mon);
        if (qemuDomainObjExitMonitor(driver, vm) < 0)
            goto endjob;

        if (rv < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainFSFreeze(virDomainPtr dom,
                   const char **mountpoints,
                   unsigned int nmountpoints,
                   unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainFSFreezeEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    ret = qemuDomainSnapshotFSFreeze(vm, mountpoints, nmountpoints);

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainFSThaw(virDomainPtr dom,
                 const char **mountpoints,
                 unsigned int nmountpoints,
                 unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    if (mountpoints || nmountpoints) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("specifying mountpoints is not supported"));
        return ret;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainFSThawEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    ret = qemuDomainSnapshotFSThaw(vm, true);

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuNodeGetFreePages(virConnectPtr conn,
                     unsigned int npages,
                     unsigned int *pages,
                     int startCell,
                     unsigned int cellCount,
                     unsigned long long *counts,
                     unsigned int flags)
{
    virCheckFlags(0, -1);

    if (virNodeGetFreePagesEnsureACL(conn) < 0)
        return -1;

    return virHostMemGetFreePages(npages, pages, startCell, cellCount, counts);
}


static char *
qemuConnectGetDomainCapabilities(virConnectPtr conn,
                                 const char *emulatorbin,
                                 const char *arch_str,
                                 const char *machine,
                                 const char *virttype_str,
                                 unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    g_autoptr(virQEMUCaps) qemuCaps = NULL;
    virArch arch;
    virDomainVirtType virttype;
    g_autoptr(virDomainCaps) domCaps = NULL;

    virCheckFlags(0, NULL);

    if (virConnectGetDomainCapabilitiesEnsureACL(conn) < 0)
        return NULL;

    qemuCaps = virQEMUCapsCacheLookupDefault(driver->qemuCapsCache,
                                             emulatorbin,
                                             arch_str,
                                             virttype_str,
                                             machine,
                                             &arch, &virttype, &machine);
    if (!qemuCaps)
        return NULL;

    if (!(domCaps = virQEMUDriverGetDomainCapabilities(driver,
                                                       qemuCaps, machine,
                                                       arch, virttype)))
        return NULL;

    return virDomainCapsFormat(domCaps);
}


static int
qemuDomainGetStatsState(virQEMUDriverPtr driver G_GNUC_UNUSED,
                        virDomainObjPtr dom,
                        virTypedParamListPtr params,
                        unsigned int privflags G_GNUC_UNUSED)
{
    if (virTypedParamListAddInt(params, dom->state.state, "state.state") < 0)
        return -1;

    if (virTypedParamListAddInt(params, dom->state.reason, "state.reason") < 0)
        return -1;

    return 0;
}


typedef enum {
    QEMU_DOMAIN_STATS_HAVE_JOB = 1 << 0, /* job is entered, monitor can be
                                            accessed */
    QEMU_DOMAIN_STATS_BACKING  = 1 << 1, /* include backing chain in
                                            block stats */
} qemuDomainStatsFlags;


#define HAVE_JOB(flags) ((flags) & QEMU_DOMAIN_STATS_HAVE_JOB)


typedef struct _virQEMUResctrlMonData virQEMUResctrlMonData;
typedef virQEMUResctrlMonData *virQEMUResctrlMonDataPtr;
struct _virQEMUResctrlMonData {
    char *name;
    char *vcpus;
    virResctrlMonitorStatsPtr *stats;
    size_t nstats;
};


static void
qemuDomainFreeResctrlMonData(virQEMUResctrlMonDataPtr resdata)
{
    size_t i = 0;

    VIR_FREE(resdata->name);
    VIR_FREE(resdata->vcpus);
    for (i = 0; i < resdata->nstats; i++)
        virResctrlMonitorStatsFree(resdata->stats[i]);
    VIR_FREE(resdata->stats);
    VIR_FREE(resdata);
}


/**
 * qemuDomainGetResctrlMonData:
 * @dom: Pointer for the domain that the resctrl monitors reside in
 * @driver: Pointer to qemu driver
 * @resdata: Pointer of virQEMUResctrlMonDataPtr pointer for receiving the
 *            virQEMUResctrlMonDataPtr array. Caller is responsible for
 *            freeing the array.
 * @nresdata: Pointer of size_t to report the size virQEMUResctrlMonDataPtr
 *            array to caller. If *@nresdata is not 0, even if function
 *            returns an error, the caller is also required to call
 *            qemuDomainFreeResctrlMonData to free each element in the
 *            *@resdata array and then the array itself.
 * @tag: Could be VIR_RESCTRL_MONITOR_TYPE_CACHE for getting cache statistics
 *       from @dom cache monitors. VIR_RESCTRL_MONITOR_TYPE_MEMBW for
 *       getting memory bandwidth statistics from memory bandwidth monitors.
 *
 * Get cache or memory bandwidth statistics from @dom monitors.
 *
 * Returns -1 on failure, or 0 on success.
 */
static int
qemuDomainGetResctrlMonData(virQEMUDriverPtr driver,
                            virDomainObjPtr dom,
                            virQEMUResctrlMonDataPtr **resdata,
                            size_t *nresdata,
                            virResctrlMonitorType tag)
{
    virDomainResctrlDefPtr resctrl = NULL;
    virQEMUResctrlMonDataPtr res = NULL;
    char **features = NULL;
    g_autoptr(virCaps) caps = NULL;
    size_t i = 0;
    size_t j = 0;

    if (!(caps = virQEMUDriverGetCapabilities(driver, false)))
        return -1;

    switch (tag) {
    case VIR_RESCTRL_MONITOR_TYPE_CACHE:
        if (caps->host.cache.monitor)
            features = caps->host.cache.monitor->features;
        break;
    case VIR_RESCTRL_MONITOR_TYPE_MEMBW:
        if (caps->host.memBW.monitor)
            features = caps->host.memBW.monitor->features;
        break;
    case VIR_RESCTRL_MONITOR_TYPE_UNSUPPORT:
    case VIR_RESCTRL_MONITOR_TYPE_LAST:
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("Unsupported resctrl monitor type"));
        return -1;
    }

    if (virStringListLength((const char * const *)features) == 0)
        return 0;

    for (i = 0; i < dom->def->nresctrls; i++) {
        resctrl = dom->def->resctrls[i];

        for (j = 0; j < resctrl->nmonitors; j++) {
            virDomainResctrlMonDefPtr domresmon = NULL;
            virResctrlMonitorPtr monitor = NULL;

            domresmon = resctrl->monitors[j];
            monitor = domresmon->instance;

            if (domresmon->tag != tag)
                continue;

            if (VIR_ALLOC(res) < 0)
                return -1;

            /* If virBitmapFormat successfully returns an vcpu string, then
             * res.vcpus is assigned with an memory space holding it,
             * let this newly allocated memory buffer to be freed along with
             * the free of 'res' */
            if (!(res->vcpus = virBitmapFormat(domresmon->vcpus)))
                goto error;

            res->name = g_strdup(virResctrlMonitorGetID(monitor));

            if (virResctrlMonitorGetStats(monitor, (const char **)features,
                                          &res->stats, &res->nstats) < 0)
                goto error;

            if (VIR_APPEND_ELEMENT(*resdata, *nresdata, res) < 0)
                goto error;
        }
    }

    return 0;

 error:
    qemuDomainFreeResctrlMonData(res);
    return -1;
}


static int
qemuDomainGetStatsMemoryBandwidth(virQEMUDriverPtr driver,
                                  virDomainObjPtr dom,
                                  virTypedParamListPtr params)
{
    virQEMUResctrlMonDataPtr *resdata = NULL;
    char **features = NULL;
    size_t nresdata = 0;
    size_t i = 0;
    size_t j = 0;
    size_t k = 0;
    int ret = -1;

    if (!virDomainObjIsActive(dom))
        return 0;

    if (qemuDomainGetResctrlMonData(driver, dom, &resdata, &nresdata,
                                    VIR_RESCTRL_MONITOR_TYPE_MEMBW) < 0)
        goto cleanup;

    if (nresdata == 0)
        return 0;

    if (virTypedParamListAddUInt(params, nresdata,
                                 "memory.bandwidth.monitor.count") < 0)
        goto cleanup;

    for (i = 0; i < nresdata; i++) {
        if (virTypedParamListAddString(params, resdata[i]->name,
                                       "memory.bandwidth.monitor.%zu.name",
                                       i) < 0)
            goto cleanup;

        if (virTypedParamListAddString(params, resdata[i]->vcpus,
                                       "memory.bandwidth.monitor.%zu.vcpus",
                                       i) < 0)
            goto cleanup;

        if (virTypedParamListAddUInt(params, resdata[i]->nstats,
                                     "memory.bandwidth.monitor.%zu.node.count",
                                     i) < 0)
            goto cleanup;


        for (j = 0; j < resdata[i]->nstats; j++) {
            if (virTypedParamListAddUInt(params, resdata[i]->stats[j]->id,
                                         "memory.bandwidth.monitor.%zu."
                                         "node.%zu.id",
                                         i, j) < 0)
                goto cleanup;


            features = resdata[i]->stats[j]->features;
            for (k = 0; features[k]; k++) {
                if (STREQ(features[k], "mbm_local_bytes")) {
                    /* The accumulative data passing through local memory
                     * controller is recorded with 64 bit counter. */
                    if (virTypedParamListAddULLong(params,
                                                   resdata[i]->stats[j]->vals[k],
                                                   "memory.bandwidth.monitor."
                                                   "%zu.node.%zu.bytes.local",
                                                   i, j) < 0)
                        goto cleanup;
                }

                if (STREQ(features[k], "mbm_total_bytes")) {
                    /* The accumulative data passing through local and remote
                     * memory controller is recorded with 64 bit counter. */
                    if (virTypedParamListAddULLong(params,
                                                   resdata[i]->stats[j]->vals[k],
                                                   "memory.bandwidth.monitor."
                                                   "%zu.node.%zu.bytes.total",
                                                   i, j) < 0)
                        goto cleanup;
                }
            }
        }
    }

    ret = 0;
 cleanup:
    for (i = 0; i < nresdata; i++)
        qemuDomainFreeResctrlMonData(resdata[i]);
    VIR_FREE(resdata);
    return ret;
}


static int
qemuDomainGetStatsCpuCache(virQEMUDriverPtr driver,
                           virDomainObjPtr dom,
                           virTypedParamListPtr params)
{
    virQEMUResctrlMonDataPtr *resdata = NULL;
    size_t nresdata = 0;
    size_t i = 0;
    size_t j = 0;
    int ret = -1;

    if (!virDomainObjIsActive(dom))
        return 0;

    if (qemuDomainGetResctrlMonData(driver, dom, &resdata, &nresdata,
                                    VIR_RESCTRL_MONITOR_TYPE_CACHE) < 0)
        goto cleanup;

    if (virTypedParamListAddUInt(params, nresdata, "cpu.cache.monitor.count") < 0)
        goto cleanup;

    for (i = 0; i < nresdata; i++) {
        if (virTypedParamListAddString(params, resdata[i]->name,
                                       "cpu.cache.monitor.%zu.name", i) < 0)
            goto cleanup;

        if (virTypedParamListAddString(params, resdata[i]->vcpus,
                                       "cpu.cache.monitor.%zu.vcpus", i) < 0)
            goto cleanup;

        if (virTypedParamListAddUInt(params, resdata[i]->nstats,
                                     "cpu.cache.monitor.%zu.bank.count", i) < 0)
            goto cleanup;

        for (j = 0; j < resdata[i]->nstats; j++) {
            if (virTypedParamListAddUInt(params, resdata[i]->stats[j]->id,
                                         "cpu.cache.monitor.%zu.bank.%zu.id", i, j) < 0)
                goto cleanup;

            /* 'resdata[i]->stats[j]->vals[0]' keeps the value of how many last
             * level cache in bank j currently occupied by the vcpus listed in
             * resource monitor i, in bytes. This value is reported through a
             * 64 bit hardware counter, so it is better to be arranged with
             * data type in 64 bit width, but considering the fact that
             * physical cache on a CPU could never be designed to be bigger
             * than 4G bytes in size, to keep the 'domstats' interface
             * historically consistent, it is safe to report the value with a
             * truncated 'UInt' data type here. */
            if (virTypedParamListAddUInt(params,
                                         (unsigned int)resdata[i]->stats[j]->vals[0],
                                         "cpu.cache.monitor.%zu.bank.%zu.bytes",
                                         i, j) < 0)
                goto cleanup;
        }
    }

    ret = 0;
 cleanup:
    for (i = 0; i < nresdata; i++)
        qemuDomainFreeResctrlMonData(resdata[i]);
    VIR_FREE(resdata);
    return ret;
}


static int
qemuDomainGetStatsCpuCgroup(virDomainObjPtr dom,
                            virTypedParamListPtr params)
{
    qemuDomainObjPrivatePtr priv = dom->privateData;
    unsigned long long cpu_time = 0;
    unsigned long long user_time = 0;
    unsigned long long sys_time = 0;
    int err = 0;

    if (!priv->cgroup)
        return 0;

    err = virCgroupGetCpuacctUsage(priv->cgroup, &cpu_time);
    if (!err && virTypedParamListAddULLong(params, cpu_time, "cpu.time") < 0)
        return -1;

    err = virCgroupGetCpuacctStat(priv->cgroup, &user_time, &sys_time);
    if (!err && virTypedParamListAddULLong(params, user_time, "cpu.user") < 0)
        return -1;
    if (!err && virTypedParamListAddULLong(params, sys_time, "cpu.system") < 0)
        return -1;

    return 0;
}


static int
qemuDomainGetStatsCpu(virQEMUDriverPtr driver,
                      virDomainObjPtr dom,
                      virTypedParamListPtr params,
                      unsigned int privflags G_GNUC_UNUSED)
{
    if (qemuDomainGetStatsCpuCgroup(dom, params) < 0)
        return -1;

    if (qemuDomainGetStatsCpuCache(driver, dom, params) < 0)
        return -1;

    return 0;
}


static int
qemuDomainGetStatsMemory(virQEMUDriverPtr driver,
                         virDomainObjPtr dom,
                         virTypedParamListPtr params,
                         unsigned int privflags G_GNUC_UNUSED)

{
    return qemuDomainGetStatsMemoryBandwidth(driver, dom, params);
}


static int
qemuDomainGetStatsBalloon(virQEMUDriverPtr driver,
                          virDomainObjPtr dom,
                          virTypedParamListPtr params,
                          unsigned int privflags)
{
    virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
    int nr_stats;
    unsigned long long cur_balloon = 0;
    size_t i;

    if (!virDomainDefHasMemballoon(dom->def)) {
        cur_balloon = virDomainDefGetMemoryTotal(dom->def);
    } else {
        cur_balloon = dom->def->mem.cur_balloon;
    }

    if (virTypedParamListAddULLong(params, cur_balloon, "balloon.current") < 0)
        return -1;

    if (virTypedParamListAddULLong(params, virDomainDefGetMemoryTotal(dom->def),
                                   "balloon.maximum") < 0)
        return -1;

    if (!HAVE_JOB(privflags) || !virDomainObjIsActive(dom))
        return 0;

    nr_stats = qemuDomainMemoryStatsInternal(driver, dom, stats,
                                             VIR_DOMAIN_MEMORY_STAT_NR);
    if (nr_stats < 0)
        return 0;

#define STORE_MEM_RECORD(TAG, NAME) \
    if (stats[i].tag == VIR_DOMAIN_MEMORY_STAT_ ##TAG) \
        if (virTypedParamListAddULLong(params, stats[i].val, "balloon." NAME) < 0) \
            return -1;

    for (i = 0; i < nr_stats; i++) {
        STORE_MEM_RECORD(SWAP_IN, "swap_in")
        STORE_MEM_RECORD(SWAP_OUT, "swap_out")
        STORE_MEM_RECORD(MAJOR_FAULT, "major_fault")
        STORE_MEM_RECORD(MINOR_FAULT, "minor_fault")
        STORE_MEM_RECORD(UNUSED, "unused")
        STORE_MEM_RECORD(AVAILABLE, "available")
        STORE_MEM_RECORD(RSS, "rss")
        STORE_MEM_RECORD(LAST_UPDATE, "last-update")
        STORE_MEM_RECORD(USABLE, "usable")
        STORE_MEM_RECORD(DISK_CACHES, "disk_caches")
        STORE_MEM_RECORD(HUGETLB_PGALLOC, "hugetlb_pgalloc")
        STORE_MEM_RECORD(HUGETLB_PGFAIL, "hugetlb_pgfail")
    }

#undef STORE_MEM_RECORD

    return 0;
}


static int
qemuDomainGetStatsVcpu(virQEMUDriverPtr driver,
                       virDomainObjPtr dom,
                       virTypedParamListPtr params,
                       unsigned int privflags)
{
    virDomainVcpuDefPtr vcpu;
    qemuDomainVcpuPrivatePtr vcpupriv;
    size_t i;
    int ret = -1;
    virVcpuInfoPtr cpuinfo = NULL;
    g_autofree unsigned long long *cpuwait = NULL;

    if (virTypedParamListAddUInt(params, virDomainDefGetVcpus(dom->def),
                                 "vcpu.current") < 0)
        return -1;

    if (virTypedParamListAddUInt(params, virDomainDefGetVcpusMax(dom->def),
                                 "vcpu.maximum") < 0)
        return -1;

    if (VIR_ALLOC_N(cpuinfo, virDomainDefGetVcpus(dom->def)) < 0 ||
        VIR_ALLOC_N(cpuwait, virDomainDefGetVcpus(dom->def)) < 0)
        goto cleanup;

    if (HAVE_JOB(privflags) && virDomainObjIsActive(dom) &&
        qemuDomainRefreshVcpuHalted(driver, dom, QEMU_ASYNC_JOB_NONE) < 0) {
            /* it's ok to be silent and go ahead, because halted vcpu info
             * wasn't here from the beginning */
            virResetLastError();
    }

    if (qemuDomainHelperGetVcpus(dom, cpuinfo, cpuwait,
                                 virDomainDefGetVcpus(dom->def),
                                 NULL, 0) < 0) {
        virResetLastError();
        ret = 0; /* it's ok to be silent and go ahead */
        goto cleanup;
    }

    for (i = 0; i < virDomainDefGetVcpus(dom->def); i++) {
        if (virTypedParamListAddInt(params, cpuinfo[i].state,
                                    "vcpu.%u.state", cpuinfo[i].number) < 0)
            goto cleanup;

        /* stats below are available only if the VM is alive */
        if (!virDomainObjIsActive(dom))
            continue;

        if (virTypedParamListAddULLong(params, cpuinfo[i].cpuTime,
                                       "vcpu.%u.time", cpuinfo[i].number) < 0)
            goto cleanup;

        if (virTypedParamListAddULLong(params, cpuwait[i],
                                       "vcpu.%u.wait", cpuinfo[i].number) < 0)
            goto cleanup;

        /* state below is extracted from the individual vcpu structs */
        if (!(vcpu = virDomainDefGetVcpu(dom->def, cpuinfo[i].number)))
            continue;

        vcpupriv = QEMU_DOMAIN_VCPU_PRIVATE(vcpu);

        if (vcpupriv->halted != VIR_TRISTATE_BOOL_ABSENT) {
            if (virTypedParamListAddBoolean(params,
                                            vcpupriv->halted == VIR_TRISTATE_BOOL_YES,
                                            "vcpu.%u.halted",
                                            cpuinfo[i].number) < 0)
                goto cleanup;
        }
    }

    ret = 0;

 cleanup:
    VIR_FREE(cpuinfo);
    return ret;
}

#define QEMU_ADD_NET_PARAM(params, num, name, value) \
    if (value >= 0 && \
        virTypedParamListAddULLong((params), (value), "net.%zu.%s", (num), (name)) < 0) \
        return -1;

static int
qemuDomainGetStatsInterface(virQEMUDriverPtr driver G_GNUC_UNUSED,
                            virDomainObjPtr dom,
                            virTypedParamListPtr params,
                            unsigned int privflags G_GNUC_UNUSED)
{
    size_t i;
    struct _virDomainInterfaceStats tmp;

    if (!virDomainObjIsActive(dom))
        return 0;

    if (virTypedParamListAddUInt(params, dom->def->nnets, "net.count") < 0)
        return -1;

    /* Check the path is one of the domain's network interfaces. */
    for (i = 0; i < dom->def->nnets; i++) {
        virDomainNetDefPtr net = dom->def->nets[i];
        virDomainNetType actualType;

        if (!net->ifname)
            continue;

        memset(&tmp, 0, sizeof(tmp));

        actualType = virDomainNetGetActualType(net);

        if (virTypedParamListAddString(params, net->ifname, "net.%zu.name", i) < 0)
            return -1;

        if (actualType == VIR_DOMAIN_NET_TYPE_VHOSTUSER) {
            if (virNetDevOpenvswitchInterfaceStats(net->ifname, &tmp) < 0) {
                virResetLastError();
                continue;
            }
        } else {
            if (virNetDevTapInterfaceStats(net->ifname, &tmp,
                                           !virDomainNetTypeSharesHostView(net)) < 0) {
                virResetLastError();
                continue;
            }
        }

        QEMU_ADD_NET_PARAM(params, i,
                           "rx.bytes", tmp.rx_bytes);
        QEMU_ADD_NET_PARAM(params, i,
                           "rx.pkts", tmp.rx_packets);
        QEMU_ADD_NET_PARAM(params, i,
                           "rx.errs", tmp.rx_errs);
        QEMU_ADD_NET_PARAM(params, i,
                           "rx.drop", tmp.rx_drop);
        QEMU_ADD_NET_PARAM(params, i,
                           "tx.bytes", tmp.tx_bytes);
        QEMU_ADD_NET_PARAM(params, i,
                           "tx.pkts", tmp.tx_packets);
        QEMU_ADD_NET_PARAM(params, i,
                           "tx.errs", tmp.tx_errs);
        QEMU_ADD_NET_PARAM(params, i,
                           "tx.drop", tmp.tx_drop);
    }

    return 0;
}

#undef QEMU_ADD_NET_PARAM

/* refresh information by opening images on the disk */
static int
qemuDomainGetStatsOneBlockFallback(virQEMUDriverPtr driver,
                                   virQEMUDriverConfigPtr cfg,
                                   virDomainObjPtr dom,
                                   virTypedParamListPtr params,
                                   virStorageSourcePtr src,
                                   size_t block_idx)
{
    if (virStorageSourceIsEmpty(src))
        return 0;

    if (qemuStorageLimitsRefresh(driver, cfg, dom, src, true) <= 0) {
        virResetLastError();
        return 0;
    }

    if (src->allocation &&
        virTypedParamListAddULLong(params, src->allocation,
                                   "block.%zu.allocation", block_idx) < 0)
        return -1;

    if (src->capacity &&
        virTypedParamListAddULLong(params, src->capacity,
                                   "block.%zu.capacity", block_idx) < 0)
        return -1;

    if (src->physical &&
        virTypedParamListAddULLong(params, src->physical,
                                   "block.%zu.physical", block_idx) < 0)
        return -1;

    return 0;
}


/**
 * qemuDomainGetStatsOneBlockRefreshNamed:
 * @src: disk source structure
 * @alias: disk alias
 * @stats: hash table containing stats for all disks
 * @nodedata: reply containing 'query-named-block-nodes' data
 *
 * Refresh disk block stats data (qemuBlockStatsPtr) which are present only
 * in the reply of 'query-named-block-nodes' in cases when the data was gathered
 * by using query-block originally.
 */
static void
qemuDomainGetStatsOneBlockRefreshNamed(virStorageSourcePtr src,
                                       const char *alias,
                                       virHashTablePtr stats,
                                       virHashTablePtr nodedata)
{
    qemuBlockStatsPtr entry;

    virJSONValuePtr data;
    unsigned long long tmp;

    if (!nodedata || !src->nodestorage)
        return;

    if (!(entry = virHashLookup(stats, alias)))
        return;

    if (!(data = virHashLookup(nodedata, src->nodestorage)))
        return;

    if (virJSONValueObjectGetNumberUlong(data, "write_threshold", &tmp) == 0)
        entry->write_threshold = tmp;
}


static int
qemuDomainGetStatsOneBlock(virQEMUDriverPtr driver,
                           virQEMUDriverConfigPtr cfg,
                           virDomainObjPtr dom,
                           virTypedParamListPtr params,
                           const char *entryname,
                           virStorageSourcePtr src,
                           size_t block_idx,
                           virHashTablePtr stats)
{
    qemuBlockStats *entry;

    /* the VM is offline so we have to go and load the stast from the disk by
     * ourselves */
    if (!virDomainObjIsActive(dom)) {
        return qemuDomainGetStatsOneBlockFallback(driver, cfg, dom, params,
                                                  src, block_idx);
    }

    /* In case where qemu didn't provide the stats we stop here rather than
     * trying to refresh the stats from the disk. Inability to provide stats is
     * usually caused by blocked storage so this would make libvirtd hang */
    if (!stats || !entryname || !(entry = virHashLookup(stats, entryname)))
        return 0;

    if (virTypedParamListAddULLong(params, entry->wr_highest_offset,
                                   "block.%zu.allocation", block_idx) < 0)
        return -1;

    if (entry->capacity &&
        virTypedParamListAddULLong(params, entry->capacity,
                                   "block.%zu.capacity", block_idx) < 0)
        return -1;

    if (entry->physical) {
        if (virTypedParamListAddULLong(params, entry->physical,
                                       "block.%zu.physical", block_idx) < 0)
            return -1;
    } else {
        if (qemuDomainStorageUpdatePhysical(driver, cfg, dom, src) == 0) {
            if (virTypedParamListAddULLong(params, src->physical,
                                           "block.%zu.physical", block_idx) < 0)
                return -1;
        }
    }

    return 0;
}


static int
qemuDomainGetStatsBlockExportBackendStorage(const char *entryname,
                                            virHashTablePtr stats,
                                            size_t recordnr,
                                            virTypedParamListPtr params)
{
    qemuBlockStats *entry;

    if (!stats || !entryname || !(entry = virHashLookup(stats, entryname)))
        return 0;

    if (entry->write_threshold &&
        virTypedParamListAddULLong(params, entry->write_threshold,
                                   "block.%zu.threshold", recordnr) < 0)
        return -1;

    return 0;
}


static int
qemuDomainGetStatsBlockExportFrontend(const char *frontendname,
                                      virHashTablePtr stats,
                                      size_t idx,
                                      virTypedParamListPtr par)
{
    qemuBlockStats *en;

    /* In case where qemu didn't provide the stats we stop here rather than
     * trying to refresh the stats from the disk. Inability to provide stats is
     * usually caused by blocked storage so this would make libvirtd hang */
    if (!stats || !frontendname || !(en = virHashLookup(stats, frontendname)))
        return 0;

    if (virTypedParamListAddULLong(par, en->rd_req, "block.%zu.rd.reqs", idx) < 0 ||
        virTypedParamListAddULLong(par, en->rd_bytes, "block.%zu.rd.bytes", idx) < 0 ||
        virTypedParamListAddULLong(par, en->rd_total_times, "block.%zu.rd.times", idx) < 0 ||
        virTypedParamListAddULLong(par, en->wr_req, "block.%zu.wr.reqs", idx) < 0 ||
        virTypedParamListAddULLong(par, en->wr_bytes, "block.%zu.wr.bytes", idx) < 0 ||
        virTypedParamListAddULLong(par, en->wr_total_times, "block.%zu.wr.times", idx) < 0 ||
        virTypedParamListAddULLong(par, en->flush_req, "block.%zu.fl.reqs", idx) < 0 ||
        virTypedParamListAddULLong(par, en->flush_total_times, "block.%zu.fl.times", idx) < 0)
        return -1;

    return 0;
}


static int
qemuDomainGetStatsBlockExportHeader(virDomainDiskDefPtr disk,
                                    virStorageSourcePtr src,
                                    size_t recordnr,
                                    virTypedParamListPtr params)
{
    if (virTypedParamListAddString(params, disk->dst, "block.%zu.name", recordnr) < 0)
        return -1;

    if (virStorageSourceIsLocalStorage(src) && src->path &&
        virTypedParamListAddString(params, src->path, "block.%zu.path", recordnr) < 0)
        return -1;

    if (src->id &&
        virTypedParamListAddUInt(params, src->id, "block.%zu.backingIndex", recordnr) < 0)
        return -1;

    return 0;
}


static int
qemuDomainGetStatsBlockExportDisk(virDomainDiskDefPtr disk,
                                  virHashTablePtr stats,
                                  virHashTablePtr nodestats,
                                  virTypedParamListPtr params,
                                  size_t *recordnr,
                                  bool visitBacking,
                                  virQEMUDriverPtr driver,
                                  virQEMUDriverConfigPtr cfg,
                                  virDomainObjPtr dom,
                                  bool blockdev)

{
    virStorageSourcePtr n;
    const char *frontendalias;
    const char *backendalias;
    const char *backendstoragealias;

    /*
     * This helps to keep logs clean from error messages on getting stats
     * for optional disk with nonexistent source file. We won't get any
     * stats for such a disk anyway in below code.
     */
    if (!virDomainObjIsActive(dom) &&
        qemuDomainDiskIsMissingLocalOptional(disk)) {
        VIR_INFO("optional disk '%s' source file is missing, "
                 "skip getting stats", disk->dst);

        return qemuDomainGetStatsBlockExportHeader(disk, disk->src, *recordnr,
                                                   params);
    }

    for (n = disk->src; virStorageSourceIsBacking(n); n = n->backingStore) {
        g_autofree char *alias = NULL;

        if (blockdev) {
            frontendalias = QEMU_DOMAIN_DISK_PRIVATE(disk)->qomName;
            backendalias = n->nodeformat;
            backendstoragealias = n->nodestorage;
        } else {
            /* alias may be NULL if the VM is not running */
            if (disk->info.alias &&
                !(alias = qemuDomainStorageAlias(disk->info.alias, n->id)))
                return -1;

            qemuDomainGetStatsOneBlockRefreshNamed(n, alias, stats, nodestats);

            frontendalias = alias;
            backendalias = alias;
            backendstoragealias = alias;
        }

        if (qemuDomainGetStatsBlockExportHeader(disk, n, *recordnr, params) < 0)
            return -1;

        /* The following stats make sense only for the frontend device */
        if (n == disk->src) {
            if (qemuDomainGetStatsBlockExportFrontend(frontendalias, stats, *recordnr,
                                                      params) < 0)
                return -1;
        }

        if (qemuDomainGetStatsOneBlock(driver, cfg, dom, params,
                                       backendalias, n, *recordnr,
                                       stats) < 0)
            return -1;

        if (qemuDomainGetStatsBlockExportBackendStorage(backendstoragealias,
                                                        stats, *recordnr,
                                                        params) < 0)
            return -1;

        (*recordnr)++;

        if (!visitBacking)
            break;
    }

    return 0;
}


static int
qemuDomainGetStatsBlock(virQEMUDriverPtr driver,
                        virDomainObjPtr dom,
                        virTypedParamListPtr params,
                        unsigned int privflags)
{
    size_t i;
    int ret = -1;
    int rc;
    virHashTablePtr stats = NULL;
    virHashTablePtr nodestats = NULL;
    virJSONValuePtr nodedata = NULL;
    qemuDomainObjPrivatePtr priv = dom->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    bool fetchnodedata = virQEMUCapsGet(priv->qemuCaps,
                                        QEMU_CAPS_QUERY_NAMED_BLOCK_NODES) && !blockdev;
    int count_index = -1;
    size_t visited = 0;
    bool visitBacking = !!(privflags & QEMU_DOMAIN_STATS_BACKING);

    if (HAVE_JOB(privflags) && virDomainObjIsActive(dom)) {
        qemuDomainObjEnterMonitor(driver, dom);

        rc = qemuMonitorGetAllBlockStatsInfo(priv->mon, &stats, visitBacking);

        if (rc >= 0) {
            if (blockdev)
                rc = qemuMonitorBlockStatsUpdateCapacityBlockdev(priv->mon, stats);
            else
                ignore_value(qemuMonitorBlockStatsUpdateCapacity(priv->mon, stats,
                                                                 visitBacking));
        }

        if (fetchnodedata)
            nodedata = qemuMonitorQueryNamedBlockNodes(priv->mon);

        if (qemuDomainObjExitMonitor(driver, dom) < 0)
            goto cleanup;

        /* failure to retrieve stats is fine at this point */
        if (rc < 0 || (fetchnodedata && !nodedata))
            virResetLastError();
    }

    if (nodedata &&
        !(nodestats = qemuBlockGetNodeData(nodedata)))
        goto cleanup;

    /* When listing backing chains, it's easier to fix up the count
     * after the iteration than it is to iterate twice; but we still
     * want count listed first.  */
    count_index = params->npar;
    if (virTypedParamListAddUInt(params, 0, "block.count") < 0)
        goto cleanup;

    for (i = 0; i < dom->def->ndisks; i++) {
        if (qemuDomainGetStatsBlockExportDisk(dom->def->disks[i], stats, nodestats,
                                              params, &visited,
                                              visitBacking, driver, cfg, dom,
                                              blockdev) < 0)
            goto cleanup;
    }

    params->par[count_index].value.ui = visited;
    ret = 0;

 cleanup:
    virHashFree(stats);
    virHashFree(nodestats);
    virJSONValueFree(nodedata);
    return ret;
}


static int
qemuDomainGetStatsIOThread(virQEMUDriverPtr driver,
                           virDomainObjPtr dom,
                           virTypedParamListPtr params,
                           unsigned int privflags)
{
    qemuDomainObjPrivatePtr priv = dom->privateData;
    size_t i;
    qemuMonitorIOThreadInfoPtr *iothreads = NULL;
    int niothreads;
    int ret = -1;

    if (!HAVE_JOB(privflags) || !virDomainObjIsActive(dom))
        return 0;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_OBJECT_IOTHREAD))
        return 0;

    if ((niothreads = qemuDomainGetIOThreadsMon(driver, dom, &iothreads)) < 0)
        return -1;

    if (niothreads == 0)
        return 0;

    if (virTypedParamListAddUInt(params, niothreads, "iothread.count") < 0)
        goto cleanup;

    for (i = 0; i < niothreads; i++) {
        if (iothreads[i]->poll_valid) {
            if (virTypedParamListAddULLong(params, iothreads[i]->poll_max_ns,
                                           "iothread.%u.poll-max-ns",
                                           iothreads[i]->iothread_id) < 0)
                goto cleanup;
            if (virTypedParamListAddUInt(params, iothreads[i]->poll_grow,
                                         "iothread.%u.poll-grow",
                                         iothreads[i]->iothread_id) < 0)
                goto cleanup;
            if (virTypedParamListAddUInt(params, iothreads[i]->poll_shrink,
                                         "iothread.%u.poll-shrink",
                                         iothreads[i]->iothread_id) < 0)
                goto cleanup;
        }
    }

    ret = 0;

 cleanup:
    for (i = 0; i < niothreads; i++)
        VIR_FREE(iothreads[i]);
    VIR_FREE(iothreads);

    return ret;
}


static int
qemuDomainGetStatsPerfOneEvent(virPerfPtr perf,
                               virPerfEventType type,
                               virTypedParamListPtr params)
{
    uint64_t value = 0;

    if (virPerfReadEvent(perf, type, &value) < 0)
        return -1;

    if (virTypedParamListAddULLong(params, value, "perf.%s",
                                   virPerfEventTypeToString(type)) < 0)
        return -1;

    return 0;
}

static int
qemuDomainGetStatsPerf(virQEMUDriverPtr driver G_GNUC_UNUSED,
                       virDomainObjPtr dom,
                       virTypedParamListPtr params,
                       unsigned int privflags G_GNUC_UNUSED)
{
    size_t i;
    qemuDomainObjPrivatePtr priv = dom->privateData;

    for (i = 0; i < VIR_PERF_EVENT_LAST; i++) {
        if (!virPerfEventIsEnabled(priv->perf, i))
             continue;

        if (qemuDomainGetStatsPerfOneEvent(priv->perf, i, params) < 0)
            return -1;
    }

    return 0;
}

typedef int
(*qemuDomainGetStatsFunc)(virQEMUDriverPtr driver,
                          virDomainObjPtr dom,
                          virTypedParamListPtr list,
                          unsigned int flags);

struct qemuDomainGetStatsWorker {
    qemuDomainGetStatsFunc func;
    unsigned int stats;
    bool monitor;
};

static struct qemuDomainGetStatsWorker qemuDomainGetStatsWorkers[] = {
    { qemuDomainGetStatsState, VIR_DOMAIN_STATS_STATE, false },
    { qemuDomainGetStatsCpu, VIR_DOMAIN_STATS_CPU_TOTAL, false },
    { qemuDomainGetStatsBalloon, VIR_DOMAIN_STATS_BALLOON, true },
    { qemuDomainGetStatsVcpu, VIR_DOMAIN_STATS_VCPU, true },
    { qemuDomainGetStatsInterface, VIR_DOMAIN_STATS_INTERFACE, false },
    { qemuDomainGetStatsBlock, VIR_DOMAIN_STATS_BLOCK, true },
    { qemuDomainGetStatsPerf, VIR_DOMAIN_STATS_PERF, false },
    { qemuDomainGetStatsIOThread, VIR_DOMAIN_STATS_IOTHREAD, true },
    { qemuDomainGetStatsMemory, VIR_DOMAIN_STATS_MEMORY, false },
    { NULL, 0, false }
};


static int
qemuDomainGetStatsCheckSupport(unsigned int *stats,
                               bool enforce)
{
    unsigned int supportedstats = 0;
    size_t i;

    for (i = 0; qemuDomainGetStatsWorkers[i].func; i++)
        supportedstats |= qemuDomainGetStatsWorkers[i].stats;

    if (*stats == 0) {
        *stats = supportedstats;
        return 0;
    }

    if (enforce &&
        *stats & ~supportedstats) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                       _("Stats types bits 0x%x are not supported by this daemon"),
                       *stats & ~supportedstats);
        return -1;
    }

    *stats &= supportedstats;
    return 0;
}


static bool
qemuDomainGetStatsNeedMonitor(unsigned int stats)
{
    size_t i;

    for (i = 0; qemuDomainGetStatsWorkers[i].func; i++)
        if (stats & qemuDomainGetStatsWorkers[i].stats &&
            qemuDomainGetStatsWorkers[i].monitor)
            return true;

    return false;
}


static int
qemuDomainGetStats(virConnectPtr conn,
                   virDomainObjPtr dom,
                   unsigned int stats,
                   virDomainStatsRecordPtr *record,
                   unsigned int flags)
{
    g_autofree virDomainStatsRecordPtr tmp = NULL;
    g_autoptr(virTypedParamList) params = NULL;
    size_t i;

    if (VIR_ALLOC(params) < 0)
        return -1;

    for (i = 0; qemuDomainGetStatsWorkers[i].func; i++) {
        if (stats & qemuDomainGetStatsWorkers[i].stats) {
            if (qemuDomainGetStatsWorkers[i].func(conn->privateData, dom, params,
                                                  flags) < 0)
                return -1;
        }
    }

    if (VIR_ALLOC(tmp) < 0)
        return -1;

    if (!(tmp->dom = virGetDomain(conn, dom->def->name,
                                  dom->def->uuid, dom->def->id)))
        return -1;

    tmp->nparams = virTypedParamListStealParams(params, &tmp->params);
    *record = g_steal_pointer(&tmp);
    return 0;
}


static int
qemuConnectGetAllDomainStats(virConnectPtr conn,
                             virDomainPtr *doms,
                             unsigned int ndoms,
                             unsigned int stats,
                             virDomainStatsRecordPtr **retStats,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    virErrorPtr orig_err = NULL;
    virDomainObjPtr *vms = NULL;
    virDomainObjPtr vm;
    size_t nvms;
    virDomainStatsRecordPtr *tmpstats = NULL;
    bool enforce = !!(flags & VIR_CONNECT_GET_ALL_DOMAINS_STATS_ENFORCE_STATS);
    int nstats = 0;
    size_t i;
    int ret = -1;
    unsigned int privflags = 0;
    unsigned int domflags = 0;
    unsigned int lflags = flags & (VIR_CONNECT_LIST_DOMAINS_FILTERS_ACTIVE |
                                   VIR_CONNECT_LIST_DOMAINS_FILTERS_PERSISTENT |
                                   VIR_CONNECT_LIST_DOMAINS_FILTERS_STATE);

    virCheckFlags(VIR_CONNECT_LIST_DOMAINS_FILTERS_ACTIVE |
                  VIR_CONNECT_LIST_DOMAINS_FILTERS_PERSISTENT |
                  VIR_CONNECT_LIST_DOMAINS_FILTERS_STATE |
                  VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT |
                  VIR_CONNECT_GET_ALL_DOMAINS_STATS_BACKING |
                  VIR_CONNECT_GET_ALL_DOMAINS_STATS_ENFORCE_STATS, -1);

    if (virConnectGetAllDomainStatsEnsureACL(conn) < 0)
        return -1;

    if (qemuDomainGetStatsCheckSupport(&stats, enforce) < 0)
        return -1;

    if (ndoms) {
        if (virDomainObjListConvert(driver->domains, conn, doms, ndoms, &vms,
                                    &nvms, virConnectGetAllDomainStatsCheckACL,
                                    lflags, true) < 0)
            return -1;
    } else {
        if (virDomainObjListCollect(driver->domains, conn, &vms, &nvms,
                                    virConnectGetAllDomainStatsCheckACL,
                                    lflags) < 0)
            return -1;
    }

    if (VIR_ALLOC_N(tmpstats, nvms + 1) < 0)
        goto cleanup;

    if (qemuDomainGetStatsNeedMonitor(stats))
        privflags |= QEMU_DOMAIN_STATS_HAVE_JOB;

    for (i = 0; i < nvms; i++) {
        virDomainStatsRecordPtr tmp = NULL;
        domflags = 0;
        vm = vms[i];

        virObjectLock(vm);

        if (HAVE_JOB(privflags)) {
            int rv;

            if (flags & VIR_CONNECT_GET_ALL_DOMAINS_STATS_NOWAIT)
                rv = qemuDomainObjBeginJobNowait(driver, vm, QEMU_JOB_QUERY);
            else
                rv = qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY);

            if (rv == 0)
                domflags |= QEMU_DOMAIN_STATS_HAVE_JOB;
        }
        /* else: without a job it's still possible to gather some data */

        if (flags & VIR_CONNECT_GET_ALL_DOMAINS_STATS_BACKING)
            domflags |= QEMU_DOMAIN_STATS_BACKING;
        if (qemuDomainGetStats(conn, vm, stats, &tmp, domflags) < 0) {
            if (HAVE_JOB(domflags) && vm)
                qemuDomainObjEndJob(driver, vm);

            virObjectUnlock(vm);
            goto cleanup;
        }

        if (tmp)
            tmpstats[nstats++] = tmp;

        if (HAVE_JOB(domflags))
            qemuDomainObjEndJob(driver, vm);

        virObjectUnlock(vm);
    }

    *retStats = tmpstats;
    tmpstats = NULL;

    ret = nstats;

 cleanup:
    virErrorPreserveLast(&orig_err);
    virDomainStatsRecordListFree(tmpstats);
    virObjectListFreeCount(vms, nvms);
    virErrorRestore(&orig_err);

    return ret;
}


static int
qemuNodeAllocPages(virConnectPtr conn,
                   unsigned int npages,
                   unsigned int *pageSizes,
                   unsigned long long *pageCounts,
                   int startCell,
                   unsigned int cellCount,
                   unsigned int flags)
{
    bool add = !(flags & VIR_NODE_ALLOC_PAGES_SET);

    virCheckFlags(VIR_NODE_ALLOC_PAGES_SET, -1);

    if (virNodeAllocPagesEnsureACL(conn) < 0)
        return -1;

    return virHostMemAllocPages(npages, pageSizes, pageCounts,
                                startCell, cellCount, add);
}


static int
qemuDomainGetFSInfo(virDomainPtr dom,
                    virDomainFSInfoPtr **info,
                    unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuAgentPtr agent;
    int ret = -1;

    virCheckFlags(0, ret);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return ret;

    if (virDomainGetFSInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJobWithAgent(driver, vm,
                                       QEMU_JOB_QUERY,
                                       QEMU_AGENT_JOB_QUERY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentGetFSInfo(agent, info, vm->def);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndJobWithAgent(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuGetDHCPInterfaces(virDomainObjPtr vm,
                      virDomainInterfacePtr **ifaces)
{
    g_autoptr(virConnect) conn = NULL;
    virDomainInterfacePtr *ifaces_ret = NULL;
    size_t ifaces_count = 0;
    size_t i;

    if (!(conn = virGetConnectNetwork()))
        return -1;

    for (i = 0; i < vm->def->nnets; i++) {
        g_autoptr(virNetwork) network = NULL;
        char macaddr[VIR_MAC_STRING_BUFLEN];
        virNetworkDHCPLeasePtr *leases = NULL;
        int n_leases = 0;
        virDomainInterfacePtr iface = NULL;
        size_t j;

        if (vm->def->nets[i]->type != VIR_DOMAIN_NET_TYPE_NETWORK)
            continue;

        virMacAddrFormat(&(vm->def->nets[i]->mac), macaddr);

        network = virNetworkLookupByName(conn,
                                         vm->def->nets[i]->data.network.name);
        if (!network)
            goto error;

        if ((n_leases = virNetworkGetDHCPLeases(network, macaddr,
                                                &leases, 0)) < 0)
            goto error;

        if (n_leases) {
            ifaces_ret = g_renew(typeof(*ifaces_ret), ifaces_ret, ifaces_count + 1);
            ifaces_ret[ifaces_count] = g_new0(typeof(**ifaces_ret), 1);
            iface = ifaces_ret[ifaces_count];
            ifaces_count++;

            /* Assuming each lease corresponds to a separate IP */
            iface->naddrs = n_leases;
            iface->addrs = g_new0(typeof(*iface->addrs), iface->naddrs);
            iface->name = g_strdup(vm->def->nets[i]->ifname);
            iface->hwaddr = g_strdup(macaddr);
        }

        for (j = 0; j < n_leases; j++) {
            virNetworkDHCPLeasePtr lease = leases[j];
            virDomainIPAddressPtr ip_addr = &iface->addrs[j];

            ip_addr->addr = g_strdup(lease->ipaddr);
            ip_addr->type = lease->type;
            ip_addr->prefix = lease->prefix;

            virNetworkDHCPLeaseFree(lease);
        }

        VIR_FREE(leases);
    }

    *ifaces = g_steal_pointer(&ifaces_ret);
    return ifaces_count;

 error:
    if (ifaces_ret) {
        for (i = 0; i < ifaces_count; i++)
            virDomainInterfaceFree(ifaces_ret[i]);
    }
    VIR_FREE(ifaces_ret);

    return -1;
}


static int
qemuARPGetInterfaces(virDomainObjPtr vm,
                     virDomainInterfacePtr **ifaces)
{
    size_t i, j;
    size_t ifaces_count = 0;
    int ret = -1;
    char macaddr[VIR_MAC_STRING_BUFLEN];
    virDomainInterfacePtr *ifaces_ret = NULL;
    virDomainInterfacePtr iface = NULL;
    virArpTablePtr table;

    table = virArpTableGet();
    if (!table)
        goto cleanup;

    for (i = 0; i < vm->def->nnets; i++) {
        virMacAddrFormat(&(vm->def->nets[i]->mac), macaddr);
        for (j = 0; j < table->n; j++) {
            virArpTableEntry entry = table->t[j];

            if (STREQ(entry.mac, macaddr)) {
                if (VIR_ALLOC(iface) < 0)
                    goto cleanup;

                iface->name = g_strdup(vm->def->nets[i]->ifname);

                iface->hwaddr = g_strdup(macaddr);

                if (VIR_ALLOC(iface->addrs) < 0)
                    goto cleanup;
                iface->naddrs = 1;

                iface->addrs->addr = g_strdup(entry.ipaddr);

                if (VIR_APPEND_ELEMENT(ifaces_ret, ifaces_count, iface) < 0)
                    goto cleanup;
            }
        }
    }

    *ifaces = g_steal_pointer(&ifaces_ret);
    ret = ifaces_count;

 cleanup:
    virArpTableFree(table);
    virDomainInterfaceFree(iface);

    if (ifaces_ret) {
        for (i = 0; i < ifaces_count; i++)
            virDomainInterfaceFree(ifaces_ret[i]);
    }
    VIR_FREE(ifaces_ret);

    return ret;
}


static int
qemuDomainInterfaceAddresses(virDomainPtr dom,
                             virDomainInterfacePtr **ifaces,
                             unsigned int source,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuAgentPtr agent;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainInterfaceAddressesEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto cleanup;

    switch (source) {
    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE:
        ret = qemuGetDHCPInterfaces(vm, ifaces);
        break;

    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT:
        if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_QUERY) < 0)
            goto cleanup;

        if (!qemuDomainAgentAvailable(vm, true))
            goto endjob;

        agent = qemuDomainObjEnterAgent(vm);
        ret = qemuAgentGetInterfaces(agent, ifaces);
        qemuDomainObjExitAgent(vm, agent);

    endjob:
        qemuDomainObjEndAgentJob(vm);

        break;

    case VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_ARP:
        ret = qemuARPGetInterfaces(vm, ifaces);
        break;

    default:
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                       _("Unknown IP address data source %d"),
                       source);
        break;
    }

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSetUserPassword(virDomainPtr dom,
                          const char *user,
                          const char *password,
                          unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm;
    qemuAgentPtr agent;
    int ret = -1;
    int rv;

    virCheckFlags(VIR_DOMAIN_PASSWORD_ENCRYPTED, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return ret;

    if (virDomainSetUserPasswordEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    rv = qemuAgentSetUserPassword(agent, user, password,
                                  flags & VIR_DOMAIN_PASSWORD_ENCRYPTED);
    qemuDomainObjExitAgent(vm, agent);

    if (rv < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainRenameCallback(virDomainObjPtr vm,
                         const char *new_name,
                         unsigned int flags,
                         void *opaque)
{
    virQEMUDriverPtr driver = opaque;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virObjectEventPtr event_new = NULL;
    virObjectEventPtr event_old = NULL;
    int ret = -1;
    g_autofree char *new_dom_name = NULL;
    g_autofree char *old_dom_name = NULL;
    g_autofree char *new_dom_cfg_file = NULL;
    g_autofree char *old_dom_cfg_file = NULL;
    g_autofree char *new_dom_autostart_link = NULL;
    g_autofree char *old_dom_autostart_link = NULL;

    virCheckFlags(0, ret);

    if (strchr(new_name, '/')) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("name %s cannot contain '/'"), new_name);
        return -1;
    }

    cfg = virQEMUDriverGetConfig(driver);

    new_dom_name = g_strdup(new_name);

    if (!(new_dom_cfg_file = virDomainConfigFile(cfg->configDir,
                                                 new_dom_name)) ||
        !(old_dom_cfg_file = virDomainConfigFile(cfg->configDir,
                                                 vm->def->name)))
        goto cleanup;

    if (vm->autostart) {
        if (!(new_dom_autostart_link = virDomainConfigFile(cfg->autostartDir,
                                                          new_dom_name)) ||
            !(old_dom_autostart_link = virDomainConfigFile(cfg->autostartDir,
                                                          vm->def->name)))
            goto cleanup;

        if (symlink(new_dom_cfg_file, new_dom_autostart_link) < 0) {
            virReportSystemError(errno,
                                 _("Failed to create symlink '%s to '%s'"),
                                 new_dom_autostart_link, new_dom_cfg_file);
            goto cleanup;
        }
    }

    event_old = virDomainEventLifecycleNewFromObj(vm,
                                            VIR_DOMAIN_EVENT_UNDEFINED,
                                            VIR_DOMAIN_EVENT_UNDEFINED_RENAMED);

    /* Switch name in domain definition. */
    old_dom_name = vm->def->name;
    vm->def->name = new_dom_name;
    new_dom_name = NULL;

    if (virDomainDefSave(vm->def, driver->xmlopt, cfg->configDir) < 0)
        goto rollback;

    if (virFileExists(old_dom_cfg_file) &&
        unlink(old_dom_cfg_file) < 0) {
        virReportSystemError(errno,
                             _("cannot remove old domain config file %s"),
                             old_dom_cfg_file);
        goto rollback;
    }

    if (vm->autostart) {
        if (virFileIsLink(old_dom_autostart_link) &&
            unlink(old_dom_autostart_link) < 0) {
            virReportSystemError(errno,
                                 _("Failed to delete symlink '%s'"),
                                 old_dom_autostart_link);
            goto rollback;
        }
    }

    event_new = virDomainEventLifecycleNewFromObj(vm,
                                              VIR_DOMAIN_EVENT_DEFINED,
                                              VIR_DOMAIN_EVENT_DEFINED_RENAMED);
    ret = 0;

 cleanup:
    virObjectEventStateQueue(driver->domainEventState, event_old);
    virObjectEventStateQueue(driver->domainEventState, event_new);
    return ret;

 rollback:
    if (old_dom_name) {
        new_dom_name = vm->def->name;
        vm->def->name = old_dom_name;
        old_dom_name = NULL;
    }

    if (virFileExists(new_dom_cfg_file))
        unlink(new_dom_cfg_file);

    if (vm->autostart &&
        virFileExists(new_dom_autostart_link))
        unlink(new_dom_autostart_link);

    goto cleanup;
}

static int qemuDomainRename(virDomainPtr dom,
                            const char *new_name,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    virCheckFlags(0, ret);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainRenameEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot rename active domain"));
        goto endjob;
    }

    if (!vm->persistent) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot rename a transient domain"));
        goto endjob;
    }

    if (vm->hasManagedSave) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("domain with a managed saved state can't be renamed"));
        goto endjob;
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_SHUTOFF) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("domain has to be shutoff before renaming"));
        goto endjob;
    }

    if (virDomainSnapshotObjListNum(vm->snapshots, NULL, 0) > 0) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot rename domain with snapshots"));
        goto endjob;
    }

    if (virDomainListCheckpoints(vm->checkpoints, NULL, dom, NULL, flags) > 0) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot rename domain with checkpoints"));
        goto endjob;
    }

    if (virDomainObjListRename(driver->domains, vm, new_name, flags,
                               qemuDomainRenameCallback, driver) < 0)
        goto endjob;

    /* Success, domain has been renamed. */
    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainGetGuestVcpusParams(virTypedParameterPtr *params,
                              unsigned int *nparams,
                              qemuAgentCPUInfoPtr info,
                              int ninfo)
{
    virTypedParameterPtr par = NULL;
    int npar = 0;
    int maxpar = 0;
    virBitmapPtr vcpus = NULL;
    virBitmapPtr online = NULL;
    virBitmapPtr offlinable = NULL;
    g_autofree char *tmp = NULL;
    size_t i;
    int ret = -1;

    if (!(vcpus = virBitmapNew(QEMU_GUEST_VCPU_MAX_ID)) ||
        !(online = virBitmapNew(QEMU_GUEST_VCPU_MAX_ID)) ||
        !(offlinable = virBitmapNew(QEMU_GUEST_VCPU_MAX_ID)))
        goto cleanup;

    for (i = 0; i < ninfo; i++) {
        if (virBitmapSetBit(vcpus, info[i].id) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("vcpu id '%u' reported by guest agent is out of "
                             "range"), info[i].id);
            goto cleanup;
        }

        if (info[i].online)
            ignore_value(virBitmapSetBit(online, info[i].id));

        if (info[i].offlinable)
            ignore_value(virBitmapSetBit(offlinable, info[i].id));
    }

#define ADD_BITMAP(name) \
    if (!(tmp = virBitmapFormat(name))) \
        goto cleanup; \
    if (virTypedParamsAddString(&par, &npar, &maxpar, #name, tmp) < 0) \
        goto cleanup; \

    ADD_BITMAP(vcpus);
    ADD_BITMAP(online);
    ADD_BITMAP(offlinable);

#undef ADD_BITMAP

    *params = par;
    *nparams = npar;
    par = NULL;
    ret = 0;

 cleanup:
    virBitmapFree(vcpus);
    virBitmapFree(online);
    virBitmapFree(offlinable);
    virTypedParamsFree(par, npar);
    return ret;
}


static int
qemuDomainGetGuestVcpus(virDomainPtr dom,
                        virTypedParameterPtr *params,
                        unsigned int *nparams,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuAgentPtr agent;
    qemuAgentCPUInfoPtr info = NULL;
    int ninfo = 0;
    int ret = -1;

    virCheckFlags(0, ret);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetGuestVcpusEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_QUERY) < 0)
        goto cleanup;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ninfo = qemuAgentGetVCPUs(agent, &info);
    qemuDomainObjExitAgent(vm, agent);

    if (ninfo < 0)
        goto endjob;

    if (qemuDomainGetGuestVcpusParams(params, nparams, info, ninfo) < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    VIR_FREE(info);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSetGuestVcpus(virDomainPtr dom,
                        const char *cpumap,
                        int state,
                        unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    virBitmapPtr map = NULL;
    qemuAgentCPUInfoPtr info = NULL;
    qemuAgentPtr agent;
    int ninfo = 0;
    size_t i;
    int ret = -1;

    virCheckFlags(0, -1);

    if (state != 0 && state != 1) {
        virReportInvalidArg(state, "%s", _("unsupported state value"));
        return -1;
    }

    if (virBitmapParse(cpumap, &map, QEMU_GUEST_VCPU_MAX_ID) < 0)
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSetGuestVcpusEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginAgentJob(driver, vm, QEMU_AGENT_JOB_MODIFY) < 0)
        goto cleanup;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ninfo = qemuAgentGetVCPUs(agent, &info);
    qemuDomainObjExitAgent(vm, agent);
    agent = NULL;

    if (ninfo < 0)
        goto endjob;

    for (i = 0; i < ninfo; i++) {
        if (!virBitmapIsBitSet(map, info[i].id))
            continue;

        if (!state && !info[i].offlinable) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                           _("vCPU '%u' is not offlinable"), info[i].id);
            goto endjob;
        }

        info[i].online = !!state;
        info[i].modified = true;

        ignore_value(virBitmapClearBit(map, info[i].id));
    }

    if (!virBitmapIsAllClear(map)) {
        char *tmp = virBitmapFormat(map);
        virReportError(VIR_ERR_INVALID_ARG,
                       _("guest is missing vCPUs '%s'"), NULLSTR(tmp));
        VIR_FREE(tmp);
        goto endjob;
    }

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);
    ret = qemuAgentSetVCPUs(agent, info, ninfo);
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndAgentJob(vm);

 cleanup:
    VIR_FREE(info);
    virBitmapFree(map);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSetVcpu(virDomainPtr dom,
                  const char *cpumap,
                  int state,
                  unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    virBitmapPtr map = NULL;
    ssize_t lastvcpu;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (state != 0 && state != 1) {
        virReportInvalidArg(state, "%s", _("unsupported state value"));
        return -1;
    }

    if (virBitmapParse(cpumap, &map, QEMU_GUEST_VCPU_MAX_ID) < 0)
        goto cleanup;

    if ((lastvcpu = virBitmapLastSetBit(map)) < 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("no vcpus selected for modification"));
        goto cleanup;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainSetVcpuEnsureACL(dom->conn, vm->def, flags) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (persistentDef) {
        if (lastvcpu >= virDomainDefGetVcpusMax(persistentDef)) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("vcpu %zd is not present in persistent config"),
                           lastvcpu);
            goto endjob;
        }
    }

    if (def) {
        if (lastvcpu >= virDomainDefGetVcpusMax(def)) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("vcpu %zd is not present in live config"),
                           lastvcpu);
            goto endjob;
        }
    }

    ret = qemuDomainSetVcpuInternal(driver, vm, def, persistentDef, map, !!state);

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virBitmapFree(map);
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainSetBlockThreshold(virDomainPtr dom,
                            const char *dev,
                            unsigned long long threshold,
                            unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    qemuDomainObjPrivatePtr priv;
    virDomainObjPtr vm = NULL;
    virStorageSourcePtr src;
    g_autofree char *nodename = NULL;
    int rc;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainSetBlockThresholdEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjCheckActive(vm) < 0)
        goto endjob;

    if (!virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCK_WRITE_THRESHOLD)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("this qemu does not support setting device threshold"));
        goto endjob;
    }

    if (!(src = qemuDomainGetStorageSourceByDevstr(dev, vm->def)))
        goto endjob;

    if (!src->nodestorage &&
        qemuBlockNodeNamesDetect(driver, vm, QEMU_ASYNC_JOB_NONE) < 0)
        goto endjob;

    if (!src->nodestorage) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("threshold currently can't be set for block device '%s'"),
                       dev);
        goto endjob;
    }

    nodename = g_strdup(src->nodestorage);

    qemuDomainObjEnterMonitor(driver, vm);
    rc = qemuMonitorSetBlockThreshold(priv->mon, nodename, threshold);
    if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static void
qemuDomainModifyLifecycleAction(virDomainDefPtr def,
                                virDomainLifecycle type,
                                virDomainLifecycleAction action)
{
    switch (type) {
    case VIR_DOMAIN_LIFECYCLE_POWEROFF:
        def->onPoweroff = action;
        break;
    case VIR_DOMAIN_LIFECYCLE_REBOOT:
        def->onReboot = action;
        break;
    case VIR_DOMAIN_LIFECYCLE_CRASH:
        def->onCrash = action;
        break;
    case VIR_DOMAIN_LIFECYCLE_LAST:
        break;
    }
}



static int
qemuDomainSetLifecycleAction(virDomainPtr dom,
                             unsigned int type,
                             unsigned int action,
                             unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    qemuDomainObjPrivatePtr priv;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def = NULL;
    virDomainDefPtr persistentDef = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE |
                  VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (!virDomainDefLifecycleActionAllowed(type, action))
        goto cleanup;

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    priv = vm->privateData;

    if (virDomainSetLifecycleActionEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_MODIFY) < 0)
        goto cleanup;

    if (virDomainObjGetDefs(vm, flags, &def, &persistentDef) < 0)
        goto endjob;

    if (def) {
        if (priv->allowReboot == VIR_TRISTATE_BOOL_NO) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("cannot update lifecycle action because QEMU "
                             "was started with -no-reboot option"));
            goto endjob;
        }

        qemuDomainModifyLifecycleAction(def, type, action);

        if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
            goto endjob;
    }

    if (persistentDef) {
        qemuDomainModifyLifecycleAction(persistentDef, type, action);

        if (virDomainDefSave(persistentDef, driver->xmlopt,
                             cfg->configDir) < 0)
            goto endjob;
    }

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuGetSEVInfoToParams(virQEMUCapsPtr qemuCaps,
                       virTypedParameterPtr *params,
                       int *nparams,
                       unsigned int flags)
{
    int maxpar = 0;
    int n = 0;
    virSEVCapabilityPtr sev = virQEMUCapsGetSEVCapabilities(qemuCaps);
    virTypedParameterPtr sevParams = NULL;

    virCheckFlags(VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (virTypedParamsAddString(&sevParams, &n, &maxpar,
                    VIR_NODE_SEV_PDH, sev->pdh) < 0)
        return -1;

    if (virTypedParamsAddString(&sevParams, &n, &maxpar,
                    VIR_NODE_SEV_CERT_CHAIN, sev->cert_chain) < 0)
        goto cleanup;

    if (virTypedParamsAddUInt(&sevParams, &n, &maxpar,
                    VIR_NODE_SEV_CBITPOS, sev->cbitpos) < 0)
        goto cleanup;

    if (virTypedParamsAddUInt(&sevParams, &n, &maxpar,
                    VIR_NODE_SEV_REDUCED_PHYS_BITS,
                    sev->reduced_phys_bits) < 0)
        goto cleanup;

    *params = g_steal_pointer(&sevParams);
    *nparams = n;
    return 0;

 cleanup:
    virTypedParamsFree(sevParams, n);
    return -1;
}


static int
qemuNodeGetSEVInfo(virConnectPtr conn,
                   virTypedParameterPtr *params,
                   int *nparams,
                   unsigned int flags)
{
    virQEMUDriverPtr driver = conn->privateData;
    g_autoptr(virQEMUCaps) qemucaps = NULL;

    if (virNodeGetSevInfoEnsureACL(conn) < 0)
        return -1;

    qemucaps = virQEMUCapsCacheLookupByArch(driver->qemuCapsCache,
                                            virArchFromHost());
    if (!qemucaps)
        return -1;

    if (!virQEMUCapsGet(qemucaps, QEMU_CAPS_SEV_GUEST)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                       _("QEMU does not support SEV guest"));
        return -1;
    }

    if (qemuGetSEVInfoToParams(qemucaps, params, nparams, flags) < 0)
        return -1;

    return 0;
}


static int
qemuDomainGetSEVMeasurement(virQEMUDriverPtr driver,
                            virDomainObjPtr vm,
                            virTypedParameterPtr *params,
                            int *nparams,
                            unsigned int flags)
{
    int ret = -1;
    g_autofree char *tmp = NULL;
    int maxpar = 0;

    virCheckFlags(VIR_TYPED_PARAM_STRING_OKAY, -1);

    if (qemuDomainObjBeginJob(driver, vm, QEMU_JOB_QUERY) < 0)
        return -1;

    qemuDomainObjEnterMonitor(driver, vm);
    tmp = qemuMonitorGetSEVMeasurement(QEMU_DOMAIN_PRIVATE(vm)->mon);

    if (qemuDomainObjExitMonitor(driver, vm) < 0)
        goto endjob;

    if (!tmp)
        goto endjob;

    if (virTypedParamsAddString(params, nparams, &maxpar,
                                VIR_DOMAIN_LAUNCH_SECURITY_SEV_MEASUREMENT,
                                tmp) < 0)
        goto endjob;

    ret = 0;

 endjob:
    qemuDomainObjEndJob(driver, vm);
    return ret;
}


static int
qemuDomainGetLaunchSecurityInfo(virDomainPtr domain,
                                virTypedParameterPtr *params,
                                int *nparams,
                                unsigned int flags)
{
    virQEMUDriverPtr driver = domain->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    if (!(vm = qemuDomainObjFromDomain(domain)))
        goto cleanup;

    if (virDomainGetLaunchSecurityInfoEnsureACL(domain->conn, vm->def) < 0)
        goto cleanup;

    if (vm->def->sev) {
        if (qemuDomainGetSEVMeasurement(driver, vm, params, nparams, flags) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}

static const unsigned int supportedGuestInfoTypes =
    VIR_DOMAIN_GUEST_INFO_USERS |
    VIR_DOMAIN_GUEST_INFO_OS |
    VIR_DOMAIN_GUEST_INFO_TIMEZONE |
    VIR_DOMAIN_GUEST_INFO_HOSTNAME |
    VIR_DOMAIN_GUEST_INFO_FILESYSTEM;

static void
qemuDomainGetGuestInfoCheckSupport(unsigned int *types)
{
    if (*types == 0)
        *types = supportedGuestInfoTypes;

    *types = *types & supportedGuestInfoTypes;
}

static int
qemuDomainGetGuestInfo(virDomainPtr dom,
                       unsigned int types,
                       virTypedParameterPtr *params,
                       int *nparams,
                       unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    qemuAgentPtr agent;
    int ret = -1;
    int maxparams = 0;
    g_autofree char *hostname = NULL;
    unsigned int supportedTypes = types;
    int rc;

    virCheckFlags(0, -1);
    qemuDomainGetGuestInfoCheckSupport(&supportedTypes);

    if (!(vm = qemuDomainObjFromDomain(dom)))
        goto cleanup;

    if (virDomainGetGuestInfoEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    if (qemuDomainObjBeginJobWithAgent(driver, vm,
                                       QEMU_JOB_QUERY,
                                       QEMU_AGENT_JOB_QUERY) < 0)
        goto cleanup;

    if (!qemuDomainAgentAvailable(vm, true))
        goto endjob;

    agent = qemuDomainObjEnterAgent(vm);

    /* The agent info commands will return -2 for any commands that are not
     * supported by the agent, or -1 for all other errors. In the case where no
     * categories were explicitly requested (i.e. 'types' is 0), ignore
     * 'unsupported' errors and gather as much information as we can. In all
     * other cases, abort on error. */
    if (supportedTypes & VIR_DOMAIN_GUEST_INFO_USERS) {
        rc = qemuAgentGetUsers(agent, params, nparams, &maxparams);
        if (rc < 0 && !(rc == -2 && types == 0))
            goto exitagent;
    }
    if (supportedTypes & VIR_DOMAIN_GUEST_INFO_OS) {
        rc = qemuAgentGetOSInfo(agent, params, nparams, &maxparams);
        if (rc < 0 && !(rc == -2 && types == 0))
            goto exitagent;
    }
    if (supportedTypes & VIR_DOMAIN_GUEST_INFO_TIMEZONE) {
        rc = qemuAgentGetTimezone(agent, params, nparams, &maxparams);
        if (rc < 0 && !(rc == -2 && types == 0))
            goto exitagent;
    }
    if (supportedTypes & VIR_DOMAIN_GUEST_INFO_HOSTNAME) {
        rc = qemuAgentGetHostname(agent, &hostname);
        if (rc < 0 && !(rc == -2 && types == 0)) {
            goto exitagent;
        } else {
            if (virTypedParamsAddString(params, nparams, &maxparams, "hostname",
                                        hostname) < 0)
                goto exitagent;
        }
    }
    if (supportedTypes & VIR_DOMAIN_GUEST_INFO_FILESYSTEM) {
        rc = qemuAgentGetFSInfoParams(agent, params, nparams, &maxparams, vm->def);
        if (rc < 0 && !(rc == -2 && types == 0))
            goto exitagent;
    }

    ret = 0;

 exitagent:
    qemuDomainObjExitAgent(vm, agent);

 endjob:
    qemuDomainObjEndJobWithAgent(driver, vm);

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static int
qemuDomainAgentSetResponseTimeout(virDomainPtr dom,
                                  int timeout,
                                  unsigned int flags)
{
    virQEMUDriverPtr driver = dom->conn->privateData;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (timeout < VIR_DOMAIN_QEMU_AGENT_COMMAND_MIN) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("guest agent timeout '%d' is "
                         "less than the minimum '%d'"),
                       timeout, VIR_DOMAIN_QEMU_AGENT_COMMAND_MIN);
        return -1;
    }

    if (!(vm = qemuDomainObjFromDomain(dom)))
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    if (virDomainAgentSetResponseTimeoutEnsureACL(dom->conn, vm->def) < 0)
        goto cleanup;

    /* If domain has an agent, change its timeout. Otherwise just save the
     * request so that we can set the timeout when the agent appears */
    if (qemuDomainAgentAvailable(vm, false)) {
        /* We don't need to acquire a job since we're not interacting with the
         * agent or the qemu monitor. We're only setting a struct member, so
         * just acquire the mutex lock. Worst case, any in-process agent
         * commands will use the newly-set agent timeout. */
        virObjectLock(QEMU_DOMAIN_PRIVATE(vm)->agent);
        qemuAgentSetResponseTimeout(QEMU_DOMAIN_PRIVATE(vm)->agent, timeout);
        virObjectUnlock(QEMU_DOMAIN_PRIVATE(vm)->agent);
    }

    QEMU_DOMAIN_PRIVATE(vm)->agentTimeout = timeout;

    if (virDomainObjIsActive(vm) &&
        virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virDomainObjEndAPI(&vm);
    return ret;
}


static virHypervisorDriver qemuHypervisorDriver = {
    .name = QEMU_DRIVER_NAME,
    .connectURIProbe = qemuConnectURIProbe,
    .connectOpen = qemuConnectOpen, /* 0.2.0 */
    .connectClose = qemuConnectClose, /* 0.2.0 */
    .connectSupportsFeature = qemuConnectSupportsFeature, /* 0.5.0 */
    .connectGetType = qemuConnectGetType, /* 0.2.0 */
    .connectGetVersion = qemuConnectGetVersion, /* 0.2.0 */
    .connectGetHostname = qemuConnectGetHostname, /* 0.3.3 */
    .connectGetSysinfo = qemuConnectGetSysinfo, /* 0.8.8 */
    .connectGetMaxVcpus = qemuConnectGetMaxVcpus, /* 0.2.1 */
    .nodeGetInfo = qemuNodeGetInfo, /* 0.2.0 */
    .connectGetCapabilities = qemuConnectGetCapabilities, /* 0.2.1 */
    .connectListDomains = qemuConnectListDomains, /* 0.2.0 */
    .connectNumOfDomains = qemuConnectNumOfDomains, /* 0.2.0 */
    .connectListAllDomains = qemuConnectListAllDomains, /* 0.9.13 */
    .domainCreateXML = qemuDomainCreateXML, /* 0.2.0 */
    .domainLookupByID = qemuDomainLookupByID, /* 0.2.0 */
    .domainLookupByUUID = qemuDomainLookupByUUID, /* 0.2.0 */
    .domainLookupByName = qemuDomainLookupByName, /* 0.2.0 */
    .domainSuspend = qemuDomainSuspend, /* 0.2.0 */
    .domainResume = qemuDomainResume, /* 0.2.0 */
    .domainShutdown = qemuDomainShutdown, /* 0.2.0 */
    .domainShutdownFlags = qemuDomainShutdownFlags, /* 0.9.10 */
    .domainReboot = qemuDomainReboot, /* 0.9.3 */
    .domainReset = qemuDomainReset, /* 0.9.7 */
    .domainDestroy = qemuDomainDestroy, /* 0.2.0 */
    .domainDestroyFlags = qemuDomainDestroyFlags, /* 0.9.4 */
    .domainGetOSType = qemuDomainGetOSType, /* 0.2.2 */
    .domainGetMaxMemory = qemuDomainGetMaxMemory, /* 0.4.2 */
    .domainSetMaxMemory = qemuDomainSetMaxMemory, /* 0.4.2 */
    .domainSetMemory = qemuDomainSetMemory, /* 0.4.2 */
    .domainSetMemoryFlags = qemuDomainSetMemoryFlags, /* 0.9.0 */
    .domainSetMemoryParameters = qemuDomainSetMemoryParameters, /* 0.8.5 */
    .domainGetMemoryParameters = qemuDomainGetMemoryParameters, /* 0.8.5 */
    .domainSetMemoryStatsPeriod = qemuDomainSetMemoryStatsPeriod, /* 1.1.1 */
    .domainSetBlkioParameters = qemuDomainSetBlkioParameters, /* 0.9.0 */
    .domainGetBlkioParameters = qemuDomainGetBlkioParameters, /* 0.9.0 */
    .domainGetInfo = qemuDomainGetInfo, /* 0.2.0 */
    .domainGetState = qemuDomainGetState, /* 0.9.2 */
    .domainGetControlInfo = qemuDomainGetControlInfo, /* 0.9.3 */
    .domainSave = qemuDomainSave, /* 0.2.0 */
    .domainSaveFlags = qemuDomainSaveFlags, /* 0.9.4 */
    .domainRestore = qemuDomainRestore, /* 0.2.0 */
    .domainRestoreFlags = qemuDomainRestoreFlags, /* 0.9.4 */
    .domainSaveImageGetXMLDesc = qemuDomainSaveImageGetXMLDesc, /* 0.9.4 */
    .domainSaveImageDefineXML = qemuDomainSaveImageDefineXML, /* 0.9.4 */
    .domainCoreDump = qemuDomainCoreDump, /* 0.7.0 */
    .domainCoreDumpWithFormat = qemuDomainCoreDumpWithFormat, /* 1.2.3 */
    .domainScreenshot = qemuDomainScreenshot, /* 0.9.2 */
    .domainSetVcpus = qemuDomainSetVcpus, /* 0.4.4 */
    .domainSetVcpusFlags = qemuDomainSetVcpusFlags, /* 0.8.5 */
    .domainGetVcpusFlags = qemuDomainGetVcpusFlags, /* 0.8.5 */
    .domainPinVcpu = qemuDomainPinVcpu, /* 0.4.4 */
    .domainPinVcpuFlags = qemuDomainPinVcpuFlags, /* 0.9.3 */
    .domainGetVcpuPinInfo = qemuDomainGetVcpuPinInfo, /* 0.9.3 */
    .domainPinEmulator = qemuDomainPinEmulator, /* 0.10.0 */
    .domainGetEmulatorPinInfo = qemuDomainGetEmulatorPinInfo, /* 0.10.0 */
    .domainGetVcpus = qemuDomainGetVcpus, /* 0.4.4 */
    .domainGetMaxVcpus = qemuDomainGetMaxVcpus, /* 0.4.4 */
    .domainGetIOThreadInfo = qemuDomainGetIOThreadInfo, /* 1.2.14 */
    .domainPinIOThread = qemuDomainPinIOThread, /* 1.2.14 */
    .domainAddIOThread = qemuDomainAddIOThread, /* 1.2.15 */
    .domainDelIOThread = qemuDomainDelIOThread, /* 1.2.15 */
    .domainSetIOThreadParams = qemuDomainSetIOThreadParams, /* 4.10.0 */
    .domainGetSecurityLabel = qemuDomainGetSecurityLabel, /* 0.6.1 */
    .domainGetSecurityLabelList = qemuDomainGetSecurityLabelList, /* 0.10.0 */
    .nodeGetSecurityModel = qemuNodeGetSecurityModel, /* 0.6.1 */
    .domainGetXMLDesc = qemuDomainGetXMLDesc, /* 0.2.0 */
    .connectDomainXMLFromNative = NULL, /* 0.6.4 - 5.5.0 */
    .connectDomainXMLToNative = qemuConnectDomainXMLToNative, /* 0.6.4 */
    .connectListDefinedDomains = qemuConnectListDefinedDomains, /* 0.2.0 */
    .connectNumOfDefinedDomains = qemuConnectNumOfDefinedDomains, /* 0.2.0 */
    .domainCreate = qemuDomainCreate, /* 0.2.0 */
    .domainCreateWithFlags = qemuDomainCreateWithFlags, /* 0.8.2 */
    .domainDefineXML = qemuDomainDefineXML, /* 0.2.0 */
    .domainDefineXMLFlags = qemuDomainDefineXMLFlags, /* 1.2.12 */
    .domainUndefine = qemuDomainUndefine, /* 0.2.0 */
    .domainUndefineFlags = qemuDomainUndefineFlags, /* 0.9.4 */
    .domainAttachDevice = qemuDomainAttachDevice, /* 0.4.1 */
    .domainAttachDeviceFlags = qemuDomainAttachDeviceFlags, /* 0.7.7 */
    .domainDetachDevice = qemuDomainDetachDevice, /* 0.5.0 */
    .domainDetachDeviceFlags = qemuDomainDetachDeviceFlags, /* 0.7.7 */
    .domainUpdateDeviceFlags = qemuDomainUpdateDeviceFlags, /* 0.8.0 */
    .domainDetachDeviceAlias = qemuDomainDetachDeviceAlias, /* 4.4.0 */
    .domainGetAutostart = qemuDomainGetAutostart, /* 0.2.1 */
    .domainSetAutostart = qemuDomainSetAutostart, /* 0.2.1 */
    .domainGetSchedulerType = qemuDomainGetSchedulerType, /* 0.7.0 */
    .domainGetSchedulerParameters = qemuDomainGetSchedulerParameters, /* 0.7.0 */
    .domainGetSchedulerParametersFlags = qemuDomainGetSchedulerParametersFlags, /* 0.9.2 */
    .domainSetSchedulerParameters = qemuDomainSetSchedulerParameters, /* 0.7.0 */
    .domainSetSchedulerParametersFlags = qemuDomainSetSchedulerParametersFlags, /* 0.9.2 */
    .domainMigratePerform = qemuDomainMigratePerform, /* 0.5.0 */
    .domainBlockResize = qemuDomainBlockResize, /* 0.9.8 */
    .domainBlockStats = qemuDomainBlockStats, /* 0.4.1 */
    .domainBlockStatsFlags = qemuDomainBlockStatsFlags, /* 0.9.5 */
    .domainInterfaceStats = qemuDomainInterfaceStats, /* 0.4.1 */
    .domainMemoryStats = qemuDomainMemoryStats, /* 0.7.5 */
    .domainBlockPeek = qemuDomainBlockPeek, /* 0.4.4 */
    .domainMemoryPeek = qemuDomainMemoryPeek, /* 0.4.4 */
    .domainGetBlockInfo = qemuDomainGetBlockInfo, /* 0.8.1 */
    .nodeGetCPUStats = qemuNodeGetCPUStats, /* 0.9.3 */
    .nodeGetMemoryStats = qemuNodeGetMemoryStats, /* 0.9.3 */
    .nodeGetCellsFreeMemory = qemuNodeGetCellsFreeMemory, /* 0.4.4 */
    .nodeGetFreeMemory = qemuNodeGetFreeMemory, /* 0.4.4 */
    .connectDomainEventRegister = qemuConnectDomainEventRegister, /* 0.5.0 */
    .connectDomainEventDeregister = qemuConnectDomainEventDeregister, /* 0.5.0 */
    .domainMigratePrepare2 = qemuDomainMigratePrepare2, /* 0.5.0 */
    .domainMigrateFinish2 = qemuDomainMigrateFinish2, /* 0.5.0 */
    .nodeDeviceDettach = qemuNodeDeviceDettach, /* 0.6.1 */
    .nodeDeviceDetachFlags = qemuNodeDeviceDetachFlags, /* 1.0.5 */
    .nodeDeviceReAttach = qemuNodeDeviceReAttach, /* 0.6.1 */
    .nodeDeviceReset = qemuNodeDeviceReset, /* 0.6.1 */
    .domainMigratePrepareTunnel = qemuDomainMigratePrepareTunnel, /* 0.7.2 */
    .connectIsEncrypted = qemuConnectIsEncrypted, /* 0.7.3 */
    .connectIsSecure = qemuConnectIsSecure, /* 0.7.3 */
    .domainIsActive = qemuDomainIsActive, /* 0.7.3 */
    .domainIsPersistent = qemuDomainIsPersistent, /* 0.7.3 */
    .domainIsUpdated = qemuDomainIsUpdated, /* 0.8.6 */
    .connectCompareCPU = qemuConnectCompareCPU, /* 0.7.5 */
    .connectBaselineCPU = qemuConnectBaselineCPU, /* 0.7.7 */
    .domainGetJobInfo = qemuDomainGetJobInfo, /* 0.7.7 */
    .domainGetJobStats = qemuDomainGetJobStats, /* 1.0.3 */
    .domainAbortJob = qemuDomainAbortJob, /* 0.7.7 */
    .domainMigrateGetMaxDowntime = qemuDomainMigrateGetMaxDowntime, /* 3.7.0 */
    .domainMigrateSetMaxDowntime = qemuDomainMigrateSetMaxDowntime, /* 0.8.0 */
    .domainMigrateGetCompressionCache = qemuDomainMigrateGetCompressionCache, /* 1.0.3 */
    .domainMigrateSetCompressionCache = qemuDomainMigrateSetCompressionCache, /* 1.0.3 */
    .domainMigrateSetMaxSpeed = qemuDomainMigrateSetMaxSpeed, /* 0.9.0 */
    .domainMigrateGetMaxSpeed = qemuDomainMigrateGetMaxSpeed, /* 0.9.5 */
    .connectDomainEventRegisterAny = qemuConnectDomainEventRegisterAny, /* 0.8.0 */
    .connectDomainEventDeregisterAny = qemuConnectDomainEventDeregisterAny, /* 0.8.0 */
    .domainManagedSave = qemuDomainManagedSave, /* 0.8.0 */
    .domainHasManagedSaveImage = qemuDomainHasManagedSaveImage, /* 0.8.0 */
    .domainManagedSaveRemove = qemuDomainManagedSaveRemove, /* 0.8.0 */
    .domainManagedSaveGetXMLDesc = qemuDomainManagedSaveGetXMLDesc, /* 3.7.0 */
    .domainManagedSaveDefineXML = qemuDomainManagedSaveDefineXML, /* 3.7.0 */
    .domainSnapshotCreateXML = qemuDomainSnapshotCreateXML, /* 0.8.0 */
    .domainSnapshotGetXMLDesc = qemuDomainSnapshotGetXMLDesc, /* 0.8.0 */
    .domainSnapshotNum = qemuDomainSnapshotNum, /* 0.8.0 */
    .domainSnapshotListNames = qemuDomainSnapshotListNames, /* 0.8.0 */
    .domainListAllSnapshots = qemuDomainListAllSnapshots, /* 0.9.13 */
    .domainSnapshotNumChildren = qemuDomainSnapshotNumChildren, /* 0.9.7 */
    .domainSnapshotListChildrenNames = qemuDomainSnapshotListChildrenNames, /* 0.9.7 */
    .domainSnapshotListAllChildren = qemuDomainSnapshotListAllChildren, /* 0.9.13 */
    .domainSnapshotLookupByName = qemuDomainSnapshotLookupByName, /* 0.8.0 */
    .domainHasCurrentSnapshot = qemuDomainHasCurrentSnapshot, /* 0.8.0 */
    .domainSnapshotGetParent = qemuDomainSnapshotGetParent, /* 0.9.7 */
    .domainSnapshotCurrent = qemuDomainSnapshotCurrent, /* 0.8.0 */
    .domainSnapshotIsCurrent = qemuDomainSnapshotIsCurrent, /* 0.9.13 */
    .domainSnapshotHasMetadata = qemuDomainSnapshotHasMetadata, /* 0.9.13 */
    .domainRevertToSnapshot = qemuDomainRevertToSnapshot, /* 0.8.0 */
    .domainSnapshotDelete = qemuDomainSnapshotDelete, /* 0.8.0 */
    .domainQemuMonitorCommand = qemuDomainQemuMonitorCommand, /* 0.8.3 */
    .domainQemuAttach = NULL, /* 0.9.4 - 5.5.0 */
    .domainQemuAgentCommand = qemuDomainQemuAgentCommand, /* 0.10.0 */
    .connectDomainQemuMonitorEventRegister = qemuConnectDomainQemuMonitorEventRegister, /* 1.2.3 */
    .connectDomainQemuMonitorEventDeregister = qemuConnectDomainQemuMonitorEventDeregister, /* 1.2.3 */
    .domainOpenConsole = qemuDomainOpenConsole, /* 0.8.6 */
    .domainOpenGraphics = qemuDomainOpenGraphics, /* 0.9.7 */
    .domainOpenGraphicsFD = qemuDomainOpenGraphicsFD, /* 1.2.8 */
    .domainInjectNMI = qemuDomainInjectNMI, /* 0.9.2 */
    .domainMigrateBegin3 = qemuDomainMigrateBegin3, /* 0.9.2 */
    .domainMigratePrepare3 = qemuDomainMigratePrepare3, /* 0.9.2 */
    .domainMigratePrepareTunnel3 = qemuDomainMigratePrepareTunnel3, /* 0.9.2 */
    .domainMigratePerform3 = qemuDomainMigratePerform3, /* 0.9.2 */
    .domainMigrateFinish3 = qemuDomainMigrateFinish3, /* 0.9.2 */
    .domainMigrateConfirm3 = qemuDomainMigrateConfirm3, /* 0.9.2 */
    .domainSendKey = qemuDomainSendKey, /* 0.9.4 */
    .domainGetPerfEvents = qemuDomainGetPerfEvents, /* 1.3.3 */
    .domainSetPerfEvents = qemuDomainSetPerfEvents, /* 1.3.3 */
    .domainBlockJobAbort = qemuDomainBlockJobAbort, /* 0.9.4 */
    .domainGetBlockJobInfo = qemuDomainGetBlockJobInfo, /* 0.9.4 */
    .domainBlockJobSetSpeed = qemuDomainBlockJobSetSpeed, /* 0.9.4 */
    .domainBlockPull = qemuDomainBlockPull, /* 0.9.4 */
    .domainBlockRebase = qemuDomainBlockRebase, /* 0.9.10 */
    .domainBlockCopy = qemuDomainBlockCopy, /* 1.2.9 */
    .domainBlockCommit = qemuDomainBlockCommit, /* 1.0.0 */
    .connectIsAlive = qemuConnectIsAlive, /* 0.9.8 */
    .nodeSuspendForDuration = qemuNodeSuspendForDuration, /* 0.9.8 */
    .domainSetBlockIoTune = qemuDomainSetBlockIoTune, /* 0.9.8 */
    .domainGetBlockIoTune = qemuDomainGetBlockIoTune, /* 0.9.8 */
    .domainSetNumaParameters = qemuDomainSetNumaParameters, /* 0.9.9 */
    .domainGetNumaParameters = qemuDomainGetNumaParameters, /* 0.9.9 */
    .domainGetInterfaceParameters = qemuDomainGetInterfaceParameters, /* 0.9.9 */
    .domainSetInterfaceParameters = qemuDomainSetInterfaceParameters, /* 0.9.9 */
    .domainGetDiskErrors = qemuDomainGetDiskErrors, /* 0.9.10 */
    .domainSetMetadata = qemuDomainSetMetadata, /* 0.9.10 */
    .domainGetMetadata = qemuDomainGetMetadata, /* 0.9.10 */
    .domainPMSuspendForDuration = qemuDomainPMSuspendForDuration, /* 0.9.11 */
    .domainPMWakeup = qemuDomainPMWakeup, /* 0.9.11 */
    .domainGetCPUStats = qemuDomainGetCPUStats, /* 0.9.11 */
    .nodeGetMemoryParameters = qemuNodeGetMemoryParameters, /* 0.10.2 */
    .nodeSetMemoryParameters = qemuNodeSetMemoryParameters, /* 0.10.2 */
    .nodeGetCPUMap = qemuNodeGetCPUMap, /* 1.0.0 */
    .domainFSTrim = qemuDomainFSTrim, /* 1.0.1 */
    .domainOpenChannel = qemuDomainOpenChannel, /* 1.0.2 */
    .domainMigrateBegin3Params = qemuDomainMigrateBegin3Params, /* 1.1.0 */
    .domainMigratePrepare3Params = qemuDomainMigratePrepare3Params, /* 1.1.0 */
    .domainMigratePrepareTunnel3Params = qemuDomainMigratePrepareTunnel3Params, /* 1.1.0 */
    .domainMigratePerform3Params = qemuDomainMigratePerform3Params, /* 1.1.0 */
    .domainMigrateFinish3Params = qemuDomainMigrateFinish3Params, /* 1.1.0 */
    .domainMigrateConfirm3Params = qemuDomainMigrateConfirm3Params, /* 1.1.0 */
    .connectGetCPUModelNames = qemuConnectGetCPUModelNames, /* 1.1.3 */
    .domainFSFreeze = qemuDomainFSFreeze, /* 1.2.5 */
    .domainFSThaw = qemuDomainFSThaw, /* 1.2.5 */
    .domainGetHostname = qemuDomainGetHostname, /* 4.8.0 */
    .domainGetTime = qemuDomainGetTime, /* 1.2.5 */
    .domainSetTime = qemuDomainSetTime, /* 1.2.5 */
    .nodeGetFreePages = qemuNodeGetFreePages, /* 1.2.6 */
    .connectGetDomainCapabilities = qemuConnectGetDomainCapabilities, /* 1.2.7 */
    .connectGetAllDomainStats = qemuConnectGetAllDomainStats, /* 1.2.8 */
    .nodeAllocPages = qemuNodeAllocPages, /* 1.2.9 */
    .domainGetFSInfo = qemuDomainGetFSInfo, /* 1.2.11 */
    .domainInterfaceAddresses = qemuDomainInterfaceAddresses, /* 1.2.14 */
    .domainSetUserPassword = qemuDomainSetUserPassword, /* 1.2.16 */
    .domainRename = qemuDomainRename, /* 1.2.19 */
    .domainMigrateStartPostCopy = qemuDomainMigrateStartPostCopy, /* 1.3.3 */
    .domainGetGuestVcpus = qemuDomainGetGuestVcpus, /* 2.0.0 */
    .domainSetGuestVcpus = qemuDomainSetGuestVcpus, /* 2.0.0 */
    .domainSetVcpu = qemuDomainSetVcpu, /* 3.1.0 */
    .domainSetBlockThreshold = qemuDomainSetBlockThreshold, /* 3.2.0 */
    .domainSetLifecycleAction = qemuDomainSetLifecycleAction, /* 3.9.0 */
    .connectCompareHypervisorCPU = qemuConnectCompareHypervisorCPU, /* 4.4.0 */
    .connectBaselineHypervisorCPU = qemuConnectBaselineHypervisorCPU, /* 4.4.0 */
    .nodeGetSEVInfo = qemuNodeGetSEVInfo, /* 4.5.0 */
    .domainGetLaunchSecurityInfo = qemuDomainGetLaunchSecurityInfo, /* 4.5.0 */
    .domainCheckpointCreateXML = qemuDomainCheckpointCreateXML, /* 5.6.0 */
    .domainCheckpointGetXMLDesc = qemuDomainCheckpointGetXMLDesc, /* 5.6.0 */

    .domainListAllCheckpoints = qemuDomainListAllCheckpoints, /* 5.6.0 */
    .domainCheckpointListAllChildren = qemuDomainCheckpointListAllChildren, /* 5.6.0 */
    .domainCheckpointLookupByName = qemuDomainCheckpointLookupByName, /* 5.6.0 */
    .domainCheckpointGetParent = qemuDomainCheckpointGetParent, /* 5.6.0 */
    .domainCheckpointDelete = qemuDomainCheckpointDelete, /* 5.6.0 */
    .domainGetGuestInfo = qemuDomainGetGuestInfo, /* 5.7.0 */
    .domainAgentSetResponseTimeout = qemuDomainAgentSetResponseTimeout, /* 5.10.0 */
    .domainBackupBegin = qemuDomainBackupBegin, /* 6.0.0 */
    .domainBackupGetXMLDesc = qemuDomainBackupGetXMLDesc, /* 6.0.0 */
};


static virConnectDriver qemuConnectDriver = {
    .localOnly = true,
    .uriSchemes = (const char *[]){ "qemu", NULL },
    .hypervisorDriver = &qemuHypervisorDriver,
};

static virStateDriver qemuStateDriver = {
    .name = QEMU_DRIVER_NAME,
    .stateInitialize = qemuStateInitialize,
    .stateCleanup = qemuStateCleanup,
    .stateReload = qemuStateReload,
    .stateStop = qemuStateStop,
};

int qemuRegister(void)
{
    if (virRegisterConnectDriver(&qemuConnectDriver,
                                 true) < 0)
        return -1;
    if (virRegisterStateDriver(&qemuStateDriver) < 0)
        return -1;
    return 0;
}
