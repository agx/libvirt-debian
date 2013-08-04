/*
 * qemu_conf.c: QEMU configuration management
 *
 * Copyright (C) 2006-2013 Red Hat, Inc.
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
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#include "virerror.h"
#include "qemu_conf.h"
#include "qemu_command.h"
#include "qemu_capabilities.h"
#include "qemu_bridge_filter.h"
#include "viruuid.h"
#include "virbuffer.h"
#include "virconf.h"
#include "viralloc.h"
#include "datatypes.h"
#include "virxml.h"
#include "nodeinfo.h"
#include "virlog.h"
#include "cpu/cpu.h"
#include "domain_nwfilter.h"
#include "virfile.h"
#include "virstring.h"
#include "viratomic.h"
#include "storage_conf.h"
#include "configmake.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

static virClassPtr virQEMUDriverConfigClass;
static void virQEMUDriverConfigDispose(void *obj);

static int virQEMUConfigOnceInit(void)
{
    virQEMUDriverConfigClass = virClassNew(virClassForObject(),
                                           "virQEMUDriverConfig",
                                           sizeof(virQEMUDriverConfig),
                                           virQEMUDriverConfigDispose);

    if (!virQEMUDriverConfigClass)
        return -1;
    else
        return 0;
}

VIR_ONCE_GLOBAL_INIT(virQEMUConfig)


static void
qemuDriverLock(virQEMUDriverPtr driver)
{
    virMutexLock(&driver->lock);
}
static void
qemuDriverUnlock(virQEMUDriverPtr driver)
{
    virMutexUnlock(&driver->lock);
}


virQEMUDriverConfigPtr virQEMUDriverConfigNew(bool privileged)
{
    virQEMUDriverConfigPtr cfg;

    if (virQEMUConfigInitialize() < 0)
        return NULL;

    if (!(cfg = virObjectNew(virQEMUDriverConfigClass)))
        return NULL;

    cfg->privileged = privileged;
    cfg->uri = privileged ? "qemu:///system" : "qemu:///session";

    if (privileged) {
        if (virGetUserID(QEMU_USER, &cfg->user) < 0)
            goto error;
        if (virGetGroupID(QEMU_GROUP, &cfg->group) < 0)
            goto error;
    } else {
        cfg->user = (uid_t)-1;
        cfg->group = (gid_t)-1;
    }
    cfg->dynamicOwnership = privileged;

    cfg->cgroupControllers = -1; /* -1 == auto-detect */

    if (privileged) {
        if (virAsprintf(&cfg->logDir,
                        "%s/log/libvirt/qemu", LOCALSTATEDIR) < 0)
            goto error;

        if (VIR_STRDUP(cfg->configBaseDir, SYSCONFDIR "/libvirt") < 0)
            goto error;

        if (virAsprintf(&cfg->stateDir,
                      "%s/run/libvirt/qemu", LOCALSTATEDIR) < 0)
            goto error;

        if (virAsprintf(&cfg->libDir,
                      "%s/lib/libvirt/qemu", LOCALSTATEDIR) < 0)
            goto error;

        if (virAsprintf(&cfg->cacheDir,
                      "%s/cache/libvirt/qemu", LOCALSTATEDIR) < 0)
            goto error;
        if (virAsprintf(&cfg->saveDir,
                      "%s/lib/libvirt/qemu/save", LOCALSTATEDIR) < 0)
            goto error;
        if (virAsprintf(&cfg->snapshotDir,
                        "%s/lib/libvirt/qemu/snapshot", LOCALSTATEDIR) < 0)
            goto error;
        if (virAsprintf(&cfg->autoDumpPath,
                        "%s/lib/libvirt/qemu/dump", LOCALSTATEDIR) < 0)
            goto error;
    } else {
        char *rundir;
        char *cachedir;

        cachedir = virGetUserCacheDirectory();
        if (!cachedir)
            goto error;

        if (virAsprintf(&cfg->logDir,
                        "%s/qemu/log", cachedir) < 0) {
            VIR_FREE(cachedir);
            goto error;
        }
        if (virAsprintf(&cfg->cacheDir, "%s/qemu/cache", cachedir) < 0) {
            VIR_FREE(cachedir);
            goto error;
        }
        VIR_FREE(cachedir);

        rundir = virGetUserRuntimeDirectory();
        if (!rundir)
            goto error;
        if (virAsprintf(&cfg->stateDir, "%s/qemu/run", rundir) < 0) {
            VIR_FREE(rundir);
            goto error;
        }
        VIR_FREE(rundir);

        if (!(cfg->configBaseDir = virGetUserConfigDirectory()))
            goto error;

        if (virAsprintf(&cfg->libDir, "%s/qemu/lib", cfg->configBaseDir) < 0)
            goto error;
        if (virAsprintf(&cfg->saveDir, "%s/qemu/save", cfg->configBaseDir) < 0)
            goto error;
        if (virAsprintf(&cfg->snapshotDir, "%s/qemu/snapshot", cfg->configBaseDir) < 0)
            goto error;
        if (virAsprintf(&cfg->autoDumpPath, "%s/qemu/dump", cfg->configBaseDir) < 0)
            goto error;
    }

    if (virAsprintf(&cfg->configDir, "%s/qemu", cfg->configBaseDir) < 0)
        goto error;
    if (virAsprintf(&cfg->autostartDir, "%s/qemu/autostart", cfg->configBaseDir) < 0)
        goto error;


    if (VIR_STRDUP(cfg->vncListen, "127.0.0.1") < 0)
        goto error;

    if (VIR_STRDUP(cfg->vncTLSx509certdir, SYSCONFDIR "/pki/libvirt-vnc") < 0)
        goto error;

    if (VIR_STRDUP(cfg->spiceListen, "127.0.0.1") < 0)
        goto error;

    if (VIR_STRDUP(cfg->spiceTLSx509certdir , SYSCONFDIR "/pki/libvirt-spice") < 0)
        goto error;

    cfg->remotePortMin = QEMU_REMOTE_PORT_MIN;
    cfg->remotePortMax = QEMU_REMOTE_PORT_MAX;

    cfg->webSocketPortMin = QEMU_WEBSOCKET_PORT_MIN;
    cfg->webSocketPortMax = QEMU_WEBSOCKET_PORT_MAX;

