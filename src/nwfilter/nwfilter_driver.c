/*
 * nwfilter_driver.c: core driver for network filter APIs
 *                    (based on storage_driver.c)
 *
 * Copyright (C) 2006-2011, 2014 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
 * Copyright (C) 2010 IBM Corporation
 * Copyright (C) 2010 Stefan Berger
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
 *         Stefan Berger <stefanb@us.ibm.com>
 */

#include <config.h>

#include "virdbus.h"
#include "virlog.h"

#include "internal.h"

#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "domain_conf.h"
#include "domain_nwfilter.h"
#include "nwfilter_driver.h"
#include "nwfilter_gentech_driver.h"
#include "configmake.h"
#include "virfile.h"
#include "virstring.h"
#include "viraccessapicheck.h"

#include "nwfilter_ipaddrmap.h"
#include "nwfilter_dhcpsnoop.h"
#include "nwfilter_learnipaddr.h"

#define VIR_FROM_THIS VIR_FROM_NWFILTER

VIR_LOG_INIT("nwfilter.nwfilter_driver");

#define DBUS_RULE_FWD_NAMEOWNERCHANGED \
    "type='signal'" \
    ",interface='"DBUS_INTERFACE_DBUS"'" \
    ",member='NameOwnerChanged'" \
    ",arg0='org.fedoraproject.FirewallD1'"

#define DBUS_RULE_FWD_RELOADED \
    "type='signal'" \
    ",interface='org.fedoraproject.FirewallD1'" \
    ",member='Reloaded'"


static virNWFilterDriverStatePtr driver;

static int nwfilterStateCleanup(void);

static int nwfilterStateReload(void);

static void nwfilterDriverLock(void)
{
    virMutexLock(&driver->lock);
}
static void nwfilterDriverUnlock(void)
{
    virMutexUnlock(&driver->lock);
}

#if HAVE_FIREWALLD