#if defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R
    /* For privileged driver, try and find hugepage mount automatically.
     * Non-privileged driver requires admin to create a dir for the
     * user, chown it, and then let user configure it manually */
    if (privileged &&
        !(cfg->hugetlbfsMount = virFileFindMountPoint("hugetlbfs"))) {
        if (errno != ENOENT) {
            virReportSystemError(errno, "%s",
                                 _("unable to find hugetlbfs mountpoint"));
            goto error;
        }
    }
#endif
    if (VIR_STRDUP(cfg->bridgeHelperName, "/usr/libexec/qemu-bridge-helper") < 0)
        goto error;

    cfg->clearEmulatorCapabilities = true;

    cfg->securityDefaultConfined = true;
    cfg->securityRequireConfined = false;

    cfg->keepAliveInterval = 5;
    cfg->keepAliveCount = 5;
    cfg->seccompSandbox = -1;

    return cfg;

error:
    virObjectUnref(cfg);
    return NULL;
}


static void virQEMUDriverConfigDispose(void *obj)
{
    virQEMUDriverConfigPtr cfg = obj;


    virStringFreeList(cfg->cgroupDeviceACL);

    VIR_FREE(cfg->configBaseDir);
    VIR_FREE(cfg->configDir);
    VIR_FREE(cfg->autostartDir);
    VIR_FREE(cfg->logDir);
    VIR_FREE(cfg->stateDir);

    VIR_FREE(cfg->libDir);
    VIR_FREE(cfg->cacheDir);
    VIR_FREE(cfg->saveDir);
    VIR_FREE(cfg->snapshotDir);

    VIR_FREE(cfg->vncTLSx509certdir);
    VIR_FREE(cfg->vncListen);
    VIR_FREE(cfg->vncPassword);
    VIR_FREE(cfg->vncSASLdir);

    VIR_FREE(cfg->spiceTLSx509certdir);
    VIR_FREE(cfg->spiceListen);
    VIR_FREE(cfg->spicePassword);

    VIR_FREE(cfg->hugetlbfsMount);
    VIR_FREE(cfg->hugepagePath);
    VIR_FREE(cfg->bridgeHelperName);

    VIR_FREE(cfg->saveImageFormat);
    VIR_FREE(cfg->dumpImageFormat);
    VIR_FREE(cfg->autoDumpPath);

    virStringFreeList(cfg->securityDriverNames);

    VIR_FREE(cfg->lockManagerName);
}