static DBusHandlerResult
nwfilterFirewalldDBusFilter(DBusConnection *connection ATTRIBUTE_UNUSED,
                            DBusMessage *message,
                            void *user_data ATTRIBUTE_UNUSED)
{
    if (dbus_message_is_signal(message, DBUS_INTERFACE_DBUS,
                               "NameOwnerChanged") ||
        dbus_message_is_signal(message, "org.fedoraproject.FirewallD1",
                               "Reloaded")) {
        VIR_DEBUG("Reload in nwfilter_driver because of firewalld.");
        nwfilterStateReload();
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
nwfilterDriverRemoveDBusMatches(void)
{
    DBusConnection *sysbus;

    sysbus = virDBusGetSystemBus();
    if (sysbus) {
        dbus_bus_remove_match(sysbus,
                              DBUS_RULE_FWD_NAMEOWNERCHANGED,
                              NULL);
        dbus_bus_remove_match(sysbus,
                              DBUS_RULE_FWD_RELOADED,
                              NULL);
        dbus_connection_remove_filter(sysbus, nwfilterFirewalldDBusFilter, NULL);
    }
}

/**
 * virNWFilterDriverInstallDBusMatches
 *
 * Startup DBus matches for monitoring the state of firewalld
 */
static int
nwfilterDriverInstallDBusMatches(DBusConnection *sysbus)
{
    int ret = 0;

    if (!sysbus) {
        ret = -1;
    } else {
        /* add matches for
         * NameOwnerChanged on org.freedesktop.DBus for firewalld start/stop
         * Reloaded on org.fedoraproject.FirewallD1 for firewalld reload
         */
        dbus_bus_add_match(sysbus,
                           DBUS_RULE_FWD_NAMEOWNERCHANGED,
                           NULL);
        dbus_bus_add_match(sysbus,
                           DBUS_RULE_FWD_RELOADED,
                           NULL);
        if (!dbus_connection_add_filter(sysbus, nwfilterFirewalldDBusFilter,
                                        NULL, NULL)) {
            VIR_WARN(("Adding a filter to the DBus connection failed"));
            nwfilterDriverRemoveDBusMatches();
            ret =  -1;
        }
    }

    return ret;
}

#else /* HAVE_FIREWALLD */

static void
nwfilterDriverRemoveDBusMatches(void)
{
}

static int
nwfilterDriverInstallDBusMatches(DBusConnection *sysbus ATTRIBUTE_UNUSED)
{
    return 0;
}

#endif /* HAVE_FIREWALLD */

/**
 * nwfilterStateInitialize:
 *
 * Initialization function for the QEmu daemon
 */
static int
nwfilterStateInitialize(bool privileged,
                        virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                        void *opaque ATTRIBUTE_UNUSED)
{
    char *base = NULL;
    DBusConnection *sysbus = NULL;

    if (virDBusHasSystemBus() &&
        !(sysbus = virDBusGetSystemBus()))
        return -1;

    if (VIR_ALLOC(driver) < 0)
        return -1;

    if (virMutexInit(&driver->lock) < 0)
        goto err_free_driverstate;

    /* remember that we are going to use firewalld */
    driver->watchingFirewallD = (sysbus != NULL);
    driver->privileged = privileged;
    if (!(driver->nwfilters = virNWFilterObjListNew()))
        goto error;

    if (!privileged)
        return 0;

    nwfilterDriverLock();

    if (virNWFilterIPAddrMapInit() < 0)
        goto err_free_driverstate;
    if (virNWFilterLearnInit() < 0)
        goto err_exit_ipaddrmapshutdown;
    if (virNWFilterDHCPSnoopInit() < 0)
        goto err_exit_learnshutdown;

    if (virNWFilterTechDriversInit(privileged) < 0)
        goto err_dhcpsnoop_shutdown;

    if (virNWFilterConfLayerInit(virNWFilterDomainFWUpdateCB,
                                 driver) < 0)
        goto err_techdrivers_shutdown;

    /*
     * startup the DBus late so we don't get a reload signal while
     * initializing
     */
    if (sysbus &&
        nwfilterDriverInstallDBusMatches(sysbus) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DBus matches could not be installed. "
                       "Disabling nwfilter driver"));
        /*
         * unfortunately this is fatal since virNWFilterTechDriversInit
         * may have caused the ebiptables driver to use the firewall tool
         * but now that the watches don't work, we just disable the nwfilter
         * driver
         *
         * This may only happen if the system bus is available.
         */
        goto error;
    }

    if (VIR_STRDUP(base, SYSCONFDIR "/libvirt") < 0)
        goto error;

    if (virAsprintf(&driver->configDir,
                    "%s/nwfilter", base) == -1)
        goto error;

    VIR_FREE(base);

    if (virFileMakePathWithMode(driver->configDir, S_IRWXU) < 0) {
        virReportSystemError(errno, _("cannot create config directory '%s'"),
                             driver->configDir);
        goto error;
    }

    if (virNWFilterObjListLoadAllConfigs(driver->nwfilters, driver->configDir) < 0)
        goto error;

    nwfilterDriverUnlock();

    return 0;

 error:
    VIR_FREE(base);
    nwfilterDriverUnlock();
    nwfilterStateCleanup();

    return -1;

 err_techdrivers_shutdown:
    virNWFilterTechDriversShutdown();
 err_dhcpsnoop_shutdown:
    virNWFilterDHCPSnoopShutdown();
 err_exit_learnshutdown:
    virNWFilterLearnShutdown();
 err_exit_ipaddrmapshutdown:
    virNWFilterIPAddrMapShutdown();

 err_free_driverstate:
    virNWFilterObjListFree(driver->nwfilters);
    VIR_FREE(driver);

    return -1;
}

/**
 * nwfilterStateReload:
 *
 * Function to restart the nwfilter driver, it will recheck the configuration
 * files and update its state
 */
static int
nwfilterStateReload(void)
{
    if (!driver)
        return -1;

    if (!driver->privileged)
        return 0;

    virNWFilterDHCPSnoopEnd(NULL);
    /* shut down all threads -- they will be restarted if necessary */
    virNWFilterLearnThreadsTerminate(true);

    nwfilterDriverLock();
    virNWFilterWriteLockFilterUpdates();
    virNWFilterCallbackDriversLock();

    virNWFilterObjListLoadAllConfigs(driver->nwfilters, driver->configDir);

    virNWFilterCallbackDriversUnlock();
    virNWFilterUnlockFilterUpdates();
    nwfilterDriverUnlock();

    virNWFilterInstFiltersOnAllVMs();

    return 0;
}


/**
 * virNWFilterIsWatchingFirewallD:
 *
 * Checks if the nwfilter has the DBus watches for FirewallD installed.
 *
 * Returns true if it is watching firewalld, false otherwise
 */
bool
virNWFilterDriverIsWatchingFirewallD(void)
{
    if (!driver)
        return false;

    return driver->watchingFirewallD;
}

/**
 * nwfilterStateCleanup:
 *
 * Shutdown the nwfilter driver, it will stop all active nwfilters
 */
static int
nwfilterStateCleanup(void)
{
    if (!driver)
        return -1;

    if (driver->privileged) {
        virNWFilterConfLayerShutdown();
        virNWFilterDHCPSnoopShutdown();
        virNWFilterLearnShutdown();
        virNWFilterIPAddrMapShutdown();
        virNWFilterTechDriversShutdown();

        nwfilterDriverLock();

        nwfilterDriverRemoveDBusMatches();

        VIR_FREE(driver->configDir);
        nwfilterDriverUnlock();
    }

    /* free inactive nwfilters */
    virNWFilterObjListFree(driver->nwfilters);

    virMutexDestroy(&driver->lock);
    VIR_FREE(driver);

    return 0;
}


static virNWFilterObjPtr
nwfilterObjFromNWFilter(const unsigned char *uuid)
{
    virNWFilterObjPtr obj;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!(obj = virNWFilterObjListFindByUUID(driver->nwfilters, uuid))) {
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_NWFILTER,
                       _("no nwfilter with matching uuid '%s'"), uuidstr);
    }
    return obj;
}


static virNWFilterPtr
nwfilterLookupByUUID(virConnectPtr conn,
                     const unsigned char *uuid)
{
    virNWFilterObjPtr obj;
    virNWFilterDefPtr def;
    virNWFilterPtr nwfilter = NULL;

    nwfilterDriverLock();
    obj = nwfilterObjFromNWFilter(uuid);
    nwfilterDriverUnlock();

    if (!obj)
        return NULL;
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterLookupByUUIDEnsureACL(conn, def) < 0)
        goto cleanup;

    nwfilter = virGetNWFilter(conn, def->name, def->uuid);

 cleanup:
    virNWFilterObjUnlock(obj);
    return nwfilter;
}


static virNWFilterPtr
nwfilterLookupByName(virConnectPtr conn,
                     const char *name)
{
    virNWFilterObjPtr obj;
    virNWFilterDefPtr def;
    virNWFilterPtr nwfilter = NULL;

    nwfilterDriverLock();
    obj = virNWFilterObjListFindByName(driver->nwfilters, name);
    nwfilterDriverUnlock();

    if (!obj) {
        virReportError(VIR_ERR_NO_NWFILTER,
                       _("no nwfilter with matching name '%s'"), name);
        return NULL;
    }
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterLookupByNameEnsureACL(conn, def) < 0)
        goto cleanup;

    nwfilter = virGetNWFilter(conn, def->name, def->uuid);

 cleanup:
    virNWFilterObjUnlock(obj);
    return nwfilter;
}


static int
nwfilterConnectNumOfNWFilters(virConnectPtr conn)
{
    if (virConnectNumOfNWFiltersEnsureACL(conn) < 0)
        return -1;

    return virNWFilterObjListNumOfNWFilters(driver->nwfilters, conn,
                                        virConnectNumOfNWFiltersCheckACL);
}


static int
nwfilterConnectListNWFilters(virConnectPtr conn,
                             char **const names,
                             int maxnames)
{
    int nnames;

    if (virConnectListNWFiltersEnsureACL(conn) < 0)
        return -1;

    nwfilterDriverLock();
    nnames = virNWFilterObjListGetNames(driver->nwfilters, conn,
                                    virConnectListNWFiltersCheckACL,
                                    names, maxnames);
    nwfilterDriverUnlock();
    return nnames;
}


static int
nwfilterConnectListAllNWFilters(virConnectPtr conn,
                                virNWFilterPtr **nwfilters,
                                unsigned int flags)
{
    int ret;

    virCheckFlags(0, -1);

    if (virConnectListAllNWFiltersEnsureACL(conn) < 0)
        return -1;

    nwfilterDriverLock();
    ret = virNWFilterObjListExport(conn, driver->nwfilters, nwfilters,
                                   virConnectListAllNWFiltersCheckACL);
    nwfilterDriverUnlock();

    return ret;
}