int virQEMUDriverConfigLoadFile(virQEMUDriverConfigPtr cfg,
                                const char *filename)
{
    virConfPtr conf = NULL;
    virConfValuePtr p;
    int ret = -1;
    size_t i;

    /* Just check the file is readable before opening it, otherwise
     * libvirt emits an error.
     */
    if (access(filename, R_OK) == -1) {
        VIR_INFO("Could not read qemu config file %s", filename);
        return 0;
    }

    if (!(conf = virConfReadFile(filename, 0)))
        goto cleanup;

#define CHECK_TYPE(name,typ)                          \
    if (p && p->type != (typ)) {                      \
        virReportError(VIR_ERR_INTERNAL_ERROR,        \
                       "%s: %s: expected type " #typ, \
                       filename, (name));             \
        goto cleanup;                                 \
    }

#define GET_VALUE_LONG(NAME, VAR)     \
    p = virConfGetValue(conf, NAME);  \
    CHECK_TYPE(NAME, VIR_CONF_LONG);  \
    if (p)                            \
        VAR = p->l;

#define GET_VALUE_BOOL(NAME, VAR)     \
    p = virConfGetValue(conf, NAME);  \
    CHECK_TYPE(NAME, VIR_CONF_LONG);  \
    if (p)                            \
        VAR = p->l != 0;

#define GET_VALUE_STR(NAME, VAR)           \
    p = virConfGetValue(conf, NAME);       \
    CHECK_TYPE(NAME, VIR_CONF_STRING);     \
    if (p && p->str) {                     \
        VIR_FREE(VAR);                     \
        if (VIR_STRDUP(VAR, p->str) < 0)   \
            goto cleanup;                  \
    }

    GET_VALUE_BOOL("vnc_auto_unix_socket", cfg->vncAutoUnixSocket);
    GET_VALUE_BOOL("vnc_tls", cfg->vncTLS);
    GET_VALUE_BOOL("vnc_tls_x509_verify", cfg->vncTLSx509verify);
    GET_VALUE_STR("vnc_tls_x509_cert_dir", cfg->vncTLSx509certdir);
    GET_VALUE_STR("vnc_listen", cfg->vncListen);
    GET_VALUE_STR("vnc_password", cfg->vncPassword);
    GET_VALUE_BOOL("vnc_sasl", cfg->vncSASL);
    GET_VALUE_STR("vnc_sasl_dir", cfg->vncSASLdir);
    GET_VALUE_BOOL("vnc_allow_host_audio", cfg->vncAllowHostAudio);

    p = virConfGetValue(conf, "security_driver");
    if (p && p->type == VIR_CONF_LIST) {
        size_t len;
        virConfValuePtr pp;

        /* Calc length and check items */
        for (len = 0, pp = p->list; pp; len++, pp = pp->next) {
            if (pp->type != VIR_CONF_STRING) {
                virReportError(VIR_ERR_CONF_SYNTAX, "%s",
                               _("security_driver must be a list of strings"));
                goto cleanup;
            }
        }

        if (VIR_ALLOC_N(cfg->securityDriverNames, len + 1) < 0)
            goto cleanup;

        for (i = 0, pp = p->list; pp; i++, pp = pp->next) {
            if (VIR_STRDUP(cfg->securityDriverNames[i], pp->str) < 0)
                goto cleanup;
        }
        cfg->securityDriverNames[len] = NULL;
    } else {
        CHECK_TYPE("security_driver", VIR_CONF_STRING);
        if (p && p->str) {
            if (VIR_ALLOC_N(cfg->securityDriverNames, 2) < 0)
                goto cleanup;
            if (VIR_STRDUP(cfg->securityDriverNames[0], p->str) < 0)
                goto cleanup;

            cfg->securityDriverNames[1] = NULL;
        }
    }

    GET_VALUE_BOOL("security_default_confined", cfg->securityDefaultConfined);
    GET_VALUE_BOOL("security_require_confined", cfg->securityRequireConfined);

    GET_VALUE_BOOL("spice_tls", cfg->spiceTLS);
    GET_VALUE_STR("spice_tls_x509_cert_dir", cfg->spiceTLSx509certdir);
    GET_VALUE_STR("spice_listen", cfg->spiceListen);
    GET_VALUE_STR("spice_password", cfg->spicePassword);


    GET_VALUE_LONG("remote_websocket_port_min", cfg->webSocketPortMin);
    if (cfg->webSocketPortMin < QEMU_WEBSOCKET_PORT_MIN) {
        /* if the port is too low, we can't get the display name
         * to tell to vnc (usually subtract 5700, e.g. localhost:1
         * for port 5701) */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s: remote_websocket_port_min: port must be greater "
                         "than or equal to %d"),
                        filename, QEMU_WEBSOCKET_PORT_MIN);
        goto cleanup;
    }

    GET_VALUE_LONG("remote_websocket_port_max", cfg->webSocketPortMax);
    if (cfg->webSocketPortMax > QEMU_WEBSOCKET_PORT_MAX ||
        cfg->webSocketPortMax < cfg->webSocketPortMin) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("%s: remote_websocket_port_max: port must be between "
                          "the minimal port and %d"),
                       filename, QEMU_WEBSOCKET_PORT_MAX);
        goto cleanup;
    }

    if (cfg->webSocketPortMin > cfg->webSocketPortMax) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("%s: remote_websocket_port_min: min port must not be "
                          "greater than max port"), filename);
        goto cleanup;
    }

    GET_VALUE_LONG("remote_display_port_min", cfg->remotePortMin);
    if (cfg->remotePortMin < QEMU_REMOTE_PORT_MIN) {
        /* if the port is too low, we can't get the display name
         * to tell to vnc (usually subtract 5900, e.g. localhost:1
         * for port 5901) */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("%s: remote_display_port_min: port must be greater "
                         "than or equal to %d"),
                        filename, QEMU_REMOTE_PORT_MIN);
        goto cleanup;
    }

    GET_VALUE_LONG("remote_display_port_max", cfg->remotePortMax);
    if (cfg->remotePortMax > QEMU_REMOTE_PORT_MAX ||
        cfg->remotePortMax < cfg->remotePortMin) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("%s: remote_display_port_max: port must be between "
                          "the minimal port and %d"),
                       filename, QEMU_REMOTE_PORT_MAX);
        goto cleanup;
    }

    if (cfg->remotePortMin > cfg->remotePortMax) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                        _("%s: remote_display_port_min: min port must not be "
                          "greater than max port"), filename);
        goto cleanup;
    }

    p = virConfGetValue(conf, "user");
    CHECK_TYPE("user", VIR_CONF_STRING);
    if (p && p->str &&
        virGetUserID(p->str, &cfg->user) < 0)
        goto cleanup;

    p = virConfGetValue(conf, "group");
    CHECK_TYPE("group", VIR_CONF_STRING);
    if (p && p->str &&
        virGetGroupID(p->str, &cfg->group) < 0)
        goto cleanup;

    GET_VALUE_BOOL("dynamic_ownership", cfg->dynamicOwnership);

    p = virConfGetValue(conf, "cgroup_controllers");
    CHECK_TYPE("cgroup_controllers", VIR_CONF_LIST);
    if (p) {
        cfg->cgroupControllers = 0;
        virConfValuePtr pp;
        for (i = 0, pp = p->list; pp; ++i, pp = pp->next) {
            int ctl;
            if (pp->type != VIR_CONF_STRING) {
                virReportError(VIR_ERR_CONF_SYNTAX, "%s",
                               _("cgroup_controllers must be a "
                                 "list of strings"));
                goto cleanup;
            }

            if ((ctl = virCgroupControllerTypeFromString(pp->str)) < 0) {
                virReportError(VIR_ERR_CONF_SYNTAX,
                               _("Unknown cgroup controller '%s'"), pp->str);
                goto cleanup;
            }
            cfg->cgroupControllers |= (1 << ctl);
        }
    }

    p = virConfGetValue(conf, "cgroup_device_acl");
    CHECK_TYPE("cgroup_device_acl", VIR_CONF_LIST);
    if (p) {
        int len = 0;
        virConfValuePtr pp;
        for (pp = p->list; pp; pp = pp->next)
            len++;
        if (VIR_ALLOC_N(cfg->cgroupDeviceACL, 1+len) < 0)
            goto cleanup;

        for (i = 0, pp = p->list; pp; ++i, pp = pp->next) {
            if (pp->type != VIR_CONF_STRING) {
                virReportError(VIR_ERR_CONF_SYNTAX, "%s",
                               _("cgroup_device_acl must be a "
                                 "list of strings"));
                goto cleanup;
            }
            if (VIR_STRDUP(cfg->cgroupDeviceACL[i], pp->str) < 0)
                goto cleanup;
        }
        cfg->cgroupDeviceACL[i] = NULL;
    }

    GET_VALUE_STR("save_image_format", cfg->saveImageFormat);
    GET_VALUE_STR("dump_image_format", cfg->dumpImageFormat);
    GET_VALUE_STR("auto_dump_path", cfg->autoDumpPath);
    GET_VALUE_BOOL("auto_dump_bypass_cache", cfg->autoDumpBypassCache);
    GET_VALUE_BOOL("auto_start_bypass_cache", cfg->autoStartBypassCache);

    GET_VALUE_STR("hugetlbfs_mount", cfg->hugetlbfsMount);
    GET_VALUE_STR("bridge_helper", cfg->bridgeHelperName);

    GET_VALUE_BOOL("mac_filter", cfg->macFilter);

    GET_VALUE_BOOL("relaxed_acs_check", cfg->relaxedACS);
    GET_VALUE_BOOL("clear_emulator_capabilities", cfg->clearEmulatorCapabilities);
    GET_VALUE_BOOL("allow_disk_format_probing", cfg->allowDiskFormatProbing);
    GET_VALUE_BOOL("set_process_name", cfg->setProcessName);
    GET_VALUE_LONG("max_processes", cfg->maxProcesses);
    GET_VALUE_LONG("max_files", cfg->maxFiles);

    GET_VALUE_STR("lock_manager", cfg->lockManagerName);

    GET_VALUE_LONG("max_queued", cfg->maxQueuedJobs);

    GET_VALUE_LONG("keepalive_interval", cfg->keepAliveInterval);
    GET_VALUE_LONG("keepalive_count", cfg->keepAliveCount);

    GET_VALUE_LONG("seccomp_sandbox", cfg->seccompSandbox);

    ret = 0;

cleanup:
    virConfFree(conf);
    return ret;
}
#undef GET_VALUE_BOOL
#undef GET_VALUE_LONG
#undef GET_VALUE_STRING

virQEMUDriverConfigPtr virQEMUDriverGetConfig(virQEMUDriverPtr driver)
{
    virQEMUDriverConfigPtr conf;
    qemuDriverLock(driver);
    conf = virObjectRef(driver->config);
    qemuDriverUnlock(driver);
    return conf;
}

virDomainXMLOptionPtr
virQEMUDriverCreateXMLConf(virQEMUDriverPtr driver)
{
    virQEMUDriverDomainDefParserConfig.priv = driver;
    return virDomainXMLOptionNew(&virQEMUDriverDomainDefParserConfig,
                                 &virQEMUDriverPrivateDataCallbacks,
                                 &virQEMUDriverDomainXMLNamespace);
}


virCapsPtr virQEMUDriverCreateCapabilities(virQEMUDriverPtr driver)
{
    size_t i;
    virCapsPtr caps;
    virSecurityManagerPtr *sec_managers = NULL;
    /* Security driver data */
    const char *doi, *model;
    virQEMUDriverConfigPtr cfg = virQEMUDriverGetConfig(driver);

    /* Basic host arch / guest machine capabilities */
    if (!(caps = virQEMUCapsInit(driver->qemuCapsCache)))
        goto error;

    if (virGetHostUUID(caps->host.host_uuid)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("cannot get the host uuid"));
        goto error;
    }

    /* access sec drivers and create a sec model for each one */
    if (!(sec_managers = virSecurityManagerGetNested(driver->securityManager)))
        goto error;

    /* calculate length */
    for (i = 0; sec_managers[i]; i++)
        ;
    caps->host.nsecModels = i;

    if (VIR_ALLOC_N(caps->host.secModels, caps->host.nsecModels) < 0)
        goto error;

    for (i = 0; sec_managers[i]; i++) {
        doi = virSecurityManagerGetDOI(sec_managers[i]);
        model = virSecurityManagerGetModel(sec_managers[i]);
        if (VIR_STRDUP(caps->host.secModels[i].model, model) < 0 ||
            VIR_STRDUP(caps->host.secModels[i].doi, doi) < 0)
            goto error;
        VIR_DEBUG("Initialized caps for security driver \"%s\" with "
                  "DOI \"%s\"", model, doi);
    }
    VIR_FREE(sec_managers);

    virObjectUnref(cfg);
    return caps;

error:
    VIR_FREE(sec_managers);
    virObjectUnref(caps);
    virObjectUnref(cfg);
    return NULL;
}


/**
 * virQEMUDriverGetCapabilities:
 *
 * Get a reference to the virCapsPtr instance for the
 * driver. If @refresh is true, the capabilities will be
 * rebuilt first
 *
 * The caller must release the reference with virObjetUnref
 *
 * Returns: a reference to a virCapsPtr instance or NULL
 */