static virNWFilterPtr
nwfilterDefineXML(virConnectPtr conn,
                  const char *xml)
{
    virNWFilterDefPtr def;
    virNWFilterObjPtr obj = NULL;
    virNWFilterDefPtr objdef;
    virNWFilterPtr nwfilter = NULL;

    if (!driver->privileged) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("Can't define NWFilters in session mode"));
        return NULL;
    }

    nwfilterDriverLock();
    virNWFilterWriteLockFilterUpdates();
    virNWFilterCallbackDriversLock();

    if (!(def = virNWFilterDefParseString(xml)))
        goto cleanup;

    if (virNWFilterDefineXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    if (!(obj = virNWFilterObjListAssignDef(driver->nwfilters, def)))
        goto cleanup;
    def = NULL;
    objdef = virNWFilterObjGetDef(obj);

    if (virNWFilterSaveConfig(driver->configDir, objdef) < 0) {
        virNWFilterObjListRemove(driver->nwfilters, obj);
        goto cleanup;
    }

    nwfilter = virGetNWFilter(conn, objdef->name, objdef->uuid);

 cleanup:
    virNWFilterDefFree(def);
    if (obj)
        virNWFilterObjUnlock(obj);

    virNWFilterCallbackDriversUnlock();
    virNWFilterUnlockFilterUpdates();
    nwfilterDriverUnlock();
    return nwfilter;
}


static int
nwfilterUndefine(virNWFilterPtr nwfilter)
{
    virNWFilterObjPtr obj;
    virNWFilterDefPtr def;
    int ret = -1;

    nwfilterDriverLock();
    virNWFilterWriteLockFilterUpdates();
    virNWFilterCallbackDriversLock();

    if (!(obj = nwfilterObjFromNWFilter(nwfilter->uuid)))
        goto cleanup;
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterUndefineEnsureACL(nwfilter->conn, def) < 0)
        goto cleanup;

    if (virNWFilterObjTestUnassignDef(obj) < 0) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s",
                       _("nwfilter is in use"));
        goto cleanup;
    }

    if (virNWFilterDeleteDef(driver->configDir, def) < 0)
        goto cleanup;

    virNWFilterObjListRemove(driver->nwfilters, obj);
    obj = NULL;
    ret = 0;

 cleanup:
    if (obj)
        virNWFilterObjUnlock(obj);

    virNWFilterCallbackDriversUnlock();
    virNWFilterUnlockFilterUpdates();
    nwfilterDriverUnlock();
    return ret;
}


static char *
nwfilterGetXMLDesc(virNWFilterPtr nwfilter,
                   unsigned int flags)
{
    virNWFilterObjPtr obj;
    virNWFilterDefPtr def;
    char *ret = NULL;

    virCheckFlags(0, NULL);

    nwfilterDriverLock();
    obj = nwfilterObjFromNWFilter(nwfilter->uuid);
    nwfilterDriverUnlock();

    if (!obj)
        return NULL;
    def = virNWFilterObjGetDef(obj);

    if (virNWFilterGetXMLDescEnsureACL(nwfilter->conn, def) < 0)
        goto cleanup;

    ret = virNWFilterDefFormat(def);

 cleanup:
    virNWFilterObjUnlock(obj);
    return ret;
}


static int
nwfilterInstantiateFilter(const unsigned char *vmuuid,
                          virDomainNetDefPtr net)
{
    return virNWFilterInstantiateFilter(driver, vmuuid, net);
}


static void
nwfilterTeardownFilter(virDomainNetDefPtr net)
{
    if ((net->ifname) && (net->filter))
        virNWFilterTeardownFilter(net);
}


static virNWFilterDriver nwfilterDriver = {
    .name = "nwfilter",
    .connectNumOfNWFilters = nwfilterConnectNumOfNWFilters, /* 0.8.0 */
    .connectListNWFilters = nwfilterConnectListNWFilters, /* 0.8.0 */
    .connectListAllNWFilters = nwfilterConnectListAllNWFilters, /* 0.10.2 */
    .nwfilterLookupByName = nwfilterLookupByName, /* 0.8.0 */
    .nwfilterLookupByUUID = nwfilterLookupByUUID, /* 0.8.0 */
    .nwfilterDefineXML = nwfilterDefineXML, /* 0.8.0 */
    .nwfilterUndefine = nwfilterUndefine, /* 0.8.0 */
    .nwfilterGetXMLDesc = nwfilterGetXMLDesc, /* 0.8.0 */
};


static virStateDriver stateDriver = {
    .name = "NWFilter",
    .stateInitialize = nwfilterStateInitialize,
    .stateCleanup = nwfilterStateCleanup,
    .stateReload = nwfilterStateReload,
};


static virDomainConfNWFilterDriver domainNWFilterDriver = {
    .instantiateFilter = nwfilterInstantiateFilter,
    .teardownFilter = nwfilterTeardownFilter,
};


int nwfilterRegister(void)
{
    if (virSetSharedNWFilterDriver(&nwfilterDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&stateDriver) < 0)
        return -1;
    virDomainConfNWFilterRegister(&domainNWFilterDriver);
    return 0;
}