virCapsPtr virQEMUDriverGetCapabilities(virQEMUDriverPtr driver,
                                        bool refresh)
{
    virCapsPtr ret = NULL;
    if (refresh) {
        virCapsPtr caps = NULL;
        if ((caps = virQEMUDriverCreateCapabilities(driver)) == NULL)
            return NULL;

        qemuDriverLock(driver);
        virObjectUnref(driver->caps);
        driver->caps = caps;
    } else {
        qemuDriverLock(driver);
    }

    ret = virObjectRef(driver->caps);
    qemuDriverUnlock(driver);
    return ret;
}

struct _qemuSharedDeviceEntry {
    size_t ref;
    char **domains; /* array of domain names */
};

/* Construct the hash key for sharedDevices as "major:minor" */
char *
qemuGetSharedDeviceKey(const char *device_path)
{
    int maj, min;
    char *key = NULL;
    int rc;

    if ((rc = virGetDeviceID(device_path, &maj, &min)) < 0) {
        virReportSystemError(-rc,
                             _("Unable to get minor number of device '%s'"),
                             device_path);
        return NULL;
    }

    if (virAsprintf(&key, "%d:%d", maj, min) < 0)
        return NULL;

    return key;
}

/* Check if a shared device's setting conflicts with the conf
 * used by other domain(s). Currently only checks the sgio
 * setting. Note that this should only be called for disk with
 * block source if the device type is disk.
 *
 * Returns 0 if no conflicts, otherwise returns -1.
 */
static int
qemuCheckSharedDevice(virHashTablePtr sharedDevices,
                      virDomainDeviceDefPtr dev)
{
    virDomainDiskDefPtr disk = NULL;
    virDomainHostdevDefPtr hostdev = NULL;
    char *sysfs_path = NULL;
    char *key = NULL;
    char *hostdev_name = NULL;
    char *hostdev_path = NULL;
    char *device_path = NULL;
    int val;
    int ret = 0;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        disk = dev->data.disk;

        /* The only conflicts between shared disk we care about now
         * is sgio setting, which is only valid for device='lun'.
         */
        if (disk->device != VIR_DOMAIN_DISK_DEVICE_LUN)
            return 0;

        device_path = disk->src;
    } else if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV) {
        hostdev = dev->data.hostdev;

        if (!(hostdev_name = virSCSIDeviceGetDevName(hostdev->source.subsys.u.scsi.adapter,
                                                     hostdev->source.subsys.u.scsi.bus,
                                                     hostdev->source.subsys.u.scsi.target,
                                                     hostdev->source.subsys.u.scsi.unit)))
            goto cleanup;

        if (virAsprintf(&hostdev_path, "/dev/%s", hostdev_name) < 0)
            goto cleanup;

        device_path = hostdev_path;
    } else {
        return 0;
    }

    if (!(sysfs_path = virGetUnprivSGIOSysfsPath(device_path, NULL))) {
        ret = -1;
        goto cleanup;
    }

    /* It can't be conflict if unpriv_sgio is not supported
     * by kernel.
     */
    if (!virFileExists(sysfs_path))
        goto cleanup;

    if (!(key = qemuGetSharedDeviceKey(device_path))) {
        ret = -1;
        goto cleanup;
    }

    /* It can't be conflict if no other domain is
     * is sharing it.
     */
    if (!(virHashLookup(sharedDevices, key)))
        goto cleanup;

    if (virGetDeviceUnprivSGIO(device_path, NULL, &val) < 0) {
        ret = -1;
        goto cleanup;
    }

    if ((val == 0 &&
         (disk->sgio == VIR_DOMAIN_DEVICE_SGIO_FILTERED ||
          disk->sgio == VIR_DOMAIN_DEVICE_SGIO_DEFAULT)) ||
        (val == 1 &&
         disk->sgio == VIR_DOMAIN_DEVICE_SGIO_UNFILTERED))
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        if (disk->type == VIR_DOMAIN_DISK_TYPE_VOLUME) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("sgio of shared disk 'pool=%s' 'volume=%s' conflicts "
                             "with other active domains"),
                           disk->srcpool->pool,
                           disk->srcpool->volume);
        } else {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("sgio of shared disk '%s' conflicts with other "
                             "active domains"), disk->src);
        }
    } else {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("sgio of shared scsi host device '%s-%d-%d-%d' conflicts "
                          "with other active domains"),
                       hostdev->source.subsys.u.scsi.adapter,
                       hostdev->source.subsys.u.scsi.bus,
                       hostdev->source.subsys.u.scsi.target,
                       hostdev->source.subsys.u.scsi.unit);
    }

    ret = -1;
cleanup:
    VIR_FREE(hostdev_name);
    VIR_FREE(hostdev_path);
    VIR_FREE(sysfs_path);
    VIR_FREE(key);
    return ret;
}
bool
qemuSharedDeviceEntryDomainExists(qemuSharedDeviceEntryPtr entry,
                                  const char *name,
                                  int *idx)
{
    size_t i;

    for (i = 0; i < entry->ref; i++) {
        if (STREQ(entry->domains[i], name)) {
            if (idx)
                *idx = i;
            return true;
        }
    }

    return false;
}

void
qemuSharedDeviceEntryFree(void *payload, const void *name ATTRIBUTE_UNUSED)
{
    qemuSharedDeviceEntryPtr entry = payload;
    size_t i;

    if (!entry)
        return;

    for (i = 0; i < entry->ref; i++) {
        VIR_FREE(entry->domains[i]);
    }
    VIR_FREE(entry->domains);
    VIR_FREE(entry);
}

static qemuSharedDeviceEntryPtr
qemuSharedDeviceEntryCopy(const qemuSharedDeviceEntryPtr entry)
{
    qemuSharedDeviceEntryPtr ret = NULL;
    size_t i;

    if (VIR_ALLOC(ret) < 0)
        return NULL;

    if (VIR_ALLOC_N(ret->domains, entry->ref) < 0)
        goto cleanup;

    for (i = 0; i < entry->ref; i++) {
        if (VIR_STRDUP(ret->domains[i], entry->domains[i]) < 0)
            goto cleanup;
        ret->ref++;
    }

    return ret;

cleanup:
    qemuSharedDeviceEntryFree(ret, NULL);
    return NULL;
}

/* qemuAddSharedDevice:
 * @driver: Pointer to qemu driver struct
 * @dev: The device def
 * @name: The domain name
 *
 * Increase ref count and add the domain name into the list which
 * records all the domains that use the shared device if the entry
 * already exists, otherwise add a new entry.
 */
int
qemuAddSharedDevice(virQEMUDriverPtr driver,
                    virDomainDeviceDefPtr dev,
                    const char *name)
{
    qemuSharedDeviceEntry *entry = NULL;
    qemuSharedDeviceEntry *new_entry = NULL;
    virDomainDiskDefPtr disk = NULL;
    virDomainHostdevDefPtr hostdev = NULL;
    char *dev_name = NULL;
    char *dev_path = NULL;
    char *key = NULL;
    int ret = -1;

    /* Currently the only conflicts we have to care about for
     * the shared disk and shared host device is "sgio" setting,
     * which is only valid for block disk and scsi host device.
     */
    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        disk = dev->data.disk;

        if (!disk->shared || !virDomainDiskSourceIsBlockType(disk))
            return 0;
    } else if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV) {
        hostdev = dev->data.hostdev;

        if (!hostdev->shareable ||
            !(hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
              hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI))
            return 0;
    } else {
        return 0;
    }

    qemuDriverLock(driver);
    if (qemuCheckSharedDevice(driver->sharedDevices, dev) < 0)
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        if (!(key = qemuGetSharedDeviceKey(disk->src)))
            goto cleanup;
    } else {
        if (!(dev_name = virSCSIDeviceGetDevName(hostdev->source.subsys.u.scsi.adapter,
                                                 hostdev->source.subsys.u.scsi.bus,
                                                 hostdev->source.subsys.u.scsi.target,
                                                 hostdev->source.subsys.u.scsi.unit)))
            goto cleanup;

        if (virAsprintf(&dev_path, "/dev/%s", dev_name) < 0)
            goto cleanup;

        if (!(key = qemuGetSharedDeviceKey(dev_path)))
            goto cleanup;
    }

    if ((entry = virHashLookup(driver->sharedDevices, key))) {
        /* Nothing to do if the shared scsi host device is already
         * recorded in the table.
         */
        if (qemuSharedDeviceEntryDomainExists(entry, name, NULL)) {
            ret = 0;
            goto cleanup;
        }

        if (!(new_entry = qemuSharedDeviceEntryCopy(entry)))
            goto cleanup;

        if (VIR_EXPAND_N(new_entry->domains, new_entry->ref, 1) < 0 ||
            VIR_STRDUP(new_entry->domains[new_entry->ref - 1], name) < 0) {
            qemuSharedDeviceEntryFree(new_entry, NULL);
            goto cleanup;
        }

        if (virHashUpdateEntry(driver->sharedDevices, key, new_entry) < 0) {
            qemuSharedDeviceEntryFree(new_entry, NULL);
            goto cleanup;
        }
    } else {
        if (VIR_ALLOC(entry) < 0 ||
            VIR_ALLOC_N(entry->domains, 1) < 0 ||
            VIR_STRDUP(entry->domains[0], name) < 0) {
            qemuSharedDeviceEntryFree(entry, NULL);
            goto cleanup;
        }

        entry->ref = 1;

        if (virHashAddEntry(driver->sharedDevices, key, entry))
            goto cleanup;
    }

    ret = 0;
cleanup:
    qemuDriverUnlock(driver);
    VIR_FREE(dev_name);
    VIR_FREE(dev_path);
    VIR_FREE(key);
    return ret;
}

/* qemuRemoveSharedDevice:
 * @driver: Pointer to qemu driver struct
 * @device: The device def
 * @name: The domain name
 *
 * Decrease ref count and remove the domain name from the list which
 * records all the domains that use the shared device if ref is not
 * 1, otherwise remove the entry.
 */
int
qemuRemoveSharedDevice(virQEMUDriverPtr driver,
                       virDomainDeviceDefPtr dev,
                       const char *name)
{
    qemuSharedDeviceEntryPtr entry = NULL;
    qemuSharedDeviceEntryPtr new_entry = NULL;
    virDomainDiskDefPtr disk = NULL;
    virDomainHostdevDefPtr hostdev = NULL;
    char *key = NULL;
    char *dev_name = NULL;
    char *dev_path = NULL;
    int ret = -1;
    int idx;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        disk = dev->data.disk;

        if (!disk->shared || !virDomainDiskSourceIsBlockType(disk))
            return 0;
    } else if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV) {
        hostdev = dev->data.hostdev;

        if (!hostdev->shareable ||
            !(hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
              hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI))
            return 0;
    } else {
        return 0;
    }

    qemuDriverLock(driver);

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        if (!(key = qemuGetSharedDeviceKey(disk->src)))
            goto cleanup;
    } else {
        if (!(dev_name = virSCSIDeviceGetDevName(hostdev->source.subsys.u.scsi.adapter,
                                                 hostdev->source.subsys.u.scsi.bus,
                                                 hostdev->source.subsys.u.scsi.target,
                                                 hostdev->source.subsys.u.scsi.unit)))
            goto cleanup;

        if (virAsprintf(&dev_path, "/dev/%s", dev_name) < 0)
            goto cleanup;

        if (!(key = qemuGetSharedDeviceKey(dev_path)))
            goto cleanup;
    }

    if (!(entry = virHashLookup(driver->sharedDevices, key)))
        goto cleanup;

    /* Nothing to do if the shared disk is not recored in
     * the table.
     */
    if (!qemuSharedDeviceEntryDomainExists(entry, name, &idx)) {
        ret = 0;
        goto cleanup;
    }

    if (entry->ref != 1) {
        if (!(new_entry = qemuSharedDeviceEntryCopy(entry)))
            goto cleanup;

        if (idx != new_entry->ref - 1)
            memmove(&new_entry->domains[idx],
                    &new_entry->domains[idx + 1],
                    sizeof(*new_entry->domains) * (new_entry->ref - idx - 1));

        VIR_SHRINK_N(new_entry->domains, new_entry->ref, 1);

        if (virHashUpdateEntry(driver->sharedDevices, key, new_entry) < 0){
            qemuSharedDeviceEntryFree(new_entry, NULL);
            goto cleanup;
        }
    } else {
        if (virHashRemoveEntry(driver->sharedDevices, key) < 0)
            goto cleanup;
    }

    ret = 0;
cleanup:
    qemuDriverUnlock(driver);
    VIR_FREE(dev_name);
    VIR_FREE(dev_path);
    VIR_FREE(key);
    return ret;
}

int
qemuSetUnprivSGIO(virDomainDeviceDefPtr dev)
{
    virDomainDiskDefPtr disk = NULL;
    virDomainHostdevDefPtr hostdev = NULL;
    char *sysfs_path = NULL;
    char *path = NULL;
    char *hostdev_name = NULL;
    char *hostdev_path = NULL;
    int val = -1;
    int ret = 0;

    /* "sgio" is only valid for block disk; cdrom
     * and floopy disk can have empty source.
     */
    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        disk = dev->data.disk;

        if (disk->device != VIR_DOMAIN_DISK_DEVICE_LUN ||
            virDomainDiskSourceIsBlockType(disk))
            return 0;

        path = disk->src;
    } else if (dev->type == VIR_DOMAIN_DEVICE_HOSTDEV) {
        hostdev = dev->data.hostdev;

        if (!hostdev->shareable ||
            !(hostdev->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
              hostdev->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_SCSI))
            return 0;

        if (!(hostdev_name = virSCSIDeviceGetDevName(hostdev->source.subsys.u.scsi.adapter,
                                                     hostdev->source.subsys.u.scsi.bus,
                                                     hostdev->source.subsys.u.scsi.target,
                                                     hostdev->source.subsys.u.scsi.unit)))
            goto cleanup;

        if (virAsprintf(&hostdev_path, "/dev/%s", hostdev_name) < 0)
            goto cleanup;

        path = hostdev_path;
    } else {
        return 0;
    }

    sysfs_path = virGetUnprivSGIOSysfsPath(path, NULL);
    if (sysfs_path == NULL) {
        ret = -1;
        goto cleanup;
    }

    /* By default, filter the SG_IO commands, i.e. set unpriv_sgio to 0.  */

    if (dev->type == VIR_DOMAIN_DEVICE_DISK)
        val = (disk->sgio == VIR_DOMAIN_DEVICE_SGIO_UNFILTERED);
    else
        val = (hostdev->source.subsys.u.scsi.sgio ==
               VIR_DOMAIN_DEVICE_SGIO_UNFILTERED);

    /* Do not do anything if unpriv_sgio is not supported by the kernel and the
     * whitelist is enabled.  But if requesting unfiltered access, always call
     * virSetDeviceUnprivSGIO, to report an error for unsupported unpriv_sgio.
     */
    if ((virFileExists(sysfs_path) || val == 1) &&
        virSetDeviceUnprivSGIO(path, NULL, val) < 0)
        ret = -1;

cleanup:
    VIR_FREE(sysfs_path);
    VIR_FREE(hostdev_name);
    VIR_FREE(hostdev_path);
    return ret;
}

int qemuDriverAllocateID(virQEMUDriverPtr driver)
{
    return virAtomicIntInc(&driver->nextvmid);
}

static int
qemuAddISCSIPoolSourceHost(virDomainDiskDefPtr def,
                           virStoragePoolDefPtr pooldef)
{
    int ret = -1;
    char **tokens = NULL;

    /* Only support one host */
    if (pooldef->source.nhost != 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Expected exactly 1 host for the storage pool"));
        goto cleanup;
    }

    /* iscsi pool only supports one host */
    def->nhosts = 1;

    if (VIR_ALLOC_N(def->hosts, def->nhosts) < 0)
        goto cleanup;

    if (VIR_STRDUP(def->hosts[0].name, pooldef->source.hosts[0].name) < 0)
        goto cleanup;

    if (virAsprintf(&def->hosts[0].port, "%d",
                    pooldef->source.hosts[0].port ?
                    pooldef->source.hosts[0].port :
                    3260) < 0)
        goto cleanup;

    /* iscsi volume has name like "unit:0:0:1" */
    if (!(tokens = virStringSplit(def->srcpool->volume, ":", 0)))
        goto cleanup;

    if (virStringListLength(tokens) != 4) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected iscsi volume name '%s'"),
                       def->srcpool->volume);
        goto cleanup;
    }

    /* iscsi pool has only one source device path */
    if (virAsprintf(&def->src, "%s/%s",
                    pooldef->source.devices[0].path,
                    tokens[3]) < 0)
        goto cleanup;

    /* Storage pool have not supported these 2 attributes yet,
     * use the defaults.
     */
    def->hosts[0].transport = VIR_DOMAIN_DISK_PROTO_TRANS_TCP;
    def->hosts[0].socket = NULL;

    def->protocol = VIR_DOMAIN_DISK_PROTOCOL_ISCSI;

    ret = 0;

cleanup:
    virStringFreeList(tokens);
    return ret;
}

static int
qemuTranslateDiskSourcePoolAuth(virDomainDiskDefPtr def,
                                virStoragePoolDefPtr pooldef)
{
    int ret = -1;

    /* Only necessary when authentication set */
    if (pooldef->source.authType == VIR_STORAGE_POOL_AUTH_NONE) {
        ret = 0;
        goto cleanup;
    }

    /* Copy the authentication information from the storage pool
     * into the virDomainDiskDef
     */
    if (pooldef->source.authType == VIR_STORAGE_POOL_AUTH_CHAP) {
        if (VIR_STRDUP(def->auth.username,
                       pooldef->source.auth.chap.username) < 0)
            goto cleanup;
        if (pooldef->source.auth.chap.secret.uuidUsable) {
            def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_UUID;
            memcpy(def->auth.secret.uuid,
                   pooldef->source.auth.chap.secret.uuid,
                   VIR_UUID_BUFLEN);
        } else {
            if (VIR_STRDUP(def->auth.secret.usage,
                           pooldef->source.auth.chap.secret.usage) < 0)
                goto cleanup;
            def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_USAGE;
        }
    } else if (pooldef->source.authType == VIR_STORAGE_POOL_AUTH_CEPHX) {
        if (VIR_STRDUP(def->auth.username,
                       pooldef->source.auth.cephx.username) < 0)
            goto cleanup;
        if (pooldef->source.auth.cephx.secret.uuidUsable) {
            def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_UUID;
            memcpy(def->auth.secret.uuid,
                   pooldef->source.auth.cephx.secret.uuid,
                   VIR_UUID_BUFLEN);
        } else {
            if (VIR_STRDUP(def->auth.secret.usage,
                           pooldef->source.auth.cephx.secret.usage) < 0)
                goto cleanup;
            def->auth.secretType = VIR_DOMAIN_DISK_SECRET_TYPE_USAGE;
        }
    }
    ret = 0;

cleanup:
    return ret;
}

int
qemuTranslateDiskSourcePool(virConnectPtr conn,
                            virDomainDiskDefPtr def)
{
    virStoragePoolDefPtr pooldef = NULL;
    virStoragePoolPtr pool = NULL;
    virStorageVolPtr vol = NULL;
    char *poolxml = NULL;
    virStorageVolInfo info;
    int ret = -1;
    virErrorPtr savedError = NULL;

    if (def->type != VIR_DOMAIN_DISK_TYPE_VOLUME)
        return 0;

    if (!def->srcpool)
        return 0;

    if (!(pool = virStoragePoolLookupByName(conn, def->srcpool->pool)))
        return -1;

    if (!(vol = virStorageVolLookupByName(pool, def->srcpool->volume)))
        goto cleanup;

    if (virStorageVolGetInfo(vol, &info) < 0)
        goto cleanup;

    if (def->startupPolicy &&
        info.type != VIR_STORAGE_VOL_FILE) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("'startupPolicy' is only valid for 'file' type volume"));
        goto cleanup;
    }

    switch (info.type) {
    case VIR_STORAGE_VOL_FILE:
    case VIR_STORAGE_VOL_DIR:
        if (!(def->src = virStorageVolGetPath(vol)))
            goto cleanup;
        break;
    case VIR_STORAGE_VOL_BLOCK:
        if (!(poolxml = virStoragePoolGetXMLDesc(pool, 0)))
            goto cleanup;

        if (!(pooldef = virStoragePoolDefParseString(poolxml)))
            goto cleanup;

        if (def->srcpool->mode && pooldef->type != VIR_STORAGE_POOL_ISCSI) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("disk source mode is only valid when "
                             "storage pool is of iscsi type"));
            goto cleanup;
        }

        def->srcpool->pooltype = pooldef->type;
        if (pooldef->type == VIR_STORAGE_POOL_ISCSI) {
            /* Default to use the LUN's path on host */
            if (!def->srcpool->mode)
                def->srcpool->mode = VIR_DOMAIN_DISK_SOURCE_POOL_MODE_HOST;

            if (def->srcpool->mode ==
                VIR_DOMAIN_DISK_SOURCE_POOL_MODE_DIRECT) {
                if (qemuAddISCSIPoolSourceHost(def, pooldef) < 0)
                    goto cleanup;
            } else if (def->srcpool->mode ==
                       VIR_DOMAIN_DISK_SOURCE_POOL_MODE_HOST) {
                if (!(def->src = virStorageVolGetPath(vol)))
                    goto cleanup;
            }

            if (qemuTranslateDiskSourcePoolAuth(def, pooldef) < 0)
                goto cleanup;
        } else {
            if (!(def->src = virStorageVolGetPath(vol)))
                goto cleanup;
        }

        break;
    case VIR_STORAGE_VOL_NETWORK:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Using network volume as disk source is not supported"));
        goto cleanup;
    }

    def->srcpool->voltype = info.type;
    ret = 0;
cleanup:
    if (ret < 0)
        savedError = virSaveLastError();
    if (pool)
        virStoragePoolFree(pool);
    if (vol)
        virStorageVolFree(vol);
    if (savedError) {
        virSetError(savedError);
        virFreeError(savedError);
    }

    VIR_FREE(poolxml);
    virStoragePoolDefFree(pooldef);
    return ret;
}
