/*
 * bridge_driver.c: core driver methods for managing network
 *
 * Copyright (C) 2006-2016 Red Hat, Inc.
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
#include <stdarg.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <net/if.h>
#ifdef HAVE_SYSCTLBYNAME
# include <sys/sysctl.h>
#endif

#include "virerror.h"
#include "datatypes.h"
#include "bridge_driver.h"
#include "bridge_driver_platform.h"
#include "device_conf.h"
#include "driver.h"
#include "virbuffer.h"
#include "virpidfile.h"
#include "vircommand.h"
#include "viralloc.h"
#include "viruuid.h"
#include "viriptables.h"
#include "virlog.h"
#include "virdnsmasq.h"
#include "configmake.h"
#include "virnetlink.h"
#include "virnetdev.h"
#include "virnetdevip.h"
#include "virnetdevbridge.h"
#include "virnetdevopenvswitch.h"
#include "virnetdevtap.h"
#include "virnetdevvportprofile.h"
#include "virpci.h"
#include "virdbus.h"
#include "virfile.h"
#include "virstring.h"
#include "viraccessapicheck.h"
#include "network_event.h"
#include "virhook.h"
#include "virjson.h"
#include "virnetworkportdef.h"

#define VIR_FROM_THIS VIR_FROM_NETWORK
#define MAX_BRIDGE_ID 256

/**
 * VIR_NETWORK_DHCP_LEASE_FILE_SIZE_MAX:
 *
 * Macro providing the upper limit on the size of leases file
 */
#define VIR_NETWORK_DHCP_LEASE_FILE_SIZE_MAX (32 * 1024 * 1024)

#define SYSCTL_PATH "/proc/sys"

VIR_LOG_INIT("network.bridge_driver");

static virNetworkDriverStatePtr network_driver;


static virNetworkDriverStatePtr
networkGetDriver(void)
{
    /* Maybe one day we can store @network_driver in the
     * connection object, but until then, it's just a global
     * variable which is returned. */
    return network_driver;
}


static void
networkDriverLock(virNetworkDriverStatePtr driver)
{
    virMutexLock(&driver->lock);
}


static void
networkDriverUnlock(virNetworkDriverStatePtr driver)
{
    virMutexUnlock(&driver->lock);
}


static dnsmasqCapsPtr
networkGetDnsmasqCaps(virNetworkDriverStatePtr driver)
{
    dnsmasqCapsPtr ret;
    networkDriverLock(driver);
    ret = virObjectRef(driver->dnsmasqCaps);
    networkDriverUnlock(driver);
    return ret;
}


static int
networkDnsmasqCapsRefresh(virNetworkDriverStatePtr driver)
{
    dnsmasqCapsPtr caps;

    if (!(caps = dnsmasqCapsNewFromBinary(DNSMASQ)))
        return -1;

    networkDriverLock(driver);
    virObjectUnref(driver->dnsmasqCaps);
    driver->dnsmasqCaps = caps;
    networkDriverUnlock(driver);
    return 0;
}


static void
networkDnsmasqDefNamespaceFree(void *nsdata)
{
    networkDnsmasqXmlNsDefPtr def = nsdata;
    if (!def)
        return;

    virStringListFreeCount(def->options, def->noptions);

    VIR_FREE(def);
}


static int
networkDnsmasqDefNamespaceParseOptions(networkDnsmasqXmlNsDefPtr nsdef,
                                       xmlXPathContextPtr ctxt)
{
    g_autofree xmlNodePtr *nodes = NULL;
    ssize_t nnodes;
    size_t i;

    if ((nnodes = virXPathNodeSet("./dnsmasq:options/dnsmasq:option",
                                  ctxt, &nodes)) < 0)
        return -1;

    if (nnodes == 0)
        return 0;

    if (VIR_ALLOC_N(nsdef->options, nnodes) < 0)
        return -1;

    for (i = 0; i < nnodes; i++) {
        if (!(nsdef->options[nsdef->noptions++] = virXMLPropString(nodes[i], "value"))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("No dnsmasq options value specified"));
            return -1;
        }
    }

    return 0;
}


static int
networkDnsmasqDefNamespaceParse(xmlXPathContextPtr ctxt,
                                void **data)
{
    networkDnsmasqXmlNsDefPtr nsdata = NULL;
    int ret = -1;

    if (VIR_ALLOC(nsdata) < 0)
        return -1;

    if (networkDnsmasqDefNamespaceParseOptions(nsdata, ctxt))
        goto cleanup;

    if (nsdata->noptions > 0)
        *data = g_steal_pointer(&nsdata);

    ret = 0;

 cleanup:
    networkDnsmasqDefNamespaceFree(nsdata);
    return ret;
}


static int
networkDnsmasqDefNamespaceFormatXML(virBufferPtr buf,
                                    void *nsdata)
{
    networkDnsmasqXmlNsDefPtr def = nsdata;
    size_t i;

    if (!def->noptions)
        return 0;

    virBufferAddLit(buf, "<dnsmasq:options>\n");
    virBufferAdjustIndent(buf, 2);

    for (i = 0; i < def->noptions; i++) {
        virBufferEscapeString(buf, "<dnsmasq:option value='%s'/>\n",
                              def->options[i]);
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</dnsmasq:options>\n");

    return 0;
}


virXMLNamespace networkDnsmasqXMLNamespace = {
    .parse = networkDnsmasqDefNamespaceParse,
    .free = networkDnsmasqDefNamespaceFree,
    .format = networkDnsmasqDefNamespaceFormatXML,
    .prefix = "dnsmasq",
    .uri = "http://libvirt.org/schemas/network/dnsmasq/1.0",
};


virNetworkXMLOptionPtr
networkDnsmasqCreateXMLConf(void)
{
    return virNetworkXMLOptionNew(&networkDnsmasqXMLNamespace);
}


static int
networkStateCleanup(void);

static int
networkStartNetwork(virNetworkDriverStatePtr driver,
                    virNetworkObjPtr obj);

static int
networkShutdownNetwork(virNetworkDriverStatePtr driver,
                       virNetworkObjPtr obj);

static int
networkStartNetworkVirtual(virNetworkDriverStatePtr driver,
                           virNetworkObjPtr obj);

static int
networkShutdownNetworkVirtual(virNetworkDriverStatePtr driver,
                              virNetworkObjPtr obj);

static int
networkStartNetworkExternal(virNetworkObjPtr obj);

static int
networkShutdownNetworkExternal(virNetworkObjPtr obj);

static void
networkReloadFirewallRules(virNetworkDriverStatePtr driver, bool startup);

static void
networkRefreshDaemons(virNetworkDriverStatePtr driver);

static int
networkPlugBandwidth(virNetworkObjPtr obj,
                     virMacAddrPtr mac,
                     virNetDevBandwidthPtr ifaceBand,
                     unsigned int *class_id);

static int
networkUnplugBandwidth(virNetworkObjPtr obj,
                       virNetDevBandwidthPtr ifaceBand,
                       unsigned int *class_id);

static void
networkNetworkObjTaint(virNetworkObjPtr obj,
                       virNetworkTaintFlags taint);


static virNetworkObjPtr
networkObjFromNetwork(virNetworkPtr net)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    obj = virNetworkObjFindByUUID(driver->networks, net->uuid);
    if (!obj) {
        virUUIDFormat(net->uuid, uuidstr);
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching uuid '%s' (%s)"),
                       uuidstr, net->name);
    }

    return obj;
}


static int
networkRunHook(virNetworkObjPtr obj,
               virNetworkPortDefPtr port,
               int op,
               int sub_op)
{
    virNetworkDefPtr def;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *xml = NULL;
    int hookret;
    int ret = -1;

    if (virHookPresent(VIR_HOOK_DRIVER_NETWORK)) {
        if (!obj) {
            VIR_DEBUG("Not running hook as @obj is NULL");
            ret = 0;
            goto cleanup;
        }
        def = virNetworkObjGetDef(obj);

        virBufferAddLit(&buf, "<hookData>\n");
        virBufferAdjustIndent(&buf, 2);
        if (virNetworkDefFormatBuf(&buf, def, network_driver->xmlopt, 0) < 0)
            goto cleanup;
        if (port && virNetworkPortDefFormatBuf(&buf, port) < 0)
            goto cleanup;

        virBufferAdjustIndent(&buf, -2);
        virBufferAddLit(&buf, "</hookData>");

        xml = virBufferContentAndReset(&buf);
        hookret = virHookCall(VIR_HOOK_DRIVER_NETWORK, def->name,
                              op, sub_op, NULL, xml, NULL);

        /*
         * If the script raised an error, pass it to the callee.
         */
        if (hookret < 0)
            goto cleanup;

        networkNetworkObjTaint(obj, VIR_NETWORK_TAINT_HOOK);
    }

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&buf);
    VIR_FREE(xml);
    return ret;
}


static char *
networkDnsmasqLeaseFileNameDefault(virNetworkDriverStatePtr driver,
                                   const char *netname)
{
    char *leasefile;

    leasefile = g_strdup_printf("%s/%s.leases", driver->dnsmasqStateDir, netname);
    return leasefile;
}


static char *
networkDnsmasqLeaseFileNameCustom(virNetworkDriverStatePtr driver,
                                  const char *bridge)
{
    char *leasefile;

    leasefile = g_strdup_printf("%s/%s.status", driver->dnsmasqStateDir, bridge);
    return leasefile;
}


static char *
networkDnsmasqConfigFileName(virNetworkDriverStatePtr driver,
                             const char *netname)
{
    char *conffile;

    conffile = g_strdup_printf("%s/%s.conf", driver->dnsmasqStateDir, netname);
    return conffile;
}


static char *
networkRadvdPidfileBasename(const char *netname)
{
    /* this is simple but we want to be sure it's consistently done */
    char *pidfilebase;

    pidfilebase = g_strdup_printf("%s-radvd", netname);
    return pidfilebase;
}


static char *
networkRadvdConfigFileName(virNetworkDriverStatePtr driver,
                           const char *netname)
{
    char *configfile;

    configfile = g_strdup_printf("%s/%s-radvd.conf", driver->radvdStateDir, netname);
    return configfile;
}


/* do needed cleanup steps and remove the network from the list */
static int
networkRemoveInactive(virNetworkDriverStatePtr driver,
                      virNetworkObjPtr obj)
{
    char *leasefile = NULL;
    char *customleasefile = NULL;
    char *radvdconfigfile = NULL;
    char *configfile = NULL;
    char *radvdpidbase = NULL;
    char *statusfile = NULL;
    char *macMapFile = NULL;
    dnsmasqContext *dctx = NULL;
    virNetworkDefPtr def = virNetworkObjGetPersistentDef(obj);

    int ret = -1;

    /* remove the (possibly) existing dnsmasq and radvd files */
    if (!(dctx = dnsmasqContextNew(def->name,
                                   driver->dnsmasqStateDir))) {
        goto cleanup;
    }

    if (!(leasefile = networkDnsmasqLeaseFileNameDefault(driver, def->name)))
        goto cleanup;

    if (!(customleasefile = networkDnsmasqLeaseFileNameCustom(driver, def->bridge)))
        goto cleanup;

    if (!(radvdconfigfile = networkRadvdConfigFileName(driver, def->name)))
        goto cleanup;

    if (!(radvdpidbase = networkRadvdPidfileBasename(def->name)))
        goto cleanup;

    if (!(configfile = networkDnsmasqConfigFileName(driver, def->name)))
        goto cleanup;

    if (!(statusfile = virNetworkConfigFile(driver->stateDir, def->name)))
        goto cleanup;

    if (!(macMapFile = virMacMapFileName(driver->dnsmasqStateDir, def->bridge)))
        goto cleanup;

    /* dnsmasq */
    dnsmasqDelete(dctx);
    unlink(leasefile);
    unlink(customleasefile);
    unlink(configfile);

    /* MAC map manager */
    unlink(macMapFile);

    /* radvd */
    unlink(radvdconfigfile);
    virPidFileDelete(driver->pidDir, radvdpidbase);

    /* remove status file */
    unlink(statusfile);

    /* remove the network definition */
    virNetworkObjRemoveInactive(driver->networks, obj);

    ret = 0;

 cleanup:
    VIR_FREE(leasefile);
    VIR_FREE(configfile);
    VIR_FREE(customleasefile);
    VIR_FREE(radvdconfigfile);
    VIR_FREE(radvdpidbase);
    VIR_FREE(statusfile);
    VIR_FREE(macMapFile);
    dnsmasqContextFree(dctx);
    return ret;
}


static char *
networkBridgeDummyNicName(const char *brname)
{
    static const char dummyNicSuffix[] = "-nic";
    char *nicname;

    if (strlen(brname) + sizeof(dummyNicSuffix) > IFNAMSIZ) {
        /* because the length of an ifname is limited to IFNAMSIZ-1
         * (usually 15), and we're adding 4 more characters, we must
         * truncate the original name to 11 to fit. In order to catch
         * a possible numeric ending (eg virbr0, virbr1, etc), we grab
         * the first 8 and last 3 characters of the string.
         */
        nicname = g_strdup_printf("%.*s%s%s",
                                  /* space for last 3 chars + "-nic" + NULL */
                                  (int)(IFNAMSIZ - (3 + sizeof(dummyNicSuffix))),
                                  brname, brname + strlen(brname) - 3,
                                  dummyNicSuffix);
    } else {
        nicname = g_strdup_printf("%s%s", brname, dummyNicSuffix);
    }
    return nicname;
}


static int
networkNotifyPort(virNetworkObjPtr obj,
                  virNetworkPortDefPtr port);

static bool
networkUpdatePort(virNetworkPortDefPtr port,
                  void *opaque)
{
    virNetworkObjPtr obj = opaque;

    networkNotifyPort(obj, port);

    return false;
}

static int
networkUpdateState(virNetworkObjPtr obj,
                   void *opaque)
{
    virNetworkDefPtr def;
    virNetworkDriverStatePtr driver = opaque;
    dnsmasqCapsPtr dnsmasq_caps = networkGetDnsmasqCaps(driver);
    virMacMapPtr macmap;
    char *macMapFile = NULL;
    int ret = -1;

    virObjectLock(obj);
    if (!virNetworkObjIsActive(obj)) {
        ret = 0;
        goto cleanup;
    }
    def = virNetworkObjGetDef(obj);

    switch ((virNetworkForwardType) def->forward.type) {
    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
    case VIR_NETWORK_FORWARD_OPEN:
        /* If bridge doesn't exist, then mark it inactive */
        if (!(def->bridge && virNetDevExists(def->bridge) == 1))
            virNetworkObjSetActive(obj, false);

        if (!(macMapFile = virMacMapFileName(driver->dnsmasqStateDir,
                                             def->bridge)))
            goto cleanup;

        if (!(macmap = virMacMapNew(macMapFile)))
            goto cleanup;

        virNetworkObjSetMacMap(obj, macmap);

        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
        if (def->bridge) {
            if (virNetDevExists(def->bridge) != 1)
                virNetworkObjSetActive(obj, false);
            break;
        }
        /* intentionally drop through to common case for all
         * macvtap networks (forward='bridge' with no bridge
         * device defined is macvtap using its 'bridge' mode)
         */
    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
        /* so far no extra checks */
        break;

    case VIR_NETWORK_FORWARD_HOSTDEV:
        /* so far no extra checks */
        break;

    case VIR_NETWORK_FORWARD_LAST:
    default:
        virReportEnumRangeError(virNetworkForwardType, def->forward.type);
        goto cleanup;
    }

    virNetworkObjPortForEach(obj, networkUpdatePort, obj);

    /* Try and read dnsmasq/radvd pids of active networks */
    if (virNetworkObjIsActive(obj) && def->ips && (def->nips > 0)) {
        pid_t radvdPid;
        pid_t dnsmasqPid;
        char *radvdpidbase;

        ignore_value(virPidFileReadIfAlive(driver->pidDir,
                                           def->name,
                                           &dnsmasqPid,
                                           dnsmasqCapsGetBinaryPath(dnsmasq_caps)));
        virNetworkObjSetDnsmasqPid(obj, dnsmasqPid);

        radvdpidbase = networkRadvdPidfileBasename(def->name);
        if (!radvdpidbase)
            goto cleanup;

        ignore_value(virPidFileReadIfAlive(driver->pidDir,
                                           radvdpidbase,
                                           &radvdPid, RADVD));
        virNetworkObjSetRadvdPid(obj, radvdPid);
        VIR_FREE(radvdpidbase);
    }

    ret = 0;
 cleanup:
    virObjectUnlock(obj);
    virObjectUnref(dnsmasq_caps);
    VIR_FREE(macMapFile);
    return ret;
}


static int
networkAutostartConfig(virNetworkObjPtr obj,
                       void *opaque)
{
    virNetworkDriverStatePtr driver = opaque;
    int ret = -1;

    virObjectLock(obj);
    if (virNetworkObjIsAutostart(obj) &&
        !virNetworkObjIsActive(obj) &&
        networkStartNetwork(driver, obj) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virObjectUnlock(obj);
    return ret;
}


#ifdef WITH_FIREWALLD
static DBusHandlerResult
firewalld_dbus_filter_bridge(DBusConnection *connection G_GNUC_UNUSED,
                             DBusMessage *message,
                             void *user_data)
{
    virNetworkDriverStatePtr driver = user_data;
    bool reload = false;

    if (dbus_message_is_signal(message,
                               "org.fedoraproject.FirewallD1", "Reloaded")) {
        reload = true;

    } else if (dbus_message_is_signal(message,
                                      DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {

        g_autofree char *name = NULL;
        g_autofree char *old_owner = NULL;
        g_autofree char *new_owner = NULL;

        if (virDBusMessageDecode(message, "sss", &name, &old_owner, &new_owner) < 0) {
            VIR_WARN("Failed to decode DBus NameOwnerChanged message");
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        /*
         * if new_owner is empty, firewalld is shutting down. If it is
         * non-empty, then it is starting
         */
        if (new_owner && *new_owner)
            reload = true;
    }

    if (reload) {
        VIR_DEBUG("Reload in bridge_driver because of firewalld.");
        networkReloadFirewallRules(driver, false);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
#endif


/**
 * networkStateInitialize:
 *
 * Initialization function for the QEMU daemon
 */
static int
networkStateInitialize(bool privileged,
                       virStateInhibitCallback callback G_GNUC_UNUSED,
                       void *opaque G_GNUC_UNUSED)
{
    int ret = VIR_DRV_STATE_INIT_ERROR;
    char *configdir = NULL;
    char *rundir = NULL;
    bool autostart = true;
#ifdef WITH_FIREWALLD
    DBusConnection *sysbus = NULL;
#endif

    if (VIR_ALLOC(network_driver) < 0)
        goto error;

    network_driver->lockFD = -1;
    if (virMutexInit(&network_driver->lock) < 0) {
        VIR_FREE(network_driver);
        goto error;
    }

    network_driver->privileged = privileged;

    if (!(network_driver->xmlopt = networkDnsmasqCreateXMLConf()))
        goto error;

    /* configuration/state paths are one of
     * ~/.config/libvirt/... (session/unprivileged)
     * /etc/libvirt/... && /var/(run|lib)/libvirt/... (system/privileged).
     */
    if (privileged) {
        network_driver->networkConfigDir = g_strdup(SYSCONFDIR "/libvirt/qemu/networks");
        network_driver->networkAutostartDir = g_strdup(SYSCONFDIR "/libvirt/qemu/networks/autostart");
        network_driver->stateDir = g_strdup(RUNSTATEDIR "/libvirt/network");
        network_driver->pidDir = g_strdup(RUNSTATEDIR "/libvirt/network");
        network_driver->dnsmasqStateDir = g_strdup(LOCALSTATEDIR "/lib/libvirt/dnsmasq");
        network_driver->radvdStateDir = g_strdup(LOCALSTATEDIR "/lib/libvirt/radvd");
    } else {
        configdir = virGetUserConfigDirectory();
        rundir = virGetUserRuntimeDirectory();

        network_driver->networkConfigDir = g_strdup_printf("%s/qemu/networks", configdir);
        network_driver->networkAutostartDir = g_strdup_printf("%s/qemu/networks/autostart", configdir);
        network_driver->stateDir = g_strdup_printf("%s/network/lib", rundir);
        network_driver->pidDir = g_strdup_printf("%s/network/run", rundir);
        network_driver->dnsmasqStateDir = g_strdup_printf("%s/dnsmasq/lib", rundir);
        network_driver->radvdStateDir = g_strdup_printf("%s/radvd/lib", rundir);
    }

    if (virFileMakePath(network_driver->stateDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             network_driver->stateDir);
        goto error;
    }

    if ((network_driver->lockFD =
         virPidFileAcquire(network_driver->stateDir, "driver",
                           false, getpid())) < 0)
        goto error;

    /* if this fails now, it will be retried later with dnsmasqCapsRefresh() */
    network_driver->dnsmasqCaps = dnsmasqCapsNewFromBinary(DNSMASQ);

    if (!(network_driver->networks = virNetworkObjListNew()))
        goto error;

    if (virNetworkObjLoadAllState(network_driver->networks,
                                  network_driver->stateDir,
                                  network_driver->xmlopt) < 0)
        goto error;

    if (virNetworkObjLoadAllConfigs(network_driver->networks,
                                    network_driver->networkConfigDir,
                                    network_driver->networkAutostartDir,
                                    network_driver->xmlopt) < 0)
        goto error;

    /* Update the internal status of all allegedly active
     * networks according to external conditions on the host
     * (i.e. anything that isn't stored directly in each
     * network's state file). */
    virNetworkObjListForEach(network_driver->networks,
                             networkUpdateState,
                             network_driver);
    virNetworkObjListPrune(network_driver->networks,
                           VIR_CONNECT_LIST_NETWORKS_INACTIVE |
                           VIR_CONNECT_LIST_NETWORKS_TRANSIENT);
    networkReloadFirewallRules(network_driver, true);
    networkRefreshDaemons(network_driver);

    if (virDriverShouldAutostart(network_driver->stateDir, &autostart) < 0)
        goto error;

    if (autostart) {
        virNetworkObjListForEach(network_driver->networks,
                                 networkAutostartConfig,
                                 network_driver);
    }

    network_driver->networkEventState = virObjectEventStateNew();

#ifdef WITH_FIREWALLD
    if (!(sysbus = virDBusGetSystemBus())) {
        VIR_WARN("DBus not available, disabling firewalld support "
                 "in bridge_network_driver: %s", virGetLastErrorMessage());
    } else {
        /* add matches for
         * NameOwnerChanged on org.freedesktop.DBus for firewalld start/stop
         * Reloaded on org.fedoraproject.FirewallD1 for firewalld reload
         */
        dbus_bus_add_match(sysbus,
                           "type='signal'"
                           ",interface='"DBUS_INTERFACE_DBUS"'"
                           ",member='NameOwnerChanged'"
                           ",arg0='org.fedoraproject.FirewallD1'",
                           NULL);
        dbus_bus_add_match(sysbus,
                           "type='signal'"
                           ",interface='org.fedoraproject.FirewallD1'"
                           ",member='Reloaded'",
                           NULL);
        dbus_connection_add_filter(sysbus, firewalld_dbus_filter_bridge,
                                   network_driver, NULL);
    }
#endif

    ret = VIR_DRV_STATE_INIT_COMPLETE;
 cleanup:
    VIR_FREE(configdir);
    VIR_FREE(rundir);
    return ret;

 error:
    networkStateCleanup();
    goto cleanup;
}


/**
 * networkStateReload:
 *
 * Function to restart the QEMU daemon, it will recheck the configuration
 * files and update its state and the networking
 */
static int
networkStateReload(void)
{
    if (!network_driver)
        return 0;

    virNetworkObjLoadAllState(network_driver->networks,
                              network_driver->stateDir,
                              network_driver->xmlopt);
    virNetworkObjLoadAllConfigs(network_driver->networks,
                                network_driver->networkConfigDir,
                                network_driver->networkAutostartDir,
                                network_driver->xmlopt);
    networkReloadFirewallRules(network_driver, false);
    networkRefreshDaemons(network_driver);
    virNetworkObjListForEach(network_driver->networks,
                             networkAutostartConfig,
                             network_driver);
    return 0;
}


/**
 * networkStateCleanup:
 *
 * Shutdown the QEMU daemon, it will stop all active domains and networks
 */
static int
networkStateCleanup(void)
{
    if (!network_driver)
        return -1;

    virObjectUnref(network_driver->networkEventState);
    virObjectUnref(network_driver->xmlopt);

    /* free inactive networks */
    virObjectUnref(network_driver->networks);

    if (network_driver->lockFD != -1)
        virPidFileRelease(network_driver->stateDir, "driver",
                          network_driver->lockFD);

    VIR_FREE(network_driver->networkConfigDir);
    VIR_FREE(network_driver->networkAutostartDir);
    VIR_FREE(network_driver->stateDir);
    VIR_FREE(network_driver->pidDir);
    VIR_FREE(network_driver->dnsmasqStateDir);
    VIR_FREE(network_driver->radvdStateDir);

    virObjectUnref(network_driver->dnsmasqCaps);

    virMutexDestroy(&network_driver->lock);

    VIR_FREE(network_driver);

    return 0;
}


static virDrvOpenStatus
networkConnectOpen(virConnectPtr conn,
                   virConnectAuthPtr auth G_GNUC_UNUSED,
                   virConfPtr conf G_GNUC_UNUSED,
                   unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (network_driver == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("network state driver is not active"));
        return VIR_DRV_OPEN_ERROR;
    }

    if (!virConnectValidateURIPath(conn->uri->path,
                                   "network",
                                   network_driver->privileged))
        return VIR_DRV_OPEN_ERROR;

    if (virConnectOpenEnsureACL(conn) < 0)
        return VIR_DRV_OPEN_ERROR;

    return VIR_DRV_OPEN_SUCCESS;
}

static int networkConnectClose(virConnectPtr conn G_GNUC_UNUSED)
{
    return 0;
}


static int networkConnectIsSecure(virConnectPtr conn G_GNUC_UNUSED)
{
    /* Trivially secure, since always inside the daemon */
    return 1;
}


static int networkConnectIsEncrypted(virConnectPtr conn G_GNUC_UNUSED)
{
    /* Not encrypted, but remote driver takes care of that */
    return 0;
}


static int networkConnectIsAlive(virConnectPtr conn G_GNUC_UNUSED)
{
    return 1;
}


/* networkKillDaemon:
 *
 * kill the specified pid/name, and wait a bit to make sure it's dead.
 */
static int
networkKillDaemon(pid_t pid,
                  const char *daemonName,
                  const char *networkName)
{
    size_t i;
    int ret = -1;
    const char *signame = "TERM";

    /* send SIGTERM, then wait up to 3 seconds for the process to
     * disappear, send SIGKILL, then wait for up to another 2
     * seconds. If that fails, log a warning and continue, hoping
     * for the best.
     */
    for (i = 0; i < 25; i++) {
        int signum = 0;
        if (i == 0) {
            signum = SIGTERM;
        } else if (i == 15) {
            signum = SIGKILL;
            signame = "KILL";
        }
        if (kill(pid, signum) < 0) {
            if (errno == ESRCH) {
                ret = 0;
            } else {
                char ebuf[1024];
                VIR_WARN("Failed to terminate %s process %d "
                         "for network '%s' with SIG%s: %s",
                         daemonName, pid, networkName, signame,
                         virStrerror(errno, ebuf, sizeof(ebuf)));
            }
            return ret;
        }
        /* NB: since networks have no reference count like
         * domains, there is no safe way to unlock the network
         * object temporarily, and so we can't follow the
         * procedure used by the qemu driver of 1) unlock driver
         * 2) sleep, 3) add ref to object 4) unlock object, 5)
         * re-lock driver, 6) re-lock object. We may need to add
         * that functionality eventually, but for now this
         * function is rarely used and, at worst, leaving the
         * network driver locked during this loop of sleeps will
         * have the effect of holding up any other thread trying
         * to make modifications to a network for up to 5 seconds;
         * since modifications to networks are much less common
         * than modifications to domains, this seems a reasonable
         * tradeoff in exchange for less code disruption.
         */
        g_usleep(20 * 1000);
    }
    VIR_WARN("Timed out waiting after SIG%s to %s process %d "
             "(network '%s')",
             signame, daemonName, pid, networkName);
    return ret;
}


/* the following does not build a file, it builds a list
 * which is later saved into a file
 */
static int
networkBuildDnsmasqDhcpHostsList(dnsmasqContext *dctx,
                                 virNetworkIPDefPtr ipdef)
{
    size_t i;
    bool ipv6 = false;

    if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6))
        ipv6 = true;
    for (i = 0; i < ipdef->nhosts; i++) {
        virNetworkDHCPHostDefPtr host = &(ipdef->hosts[i]);
        if (VIR_SOCKET_ADDR_VALID(&host->ip))
            if (dnsmasqAddDhcpHost(dctx, host->mac, &host->ip,
                                   host->name, host->id, ipv6) < 0)
                return -1;
    }

    return 0;
}


static int
networkBuildDnsmasqHostsList(dnsmasqContext *dctx,
                             virNetworkDNSDefPtr dnsdef)
{
    size_t i, j;

    if (dnsdef) {
        for (i = 0; i < dnsdef->nhosts; i++) {
            virNetworkDNSHostDefPtr host = &(dnsdef->hosts[i]);
            if (VIR_SOCKET_ADDR_VALID(&host->ip)) {
                for (j = 0; j < host->nnames; j++)
                    if (dnsmasqAddHost(dctx, &host->ip, host->names[j]) < 0)
                        return -1;
            }
        }
    }

    return 0;
}


static int
networkDnsmasqConfLocalPTRs(virBufferPtr buf,
                            virNetworkDefPtr def)
{
    virNetworkIPDefPtr ip;
    size_t i;
    char *ptr = NULL;
    int rc;

    for (i = 0; i < def->nips; i++) {
        ip = def->ips + i;

        if (ip->localPTR != VIR_TRISTATE_BOOL_YES)
            continue;

        if ((rc = virSocketAddrPTRDomain(&ip->address,
                                         virNetworkIPDefPrefix(ip),
                                         &ptr)) < 0) {
            if (rc == -2) {
                int family = VIR_SOCKET_ADDR_FAMILY(&ip->address);
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("PTR domain for %s network with prefix %u "
                                 "cannot be automatically created"),
                               (family == AF_INET) ? "IPv4" : "IPv6",
                               virNetworkIPDefPrefix(ip));
            }
            return -1;
        }

        virBufferAsprintf(buf, "local=/%s/\n", ptr);
        VIR_FREE(ptr);
    }

    return 0;
}


int
networkDnsmasqConfContents(virNetworkObjPtr obj,
                           const char *pidfile,
                           char **configstr,
                           dnsmasqContext *dctx,
                           dnsmasqCapsPtr caps G_GNUC_UNUSED)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virBuffer configbuf = VIR_BUFFER_INITIALIZER;
    int r, ret = -1;
    int nbleases = 0;
    size_t i;
    virNetworkDNSDefPtr dns = &def->dns;
    bool wantDNS = dns->enable != VIR_TRISTATE_BOOL_NO;
    virNetworkIPDefPtr tmpipdef, ipdef, ipv4def, ipv6def;
    bool ipv6SLAAC;
    char *saddr = NULL, *eaddr = NULL;

    *configstr = NULL;

    /*
     * All dnsmasq parameters are put into a configuration file, except the
     * command line --conf-file=parameter which specifies the location of
     * configuration file.
     *
     * All dnsmasq conf-file parameters must be specified as "foo=bar"
     * as oppose to "--foo bar" which was acceptable on the command line.
     */

    /*
     * Needed to ensure dnsmasq uses same algorithm for processing
     * multiple namedriver entries in /etc/resolv.conf as GLibC.
     */

    /* create dnsmasq config file appropriate for this network */

    /* Don't forget to update networkxml2conftest :-) */
    virBufferAsprintf(&configbuf,
                      "##WARNING:  THIS IS AN AUTO-GENERATED FILE. "
                      "CHANGES TO IT ARE LIKELY TO BE\n"
                      "##OVERWRITTEN AND LOST.  Changes to this "
                      "configuration should be made using:\n"
                      "##    virsh net-edit %s\n"
                      "## or other application using the libvirt API.\n"
                      "##\n## dnsmasq conf file created by libvirt\n"
                      "strict-order\n",
                      def->name);

    /* if dns is disabled, set its listening port to 0, which
     * tells dnsmasq to not listen
     */
    if (!wantDNS)
        virBufferAddLit(&configbuf, "port=0\n");

    if (wantDNS && def->dns.forwarders) {
        /* addNoResolv should be set to true if there are any entries
         * that specify an IP address for requests, but no domain
         * qualifier (implying that all requests otherwise "unclaimed"
         * should be sent to that address). if it is still false when
         * we've looked at all entries, it means we still need the
         * host's resolv.conf for some cases.
         */
        bool addNoResolv = false;

        for (i = 0; i < def->dns.nfwds; i++) {
            virNetworkDNSForwarderPtr fwd = &def->dns.forwarders[i];

            virBufferAddLit(&configbuf, "server=");
            if (fwd->domain)
                virBufferAsprintf(&configbuf, "/%s/", fwd->domain);
            if (VIR_SOCKET_ADDR_VALID(&fwd->addr)) {
                char *addr = virSocketAddrFormat(&fwd->addr);

                if (!addr)
                    goto cleanup;
                virBufferAsprintf(&configbuf, "%s\n", addr);
                VIR_FREE(addr);
                if (!fwd->domain)
                    addNoResolv = true;
            } else {
                /* "don't forward requests for this domain" */
                virBufferAddLit(&configbuf, "#\n");
            }
        }
        if (addNoResolv)
            virBufferAddLit(&configbuf, "no-resolv\n");
    }

    if (def->domain) {
        if (def->domainLocalOnly == VIR_TRISTATE_BOOL_YES) {
            virBufferAsprintf(&configbuf,
                              "local=/%s/\n",
                              def->domain);
        }
        virBufferAsprintf(&configbuf,
                          "domain=%s\n"
                          "expand-hosts\n",
                          def->domain);
    }

    if (wantDNS &&
        networkDnsmasqConfLocalPTRs(&configbuf, def) < 0)
        goto cleanup;

    if (wantDNS && def->dns.forwardPlainNames == VIR_TRISTATE_BOOL_NO) {
        virBufferAddLit(&configbuf, "domain-needed\n");
        /* need to specify local=// whether or not a domain is
         * specified, unless the config says we should forward "plain"
         * names (i.e. not fully qualified, no '.' characters)
         */
        virBufferAddLit(&configbuf, "local=//\n");
    }

    if (pidfile)
        virBufferAsprintf(&configbuf, "pid-file=%s\n", pidfile);

    /* dnsmasq will *always* listen on localhost unless told otherwise */
#ifdef __linux__
    virBufferAddLit(&configbuf, "except-interface=lo\n");
#else
    /* BSD family OSes and Solaris call loopback interface as lo0 */
    virBufferAddLit(&configbuf, "except-interface=lo0\n");
#endif

    if (dnsmasqCapsGet(caps, DNSMASQ_CAPS_BIND_DYNAMIC)) {
        /* using --bind-dynamic with only --interface (no
         * --listen-address) prevents dnsmasq from responding to dns
         * queries that arrive on some interface other than our bridge
         * interface (in other words, requests originating somewhere
         * other than one of the virtual guests connected directly to
         * this network). This was added in response to CVE 2012-3411.
         */
        virBufferAsprintf(&configbuf,
                          "bind-dynamic\n"
                          "interface=%s\n",
                          def->bridge);
    } else {
        virBufferAddLit(&configbuf, "bind-interfaces\n");
        /*
         * --interface does not actually work with dnsmasq < 2.47,
         * due to DAD for ipv6 addresses on the interface.
         *
         * virCommandAddArgList(cmd, "--interface", def->bridge, NULL);
         *
         * So listen on all defined IPv[46] addresses
         */
        for (i = 0;
             (tmpipdef = virNetworkDefGetIPByIndex(def, AF_UNSPEC, i));
             i++) {
            char *ipaddr = virSocketAddrFormat(&tmpipdef->address);

            if (!ipaddr)
                goto cleanup;

            /* also part of CVE 2012-3411 - if the host's version of
             * dnsmasq doesn't have bind-dynamic, only allow listening on
             * private/local IP addresses (see RFC1918/RFC3484/RFC4193)
             */
            if (!dnsmasqCapsGet(caps, DNSMASQ_CAPS_BINDTODEVICE) &&
                !virSocketAddrIsPrivate(&tmpipdef->address)) {
                unsigned long version = dnsmasqCapsGetVersion(caps);

                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("Publicly routable address %s is prohibited. "
                                 "The version of dnsmasq on this host (%d.%d) "
                                 "doesn't support the bind-dynamic option or "
                                 "use SO_BINDTODEVICE on listening sockets, "
                                 "one of which is required for safe operation "
                                 "on a publicly routable subnet "
                                 "(see CVE-2012-3411). You must either "
                                 "upgrade dnsmasq, or use a private/local "
                                 "subnet range for this network "
                                 "(as described in RFC1918/RFC3484/RFC4193)."),
                               ipaddr, (int)version / 1000000,
                               (int)(version % 1000000) / 1000);
                VIR_FREE(ipaddr);
                goto cleanup;
            }
            virBufferAsprintf(&configbuf, "listen-address=%s\n", ipaddr);
            VIR_FREE(ipaddr);
        }
    }

    /* If this is an isolated network, set the default route option
     * (3) to be empty to avoid setting a default route that's
     * guaranteed to not work, and set no-resolv so that no dns
     * requests are forwarded on to the dns server listed in the
     * host's /etc/resolv.conf (since this could be used as a channel
     * to build a connection to the outside).
     * IPv6 RA always contains an implicit default route
     * via the sender's link-local address. The only thing we can do
     * is set the lifetime of this route to 0, i.e. disable it.
     */
    if (def->forward.type == VIR_NETWORK_FORWARD_NONE) {
        virBufferAddLit(&configbuf, "dhcp-option=3\n"
                        "no-resolv\n");
        if (dnsmasqCapsGet(caps, DNSMASQ_CAPS_RA_PARAM)) {
            /* interface=* (any), interval=0 (default), lifetime=0 (seconds) */
            virBufferAddLit(&configbuf, "ra-param=*,0,0\n");
        }
    }

    if (wantDNS) {
        for (i = 0; i < dns->ntxts; i++) {
            virBufferAsprintf(&configbuf, "txt-record=%s,%s\n",
                              dns->txts[i].name,
                              dns->txts[i].value);
        }

        for (i = 0; i < dns->nsrvs; i++) {
            /* service/protocol are required, and should have been validated
             * by the parser.
             */
            if (!dns->srvs[i].service) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Missing required 'service' "
                                 "attribute in SRV record of network '%s'"),
                               def->name);
                goto cleanup;
            }
            if (!dns->srvs[i].protocol) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Missing required 'service' "
                                 "attribute in SRV record of network '%s'"),
                               def->name);
                goto cleanup;
            }
            /* RFC2782 requires that service and protocol be preceded by
             * an underscore.
             */
            virBufferAsprintf(&configbuf, "srv-host=_%s._%s",
                              dns->srvs[i].service, dns->srvs[i].protocol);

            /* domain is optional - it defaults to the domain of this network */
            if (dns->srvs[i].domain)
                virBufferAsprintf(&configbuf, ".%s", dns->srvs[i].domain);

            /* If target is empty or ".", that means "the service is
             * decidedly not available at this domain" (RFC2782). In that
             * case, any port, priority, or weight is irrelevant.
             */
            if (dns->srvs[i].target && STRNEQ(dns->srvs[i].target, ".")) {

                virBufferAsprintf(&configbuf, ",%s", dns->srvs[i].target);
                /* port, priority, and weight are optional, but are
                 * identified by their position in the line. If an item is
                 * unspecified, but something later in the line *is*
                 * specified, we need to give the default value for the
                 * unspecified item. (According to the dnsmasq manpage,
                 * the default for port is 1).
                 */
                if (dns->srvs[i].port ||
                    dns->srvs[i].priority || dns->srvs[i].weight)
                    virBufferAsprintf(&configbuf, ",%d",
                                      dns->srvs[i].port ? dns->srvs[i].port : 1);
                if (dns->srvs[i].priority || dns->srvs[i].weight)
                    virBufferAsprintf(&configbuf, ",%d", dns->srvs[i].priority);
                if (dns->srvs[i].weight)
                    virBufferAsprintf(&configbuf, ",%d", dns->srvs[i].weight);
            }
            virBufferAddLit(&configbuf, "\n");
        }
    }

    /* Find the first dhcp for both IPv4 and IPv6 */
    for (i = 0, ipv4def = NULL, ipv6def = NULL, ipv6SLAAC = false;
         (ipdef = virNetworkDefGetIPByIndex(def, AF_UNSPEC, i));
         i++) {
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (ipv4def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("For IPv4, multiple DHCP definitions "
                                     "cannot be specified."));
                    goto cleanup;
                } else {
                    ipv4def = ipdef;
                }
            }
        }
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (!DNSMASQ_DHCPv6_SUPPORT(caps)) {
                    unsigned long version = dnsmasqCapsGetVersion(caps);
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("The version of dnsmasq on this host "
                                     "(%d.%d) doesn't adequately support "
                                     "IPv6 dhcp range or dhcp host "
                                     "specification. Version %d.%d or later "
                                     "is required."),
                                   (int)version / 1000000,
                                   (int)(version % 1000000) / 1000,
                                   DNSMASQ_DHCPv6_MAJOR_REQD,
                                   DNSMASQ_DHCPv6_MINOR_REQD);
                    goto cleanup;
                }
                if (ipv6def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                                   _("For IPv6, multiple DHCP definitions "
                                     "cannot be specified."));
                    goto cleanup;
                } else {
                    ipv6def = ipdef;
                }
            } else {
                ipv6SLAAC = true;
            }
        }
    }

    if (ipv6def && ipv6SLAAC) {
        VIR_WARN("For IPv6, when DHCP is specified for one address, then "
                 "state-full Router Advertising will occur.  The additional "
                 "IPv6 addresses specified require manually configured guest "
                 "network to work properly since both state-full (DHCP) "
                 "and state-less (SLAAC) addressing are not supported "
                 "on the same network interface.");
    }

    ipdef = ipv4def ? ipv4def : ipv6def;

    while (ipdef) {
        int prefix;

        prefix = virNetworkIPDefPrefix(ipdef);
        if (prefix < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("bridge '%s' has an invalid prefix"),
                           def->bridge);
            goto cleanup;
        }
        for (r = 0; r < ipdef->nranges; r++) {
            int thisRange;

            if (!(saddr = virSocketAddrFormat(&ipdef->ranges[r].start)) ||
                !(eaddr = virSocketAddrFormat(&ipdef->ranges[r].end)))
                goto cleanup;

            if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6)) {
               virBufferAsprintf(&configbuf, "dhcp-range=%s,%s,%d\n",
                                 saddr, eaddr, prefix);
            } else {
                /* IPv4 - dnsmasq requires a netmask rather than prefix */
                virSocketAddr netmask;
                g_autofree char *netmaskStr = NULL;

                if (virSocketAddrPrefixToNetmask(prefix, &netmask, AF_INET) < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   _("Failed to translate bridge '%s' "
                                     "prefix %d to netmask"),
                                   def->bridge, prefix);
                    goto cleanup;
                }

                if (!(netmaskStr = virSocketAddrFormat(&netmask)))
                    goto cleanup;
                virBufferAsprintf(&configbuf, "dhcp-range=%s,%s,%s\n",
                                  saddr, eaddr, netmaskStr);
            }

            VIR_FREE(saddr);
            VIR_FREE(eaddr);
            thisRange = virSocketAddrGetRange(&ipdef->ranges[r].start,
                                              &ipdef->ranges[r].end,
                                              &ipdef->address,
                                              virNetworkIPDefPrefix(ipdef));
            if (thisRange < 0)
                goto cleanup;
            nbleases += thisRange;
        }

        /*
         * For static-only DHCP, i.e. with no range but at least one
         * host element, we have to add a special --dhcp-range option
         * to enable the service in dnsmasq. (this is for dhcp-hosts=
         * support)
         */
        if (!ipdef->nranges && ipdef->nhosts) {
            char *bridgeaddr = virSocketAddrFormat(&ipdef->address);
            if (!bridgeaddr)
                goto cleanup;
            virBufferAsprintf(&configbuf, "dhcp-range=%s,static",
                              bridgeaddr);
            if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6))
                virBufferAsprintf(&configbuf, ",%d", prefix);
            virBufferAddLit(&configbuf, "\n");
            VIR_FREE(bridgeaddr);
        }

        if (networkBuildDnsmasqDhcpHostsList(dctx, ipdef) < 0)
            goto cleanup;

        /* Note: the following is IPv4 only */
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET)) {
            if (ipdef->nranges || ipdef->nhosts) {
                virBufferAddLit(&configbuf, "dhcp-no-override\n");
                virBufferAddLit(&configbuf, "dhcp-authoritative\n");
            }

            if (ipdef->tftproot) {
                virBufferAddLit(&configbuf, "enable-tftp\n");
                virBufferAsprintf(&configbuf, "tftp-root=%s\n", ipdef->tftproot);
            }

            if (ipdef->bootfile) {
                if (VIR_SOCKET_ADDR_VALID(&ipdef->bootserver)) {
                    char *bootserver = virSocketAddrFormat(&ipdef->bootserver);

                    if (!bootserver)
                        goto cleanup;
                    virBufferAsprintf(&configbuf, "dhcp-boot=%s%s%s\n",
                                      ipdef->bootfile, ",,", bootserver);
                    VIR_FREE(bootserver);
                } else {
                    virBufferAsprintf(&configbuf, "dhcp-boot=%s\n", ipdef->bootfile);
                }
            }
        }
        ipdef = (ipdef == ipv6def) ? NULL : ipv6def;
    }

    if (nbleases > 0)
        virBufferAsprintf(&configbuf, "dhcp-lease-max=%d\n", nbleases);

    /* this is done once per interface */
    if (networkBuildDnsmasqHostsList(dctx, dns) < 0)
        goto cleanup;

    /* Even if there are currently no static hosts, if we're
     * listening for DHCP, we should write a 0-length hosts
     * file to allow for runtime additions.
     */
    if (ipv4def || ipv6def)
        virBufferAsprintf(&configbuf, "dhcp-hostsfile=%s\n",
                          dctx->hostsfile->path);

    /* Likewise, always create this file and put it on the
     * commandline, to allow for runtime additions.
     */
    if (wantDNS) {
        virBufferAsprintf(&configbuf, "addn-hosts=%s\n",
                          dctx->addnhostsfile->path);
    }

    /* Configure DHCP to tell clients about the MTU. */
    if (def->mtu > 0)
        virBufferAsprintf(&configbuf, "dhcp-option=option:mtu,%d\n", def->mtu);

    /* Are we doing RA instead of radvd? */
    if (DNSMASQ_RA_SUPPORT(caps)) {
        if (ipv6def) {
            virBufferAddLit(&configbuf, "enable-ra\n");
        } else {
            for (i = 0;
                 (ipdef = virNetworkDefGetIPByIndex(def, AF_INET6, i));
                 i++) {
                if (!(ipdef->nranges || ipdef->nhosts)) {
                    char *bridgeaddr = virSocketAddrFormat(&ipdef->address);
                    if (!bridgeaddr)
                        goto cleanup;
                    virBufferAsprintf(&configbuf,
                                      "dhcp-range=%s,ra-only\n", bridgeaddr);
                    VIR_FREE(bridgeaddr);
                }
            }
        }
    }

    if (def->namespaceData) {
        networkDnsmasqXmlNsDefPtr dnsmasqxmlns = def->namespaceData;
        for (i = 0; i < dnsmasqxmlns->noptions; i++)
            virBufferAsprintf(&configbuf, "%s\n", dnsmasqxmlns->options[i]);
    }

    if (!(*configstr = virBufferContentAndReset(&configbuf)))
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(saddr);
    VIR_FREE(eaddr);
    virBufferFreeAndReset(&configbuf);
    return ret;
}


/* build the dnsmasq command line */
static int ATTRIBUTE_NONNULL(3)
networkBuildDhcpDaemonCommandLine(virNetworkDriverStatePtr driver,
                                  virNetworkObjPtr obj,
                                  virCommandPtr *cmdout,
                                  char *pidfile,
                                  dnsmasqContext *dctx)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    dnsmasqCapsPtr dnsmasq_caps = networkGetDnsmasqCaps(driver);
    virCommandPtr cmd = NULL;
    int ret = -1;
    char *configfile = NULL;
    char *configstr = NULL;
    char *leaseshelper_path = NULL;

    virNetworkObjSetDnsmasqPid(obj, -1);

    if (networkDnsmasqConfContents(obj, pidfile, &configstr,
                                   dctx, dnsmasq_caps) < 0)
        goto cleanup;
    if (!configstr)
        goto cleanup;

    /* construct the filename */
    if (!(configfile = networkDnsmasqConfigFileName(driver, def->name)))
        goto cleanup;

    /* Write the file */
    if (virFileWriteStr(configfile, configstr, 0600) < 0) {
        virReportSystemError(errno,
                             _("couldn't write dnsmasq config file '%s'"),
                             configfile);
        goto cleanup;
    }

    /* This helper is used to create custom leases file for libvirt */
    if (!(leaseshelper_path = virFileFindResource("libvirt_leaseshelper",
                                                  abs_top_builddir "/src",
                                                  LIBEXECDIR)))
        goto cleanup;

    cmd = virCommandNew(dnsmasqCapsGetBinaryPath(dnsmasq_caps));
    virCommandAddArgFormat(cmd, "--conf-file=%s", configfile);
    /* Libvirt gains full control of leases database */
    virCommandAddArgFormat(cmd, "--leasefile-ro");
    virCommandAddArgFormat(cmd, "--dhcp-script=%s", leaseshelper_path);
    virCommandAddEnvPair(cmd, "VIR_BRIDGE_NAME", def->bridge);

    *cmdout = cmd;
    ret = 0;
 cleanup:
    virObjectUnref(dnsmasq_caps);
    VIR_FREE(configfile);
    VIR_FREE(configstr);
    VIR_FREE(leaseshelper_path);
    return ret;
}


static int
networkStartDhcpDaemon(virNetworkDriverStatePtr driver,
                       virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virNetworkIPDefPtr ipdef;
    size_t i;
    bool needDnsmasq = false;
    virCommandPtr cmd = NULL;
    char *pidfile = NULL;
    pid_t dnsmasqPid;
    int ret = -1;
    dnsmasqContext *dctx = NULL;

    /* see if there are any IP addresses that need a dhcp server */
    i = 0;
    while ((ipdef = virNetworkDefGetIPByIndex(def, AF_UNSPEC, i))) {
        i++;
        if (ipdef->nranges || ipdef->nhosts)
            needDnsmasq = true;
    }

    if (i == 0) {
        /* no IP addresses at all, so we don't need to run */
        ret = 0;
        goto cleanup;
    }

    if (!needDnsmasq && def->dns.enable == VIR_TRISTATE_BOOL_NO) {
        /* no DHCP services needed, and user disabled DNS service */
        ret = 0;
        goto cleanup;
    }

    if (virFileMakePath(driver->pidDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->pidDir);
        goto cleanup;
    }

    if (!(pidfile = virPidFileBuildPath(driver->pidDir, def->name)))
        goto cleanup;

    if (virFileMakePath(driver->dnsmasqStateDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->dnsmasqStateDir);
        goto cleanup;
    }

    dctx = dnsmasqContextNew(def->name, driver->dnsmasqStateDir);
    if (dctx == NULL)
        goto cleanup;

    if (networkDnsmasqCapsRefresh(driver) < 0)
        goto cleanup;

    ret = networkBuildDhcpDaemonCommandLine(driver, obj, &cmd, pidfile, dctx);
    if (ret < 0)
        goto cleanup;

    ret = dnsmasqSave(dctx);
    if (ret < 0)
        goto cleanup;

    ret = virCommandRun(cmd, NULL);
    if (ret < 0)
        goto cleanup;

    /*
     * There really is no race here - when dnsmasq daemonizes, its
     * leader process stays around until its child has actually
     * written its pidfile. So by time virCommandRun exits it has
     * waitpid'd and guaranteed the proess has started and written a
     * pid
     */

    ret = virPidFileRead(driver->pidDir, def->name, &dnsmasqPid);
    if (ret < 0)
        goto cleanup;
    virNetworkObjSetDnsmasqPid(obj, dnsmasqPid);

    ret = 0;
 cleanup:
    VIR_FREE(pidfile);
    virCommandFree(cmd);
    dnsmasqContextFree(dctx);
    return ret;
}


/* networkRefreshDhcpDaemon:
 *  Update dnsmasq config files, then send a SIGHUP so that it rereads
 *  them.   This only works for the dhcp-hostsfile and the
 *  addn-hosts file.
 *
 *  Returns 0 on success, -1 on failure.
 */
static int
networkRefreshDhcpDaemon(virNetworkDriverStatePtr driver,
                         virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    int ret = -1;
    size_t i;
    pid_t dnsmasqPid;
    virNetworkIPDefPtr ipdef, ipv4def, ipv6def;
    dnsmasqContext *dctx = NULL;

    /* if no IP addresses specified, nothing to do */
    if (!virNetworkDefGetIPByIndex(def, AF_UNSPEC, 0))
        return 0;

    /* if there's no running dnsmasq, just start it */
    dnsmasqPid = virNetworkObjGetDnsmasqPid(obj);
    if (dnsmasqPid <= 0 || (kill(dnsmasqPid, 0) < 0))
        return networkStartDhcpDaemon(driver, obj);

    VIR_INFO("Refreshing dnsmasq for network %s", def->bridge);
    if (!(dctx = dnsmasqContextNew(def->name,
                                   driver->dnsmasqStateDir))) {
        goto cleanup;
    }

    /* Look for first IPv4 address that has dhcp defined.
     * We only support dhcp-host config on one IPv4 subnetwork
     * and on one IPv6 subnetwork.
     */
    ipv4def = NULL;
    for (i = 0;
         (ipdef = virNetworkDefGetIPByIndex(def, AF_INET, i));
         i++) {
        if (!ipv4def && (ipdef->nranges || ipdef->nhosts))
            ipv4def = ipdef;
    }

    ipv6def = NULL;
    for (i = 0;
         (ipdef = virNetworkDefGetIPByIndex(def, AF_INET6, i));
         i++) {
        if (!ipv6def && (ipdef->nranges || ipdef->nhosts))
            ipv6def = ipdef;
    }

    if (ipv4def && (networkBuildDnsmasqDhcpHostsList(dctx, ipv4def) < 0))
        goto cleanup;

    if (ipv6def && (networkBuildDnsmasqDhcpHostsList(dctx, ipv6def) < 0))
        goto cleanup;

    if (networkBuildDnsmasqHostsList(dctx, &def->dns) < 0)
        goto cleanup;

    if ((ret = dnsmasqSave(dctx)) < 0)
        goto cleanup;

    dnsmasqPid = virNetworkObjGetDnsmasqPid(obj);
    ret = kill(dnsmasqPid, SIGHUP);
 cleanup:
    dnsmasqContextFree(dctx);
    return ret;
}


/* networkRestartDhcpDaemon:
 *
 * kill and restart dnsmasq, in order to update any config that is on
 * the dnsmasq commandline (and any placed in separate config files).
 *
 *  Returns 0 on success, -1 on failure.
 */
static int
networkRestartDhcpDaemon(virNetworkDriverStatePtr driver,
                         virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    pid_t dnsmasqPid = virNetworkObjGetDnsmasqPid(obj);

    /* if there is a running dnsmasq, kill it */
    if (dnsmasqPid > 0) {
        networkKillDaemon(dnsmasqPid, "dnsmasq", def->name);
        virNetworkObjSetDnsmasqPid(obj, -1);
    }
    /* now start dnsmasq if it should be started */
    return networkStartDhcpDaemon(driver, obj);
}


static char radvd1[] = "  AdvOtherConfigFlag off;\n\n";
static char radvd2[] = "    AdvAutonomous off;\n";
static char radvd3[] = "    AdvOnLink on;\n"
                       "    AdvAutonomous on;\n"
                       "    AdvRouterAddr off;\n";

static int
networkRadvdConfContents(virNetworkObjPtr obj,
                         char **configstr)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virBuffer configbuf = VIR_BUFFER_INITIALIZER;
    int ret = -1;
    size_t i;
    virNetworkIPDefPtr ipdef;
    bool v6present = false, dhcp6 = false;

    *configstr = NULL;

    /* Check if DHCPv6 is needed */
    for (i = 0; (ipdef = virNetworkDefGetIPByIndex(def, AF_INET6, i)); i++) {
        v6present = true;
        if (ipdef->nranges || ipdef->nhosts) {
            dhcp6 = true;
            break;
        }
    }

    /* If there are no IPv6 addresses, then we are done */
    if (!v6present) {
        ret = 0;
        goto cleanup;
    }

    /* create radvd config file appropriate for this network;
     * IgnoreIfMissing allows radvd to start even when the bridge is down
     */
    virBufferAsprintf(&configbuf, "interface %s\n"
                      "{\n"
                      "  AdvSendAdvert on;\n"
                      "  IgnoreIfMissing on;\n"
                      "  AdvManagedFlag %s;\n"
                      "%s",
                      def->bridge,
                      dhcp6 ? "on" : "off",
                      dhcp6 ? "\n" : radvd1);

    /* add a section for each IPv6 address in the config */
    for (i = 0; (ipdef = virNetworkDefGetIPByIndex(def, AF_INET6, i)); i++) {
        int prefix;
        char *netaddr;

        prefix = virNetworkIPDefPrefix(ipdef);
        if (prefix < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("bridge '%s' has an invalid prefix"),
                           def->bridge);
            goto cleanup;
        }
        if (!(netaddr = virSocketAddrFormat(&ipdef->address)))
            goto cleanup;
        virBufferAsprintf(&configbuf,
                          "  prefix %s/%d\n"
                          "  {\n%s  };\n",
                          netaddr, prefix,
                          dhcp6 ? radvd2 : radvd3);
        VIR_FREE(netaddr);
    }

    virBufferAddLit(&configbuf, "};\n");

    *configstr = virBufferContentAndReset(&configbuf);

    ret = 0;
 cleanup:
    virBufferFreeAndReset(&configbuf);
    return ret;
}


/* write file and return its name (which must be freed by caller) */
static int
networkRadvdConfWrite(virNetworkDriverStatePtr driver,
                      virNetworkObjPtr obj,
                      char **configFile)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    int ret = -1;
    char *configStr = NULL;
    char *myConfigFile = NULL;

    if (!configFile)
        configFile = &myConfigFile;

    *configFile = NULL;

    if (networkRadvdConfContents(obj, &configStr) < 0)
        goto cleanup;

    if (!configStr) {
        ret = 0;
        goto cleanup;
    }

    /* construct the filename */
    if (!(*configFile = networkRadvdConfigFileName(driver, def->name)))
        goto cleanup;
    /* write the file */
    if (virFileWriteStr(*configFile, configStr, 0600) < 0) {
        virReportSystemError(errno,
                             _("couldn't write radvd config file '%s'"),
                             *configFile);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(configStr);
    VIR_FREE(myConfigFile);
    return ret;
}


static int
networkStartRadvd(virNetworkDriverStatePtr driver,
                  virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    dnsmasqCapsPtr dnsmasq_caps = networkGetDnsmasqCaps(driver);
    pid_t radvdPid;
    char *pidfile = NULL;
    char *radvdpidbase = NULL;
    char *configfile = NULL;
    virCommandPtr cmd = NULL;
    int ret = -1;

    virNetworkObjSetRadvdPid(obj, -1);

    /* Is dnsmasq handling RA? */
    if (DNSMASQ_RA_SUPPORT(dnsmasq_caps)) {
        ret = 0;
        goto cleanup;
    }

    if (!virNetworkDefGetIPByIndex(def, AF_INET6, 0)) {
        /* no IPv6 addresses, so we don't need to run radvd */
        ret = 0;
        goto cleanup;
    }

    if (!virFileIsExecutable(RADVD)) {
        virReportSystemError(errno,
                             _("Cannot find %s - "
                               "Possibly the package isn't installed"),
                             RADVD);
        goto cleanup;
    }

    if (virFileMakePath(driver->pidDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->pidDir);
        goto cleanup;
    }
    if (virFileMakePath(driver->radvdStateDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create directory %s"),
                             driver->radvdStateDir);
        goto cleanup;
    }

    /* construct pidfile name */
    if (!(radvdpidbase = networkRadvdPidfileBasename(def->name)))
        goto cleanup;
    if (!(pidfile = virPidFileBuildPath(driver->pidDir, radvdpidbase)))
        goto cleanup;

    if (networkRadvdConfWrite(driver, obj, &configfile) < 0)
        goto cleanup;

    /* prevent radvd from daemonizing itself with "--debug 1", and use
     * a dummy pidfile name - virCommand will create the pidfile we
     * want to use (this is necessary because radvd's internal
     * daemonization and pidfile creation causes a race, and the
     * virPidFileRead() below will fail if we use them).
     * Unfortunately, it isn't possible to tell radvd to not create
     * its own pidfile, so we just let it do so, with a slightly
     * different name. Unused, but harmless.
     */
    cmd = virCommandNewArgList(RADVD, "--debug", "1",
                               "--config", configfile,
                               "--pidfile", NULL);
    virCommandAddArgFormat(cmd, "%s-bin", pidfile);

    virCommandSetPidFile(cmd, pidfile);
    virCommandDaemonize(cmd);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (virPidFileRead(driver->pidDir, radvdpidbase, &radvdPid) < 0)
        goto cleanup;
    virNetworkObjSetRadvdPid(obj, radvdPid);

    ret = 0;
 cleanup:
    virObjectUnref(dnsmasq_caps);
    virCommandFree(cmd);
    VIR_FREE(configfile);
    VIR_FREE(radvdpidbase);
    VIR_FREE(pidfile);
    return ret;
}


static int
networkRefreshRadvd(virNetworkDriverStatePtr driver,
                    virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    dnsmasqCapsPtr dnsmasq_caps = networkGetDnsmasqCaps(driver);
    char *radvdpidbase;
    pid_t radvdPid;

    /* Is dnsmasq handling RA? */
    if (DNSMASQ_RA_SUPPORT(dnsmasq_caps)) {
        virObjectUnref(dnsmasq_caps);
        radvdPid = virNetworkObjGetRadvdPid(obj);
        if (radvdPid <= 0)
            return 0;
        /* radvd should not be running but in case it is */
        if ((networkKillDaemon(radvdPid, "radvd", def->name) >= 0) &&
            ((radvdpidbase = networkRadvdPidfileBasename(def->name))
             != NULL)) {
            virPidFileDelete(driver->pidDir, radvdpidbase);
            VIR_FREE(radvdpidbase);
        }
        virNetworkObjSetRadvdPid(obj, -1);
        return 0;
    }
    virObjectUnref(dnsmasq_caps);

    /* if there's no running radvd, just start it */
    radvdPid = virNetworkObjGetRadvdPid(obj);
    if (radvdPid <= 0 || (kill(radvdPid, 0) < 0))
        return networkStartRadvd(driver, obj);

    if (!virNetworkDefGetIPByIndex(def, AF_INET6, 0)) {
        /* no IPv6 addresses, so we don't need to run radvd */
        return 0;
    }

    if (networkRadvdConfWrite(driver, obj, NULL) < 0)
        return -1;

    return kill(radvdPid, SIGHUP);
}


#if 0
/* currently unused, so it causes a build error unless we #if it out */
static int
networkRestartRadvd(virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    char *radvdpidbase;
    pid_t radvdPid = virNeworkObjGetRadvdPid(obj);

    /* if there is a running radvd, kill it */
    if (radvdPid > 0) {
        /* essentially ignore errors from the following two functions,
         * since there's really no better recovery to be done than to
         * just push ahead (and that may be exactly what's needed).
         */
        if ((networkKillDaemon(radvdPid, "radvd", def->name) >= 0) &&
            ((radvdpidbase = networkRadvdPidfileBasename(def->name))
             != NULL)) {
            virPidFileDelete(driver->pidDir, radvdpidbase);
            VIR_FREE(radvdpidbase);
        }
        virNetworkObjSetRadvdPid(obj, -1);
    }
    /* now start radvd if it should be started */
    return networkStartRadvd(obj);
}
#endif /* #if 0 */


static int
networkRefreshDaemonsHelper(virNetworkObjPtr obj,
                            void *opaque)
{
    virNetworkDefPtr def;
    virNetworkDriverStatePtr driver = opaque;

    virObjectLock(obj);
    def = virNetworkObjGetDef(obj);
    if (virNetworkObjIsActive(obj)) {
        switch ((virNetworkForwardType) def->forward.type) {
        case VIR_NETWORK_FORWARD_NONE:
        case VIR_NETWORK_FORWARD_NAT:
        case VIR_NETWORK_FORWARD_ROUTE:
        case VIR_NETWORK_FORWARD_OPEN:
            /* Only the three L3 network types that are configured by
             * libvirt will have a dnsmasq or radvd daemon associated
             * with them.  Here we send a SIGHUP to an existing
             * dnsmasq and/or radvd, or restart them if they've
             * disappeared.
             */
            networkRefreshDhcpDaemon(driver, obj);
            networkRefreshRadvd(driver, obj);
            break;

        case VIR_NETWORK_FORWARD_BRIDGE:
        case VIR_NETWORK_FORWARD_PRIVATE:
        case VIR_NETWORK_FORWARD_VEPA:
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
        case VIR_NETWORK_FORWARD_HOSTDEV:
            break;

        case VIR_NETWORK_FORWARD_LAST:
        default:
            virReportEnumRangeError(virNetworkForwardType, def->forward.type);
            goto cleanup;
        }
    }

 cleanup:
    virObjectUnlock(obj);
    return 0;
}


/* SIGHUP/restart any dnsmasq or radvd daemons.
 * This should be called when libvirtd is restarted.
 */
static void
networkRefreshDaemons(virNetworkDriverStatePtr driver)
{
    VIR_INFO("Refreshing network daemons");
    virNetworkObjListForEach(driver->networks,
                             networkRefreshDaemonsHelper,
                             driver);
}


static int
networkReloadFirewallRulesHelper(virNetworkObjPtr obj,
                                 void *opaque G_GNUC_UNUSED)
{
    virNetworkDefPtr def;

    virObjectLock(obj);
    def = virNetworkObjGetDef(obj);
    if (virNetworkObjIsActive(obj)) {
        switch ((virNetworkForwardType) def->forward.type) {
        case VIR_NETWORK_FORWARD_NONE:
        case VIR_NETWORK_FORWARD_NAT:
        case VIR_NETWORK_FORWARD_ROUTE:
            /* Only three of the L3 network types that are configured by
             * libvirt need to have iptables rules reloaded. The 4th L3
             * network type, forward='open', doesn't need this because it
             * has no iptables rules.
             */
            networkRemoveFirewallRules(def);
            ignore_value(networkAddFirewallRules(def));
            break;

        case VIR_NETWORK_FORWARD_OPEN:
        case VIR_NETWORK_FORWARD_BRIDGE:
        case VIR_NETWORK_FORWARD_PRIVATE:
        case VIR_NETWORK_FORWARD_VEPA:
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
        case VIR_NETWORK_FORWARD_HOSTDEV:
            break;

        case VIR_NETWORK_FORWARD_LAST:
        default:
            virReportEnumRangeError(virNetworkForwardType, def->forward.type);
            goto cleanup;
        }
    }

 cleanup:
    virObjectUnlock(obj);
    return 0;
}


static void
networkReloadFirewallRules(virNetworkDriverStatePtr driver, bool startup)
{
    VIR_INFO("Reloading iptables rules");
    /* Ideally we'd not even register the driver when unprivilegd
     * but until we untangle the virt driver that's not viable */
    if (!driver->privileged)
        return;
    networkPreReloadFirewallRules(driver, startup);
    virNetworkObjListForEach(driver->networks,
                             networkReloadFirewallRulesHelper,
                             NULL);
    networkPostReloadFirewallRules(startup);
}


/* Enable IP Forwarding. Return 0 for success, -1 for failure. */
static int
networkEnableIPForwarding(bool enableIPv4,
                          bool enableIPv6)
{
    int ret = 0;
#ifdef HAVE_SYSCTLBYNAME
    int enabled = 1;
    if (enableIPv4)
        ret = sysctlbyname("net.inet.ip.forwarding", NULL, 0,
                           &enabled, sizeof(enabled));
    if (enableIPv6 && ret == 0)
        ret = sysctlbyname("net.inet6.ip6.forwarding", NULL, 0,
                           &enabled, sizeof(enabled));
#else
    if (enableIPv4)
        ret = virFileWriteStr(SYSCTL_PATH "/net/ipv4/ip_forward", "1\n", 0);
    if (enableIPv6 && ret == 0)
        ret = virFileWriteStr(SYSCTL_PATH "/net/ipv6/conf/all/forwarding", "1\n", 0);

#endif
    return ret;
}


static int
networkSetIPv6Sysctls(virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    char *field = NULL;
    int ret = -1;
    bool enableIPv6 = !!virNetworkDefGetIPByIndex(def, AF_INET6, 0);

    /* set disable_ipv6 if there are no ipv6 addresses defined for the
     * network. But also unset it if there *are* ipv6 addresses, as we
     * can't be sure of its default value.
     */
    field = g_strdup_printf(SYSCTL_PATH "/net/ipv6/conf/%s/disable_ipv6",
                            def->bridge);

    if (access(field, W_OK) < 0 && errno == ENOENT) {
        if (!enableIPv6)
            VIR_DEBUG("ipv6 appears to already be disabled on %s",
                      def->bridge);
        ret = 0;
        goto cleanup;
    }

    if (virFileWriteStr(field, enableIPv6 ? "0" : "1", 0) < 0) {
        virReportSystemError(errno,
                             _("cannot write to %s to enable/disable IPv6 "
                               "on bridge %s"), field, def->bridge);
        goto cleanup;
    }
    VIR_FREE(field);

    /* The rest of the ipv6 sysctl tunables should always be set the
     * same, whether or not we're using ipv6 on this bridge.
     */

    /* Prevent guests from hijacking the host network by sending out
     * their own router advertisements.
     */
    field = g_strdup_printf(SYSCTL_PATH "/net/ipv6/conf/%s/accept_ra",
                            def->bridge);

    if (virFileWriteStr(field, "0", 0) < 0) {
        virReportSystemError(errno,
                             _("cannot disable %s"), field);
        goto cleanup;
    }
    VIR_FREE(field);

    /* All interfaces used as a gateway (which is what this is, by
     * definition), must always have autoconf=0.
     */
    field = g_strdup_printf(SYSCTL_PATH "/net/ipv6/conf/%s/autoconf", def->bridge);

    if (virFileWriteStr(field, "0", 0) < 0) {
        virReportSystemError(errno,
                             _("cannot disable %s"), field);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    VIR_FREE(field);
    return ret;
}


/* add an IP address to a bridge */
static int
networkAddAddrToBridge(virNetworkObjPtr obj,
                       virNetworkIPDefPtr ipdef)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    int prefix = virNetworkIPDefPrefix(ipdef);

    if (prefix < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("bridge '%s' has an invalid netmask or IP address"),
                       def->bridge);
        return -1;
    }

    if (virNetDevIPAddrAdd(def->bridge, &ipdef->address, NULL, prefix) < 0)
        return -1;

    return 0;
}


static int
networkStartHandleMACTableManagerMode(virNetworkObjPtr obj,
                                      const char *macTapIfName)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    const char *brname = def->bridge;

    if (brname &&
        def->macTableManager == VIR_NETWORK_BRIDGE_MAC_TABLE_MANAGER_LIBVIRT) {
        if (virNetDevBridgeSetVlanFiltering(brname, true) < 0)
            return -1;
        if (macTapIfName) {
            if (virNetDevBridgePortSetLearning(brname, macTapIfName, false) < 0)
                return -1;
            if (virNetDevBridgePortSetUnicastFlood(brname, macTapIfName, false) < 0)
                return -1;
        }
    }
    return 0;
}


/* add an IP (static) route to a bridge */
static int
networkAddRouteToBridge(virNetworkObjPtr obj,
                        virNetDevIPRoutePtr routedef)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    int prefix = virNetDevIPRouteGetPrefix(routedef);
    unsigned int metric = virNetDevIPRouteGetMetric(routedef);
    virSocketAddrPtr addr = virNetDevIPRouteGetAddress(routedef);
    virSocketAddrPtr gateway = virNetDevIPRouteGetGateway(routedef);

    if (prefix < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' has an invalid netmask "
                         "or IP address in route definition"),
                       def->name);
        return -1;
    }

    if (virNetDevIPRouteAdd(def->bridge, addr, prefix, gateway, metric) < 0)
        return -1;

    return 0;
}

static int
networkWaitDadFinish(virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virNetworkIPDefPtr ipdef;
    virSocketAddrPtr *addrs = NULL, addr = NULL;
    size_t naddrs = 0;
    int ret = -1;

    VIR_DEBUG("Begin waiting for IPv6 DAD on network %s", def->name);

    while ((ipdef = virNetworkDefGetIPByIndex(def, AF_INET6, naddrs))) {
        addr = &ipdef->address;
        if (VIR_APPEND_ELEMENT_COPY(addrs, naddrs, addr) < 0)
            goto cleanup;
    }

    ret = (naddrs == 0) ? 0 : virNetDevIPWaitDadFinish(addrs, naddrs);

 cleanup:
    VIR_FREE(addrs);
    VIR_DEBUG("Finished waiting for IPv6 DAD on network %s with status %d",
              def->name, ret);
    return ret;
}


static int
networkStartNetworkVirtual(virNetworkDriverStatePtr driver,
                           virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    size_t i;
    bool v4present = false, v6present = false;
    virErrorPtr save_err = NULL;
    virNetworkIPDefPtr ipdef;
    virNetDevIPRoutePtr routedef;
    char *macTapIfName = NULL;
    virMacMapPtr macmap;
    char *macMapFile = NULL;
    int tapfd = -1;
    bool dnsmasqStarted = false;
    bool devOnline = false;
    bool firewalRulesAdded = false;

    /* Check to see if any network IP collides with an existing route */
    if (networkCheckRouteCollision(def) < 0)
        return -1;

    /* Create and configure the bridge device */
    if (!def->bridge) {
        /* bridge name can only be empty if the config files were
         * edited directly. Otherwise networkValidate() (called after
         * parsing the XML from networkCreateXML() and
         * networkDefine()) guarantees we will have a valid bridge
         * name before this point. Since hand editing of the config
         * files is explicitly prohibited we can, with clear
         * conscience, log an error and fail at this point.
         */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' has no bridge name defined"),
                       def->name);
        return -1;
    }
    if (virNetDevBridgeCreate(def->bridge, &def->mac) < 0)
        return -1;

    if (def->mac_specified) {
        /* To set a mac for the bridge, we need to define a dummy tap
         * device, set its mac, then attach it to the bridge. As long
         * as its mac address is lower than any other interface that
         * gets attached, the bridge will always maintain this mac
         * address.
         */
        macTapIfName = networkBridgeDummyNicName(def->bridge);
        if (!macTapIfName)
            goto error;
        /* Keep tun fd open and interface up to allow for IPv6 DAD to happen */
        if (virNetDevTapCreateInBridgePort(def->bridge,
                                           &macTapIfName, &def->mac,
                                           NULL, NULL, &tapfd, 1, NULL, NULL,
                                           NULL, def->mtu, NULL,
                                           VIR_NETDEV_TAP_CREATE_USE_MAC_FOR_BRIDGE |
                                           VIR_NETDEV_TAP_CREATE_IFUP |
                                           VIR_NETDEV_TAP_CREATE_PERSIST) < 0) {
            VIR_FREE(macTapIfName);
            goto error;
        }
    }

    if (!(macMapFile = virMacMapFileName(driver->dnsmasqStateDir,
                                         def->bridge)) ||
        !(macmap = virMacMapNew(macMapFile)))
        goto error;

    virNetworkObjSetMacMap(obj, macmap);
    macmap = NULL;

    /* Set bridge options */

    /* delay is configured in seconds, but virNetDevBridgeSetSTPDelay
     * expects milliseconds
     */
    if (virNetDevBridgeSetSTPDelay(def->bridge, def->delay * 1000) < 0)
        goto error;

    if (virNetDevBridgeSetSTP(def->bridge, def->stp ? true : false) < 0)
        goto error;

    /* Disable IPv6 on the bridge if there are no IPv6 addresses
     * defined, and set other IPv6 sysctl tunables appropriately.
     */
    if (networkSetIPv6Sysctls(obj) < 0)
        goto error;

    /* Add "once per network" rules */
    if (def->forward.type != VIR_NETWORK_FORWARD_OPEN &&
        networkAddFirewallRules(def) < 0)
        goto error;

    firewalRulesAdded = true;

    for (i = 0; (ipdef = virNetworkDefGetIPByIndex(def, AF_UNSPEC, i)); i++) {
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET))
            v4present = true;
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6))
            v6present = true;

        /* Add the IP address/netmask to the bridge */
        if (networkAddAddrToBridge(obj, ipdef) < 0)
            goto error;
    }

    if (networkStartHandleMACTableManagerMode(obj, macTapIfName) < 0)
        goto error;

    /* Bring up the bridge interface */
    if (virNetDevSetOnline(def->bridge, 1) < 0)
        goto error;

    devOnline = true;

    for (i = 0; i < def->nroutes; i++) {
        virSocketAddrPtr gateway = NULL;

        routedef = def->routes[i];
        gateway = virNetDevIPRouteGetGateway(routedef);

        /* Add the IP route to the bridge */
        /* ignore errors, error msg will be generated */
        /* but libvirt will not know and net-destroy will work. */
        if (VIR_SOCKET_ADDR_VALID(gateway)) {
            if (networkAddRouteToBridge(obj, routedef) < 0) {
                /* an error occurred adding the static route */
                continue; /* for now, do nothing */
            }
        }
    }

    /* If forward.type != NONE, turn on global IP forwarding */
    if (def->forward.type != VIR_NETWORK_FORWARD_NONE) {
        if (v6present && !virNetDevIPCheckIPv6Forwarding())
            goto error; /* Precise error message already provided */

        if (networkEnableIPForwarding(v4present, v6present) < 0) {
            virReportSystemError(errno, "%s",
                                 _("failed to enable IP forwarding"));
            goto error;
        }
    }


    /* start dnsmasq if there are any IP addresses (v4 or v6) */
    if ((v4present || v6present) &&
        networkStartDhcpDaemon(driver, obj) < 0)
        goto error;

    dnsmasqStarted = true;

    /* start radvd if there are any ipv6 addresses */
    if (v6present && networkStartRadvd(driver, obj) < 0)
        goto error;

    /* dnsmasq does not wait for DAD to complete before daemonizing,
     * so we need to wait for it ourselves.
     */
    if (v6present && networkWaitDadFinish(obj) < 0)
        goto error;

    /* DAD has finished, dnsmasq is now bound to the
     * bridge's IPv6 address, so we can set the dummy tun down.
     */
    if (tapfd >= 0) {
        if (virNetDevSetOnline(macTapIfName, false) < 0)
            goto error;
        VIR_FORCE_CLOSE(tapfd);
    }

    if (virNetDevBandwidthSet(def->bridge, def->bandwidth, true, true) < 0)
        goto error;

    VIR_FREE(macTapIfName);
    VIR_FREE(macMapFile);

    return 0;

 error:
    virErrorPreserveLast(&save_err);
    if (def->bandwidth)
       virNetDevBandwidthClear(def->bridge);

    if (dnsmasqStarted) {
        pid_t dnsmasqPid = virNetworkObjGetDnsmasqPid(obj);
        kill(dnsmasqPid, SIGTERM);
        virNetworkObjSetDnsmasqPid(obj, -1);
    }

    if (devOnline)
        ignore_value(virNetDevSetOnline(def->bridge, 0));

    if (firewalRulesAdded &&
        def->forward.type != VIR_NETWORK_FORWARD_OPEN)
        networkRemoveFirewallRules(def);

    if (macTapIfName) {
        VIR_FORCE_CLOSE(tapfd);
        ignore_value(virNetDevTapDelete(macTapIfName, NULL));
        VIR_FREE(macTapIfName);
    }
    virNetworkObjUnrefMacMap(obj);
    VIR_FREE(macMapFile);

    ignore_value(virNetDevBridgeDelete(def->bridge));

    virErrorRestore(&save_err);
    /* coverity[leaked_handle] - 'tapfd' is not leaked */
    return -1;
}


static int
networkShutdownNetworkVirtual(virNetworkDriverStatePtr driver,
                              virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    pid_t radvdPid;
    pid_t dnsmasqPid;

    if (def->bandwidth)
        virNetDevBandwidthClear(def->bridge);

    virNetworkObjUnrefMacMap(obj);

    radvdPid = virNetworkObjGetRadvdPid(obj);
    if (radvdPid > 0) {
        char *radvdpidbase;

        kill(radvdPid, SIGTERM);
        /* attempt to delete the pidfile we created */
        if ((radvdpidbase = networkRadvdPidfileBasename(def->name))) {
            virPidFileDelete(driver->pidDir, radvdpidbase);
            VIR_FREE(radvdpidbase);
        }
    }

    dnsmasqPid = virNetworkObjGetDnsmasqPid(obj);
    if (dnsmasqPid > 0)
        kill(dnsmasqPid, SIGTERM);

    if (def->mac_specified) {
        char *macTapIfName = networkBridgeDummyNicName(def->bridge);
        if (macTapIfName) {
            ignore_value(virNetDevTapDelete(macTapIfName, NULL));
            VIR_FREE(macTapIfName);
        }
    }

    ignore_value(virNetDevSetOnline(def->bridge, 0));

    if (def->forward.type != VIR_NETWORK_FORWARD_OPEN)
        networkRemoveFirewallRules(def);

    ignore_value(virNetDevBridgeDelete(def->bridge));

    /* See if its still alive and really really kill it */
    dnsmasqPid = virNetworkObjGetDnsmasqPid(obj);
    if (dnsmasqPid > 0 &&
        (kill(dnsmasqPid, 0) == 0))
        kill(dnsmasqPid, SIGKILL);
    virNetworkObjSetDnsmasqPid(obj, -1);

    radvdPid = virNetworkObjGetRadvdPid(obj);
    if (radvdPid > 0 &&
        (kill(radvdPid, 0) == 0))
        kill(radvdPid, SIGKILL);
    virNetworkObjSetRadvdPid(obj, -1);

    return 0;
}


static int
networkStartNetworkBridge(virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);

    /* put anything here that needs to be done each time a network of
     * type BRIDGE, is started. On failure, undo anything you've done,
     * and return -1. On success return 0.
     */
    if (virNetDevBandwidthSet(def->bridge, def->bandwidth, true, true) < 0)
        goto error;

    if (networkStartHandleMACTableManagerMode(obj, NULL) < 0)
        goto error;

    return 0;

 error:
    if (def->bandwidth)
       virNetDevBandwidthClear(def->bridge);
    return -1;
}


static int
networkShutdownNetworkBridge(virNetworkObjPtr obj G_GNUC_UNUSED)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);

    /* put anything here that needs to be done each time a network of
     * type BRIDGE is shutdown. On failure, undo anything you've done,
     * and return -1. On success return 0.
     */
    if (def->bandwidth)
       virNetDevBandwidthClear(def->bridge);

    return 0;
}


/* networkCreateInterfacePool:
 * @netdef: the original NetDef from the network
 *
 * Creates an implicit interface pool of VF's when a PF dev is given
 */
static int
networkCreateInterfacePool(virNetworkDefPtr netdef)
{
    size_t numVirtFns = 0;
    unsigned int maxVirtFns = 0;
    char **vfNames = NULL;
    virPCIDeviceAddressPtr *virtFns;

    int ret = -1;
    size_t i;

    if (netdef->forward.npfs == 0 || netdef->forward.nifs > 0)
       return 0;

    if ((virNetDevGetVirtualFunctions(netdef->forward.pfs->dev, &vfNames,
                                      &virtFns, &numVirtFns, &maxVirtFns)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get Virtual functions on %s"),
                       netdef->forward.pfs->dev);
        goto cleanup;
    }

    if (VIR_ALLOC_N(netdef->forward.ifs, numVirtFns) < 0)
        goto cleanup;

    for (i = 0; i < numVirtFns; i++) {
        virPCIDeviceAddressPtr thisVirtFn = virtFns[i];
        const char *thisName = vfNames[i];
        virNetworkForwardIfDefPtr thisIf
            = &netdef->forward.ifs[netdef->forward.nifs];

        switch ((virNetworkForwardType) netdef->forward.type) {
        case VIR_NETWORK_FORWARD_BRIDGE:
        case VIR_NETWORK_FORWARD_PRIVATE:
        case VIR_NETWORK_FORWARD_VEPA:
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
            if (thisName) {
                thisIf->device.dev = g_strdup(thisName);
                thisIf->type = VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV;
                netdef->forward.nifs++;
            } else {
                VIR_WARN("VF %zu of SRIOV PF %s couldn't be added to the "
                         "interface pool because it isn't bound "
                         "to a network driver - possibly in use elsewhere",
                         i, netdef->forward.pfs->dev);
            }
            break;

        case VIR_NETWORK_FORWARD_HOSTDEV:
            /* VF's are always PCI devices */
            thisIf->type = VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI;
            thisIf->device.pci.domain = thisVirtFn->domain;
            thisIf->device.pci.bus = thisVirtFn->bus;
            thisIf->device.pci.slot = thisVirtFn->slot;
            thisIf->device.pci.function = thisVirtFn->function;
            netdef->forward.nifs++;
            break;

        case VIR_NETWORK_FORWARD_NONE:
        case VIR_NETWORK_FORWARD_NAT:
        case VIR_NETWORK_FORWARD_ROUTE:
        case VIR_NETWORK_FORWARD_OPEN:
            /* by definition these will never be encountered here */
            break;

        case VIR_NETWORK_FORWARD_LAST:
        default:
            virReportEnumRangeError(virNetworkForwardType, netdef->forward.type);
            goto cleanup;
        }
    }

    if (netdef->forward.nifs == 0) {
        /* If we don't get at least one interface in the pool, declare
         * failure
         */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("No usable Vf's present on SRIOV PF %s"),
                       netdef->forward.pfs->dev);
        goto cleanup;
    }

    ret = 0;
 cleanup:
    if (ret < 0) {
        /* free all the entries made before error */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV)
                VIR_FREE(netdef->forward.ifs[i].device.dev);
        }
        netdef->forward.nifs = 0;
    }
    if (netdef->forward.nifs == 0)
        VIR_FREE(netdef->forward.ifs);

    for (i = 0; i < numVirtFns; i++) {
        VIR_FREE(vfNames[i]);
        VIR_FREE(virtFns[i]);
    }
    VIR_FREE(vfNames);
    VIR_FREE(virtFns);
    return ret;
}


static int
networkStartNetworkExternal(virNetworkObjPtr obj)
{
    /* put anything here that needs to be done each time a network of
     * type BRIDGE, PRIVATE, VEPA, HOSTDEV or PASSTHROUGH is started. On
     * failure, undo anything you've done, and return -1. On success
     * return 0.
     */
    return networkCreateInterfacePool(virNetworkObjGetDef(obj));
}


static int
networkShutdownNetworkExternal(virNetworkObjPtr obj G_GNUC_UNUSED)
{
    /* put anything here that needs to be done each time a network of
     * type BRIDGE, PRIVATE, VEPA, HOSTDEV or PASSTHROUGH is shutdown. On
     * failure, undo anything you've done, and return -1. On success
     * return 0.
     */
    return 0;
}


static int
networkStartNetwork(virNetworkDriverStatePtr driver,
                    virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    int ret = -1;

    VIR_DEBUG("driver=%p, network=%p", driver, obj);

    if (virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("network is already active"));
        return ret;
    }

    VIR_DEBUG("Beginning network startup process");

    virNetworkObjDeleteAllPorts(obj, driver->stateDir);

    VIR_DEBUG("Setting current network def as transient");
    if (virNetworkObjSetDefTransient(obj, true, network_driver->xmlopt) < 0)
        goto cleanup;

    /* Run an early hook to set-up missing devices.
     * If the script raised an error abort the launch. */
    if (networkRunHook(obj, NULL,
                       VIR_HOOK_NETWORK_OP_START,
                       VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    switch ((virNetworkForwardType) def->forward.type) {

    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
    case VIR_NETWORK_FORWARD_OPEN:
        if (networkStartNetworkVirtual(driver, obj) < 0)
            goto cleanup;
        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
        if (def->bridge) {
            if (networkStartNetworkBridge(obj) < 0)
                goto cleanup;
            break;
        }
        /* intentionally fall through to the macvtap/direct case for
         * VIR_NETWORK_FORWARD_BRIDGE with no bridge device defined
         * (since that is macvtap bridge mode).
         */
        G_GNUC_FALLTHROUGH;

    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
    case VIR_NETWORK_FORWARD_HOSTDEV:
        if (networkStartNetworkExternal(obj) < 0)
            goto cleanup;
        break;

    case VIR_NETWORK_FORWARD_LAST:
    default:
        virReportEnumRangeError(virNetworkForwardType, def->forward.type);
        goto cleanup;
    }

    virNetworkObjSetFloorSum(obj, 0);

    /* finally we can call the 'started' hook script if any */
    if (networkRunHook(obj, NULL,
                       VIR_HOOK_NETWORK_OP_STARTED,
                       VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    /* Persist the live configuration now that anything autogenerated
     * is setup.
     */
    VIR_DEBUG("Writing network status to disk");
    if (virNetworkObjSaveStatus(driver->stateDir,
                                obj, network_driver->xmlopt) < 0)
        goto cleanup;

    virNetworkObjSetActive(obj, true);
    VIR_INFO("Network '%s' started up", def->name);
    ret = 0;

 cleanup:
    if (ret < 0) {
        virErrorPtr save_err;

        virErrorPreserveLast(&save_err);
        virNetworkObjUnsetDefTransient(obj);
        networkShutdownNetwork(driver, obj);
        virErrorRestore(&save_err);
    }
    return ret;
}


static int
networkShutdownNetwork(virNetworkDriverStatePtr driver,
                       virNetworkObjPtr obj)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    int ret = 0;
    char *stateFile;

    VIR_INFO("Shutting down network '%s'", def->name);

    if (!virNetworkObjIsActive(obj))
        return 0;

    stateFile = virNetworkConfigFile(driver->stateDir, def->name);
    if (!stateFile)
        return -1;

    unlink(stateFile);
    VIR_FREE(stateFile);

    switch ((virNetworkForwardType) def->forward.type) {

    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
    case VIR_NETWORK_FORWARD_OPEN:
        ret = networkShutdownNetworkVirtual(driver, obj);
        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
        if (def->bridge) {
            ret = networkShutdownNetworkBridge(obj);
            break;
        }
        /* intentionally fall through to the macvtap/direct case for
         * VIR_NETWORK_FORWARD_BRIDGE with no bridge device defined
         * (since that is macvtap bridge mode).
         */
        G_GNUC_FALLTHROUGH;

    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
    case VIR_NETWORK_FORWARD_HOSTDEV:
        ret = networkShutdownNetworkExternal(obj);
        break;

    case VIR_NETWORK_FORWARD_LAST:
    default:
        virReportEnumRangeError(virNetworkForwardType, def->forward.type);
        return -1;
    }

    /* now that we know it's stopped call the hook if present */
    networkRunHook(obj, NULL, VIR_HOOK_NETWORK_OP_STOPPED,
                   VIR_HOOK_SUBOP_END);

    virNetworkObjSetActive(obj, false);
    virNetworkObjUnsetDefTransient(obj);
    return ret;
}


static virNetworkPtr
networkLookupByUUID(virConnectPtr conn,
                    const unsigned char *uuid)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPtr net = NULL;

    obj = virNetworkObjFindByUUID(driver->networks, uuid);
    if (!obj) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching uuid '%s'"),
                       uuidstr);
        goto cleanup;
    }
    def = virNetworkObjGetDef(obj);

    if (virNetworkLookupByUUIDEnsureACL(conn, def) < 0)
        goto cleanup;

    net = virGetNetwork(conn, def->name, def->uuid);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return net;
}


static virNetworkPtr
networkLookupByName(virConnectPtr conn,
                    const char *name)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPtr net = NULL;

    obj = virNetworkObjFindByName(driver->networks, name);
    if (!obj) {
        virReportError(VIR_ERR_NO_NETWORK,
                       _("no network with matching name '%s'"), name);
        goto cleanup;
    }
    def = virNetworkObjGetDef(obj);

    if (virNetworkLookupByNameEnsureACL(conn, def) < 0)
        goto cleanup;

    net = virGetNetwork(conn, def->name, def->uuid);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return net;
}


static int
networkConnectNumOfNetworks(virConnectPtr conn)
{
    virNetworkDriverStatePtr driver = networkGetDriver();

    if (virConnectNumOfNetworksEnsureACL(conn) < 0)
        return -1;

    return virNetworkObjListNumOfNetworks(driver->networks, true,
                                          virConnectNumOfNetworksCheckACL,
                                          conn);
}


static int
networkConnectListNetworks(virConnectPtr conn,
                           char **const names,
                           int maxnames)
{
    virNetworkDriverStatePtr driver = networkGetDriver();

    if (virConnectListNetworksEnsureACL(conn) < 0)
        return -1;

    return virNetworkObjListGetNames(driver->networks, true, names, maxnames,
                                     virConnectListNetworksCheckACL, conn);
}


static int
networkConnectNumOfDefinedNetworks(virConnectPtr conn)
{
    virNetworkDriverStatePtr driver = networkGetDriver();

    if (virConnectNumOfDefinedNetworksEnsureACL(conn) < 0)
        return -1;

    return virNetworkObjListNumOfNetworks(driver->networks, false,
                                          virConnectNumOfDefinedNetworksCheckACL,
                                          conn);
}


static int
networkConnectListDefinedNetworks(virConnectPtr conn,
                                  char **const names,
                                  int maxnames)
{
    virNetworkDriverStatePtr driver = networkGetDriver();

    if (virConnectListDefinedNetworksEnsureACL(conn) < 0)
        return -1;

    return virNetworkObjListGetNames(driver->networks, false, names, maxnames,
                                     virConnectListDefinedNetworksCheckACL,
                                     conn);
}


static int
networkConnectListAllNetworks(virConnectPtr conn,
                              virNetworkPtr **nets,
                              unsigned int flags)
{
    virNetworkDriverStatePtr driver = networkGetDriver();

    virCheckFlags(VIR_CONNECT_LIST_NETWORKS_FILTERS_ALL, -1);

    if (virConnectListAllNetworksEnsureACL(conn) < 0)
        return -1;

    return virNetworkObjListExport(conn, driver->networks, nets,
                                   virConnectListAllNetworksCheckACL,
                                   flags);
}


static int
networkConnectNetworkEventRegisterAny(virConnectPtr conn,
                                      virNetworkPtr net,
                                      int eventID,
                                      virConnectNetworkEventGenericCallback callback,
                                      void *opaque,
                                      virFreeCallback freecb)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    int ret = -1;

    if (virConnectNetworkEventRegisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virNetworkEventStateRegisterID(conn, driver->networkEventState,
                                       net, eventID, callback,
                                       opaque, freecb, &ret) < 0)
        ret = -1;

    return ret;
}


static int
networkConnectNetworkEventDeregisterAny(virConnectPtr conn,
                                        int callbackID)
{
    virNetworkDriverStatePtr driver = networkGetDriver();

    if (virConnectNetworkEventDeregisterAnyEnsureACL(conn) < 0)
        return -1;

    if (virObjectEventStateDeregisterID(conn,
                                        driver->networkEventState,
                                        callbackID, true) < 0)
        return -1;

    return 0;
}


static int
networkIsActive(virNetworkPtr net)
{
    virNetworkObjPtr obj;
    int ret = -1;

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkIsActiveEnsureACL(net->conn, virNetworkObjGetDef(obj)) < 0)
        goto cleanup;

    ret = virNetworkObjIsActive(obj);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkIsPersistent(virNetworkPtr net)
{
    virNetworkObjPtr obj;
    int ret = -1;

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkIsPersistentEnsureACL(net->conn, virNetworkObjGetDef(obj)) < 0)
        goto cleanup;

    ret = virNetworkObjIsPersistent(obj);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


/*
 * networkFindUnusedBridgeName() - try to find a bridge name that is
 * unused by the currently configured libvirt networks, as well as by
 * the host system itself (possibly created by someone/something other
 * than libvirt). Set this network's name to that new name.
 */
static int
networkFindUnusedBridgeName(virNetworkObjListPtr nets,
                            virNetworkDefPtr def)
{
    int ret = -1, id = 0;
    char *newname = NULL;
    const char *templ = "virbr%d";
    const char *p;

    if (def->bridge &&
        (p = strchr(def->bridge, '%')) == strrchr(def->bridge, '%') &&
        p && p[1] == 'd')
        templ = def->bridge;

    do {
        newname = g_strdup_printf(templ, id);
        /* check if this name is used in another libvirt network or
         * there is an existing device with that name. ignore errors
         * from virNetDevExists(), just in case it isn't implemented
         * on this platform (probably impossible).
         */
        if (!(virNetworkObjBridgeInUse(nets, newname, def->name) ||
              virNetDevExists(newname) == 1)) {
            VIR_FREE(def->bridge); /*could contain template */
            def->bridge = newname;
            ret = 0;
            goto cleanup;
        }
        VIR_FREE(newname);
    } while (++id <= MAX_BRIDGE_ID);

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Bridge generation exceeded max id %d"),
                   MAX_BRIDGE_ID);
    ret = 0;
 cleanup:
    if (ret < 0)
        VIR_FREE(newname);
    return ret;
}


/*
 * networkValidateBridgeName() - if no bridge name is set, or if the
 * bridge name contains a %d (indicating that this is a template for
 * the actual name) try to set an appropriate bridge name.  If a
 * bridge name *is* set, make sure it doesn't conflict with any other
 * network's bridge name.
 */
static int
networkBridgeNameValidate(virNetworkObjListPtr nets,
                          virNetworkDefPtr def)
{
    if (def->bridge && !strstr(def->bridge, "%d")) {
        if (virNetworkObjBridgeInUse(nets, def->bridge, def->name)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("bridge name '%s' already in use."),
                           def->bridge);
            return -1;
        }
    } else {
        /* Allocate a bridge name */
        if (networkFindUnusedBridgeName(nets, def) < 0)
            return -1;
    }

    return 0;
}


static int
networkValidate(virNetworkDriverStatePtr driver,
                virNetworkDefPtr def)
{
    size_t i, j;
    bool vlanUsed, vlanAllowed, badVlanUse = false;
    virPortGroupDefPtr defaultPortGroup = NULL;
    virNetworkIPDefPtr ipdef;
    bool ipv4def = false, ipv6def = false;
    bool bandwidthAllowed = false;
    bool usesInterface = false, usesAddress = false;

    if (virXMLCheckIllegalChars("name", def->name, "\n") < 0)
        return -1;

    /* Only the three L3 network types that are configured by libvirt
     * need to have a bridge device name / mac address provided
     */
    switch ((virNetworkForwardType) def->forward.type) {
    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
    case VIR_NETWORK_FORWARD_OPEN:
        /* if no bridge name was given in the config, find a name
         * unused by any other libvirt networks and assign it.
         */
        if (networkBridgeNameValidate(driver->networks, def) < 0)
            return -1;

        virNetworkSetBridgeMacAddr(def);
        bandwidthAllowed = true;
        break;

    case VIR_NETWORK_FORWARD_BRIDGE:
        if (def->bridge != NULL)
            bandwidthAllowed = true;

        G_GNUC_FALLTHROUGH;

    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
    case VIR_NETWORK_FORWARD_HOSTDEV:
        /* They are also the only types that currently support setting
         * a MAC or IP address for the host-side device (bridge), DNS
         * configuration, or network-wide bandwidth limits.
         */
        if (def->mac_specified) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <mac> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (virNetworkDefGetIPByIndex(def, AF_UNSPEC, 0)) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <ip> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (def->dns.ntxts || def->dns.nhosts || def->dns.nsrvs) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <dns> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        if (def->domain) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <domain> element in network %s "
                             "with forward mode='%s'"),
                           def->name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
        break;

    case VIR_NETWORK_FORWARD_LAST:
    default:
        virReportEnumRangeError(virNetworkForwardType, def->forward.type);
        return -1;
    }

    if (def->bandwidth &&
        !bandwidthAllowed) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Unsupported network-wide <bandwidth> element "
                         "in network %s with forward mode='%s'"),
                       def->name,
                       virNetworkForwardTypeToString(def->forward.type));
        return -1;
    }

    /* we support configs with a single PF defined:
     *   <pf dev='eth0'/>
     * or with a list of netdev names:
     *   <interface dev='eth9'/>
     * OR a list of PCI addresses
     *   <address type='pci' domain='0' bus='4' slot='0' function='1'/>
     * but not any combination of those.
     *
     * Since <interface> and <address> are for some strange reason
     * stored in the same array, we need to cycle through it and check
     * the type of each.
     */
    for (i = 0; i < def->forward.nifs; i++) {
        virNetworkForwardIfDefPtr iface = &def->forward.ifs[i];
        char *sysfs_path = NULL;

        switch ((virNetworkForwardHostdevDeviceType)iface->type) {
        case VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV:
            usesInterface = true;

            if (def->forward.type == VIR_NETWORK_FORWARD_HOSTDEV) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("hostdev network '%s' lists '%s' "
                                 "in the device pool, but hostdev "
                                 "networks require all devices to "
                                 "be listed by PCI address, not "
                                 "network device name"),
                               def->name, iface->device.dev);
                return -1;
            }
            break;

        case VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI: {
            usesAddress = true;

            if (def->forward.type != VIR_NETWORK_FORWARD_HOSTDEV) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("network '%s' has forward mode '%s' "
                                 " but lists a device by PCI address "
                                 "in the device pool. This is only "
                                 "supported for networks with forward "
                                 "mode 'hostdev'"),
                               def->name,
                               virNetworkForwardTypeToString(def->forward.type));
                return -1;
            }

            if (virPCIDeviceAddressGetSysfsFile(&iface->device.pci, &sysfs_path) < 0)
                return -1;

            if (!virPCIIsVirtualFunction(sysfs_path)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("device '%s' in network '%s' is not "
                                 "an SR-IOV Virtual Function"),
                               sysfs_path, def->name);
                VIR_FREE(sysfs_path);
                return -1;
            }
            VIR_FREE(sysfs_path);
            break;
        }

        case VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NONE:
        case VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_LAST:
            break;
        }
    }
    if ((def->forward.npfs > 0) + usesInterface + usesAddress > 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("<address>, <interface>, and <pf> elements of "
                         "<forward> in network %s are mutually exclusive"),
                       def->name);
        return -1;
    }

    /* We only support dhcp on one IPv4 address and
     * on one IPv6 address per defined network
     */
    for (i = 0;
         (ipdef = virNetworkDefGetIPByIndex(def, AF_UNSPEC, i));
         i++) {
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (ipv4def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Multiple IPv4 dhcp sections found -- "
                                 "dhcp is supported only for a "
                                 "single IPv4 address on each network"));
                    return -1;
                } else {
                    ipv4def = true;
                }
            }
        }
        if (VIR_SOCKET_ADDR_IS_FAMILY(&ipdef->address, AF_INET6)) {
            if (ipdef->nranges || ipdef->nhosts) {
                if (ipv6def) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                               _("Multiple IPv6 dhcp sections found -- "
                                 "dhcp is supported only for a "
                                 "single IPv6 address on each network"));
                    return -1;
                } else {
                    ipv6def = true;
                }
            }
        }
    }

    /* The only type of networks that currently support transparent
     * vlan configuration are those using hostdev sr-iov devices from
     * a pool, and those using an Open vSwitch bridge.
     */

    vlanAllowed = (def->forward.type == VIR_NETWORK_FORWARD_HOSTDEV ||
                   def->forward.type == VIR_NETWORK_FORWARD_PASSTHROUGH ||
                   (def->forward.type == VIR_NETWORK_FORWARD_BRIDGE &&
                    def->virtPortProfile &&
                    def->virtPortProfile->virtPortType
                    == VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH));

    vlanUsed = def->vlan.nTags > 0;
    for (i = 0; i < def->nPortGroups; i++) {
        if (vlanUsed || def->portGroups[i].vlan.nTags > 0) {
            /* anyone using this portgroup will get a vlan tag. Verify
             * that they will also be using an openvswitch connection,
             * as that is the only type of network that currently
             * supports a vlan tag.
             */
            if (def->portGroups[i].virtPortProfile) {
                if (def->forward.type != VIR_NETWORK_FORWARD_BRIDGE ||
                    def->portGroups[i].virtPortProfile->virtPortType
                    != VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) {
                    badVlanUse = true;
                }
            } else if (!vlanAllowed) {
                /* virtualport taken from base network definition */
                badVlanUse = true;
            }
        }
        if (def->portGroups[i].isDefault) {
            if (defaultPortGroup) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("network '%s' has multiple default "
                                 "<portgroup> elements (%s and %s), "
                                 "but only one default is allowed"),
                               def->name, defaultPortGroup->name,
                               def->portGroups[i].name);
                return -1;
            }
            defaultPortGroup = &def->portGroups[i];
        }
        for (j = i + 1; j < def->nPortGroups; j++) {
            if (STREQ(def->portGroups[i].name, def->portGroups[j].name)) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("multiple <portgroup> elements with the "
                                 "same name (%s) in network '%s'"),
                               def->portGroups[i].name, def->name);
                return -1;
            }
        }
        if (def->portGroups[i].bandwidth && !bandwidthAllowed) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("Unsupported <bandwidth> element in network '%s' "
                             "in portgroup '%s' with forward mode='%s'"),
                           def->name, def->portGroups[i].name,
                           virNetworkForwardTypeToString(def->forward.type));
            return -1;
        }
    }
    if (badVlanUse ||
        (vlanUsed && !vlanAllowed && !defaultPortGroup)) {
        /* NB: if defaultPortGroup is set, we don't directly look at
         * vlanUsed && !vlanAllowed, because the network will never be
         * used without having a portgroup added in, so all necessary
         * checks were done in the loop above.
         */
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("<vlan> element specified for network %s, "
                         "whose type doesn't support vlan configuration"),
                       def->name);
        return -1;
    }

    if (def->forward.type == VIR_NETWORK_FORWARD_HOSTDEV) {
        for (i = 0; i < def->nPortGroups; i++) {
            if (def->portGroups[i].bandwidth) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("unsupported <bandwidth> element "
                                 "in <portgroup name='%s'> of "
                                 "network '%s' with forward mode='%s'"),
                               def->portGroups[i].name, def->name,
                               virNetworkForwardTypeToString(def->forward.type));
                return -1;
            }
        }
    }
    return 0;
}


static virNetworkPtr
networkCreateXML(virConnectPtr conn,
                 const char *xml)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkDefPtr newDef;
    virNetworkObjPtr obj = NULL;
    virNetworkDefPtr def;
    virNetworkPtr net = NULL;
    virObjectEventPtr event = NULL;

    if (!(newDef = virNetworkDefParseString(xml, network_driver->xmlopt)))
        goto cleanup;

    if (virNetworkCreateXMLEnsureACL(conn, newDef) < 0)
        goto cleanup;

    if (networkValidate(driver, newDef) < 0)
        goto cleanup;

    /* NB: even though this transient network hasn't yet been started,
     * we assign the def with live = true in anticipation that it will
     * be started momentarily.
     */
    if (!(obj = virNetworkObjAssignDef(driver->networks, newDef,
                                       VIR_NETWORK_OBJ_LIST_ADD_LIVE |
                                       VIR_NETWORK_OBJ_LIST_ADD_CHECK_LIVE)))
        goto cleanup;
    newDef = NULL;
    def = virNetworkObjGetDef(obj);

    if (networkStartNetwork(driver, obj) < 0) {
        virNetworkObjRemoveInactive(driver->networks, obj);
        goto cleanup;
    }

    event = virNetworkEventLifecycleNew(def->name,
                                        def->uuid,
                                        VIR_NETWORK_EVENT_STARTED,
                                        0);

    VIR_INFO("Creating network '%s'", def->name);
    net = virGetNetwork(conn, def->name, def->uuid);

 cleanup:
    virNetworkDefFree(newDef);
    virObjectEventStateQueue(driver->networkEventState, event);
    virNetworkObjEndAPI(&obj);
    return net;
}


static virNetworkPtr
networkDefineXML(virConnectPtr conn,
                 const char *xml)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkDefPtr def = NULL;
    bool freeDef = true;
    virNetworkObjPtr obj = NULL;
    virNetworkPtr net = NULL;
    virObjectEventPtr event = NULL;

    if (!(def = virNetworkDefParseString(xml, network_driver->xmlopt)))
        goto cleanup;

    if (virNetworkDefineXMLEnsureACL(conn, def) < 0)
        goto cleanup;

    if (networkValidate(driver, def) < 0)
        goto cleanup;

    if (!(obj = virNetworkObjAssignDef(driver->networks, def, 0)))
        goto cleanup;

    /* def was assigned to network object */
    freeDef = false;

    if (virNetworkSaveConfig(driver->networkConfigDir,
                             def, network_driver->xmlopt) < 0) {
        if (!virNetworkObjIsActive(obj)) {
            virNetworkObjRemoveInactive(driver->networks, obj);
            goto cleanup;
        }
        /* if network was active already, just undo new persistent
         * definition by making it transient.
         * XXX - this isn't necessarily the correct thing to do.
         */
        virNetworkObjUpdateAssignDef(obj, NULL, false);
        goto cleanup;
    }

    event = virNetworkEventLifecycleNew(def->name, def->uuid,
                                        VIR_NETWORK_EVENT_DEFINED,
                                        0);

    VIR_INFO("Defining network '%s'", def->name);
    net = virGetNetwork(conn, def->name, def->uuid);

 cleanup:
    virObjectEventStateQueue(driver->networkEventState, event);
    if (freeDef)
        virNetworkDefFree(def);
    virNetworkObjEndAPI(&obj);
    return net;
}


static int
networkUndefine(virNetworkPtr net)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    int ret = -1;
    bool active = false;
    virObjectEventPtr event = NULL;

    if (!(obj = networkObjFromNetwork(net)))
        goto cleanup;
    def = virNetworkObjGetDef(obj);

    if (virNetworkUndefineEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if (virNetworkObjIsActive(obj))
        active = true;

    if (!virNetworkObjIsPersistent(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("can't undefine transient network"));
        goto cleanup;
    }

    /* remove autostart link */
    if (virNetworkObjDeleteConfig(driver->networkConfigDir,
                                  driver->networkAutostartDir,
                                  obj) < 0)
        goto cleanup;

    event = virNetworkEventLifecycleNew(def->name,
                                        def->uuid,
                                        VIR_NETWORK_EVENT_UNDEFINED,
                                        0);

    VIR_INFO("Undefining network '%s'", def->name);
    if (!active) {
        if (networkRemoveInactive(driver, obj) < 0)
            goto cleanup;
    } else {

        /* if the network still exists, it was active, and we need to make
         * it transient (by deleting the persistent def)
         */
        virNetworkObjUpdateAssignDef(obj, NULL, false);
    }

    ret = 0;

 cleanup:
    virObjectEventStateQueue(driver->networkEventState, event);
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkUpdate(virNetworkPtr net,
              unsigned int command,
              unsigned int section,
              int parentIndex,
              const char *xml,
              unsigned int flags)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj = NULL;
    virNetworkDefPtr def;
    int isActive, ret = -1;
    size_t i;
    virNetworkIPDefPtr ipdef;
    bool oldDhcpActive = false;
    bool needFirewallRefresh = false;


    virCheckFlags(VIR_NETWORK_UPDATE_AFFECT_LIVE |
                  VIR_NETWORK_UPDATE_AFFECT_CONFIG,
                  -1);

    if (!(obj = networkObjFromNetwork(net)))
        goto cleanup;
    def = virNetworkObjGetDef(obj);

    if (virNetworkUpdateEnsureACL(net->conn, def, flags) < 0)
        goto cleanup;

    /* see if we are listening for dhcp pre-modification */
    for (i = 0;
         (ipdef = virNetworkDefGetIPByIndex(def, AF_INET, i));
         i++) {
        if (ipdef->nranges || ipdef->nhosts) {
            oldDhcpActive = true;
            break;
        }
    }

    /* VIR_NETWORK_UPDATE_AFFECT_CURRENT means "change LIVE if network
     * is active, else change CONFIG
     */
    isActive = virNetworkObjIsActive(obj);
    if ((flags & (VIR_NETWORK_UPDATE_AFFECT_LIVE |
                  VIR_NETWORK_UPDATE_AFFECT_CONFIG)) ==
        VIR_NETWORK_UPDATE_AFFECT_CURRENT) {
        if (isActive)
            flags |= VIR_NETWORK_UPDATE_AFFECT_LIVE;
        else
            flags |= VIR_NETWORK_UPDATE_AFFECT_CONFIG;
    }

    if (isActive && (flags & VIR_NETWORK_UPDATE_AFFECT_LIVE)) {
        /* Take care of anything that must be done before updating the
         * live NetworkDef.
         */
        switch ((virNetworkForwardType) def->forward.type) {
        case VIR_NETWORK_FORWARD_NONE:
        case VIR_NETWORK_FORWARD_NAT:
        case VIR_NETWORK_FORWARD_ROUTE:
            switch (section) {
            case VIR_NETWORK_SECTION_FORWARD:
            case VIR_NETWORK_SECTION_FORWARD_INTERFACE:
            case VIR_NETWORK_SECTION_IP:
            case VIR_NETWORK_SECTION_IP_DHCP_RANGE:
            case VIR_NETWORK_SECTION_IP_DHCP_HOST:
                /* these could affect the firewall rules, so remove the
                 * old rules (and remember to load new ones after the
                 * update).
                 */
                networkRemoveFirewallRules(def);
                needFirewallRefresh = true;
                break;
            default:
                break;
            }
            break;

        case VIR_NETWORK_FORWARD_OPEN:
        case VIR_NETWORK_FORWARD_BRIDGE:
        case VIR_NETWORK_FORWARD_PRIVATE:
        case VIR_NETWORK_FORWARD_VEPA:
        case VIR_NETWORK_FORWARD_PASSTHROUGH:
        case VIR_NETWORK_FORWARD_HOSTDEV:
            break;

        case VIR_NETWORK_FORWARD_LAST:
        default:
            virReportEnumRangeError(virNetworkForwardType, def->forward.type);
            goto cleanup;
        }
    }

    /* update the network config in memory/on disk */
    if (virNetworkObjUpdate(obj, command, section,
                            parentIndex, xml,
                            network_driver->xmlopt, flags) < 0) {
        if (needFirewallRefresh)
            ignore_value(networkAddFirewallRules(def));
        goto cleanup;
    }

    /* @def is replaced */
    def = virNetworkObjGetDef(obj);

    if (needFirewallRefresh && networkAddFirewallRules(def) < 0)
        goto cleanup;

    if (flags & VIR_NETWORK_UPDATE_AFFECT_CONFIG) {
        /* save updated persistent config to disk */
        if (virNetworkSaveConfig(driver->networkConfigDir,
                                 virNetworkObjGetPersistentDef(obj),
                                 network_driver->xmlopt) < 0) {
            goto cleanup;
        }
    }

    if (isActive && (flags & VIR_NETWORK_UPDATE_AFFECT_LIVE)) {
        /* rewrite dnsmasq host files, restart dnsmasq, update iptables
         * rules, etc, according to which section was modified. Note that
         * some sections require multiple actions, so a single switch
         * statement is inadequate.
         */
        if (section == VIR_NETWORK_SECTION_BRIDGE ||
            section == VIR_NETWORK_SECTION_DOMAIN ||
            section == VIR_NETWORK_SECTION_IP ||
            section == VIR_NETWORK_SECTION_IP_DHCP_RANGE ||
            section == VIR_NETWORK_SECTION_DNS_TXT ||
            section == VIR_NETWORK_SECTION_DNS_SRV) {
            /* these sections all change things on the dnsmasq
             * commandline (i.e. in the .conf file), so we need to
             * kill and restart dnsmasq, because dnsmasq sets its uid
             * to "nobody" after it starts, and is unable to re-read
             * the conf file (owned by root, mode 600)
             */
            if (networkRestartDhcpDaemon(driver, obj) < 0)
                goto cleanup;

        } else if (section == VIR_NETWORK_SECTION_IP_DHCP_HOST) {
            /* if we previously weren't listening for dhcp and now we
             * are (or vice-versa) then we need to do a restart,
             * otherwise we just need to do a refresh (redo the config
             * files and send SIGHUP)
             */
            bool newDhcpActive = false;

            for (i = 0; (ipdef = virNetworkDefGetIPByIndex(def, AF_INET, i));
                 i++) {
                if (ipdef->nranges || ipdef->nhosts) {
                    newDhcpActive = true;
                    break;
                }
            }

            if ((newDhcpActive != oldDhcpActive &&
                 networkRestartDhcpDaemon(driver, obj) < 0) ||
                networkRefreshDhcpDaemon(driver, obj) < 0) {
                goto cleanup;
            }

        } else if (section == VIR_NETWORK_SECTION_DNS_HOST) {
            /* this section only changes data in an external file
             * (not the .conf file) so we can just update the config
             * files and send SIGHUP to dnsmasq.
             */
            if (networkRefreshDhcpDaemon(driver, obj) < 0)
                goto cleanup;

        }

        if (section == VIR_NETWORK_SECTION_IP) {
            /* only a change in IP addresses will affect radvd, and all of radvd's
             * config is stored in the conf file which will be re-read with a SIGHUP.
             */
            if (networkRefreshRadvd(driver, obj) < 0)
                goto cleanup;
        }

        /* save current network state to disk */
        if ((ret = virNetworkObjSaveStatus(driver->stateDir,
                                           obj, network_driver->xmlopt)) < 0)
            goto cleanup;
    }

    /* call the 'updated' network hook script */
    if (networkRunHook(obj, NULL, VIR_HOOK_NETWORK_OP_UPDATED,
                       VIR_HOOK_SUBOP_BEGIN) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkCreate(virNetworkPtr net)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    int ret = -1;
    virObjectEventPtr event = NULL;

    if (!(obj = networkObjFromNetwork(net)))
        goto cleanup;
    def = virNetworkObjGetDef(obj);

    if (virNetworkCreateEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if ((ret = networkStartNetwork(driver, obj)) < 0)
        goto cleanup;

    event = virNetworkEventLifecycleNew(def->name,
                                        def->uuid,
                                        VIR_NETWORK_EVENT_STARTED,
                                        0);

 cleanup:
    virObjectEventStateQueue(driver->networkEventState, event);
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkDestroy(virNetworkPtr net)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    int ret = -1;
    virObjectEventPtr event = NULL;

    if (!(obj = networkObjFromNetwork(net)))
        goto cleanup;
    def = virNetworkObjGetDef(obj);

    if (virNetworkDestroyEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    if ((ret = networkShutdownNetwork(driver, obj)) < 0)
        goto cleanup;

    virNetworkObjDeleteAllPorts(obj, driver->stateDir);

    /* @def replaced in virNetworkObjUnsetDefTransient*/
    def = virNetworkObjGetDef(obj);

    event = virNetworkEventLifecycleNew(def->name,
                                        def->uuid,
                                        VIR_NETWORK_EVENT_STOPPED,
                                        0);

    if (!virNetworkObjIsPersistent(obj) &&
        networkRemoveInactive(driver, obj) < 0) {
        ret = -1;
        goto cleanup;
    }

 cleanup:
    virObjectEventStateQueue(driver->networkEventState, event);
    virNetworkObjEndAPI(&obj);
    return ret;
}


static char *
networkGetXMLDesc(virNetworkPtr net,
                  unsigned int flags)
{
    virNetworkObjPtr obj;
    virNetworkDefPtr curDef;
    virNetworkDefPtr def;
    virNetworkDefPtr newDef;
    char *ret = NULL;

    virCheckFlags(VIR_NETWORK_XML_INACTIVE, NULL);

    if (!(obj = networkObjFromNetwork(net)))
        return ret;
    def = virNetworkObjGetDef(obj);
    newDef = virNetworkObjGetNewDef(obj);

    if (virNetworkGetXMLDescEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if ((flags & VIR_NETWORK_XML_INACTIVE) && newDef)
        curDef = newDef;
    else
        curDef = def;

    ret = virNetworkDefFormat(curDef, network_driver->xmlopt, flags);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static char *
networkGetBridgeName(virNetworkPtr net)
{
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    char *bridge = NULL;

    if (!(obj = networkObjFromNetwork(net)))
        return bridge;
    def = virNetworkObjGetDef(obj);

    if (virNetworkGetBridgeNameEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if (!(def->bridge)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("network '%s' does not have a bridge name."),
                       def->name);
        goto cleanup;
    }

    bridge = g_strdup(def->bridge);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return bridge;
}


static int
networkGetAutostart(virNetworkPtr net,
                    int *autostart)
{
    virNetworkObjPtr obj;
    int ret = -1;

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    if (virNetworkGetAutostartEnsureACL(net->conn, virNetworkObjGetDef(obj)) < 0)
        goto cleanup;

    *autostart = virNetworkObjIsAutostart(obj) ? 1 : 0;
    ret = 0;

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkSetAutostart(virNetworkPtr net,
                    int autostart)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    char *configFile = NULL, *autostartLink = NULL;
    bool new_autostart;
    bool cur_autostart;
    int ret = -1;

    if (!(obj = networkObjFromNetwork(net)))
        goto cleanup;
    def = virNetworkObjGetDef(obj);

    if (virNetworkSetAutostartEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if (!virNetworkObjIsPersistent(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot set autostart for transient network"));
        goto cleanup;
    }

    new_autostart = (autostart != 0);
    cur_autostart = virNetworkObjIsAutostart(obj);
    if (cur_autostart != new_autostart) {
        if ((configFile = virNetworkConfigFile(driver->networkConfigDir,
                                               def->name)) == NULL)
            goto cleanup;
        if ((autostartLink = virNetworkConfigFile(driver->networkAutostartDir,
                                                  def->name)) == NULL)
            goto cleanup;

        if (new_autostart) {
            if (virFileMakePath(driver->networkAutostartDir) < 0) {
                virReportSystemError(errno,
                                     _("cannot create autostart directory '%s'"),
                                     driver->networkAutostartDir);
                goto cleanup;
            }

            if (symlink(configFile, autostartLink) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create symlink '%s' to '%s'"),
                                     autostartLink, configFile);
                goto cleanup;
            }
        } else {
            if (unlink(autostartLink) < 0 && errno != ENOENT && errno != ENOTDIR) {
                virReportSystemError(errno,
                                     _("Failed to delete symlink '%s'"),
                                     autostartLink);
                goto cleanup;
            }
        }

        virNetworkObjSetAutostart(obj, new_autostart);
    }

    ret = 0;

 cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkGetDHCPLeases(virNetworkPtr net,
                     const char *mac,
                     virNetworkDHCPLeasePtr **leases,
                     unsigned int flags)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    size_t i, j;
    size_t nleases = 0;
    int rv = -1;
    size_t size = 0;
    int custom_lease_file_len = 0;
    bool need_results = !!leases;
    long long currtime = 0;
    long long expirytime_tmp = -1;
    bool ipv6 = false;
    char *lease_entries = NULL;
    char *custom_lease_file = NULL;
    const char *ip_tmp = NULL;
    const char *mac_tmp = NULL;
    virJSONValuePtr lease_tmp = NULL;
    virJSONValuePtr leases_array = NULL;
    virNetworkIPDefPtr ipdef_tmp = NULL;
    virNetworkDHCPLeasePtr lease = NULL;
    virNetworkDHCPLeasePtr *leases_ret = NULL;
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virMacAddr mac_addr;

    virCheckFlags(0, -1);

    /* only to check if the MAC is valid */
    if (mac && virMacAddrParse(mac, &mac_addr) < 0) {
        virReportError(VIR_ERR_INVALID_MAC, "%s", mac);
        return -1;
    }

    if (!(obj = networkObjFromNetwork(net)))
        return -1;
    def = virNetworkObjGetDef(obj);

    if (virNetworkGetDHCPLeasesEnsureACL(net->conn, def) < 0)
        goto cleanup;

    /* Retrieve custom leases file location */
    custom_lease_file = networkDnsmasqLeaseFileNameCustom(driver, def->bridge);

    /* Read entire contents */
    if ((custom_lease_file_len = virFileReadAllQuiet(custom_lease_file,
                                                     VIR_NETWORK_DHCP_LEASE_FILE_SIZE_MAX,
                                                     &lease_entries)) < 0) {
        /* Not all networks are guaranteed to have leases file.
         * Only those which run dnsmasq. Therefore, if we failed
         * to read the leases file, don't report error. Return 0
         * leases instead. */
        if (errno == ENOENT) {
            rv = 0;
        } else {
            virReportSystemError(errno,
                                 _("Unable to read leases file: %s"),
                                 custom_lease_file);
        }
        goto error;
    }

    if (custom_lease_file_len) {
        if (!(leases_array = virJSONValueFromString(lease_entries))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid json in file: %s"), custom_lease_file);
            goto error;
        }

        if (!virJSONValueIsArray(leases_array)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Malformed lease_entries array"));
            goto error;
        }
        size = virJSONValueArraySize(leases_array);
    }

    currtime = (long long)time(NULL);

    for (i = 0; i < size; i++) {
        if (!(lease_tmp = virJSONValueArrayGet(leases_array, i))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("failed to parse json"));
            goto error;
        }

        if (!(mac_tmp = virJSONValueObjectGetString(lease_tmp, "mac-address"))) {
            /* leaseshelper program guarantees that lease will be stored only if
             * mac-address is known otherwise not */
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("found lease without mac-address"));
            goto error;
        }

        if (mac && virMacAddrCompare(mac, mac_tmp))
            continue;

        if (virJSONValueObjectGetNumberLong(lease_tmp, "expiry-time", &expirytime_tmp) < 0) {
            /* A lease cannot be present without expiry-time */
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("found lease without expiry-time"));
            goto error;
        }

        /* Do not report expired lease */
        if (expirytime_tmp < currtime)
            continue;

        if (need_results) {
            if (VIR_ALLOC(lease) < 0)
                goto error;

            lease->expirytime = expirytime_tmp;

            if (!(ip_tmp = virJSONValueObjectGetString(lease_tmp, "ip-address"))) {
                /* A lease without ip-address makes no sense */
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("found lease without ip-address"));
                goto error;
            }

            /* Unlike IPv4, IPv6 uses ':' instead of '.' as separator */
            ipv6 = strchr(ip_tmp, ':') ? true : false;
            lease->type = ipv6 ? VIR_IP_ADDR_TYPE_IPV6 : VIR_IP_ADDR_TYPE_IPV4;

            /* Obtain prefix */
            for (j = 0; j < def->nips; j++) {
                ipdef_tmp = &def->ips[j];

                if (ipv6 && VIR_SOCKET_ADDR_IS_FAMILY(&ipdef_tmp->address,
                                                      AF_INET6)) {
                    lease->prefix = ipdef_tmp->prefix;
                    break;
                }
                if (!ipv6 && VIR_SOCKET_ADDR_IS_FAMILY(&ipdef_tmp->address,
                                                      AF_INET)) {
                    lease->prefix = virSocketAddrGetIPPrefix(&ipdef_tmp->address,
                                                             &ipdef_tmp->netmask,
                                                             ipdef_tmp->prefix);
                    break;
                }
            }

            lease->mac = g_strdup(mac_tmp);
            lease->ipaddr = g_strdup(ip_tmp);
            lease->iface = g_strdup(def->bridge);

            /* Fields that can be NULL */
            lease->iaid = g_strdup(virJSONValueObjectGetString(lease_tmp, "iaid"));
            lease->clientid = g_strdup(virJSONValueObjectGetString(lease_tmp, "client-id"));
            lease->hostname = g_strdup(virJSONValueObjectGetString(lease_tmp, "hostname"));

            if (VIR_INSERT_ELEMENT(leases_ret, nleases, nleases, lease) < 0)
                goto error;

        } else {
            nleases++;
        }

        VIR_FREE(lease);
    }

    if (leases_ret) {
        /* NULL terminated array */
        ignore_value(VIR_REALLOC_N(leases_ret, nleases + 1));
        *leases = leases_ret;
        leases_ret = NULL;
    }

    rv = nleases;

 cleanup:
    VIR_FREE(lease);
    VIR_FREE(lease_entries);
    VIR_FREE(custom_lease_file);
    virJSONValueFree(leases_array);

    virNetworkObjEndAPI(&obj);

    return rv;

 error:
    if (leases_ret) {
        for (i = 0; i < nleases; i++)
            virNetworkDHCPLeaseFree(leases_ret[i]);
        VIR_FREE(leases_ret);
    }
    goto cleanup;
}


/* A unified function to log network connections and disconnections */

static void
networkLogAllocation(virNetworkDefPtr netdef,
                     virNetworkForwardIfDefPtr dev,
                     virMacAddrPtr mac,
                     bool inUse)
{
    char macStr[VIR_MAC_STRING_BUFLEN];
    const char *verb = inUse ? "using" : "releasing";

    virMacAddrFormat(mac, macStr);
    if (!dev) {
        VIR_INFO("MAC %s %s network %s (%d connections)",
                 macStr, verb, netdef->name, netdef->connections);
    } else {
        if (dev->type == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI) {
            VIR_INFO("MAC %s %s network %s (%d connections) "
                     "physical device %04x:%02x:%02x.%x (%d connections)",
                     macStr, verb, netdef->name, netdef->connections,
                     dev->device.pci.domain, dev->device.pci.bus,
                     dev->device.pci.slot, dev->device.pci.function,
                     dev->connections);
        } else {
            VIR_INFO("MAC %s %s network %s (%d connections) "
                     "physical device %s (%d connections)",
                     macStr, verb, netdef->name, netdef->connections,
                     dev->device.dev, dev->connections);
        }
    }
}


/* Private API to deal with logical switch capabilities.
 * These functions are exported so that other parts of libvirt can
 * call them, but are not part of the public API and not in the
 * driver's function table. If we ever have more than one network
 * driver, we will need to present these functions via a second
 * "backend" function table.
 */

/* networkAllocatePort:
 * @obj: the network to allocate from
 * @port: the port definition to allocate
 *
 * Looks up the network reference by port, allocates a physical
 * device from that network (if appropriate), and returns with the
 * port configuration filled in accordingly.
 *
 * Returns 0 on success, -1 on failure.
 */
static int
networkAllocatePort(virNetworkObjPtr obj,
                    virNetworkPortDefPtr port)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkDefPtr netdef = NULL;
    virPortGroupDefPtr portgroup = NULL;
    virNetworkForwardIfDefPtr dev = NULL;
    size_t i;
    virNetDevVPortProfilePtr portprofile = NULL;

    netdef = virNetworkObjGetDef(obj);
    VIR_DEBUG("Allocating port from net %s", netdef->name);

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       netdef->name);
        return -1;
    }

    VIR_DEBUG("Interface port group %s", port->group);
    /* portgroup can be present for any type of network, in particular
     * for bandwidth information, so we need to check for that and
     * fill it in appropriately for all forward types.
     */
    portgroup = virPortGroupFindByName(netdef, port->group);

    if (!port->bandwidth) {
        if (portgroup && portgroup->bandwidth &&
            virNetDevBandwidthCopy(&port->bandwidth,
                                   portgroup->bandwidth) < 0)
            return -1;
    }

    if (port->vlan.nTags == 0) {
        virNetDevVlanPtr vlan = NULL;
        if (portgroup && portgroup->vlan.nTags > 0)
            vlan = &portgroup->vlan;
        else if (netdef->vlan.nTags > 0)
            vlan = &netdef->vlan;

        if (vlan && virNetDevVlanCopy(&port->vlan, vlan) < 0)
            return -1;
    }

    if (!port->trustGuestRxFilters) {
        if (portgroup && portgroup->trustGuestRxFilters)
            port->trustGuestRxFilters = portgroup->trustGuestRxFilters;
        else if (netdef->trustGuestRxFilters)
            port->trustGuestRxFilters = netdef->trustGuestRxFilters;
    }

    /* merge virtualports from interface, network, and portgroup to
     * arrive at actual virtualport to use
     */
    if (virNetDevVPortProfileMerge3(&portprofile,
                                    port->virtPortProfile,
                                    netdef->virtPortProfile,
                                    portgroup
                                    ? portgroup->virtPortProfile : NULL) < 0) {
                return -1;
    }
    if (portprofile) {
        VIR_FREE(port->virtPortProfile);
        port->virtPortProfile = portprofile;
    }

    VIR_DEBUG("Processing forward type %d", netdef->forward.type);
    switch ((virNetworkForwardType) netdef->forward.type) {
    case VIR_NETWORK_FORWARD_NONE:
    case VIR_NETWORK_FORWARD_NAT:
    case VIR_NETWORK_FORWARD_ROUTE:
    case VIR_NETWORK_FORWARD_OPEN:
        /* for these forward types, the actual net type really *is*
         * NETWORK; we just keep the info from the portgroup in
         * iface->data.network.actual
         */
        port->plugtype = VIR_NETWORK_PORT_PLUG_TYPE_NETWORK;

        port->plug.bridge.brname = g_strdup(netdef->bridge);
        port->plug.bridge.macTableManager = netdef->macTableManager;

        if (port->virtPortProfile) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("<virtualport type='%s'> not supported for network "
                             "'%s' which uses IP forwarding"),
                           virNetDevVPortTypeToString(port->virtPortProfile->virtPortType),
                           netdef->name);
            return -1;
        }

        if (networkPlugBandwidth(obj, &port->mac, port->bandwidth, &port->class_id) < 0)
            return -1;
        break;

    case VIR_NETWORK_FORWARD_HOSTDEV: {
        port->plugtype = VIR_NETWORK_PORT_PLUG_TYPE_HOSTDEV_PCI;

        if (networkCreateInterfacePool(netdef) < 0)
            return -1;

        /* pick first dev with 0 connections */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].connections == 0) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' requires exclusive access "
                             "to interfaces, but none are available"),
                           netdef->name);
            return -1;
        }
        port->plug.hostdevpci.addr = dev->device.pci;
        port->plug.hostdevpci.driver = netdef->forward.driverName;
        port->plug.hostdevpci.managed = netdef->forward.managed;

        if (port->virtPortProfile) {
            /* make sure type is supported for hostdev connections */
            if (port->virtPortProfile->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBG &&
                port->virtPortProfile->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBH) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("<virtualport type='%s'> not supported for network "
                                 "'%s' which uses an SR-IOV Virtual Function "
                                 "via PCI passthrough"),
                               virNetDevVPortTypeToString(port->virtPortProfile->virtPortType),
                               netdef->name);
                return -1;
            }
        }
        break;
    }

    case VIR_NETWORK_FORWARD_BRIDGE:
        if (netdef->bridge) {
            /* <forward type='bridge'/> <bridge name='xxx'/>
             * is VIR_DOMAIN_NET_TYPE_BRIDGE
             */

            port->plugtype = VIR_NETWORK_PORT_PLUG_TYPE_BRIDGE;
            port->plug.bridge.brname = g_strdup(netdef->bridge);
            port->plug.bridge.macTableManager = netdef->macTableManager;

            if (port->virtPortProfile) {
                /* only type='openvswitch' is allowed for bridges */
                if (port->virtPortProfile->virtPortType != VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH) {
                    virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                                   _("<virtualport type='%s'> not supported for network "
                                     "'%s' which uses a bridge device"),
                                   virNetDevVPortTypeToString(port->virtPortProfile->virtPortType),
                                   netdef->name);
                    return -1;
                }
            }

            if (networkPlugBandwidth(obj, &port->mac, port->bandwidth, &port->class_id) < 0)
                return -1;
            break;
        }

        /* intentionally fall through to the direct case for
         * VIR_NETWORK_FORWARD_BRIDGE with no bridge device defined
         */
        G_GNUC_FALLTHROUGH;

    case VIR_NETWORK_FORWARD_PRIVATE:
    case VIR_NETWORK_FORWARD_VEPA:
    case VIR_NETWORK_FORWARD_PASSTHROUGH:
        /* <forward type='bridge|private|vepa|passthrough'> are all
         * VIR_DOMAIN_NET_TYPE_DIRECT.
         */

        /* Set type=direct and appropriate <source mode='xxx'/> */
        port->plugtype = VIR_NETWORK_PORT_PLUG_TYPE_DIRECT;

        /* NO need to check the value returned from virNetDevMacVLanModeTypeFromString
         * it must be valid for these forward type(bridge|private|vepa|passthrough)
         */
        port->plug.direct.mode =
            virNetDevMacVLanModeTypeFromString(virNetworkForwardTypeToString(netdef->forward.type));

        if (port->virtPortProfile) {
            /* make sure type is supported for macvtap connections */
            if (port->virtPortProfile->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBG &&
                port->virtPortProfile->virtPortType != VIR_NETDEV_VPORT_PROFILE_8021QBH) {
                virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                               _("<virtualport type='%s'> not supported for network "
                                 "'%s' which uses a macvtap device"),
                               virNetDevVPortTypeToString(port->virtPortProfile->virtPortType),
                               netdef->name);
                return -1;
            }
        }

        /* If there is only a single device, just return it (caller will detect
         * any error if exclusive use is required but could not be acquired).
         */
        if ((netdef->forward.nifs <= 0) && (netdef->forward.npfs <= 0)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' uses a direct mode, but "
                             "has no forward dev and no interface pool"),
                           netdef->name);
            return -1;
        } else {
            /* pick an interface from the pool */

            if (networkCreateInterfacePool(netdef) < 0)
                return -1;

            /* PASSTHROUGH mode, and PRIVATE Mode + 802.1Qbh both
             * require exclusive access to a device, so current
             * connections count must be 0.  Other modes can share, so
             * just search for the one with the lowest number of
             * connections.
             */
            if ((netdef->forward.type == VIR_NETWORK_FORWARD_PASSTHROUGH) ||
                ((netdef->forward.type == VIR_NETWORK_FORWARD_PRIVATE) &&
                 port->virtPortProfile &&
                 (port->virtPortProfile->virtPortType
                  == VIR_NETDEV_VPORT_PROFILE_8021QBH))) {

                /* pick first dev with 0 connections */
                for (i = 0; i < netdef->forward.nifs; i++) {
                    if (netdef->forward.ifs[i].connections == 0) {
                        dev = &netdef->forward.ifs[i];
                        break;
                    }
                }
            } else {
                /* pick least used dev */
                dev = &netdef->forward.ifs[0];
                for (i = 1; i < netdef->forward.nifs; i++) {
                    if (netdef->forward.ifs[i].connections < dev->connections)
                        dev = &netdef->forward.ifs[i];
                }
            }
            /* dev points at the physical device we want to use */
            if (!dev) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("network '%s' requires exclusive access "
                                 "to interfaces, but none are available"),
                               netdef->name);
                return -1;
            }
            port->plug.direct.linkdev = g_strdup(dev->device.dev);
        }
        break;

    case VIR_NETWORK_FORWARD_LAST:
    default:
        virReportEnumRangeError(virNetworkForwardType, netdef->forward.type);
        return -1;
    }

    if (virNetworkObjMacMgrAdd(obj, driver->dnsmasqStateDir,
                               port->ownername, &port->mac) < 0)
        return -1;

    if (virNetDevVPortProfileCheckComplete(port->virtPortProfile, true) < 0)
        return -1;

    netdef->connections++;
    if (dev)
        dev->connections++;
    /* finally we can call the 'plugged' hook script if any */
    if (networkRunHook(obj, port,
                       VIR_HOOK_NETWORK_OP_PORT_CREATED,
                       VIR_HOOK_SUBOP_BEGIN) < 0) {
        /* adjust for failure */
        netdef->connections--;
        if (dev)
            dev->connections--;
        return -1;
    }
    networkLogAllocation(netdef, dev, &port->mac, true);

    VIR_DEBUG("Port allocated");

    return 0;
}


/* networkNotifyPort:
 * @obj: the network to notify
 * @port: the port definition to notify
 *
 * Called to notify the network driver when libvirtd is restarted and
 * finds an already running domain. If appropriate it will force an
 * allocation of the actual->direct.linkdev to get everything back in
 * order.
 */
static int
networkNotifyPort(virNetworkObjPtr obj,
                  virNetworkPortDefPtr port)
{
    virNetworkDefPtr netdef;
    virNetworkForwardIfDefPtr dev = NULL;
    size_t i;

    netdef = virNetworkObjGetDef(obj);

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       netdef->name);
        return -1;
    }

    switch (port->plugtype) {
    case VIR_NETWORK_PORT_PLUG_TYPE_NONE:
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unexpectedly got a network port without a plug"));
        return -1;

    case VIR_NETWORK_PORT_PLUG_TYPE_NETWORK:
    case VIR_NETWORK_PORT_PLUG_TYPE_BRIDGE:
        /* see if we're connected to the correct bridge */
        if (!netdef->bridge) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unexpectedly got a network port without a network bridge"));
            return -1;
        }
        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_DIRECT:
        if (networkCreateInterfacePool(netdef) < 0)
            return -1;

        /* find the matching interface and increment its connections */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV &&
                STREQ(port->plug.direct.linkdev,
                      netdef->forward.ifs[i].device.dev)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }
        /* dev points at the physical device we want to use */
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have dev='%s' "
                             "in use by network port '%s'"),
                           netdef->name, port->plug.direct.linkdev,
                           port->uuid);
            return -1;
        }

        /* PASSTHROUGH mode and PRIVATE Mode + 802.1Qbh both require
         * exclusive access to a device, so current connections count
         * must be 0 in those cases.
         */
        if ((dev->connections > 0) &&
            ((netdef->forward.type == VIR_NETWORK_FORWARD_PASSTHROUGH) ||
             ((netdef->forward.type == VIR_NETWORK_FORWARD_PRIVATE) &&
              port->virtPortProfile &&
              (port->virtPortProfile->virtPortType == VIR_NETDEV_VPORT_PROFILE_8021QBH)))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' claims dev='%s' is already in "
                             "use by a different port"),
                           netdef->name, port->plug.direct.linkdev);
            return -1;
        }
        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_HOSTDEV_PCI:

        if (networkCreateInterfacePool(netdef) < 0)
            return -1;

        /* find the matching interface and increment its connections */
        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI &&
                virPCIDeviceAddressEqual(&port->plug.hostdevpci.addr,
                                         &netdef->forward.ifs[i].device.pci)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }
        /* dev points at the physical device we want to use */
        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have "
                             "PCI device %04x:%02x:%02x.%x in use by network port"),
                           netdef->name,
                           port->plug.hostdevpci.addr.domain,
                           port->plug.hostdevpci.addr.bus,
                           port->plug.hostdevpci.addr.slot,
                           port->plug.hostdevpci.addr.function);
            return -1;
        }

        /* PASSTHROUGH mode, PRIVATE Mode + 802.1Qbh, and hostdev (PCI
         * passthrough) all require exclusive access to a device, so
         * current connections count must be 0 in those cases.
         */
        if ((dev->connections > 0) &&
            netdef->forward.type == VIR_NETWORK_FORWARD_HOSTDEV) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' claims the PCI device at "
                             "domain=%d bus=%d slot=%d function=%d "
                             "is already in use by a different network port"),
                           netdef->name,
                           dev->device.pci.domain, dev->device.pci.bus,
                           dev->device.pci.slot, dev->device.pci.function);
            return -1;
        }

        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_LAST:
    default:
        virReportEnumRangeError(virNetworkPortPlugType, port->plugtype);
        return -1;
    }

    netdef->connections++;
    if (dev)
        dev->connections++;
    /* finally we can call the 'plugged' hook script if any */
    if (networkRunHook(obj, port, VIR_HOOK_NETWORK_OP_PORT_CREATED,
                       VIR_HOOK_SUBOP_BEGIN) < 0) {
        /* adjust for failure */
        if (dev)
            dev->connections--;
        netdef->connections--;
        return -1;
    }
    networkLogAllocation(netdef, dev, &port->mac, true);

    return 0;
}


/* networkReleasePort:
 * @obj: the network to release from
 * @port: the port definition to release
 *
 * Given a domain <interface> element that previously had its <actual>
 * element filled in (and possibly a physical device allocated to it),
 * free up the physical device for use by someone else, and free the
 * virDomainActualNetDef.
 *
 * Returns 0 on success, -1 on failure.
 */
static int
networkReleasePort(virNetworkObjPtr obj,
                   virNetworkPortDefPtr port)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkDefPtr netdef;
    virNetworkForwardIfDefPtr dev = NULL;
    size_t i;

    netdef = virNetworkObjGetDef(obj);

    switch ((virNetworkPortPlugType)port->plugtype) {
    case VIR_NETWORK_PORT_PLUG_TYPE_NONE:
        VIR_DEBUG("Releasing network device with no plug type");
        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_NETWORK:
    case VIR_NETWORK_PORT_PLUG_TYPE_BRIDGE:
        if (networkUnplugBandwidth(obj, port->bandwidth,
                                   &port->class_id) < 0)
            return -1;
        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_DIRECT:
        if (netdef->forward.nifs == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' uses a direct mode, but "
                             "has no forward dev and no interface pool"),
                           netdef->name);
            return -1;
        }

        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_NETDEV &&
                STREQ(port->plug.direct.linkdev, netdef->forward.ifs[i].device.dev)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }

        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have dev='%s' "
                             "in use by domain"),
                           netdef->name, port->plug.direct.linkdev);
            return -1;
        }
        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_HOSTDEV_PCI:
        if (netdef->forward.nifs == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' uses a hostdev mode, but "
                             "has no forward dev and no interface pool"),
                           netdef->name);
            return -1;
        }

        for (i = 0; i < netdef->forward.nifs; i++) {
            if (netdef->forward.ifs[i].type
                == VIR_NETWORK_FORWARD_HOSTDEV_DEVICE_PCI &&
                virPCIDeviceAddressEqual(&port->plug.hostdevpci.addr,
                                         &netdef->forward.ifs[i].device.pci)) {
                dev = &netdef->forward.ifs[i];
                break;
            }
        }

        if (!dev) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("network '%s' doesn't have "
                             "PCI device %04x:%02x:%02x.%x in use by domain"),
                           netdef->name,
                           port->plug.hostdevpci.addr.domain,
                           port->plug.hostdevpci.addr.bus,
                           port->plug.hostdevpci.addr.slot,
                           port->plug.hostdevpci.addr.function);
            return -1;
        }
        break;

    case VIR_NETWORK_PORT_PLUG_TYPE_LAST:
    default:
        virReportEnumRangeError(virNetworkPortPlugType, port->plugtype);
        return -1;
    }

    virNetworkObjMacMgrDel(obj, driver->dnsmasqStateDir, port->ownername, &port->mac);

    netdef->connections--;
    if (dev)
        dev->connections--;
    /* finally we can call the 'unplugged' hook script if any */
    networkRunHook(obj, port, VIR_HOOK_NETWORK_OP_PORT_DELETED,
                   VIR_HOOK_SUBOP_BEGIN);
    networkLogAllocation(netdef, dev, &port->mac, false);

    return 0;
}


/**
 * networkCheckBandwidth:
 * @net: network QoS
 * @ifaceBand: interface QoS (may be NULL if no QoS)
 * @oldBandwidth: new interface QoS (may be NULL if no QoS)
 * @ifaceMac: interface MAC (used in error messages for identification)
 * @new_rate: new rate for non guaranteed class
 *
 * Function checks if @ifaceBand can be satisfied on @net. However, sometimes it
 * may happen that the interface that @ifaceBand corresponds to is already
 * plugged into the @net and the bandwidth is to be updated. In that case we
 * need to check if new bandwidth can be satisfied. If that's the case
 * @ifaceBand should point to new bandwidth settings and @oldBandwidth to
 * current ones. If you want to suppress this functionality just pass
 * @oldBandwidth == NULL.
 *
 * Returns: -1 if plugging would overcommit network QoS
 *           0 if plugging is safe (@new_rate updated)
 *           1 if no QoS is set (@new_rate untouched)
 */
static int
networkCheckBandwidth(virNetworkObjPtr obj,
                      virNetDevBandwidthPtr ifaceBand,
                      virNetDevBandwidthPtr oldBandwidth,
                      virMacAddrPtr ifaceMac,
                      unsigned long long *new_rate)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virNetDevBandwidthPtr netBand = def->bandwidth;
    unsigned long long tmp_floor_sum = virNetworkObjGetFloorSum(obj);
    unsigned long long tmp_new_rate = 0;
    char ifmac[VIR_MAC_STRING_BUFLEN];

    virMacAddrFormat(ifaceMac, ifmac);

    if (ifaceBand && ifaceBand->in && ifaceBand->in->floor &&
        !(netBand && netBand->in)) {
        virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                       _("Invalid use of 'floor' on interface with MAC "
                         "address %s - network '%s' has no inbound QoS set"),
                       ifmac, def->name);
        return -1;
    }

    if (!netBand || !netBand->in) {
        VIR_DEBUG("No network bandwidth controls present");
        /* no QoS required, claim success */
        return 1;
    }
    if (((!ifaceBand || !ifaceBand->in || !ifaceBand->in->floor) &&
         (!oldBandwidth || !oldBandwidth->in || !oldBandwidth->in->floor))) {
        VIR_DEBUG("No old/new interface bandwidth floor");
        /* no QoS required, claim success */
        return 1;
    }

    tmp_new_rate = netBand->in->average;
    if (oldBandwidth && oldBandwidth->in)
        tmp_floor_sum -= oldBandwidth->in->floor;
    if (ifaceBand && ifaceBand->in)
        tmp_floor_sum += ifaceBand->in->floor;

    /* check against peak */
    if (netBand->in->peak) {
        tmp_new_rate = netBand->in->peak;
        if (tmp_floor_sum > netBand->in->peak) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("Cannot plug '%s' interface into '%s' because "
                             "new combined inbound floor=%llu would overcommit "
                             "peak=%llu on network '%s'"),
                           ifmac,
                           def->bridge,
                           tmp_floor_sum,
                           netBand->in->peak,
                           def->name);
            return -1;
        }
    } else if (tmp_floor_sum > netBand->in->average) {
        /* tmp_floor_sum can be between 'average' and 'peak' iff 'peak' is set.
         * Otherwise, tmp_floor_sum must be below 'average'. */
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("Cannot plug '%s' interface into '%s' because "
                         "new combined inbound floor=%llu would overcommit "
                         "average=%llu on network '%s'"),
                       ifmac,
                       def->bridge,
                       tmp_floor_sum,
                       netBand->in->average,
                       def->name);
        return -1;
    }

    if (new_rate)
        *new_rate = tmp_new_rate;

    return 0;
}


/**
 * networkNextClassID:
 * @net: network object
 *
 * Find next free class ID. @net is supposed
 * to be locked already. If there is a free ID,
 * it is marked as used and returned.
 *
 * Returns next free class ID or -1 if none is available.
 */
static ssize_t
networkNextClassID(virNetworkObjPtr obj)
{
    ssize_t ret = 0;
    virBitmapPtr classIdMap = virNetworkObjGetClassIdMap(obj);

    if ((ret = virBitmapNextClearBit(classIdMap, -1)) < 0)
        ret = virBitmapSize(classIdMap);

    if (virBitmapSetBitExpand(classIdMap, ret) < 0)
        return -1;

    return ret;
}


static int
networkPlugBandwidthImpl(virNetworkObjPtr obj,
                         virMacAddrPtr mac,
                         virNetDevBandwidthPtr ifaceBand,
                         unsigned int *class_id,
                         unsigned long long new_rate)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virBitmapPtr classIdMap = virNetworkObjGetClassIdMap(obj);
    unsigned long long tmp_floor_sum = virNetworkObjGetFloorSum(obj);
    ssize_t next_id = 0;
    int plug_ret;

    /* generate new class_id */
    if ((next_id = networkNextClassID(obj)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not generate next class ID"));
        return -1;
    }

    plug_ret = virNetDevBandwidthPlug(def->bridge, def->bandwidth,
                                      mac, ifaceBand, next_id);
    if (plug_ret < 0) {
        ignore_value(virNetDevBandwidthUnplug(def->bridge, next_id));
        return -1;
    }

    /* QoS was set, generate new class ID */
    *class_id = next_id;
    /* update sum of 'floor'-s of attached NICs */
    tmp_floor_sum += ifaceBand->in->floor;
    virNetworkObjSetFloorSum(obj, tmp_floor_sum);
    /* update status file */
    if (virNetworkObjSaveStatus(driver->stateDir, obj, network_driver->xmlopt) < 0) {
        ignore_value(virBitmapClearBit(classIdMap, next_id));
        tmp_floor_sum -= ifaceBand->in->floor;
        virNetworkObjSetFloorSum(obj, tmp_floor_sum);
        *class_id = 0;
        ignore_value(virNetDevBandwidthUnplug(def->bridge, next_id));
        return -1;
    }
    /* update rate for non guaranteed NICs */
    new_rate -= tmp_floor_sum;
    if (virNetDevBandwidthUpdateRate(def->bridge, 2,
                                     def->bandwidth, new_rate) < 0)
        VIR_WARN("Unable to update rate for 1:2 class on %s bridge",
                 def->bridge);

    return 0;
}


static int
networkPlugBandwidth(virNetworkObjPtr obj,
                     virMacAddrPtr mac,
                     virNetDevBandwidthPtr ifaceBand,
                     unsigned int *class_id)
{
    int plug_ret;
    unsigned long long new_rate = 0;
    char ifmac[VIR_MAC_STRING_BUFLEN];

    if ((plug_ret = networkCheckBandwidth(obj, ifaceBand, NULL,
                                          mac, &new_rate)) < 0) {
        /* helper reported error */
        return -1;
    }

    if (plug_ret > 0)
        /* no QoS needs to be set; claim success */
        return 0;

    virMacAddrFormat(mac, ifmac);

    if (networkPlugBandwidthImpl(obj, mac, ifaceBand, class_id, new_rate) < 0)
        return -1;

    return 0;
}


static int
networkUnplugBandwidth(virNetworkObjPtr obj,
                       virNetDevBandwidthPtr ifaceBand,
                       unsigned int *class_id)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);
    virBitmapPtr classIdMap = virNetworkObjGetClassIdMap(obj);
    unsigned long long tmp_floor_sum = virNetworkObjGetFloorSum(obj);
    virNetworkDriverStatePtr driver = networkGetDriver();
    int ret = 0;
    unsigned long long new_rate;

    if (class_id && *class_id) {
        if (!def->bandwidth || !def->bandwidth->in) {
            VIR_WARN("Network %s has no bandwidth but unplug requested",
                     def->name);
            return 0;
        }
        /* we must remove class from bridge */
        new_rate = def->bandwidth->in->average;

        if (def->bandwidth->in->peak > 0)
            new_rate = def->bandwidth->in->peak;

        ret = virNetDevBandwidthUnplug(def->bridge, *class_id);
        if (ret < 0)
            return ret;
        /* update sum of 'floor'-s of attached NICs */
        tmp_floor_sum -= ifaceBand->in->floor;
        virNetworkObjSetFloorSum(obj, tmp_floor_sum);

        /* return class ID */
        ignore_value(virBitmapClearBit(classIdMap, *class_id));
        /* update status file */
        if (virNetworkObjSaveStatus(driver->stateDir,
                                    obj, network_driver->xmlopt) < 0) {
            tmp_floor_sum += ifaceBand->in->floor;
            virNetworkObjSetFloorSum(obj, tmp_floor_sum);
            ignore_value(virBitmapSetBit(classIdMap, *class_id));
            return ret;
        }
        /* update rate for non guaranteed NICs */
        new_rate -= tmp_floor_sum;
        if (virNetDevBandwidthUpdateRate(def->bridge, 2,
                                         def->bandwidth, new_rate) < 0)
            VIR_WARN("Unable to update rate for 1:2 class on %s bridge",
                     def->bridge);
        /* no class is associated any longer */
        *class_id = 0;
    }

    return ret;
}


static void
networkNetworkObjTaint(virNetworkObjPtr obj,
                       virNetworkTaintFlags taint)
{
    virNetworkDefPtr def = virNetworkObjGetDef(obj);

    if (virNetworkObjTaint(obj, taint)) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(def->uuid, uuidstr);

        VIR_WARN("Network name='%s' uuid=%s is tainted: %s",
                 def->name, uuidstr, virNetworkTaintTypeToString(taint));
    }
}


static int
networkUpdatePortBandwidth(virNetworkObjPtr obj,
                           virMacAddrPtr mac,
                           unsigned int *class_id,
                           virNetDevBandwidthPtr oldBandwidth,
                           virNetDevBandwidthPtr newBandwidth)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkDefPtr def;
    unsigned long long tmp_floor_sum;
    unsigned long long new_rate = 0;
    unsigned long long old_floor, new_floor;
    int plug_ret;

    old_floor = new_floor = 0;

    if (oldBandwidth && oldBandwidth->in)
        old_floor = oldBandwidth->in->floor;
    if (newBandwidth && newBandwidth->in)
        new_floor = newBandwidth->in->floor;

    if (new_floor == old_floor)
        return 0;

    def = virNetworkObjGetDef(obj);

    if ((plug_ret = networkCheckBandwidth(obj, newBandwidth, oldBandwidth,
                                          mac, &new_rate)) < 0) {
        /* helper reported error */
        return -1;
    }

    if (plug_ret > 0) {
        /* no QoS needs to be set; claim success */
        return 0;
    }

    /* Okay, there are three possible scenarios: */

    if (old_floor > 0 && new_floor > 0) {
        /* Either we just need to update @floor .. */

        if (virNetDevBandwidthUpdateRate(def->bridge,
                                         *class_id,
                                         def->bandwidth,
                                         new_floor) < 0)
            return -1;

        tmp_floor_sum = virNetworkObjGetFloorSum(obj);
        tmp_floor_sum -= old_floor;
        tmp_floor_sum += new_floor;
        virNetworkObjSetFloorSum(obj, tmp_floor_sum);
        new_rate -= tmp_floor_sum;

        if (virNetDevBandwidthUpdateRate(def->bridge, 2,
                                         def->bandwidth, new_rate) < 0 ||
            virNetworkObjSaveStatus(driver->stateDir,
                                    obj, network_driver->xmlopt) < 0) {
            /* Ouch, rollback */
            tmp_floor_sum -= new_floor;
            tmp_floor_sum += old_floor;
            virNetworkObjSetFloorSum(obj, tmp_floor_sum);

            ignore_value(virNetDevBandwidthUpdateRate(def->bridge,
                                                      *class_id,
                                                      def->bandwidth,
                                                      old_floor));
            return -1;
        }
    } else if (new_floor > 0) {
        /* .. or we need to plug in new .. */

        if (networkPlugBandwidthImpl(obj, mac, newBandwidth,
                                     class_id,
                                     new_rate) < 0)
            return -1;
    } else {
        /* .. or unplug old. */

        if (networkUnplugBandwidth(obj, oldBandwidth, class_id) < 0)
            return -1;
    }

    return 0;
}


static virNetworkPortPtr
networkPortLookupByUUID(virNetworkPtr net,
                        const unsigned char *uuid)
{
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPortDefPtr portdef = NULL;
    virNetworkPortPtr ret = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virUUIDFormat(uuid, uuidstr);

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (!(portdef = virNetworkObjLookupPort(obj, uuid)))
        goto cleanup;

    if (virNetworkPortLookupByUUIDEnsureACL(net->conn, def, portdef) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    ret = virGetNetworkPort(net, uuid);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static virNetworkPortPtr
networkPortCreateXML(virNetworkPtr net,
                     const char *xmldesc,
                     unsigned int flags)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    g_autoptr(virNetworkPortDef) portdef = NULL;
    virNetworkPortPtr ret = NULL;
    int rc;

    virCheckFlags(VIR_NETWORK_PORT_CREATE_RECLAIM, NULL);

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (!(portdef = virNetworkPortDefParseString(xmldesc)))
        goto cleanup;

    if (virNetworkPortCreateXMLEnsureACL(net->conn, def, portdef) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    if (portdef->plugtype == VIR_NETWORK_PORT_PLUG_TYPE_NONE) {
        if (flags & VIR_NETWORK_PORT_CREATE_RECLAIM) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("Port reclaim requested but plug type is none"));
            goto cleanup;
        }
    } else {
        if (!(flags & VIR_NETWORK_PORT_CREATE_RECLAIM)) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("Port reclaim not requested but plug type is not none"));
            goto cleanup;
        }
    }

    if (flags & VIR_NETWORK_PORT_CREATE_RECLAIM)
        rc = networkNotifyPort(obj, portdef);
    else
        rc = networkAllocatePort(obj, portdef);
    if (rc < 0)
        goto cleanup;

    if (virNetworkObjAddPort(obj, portdef, driver->stateDir) < 0) {
        virErrorPtr save_err;

        virErrorPreserveLast(&save_err);
        ignore_value(networkReleasePort(obj, portdef));
        virErrorRestore(&save_err);

        goto cleanup;
    }

    ret = virGetNetworkPort(net, portdef->uuid);
    portdef = NULL;
 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static char *
networkPortGetXMLDesc(virNetworkPortPtr port,
                      unsigned int flags)
{
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPortDefPtr portdef = NULL;
    char *ret = NULL;

    virCheckFlags(0, NULL);

    if (!(obj = networkObjFromNetwork(port->net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (!(portdef = virNetworkObjLookupPort(obj, port->uuid)))
        goto cleanup;

    if (virNetworkPortGetXMLDescEnsureACL(port->net->conn, def, portdef) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

   if (!(ret = virNetworkPortDefFormat(portdef)))
       goto cleanup;

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkPortDelete(virNetworkPortPtr port,
                  unsigned int flags)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPortDefPtr portdef;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(obj = networkObjFromNetwork(port->net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (!(portdef = virNetworkObjLookupPort(obj, port->uuid)))
        goto cleanup;

    if (virNetworkPortDeleteEnsureACL(port->net->conn, def, portdef) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    if (networkReleasePort(obj, portdef) < 0)
        goto cleanup;

    virNetworkObjDeletePort(obj, port->uuid, driver->stateDir);

    ret = 0;
 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkPortSetParameters(virNetworkPortPtr port,
                         virTypedParameterPtr params,
                         int nparams,
                         unsigned int flags)
{
    virNetworkDriverStatePtr driver = networkGetDriver();
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPortDefPtr portdef;
    virNetDevBandwidthPtr bandwidth = NULL;
    char *dir = NULL;
    int ret = -1;
    size_t i;

    virCheckFlags(0, -1);

    if (!(obj = networkObjFromNetwork(port->net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (!(portdef = virNetworkObjLookupPort(obj, port->uuid)))
        goto cleanup;

    if (virNetworkPortSetParametersEnsureACL(port->net->conn, def, portdef) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    if (!(dir = virNetworkObjGetPortStatusDir(obj, driver->stateDir)))
        goto cleanup;

    if ((VIR_ALLOC(bandwidth) < 0) ||
        (VIR_ALLOC(bandwidth->in) < 0) ||
        (VIR_ALLOC(bandwidth->out) < 0))
        goto cleanup;

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_IN_AVERAGE)) {
            bandwidth->in->average = param->value.ui;
        } else if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_IN_PEAK)) {
            bandwidth->in->peak = param->value.ui;
        } else if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_IN_BURST)) {
            bandwidth->in->burst = param->value.ui;
        } else if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_IN_FLOOR)) {
            bandwidth->in->floor = param->value.ui;
        } else if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_OUT_AVERAGE)) {
            bandwidth->out->average = param->value.ui;
        } else if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_OUT_PEAK)) {
            bandwidth->out->peak = param->value.ui;
        } else if (STREQ(param->field, VIR_NETWORK_PORT_BANDWIDTH_OUT_BURST)) {
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

    if (networkUpdatePortBandwidth(obj,
                                   &portdef->mac,
                                   &portdef->class_id,
                                   portdef->bandwidth,
                                   bandwidth) < 0)
        goto cleanup;

    virNetDevBandwidthFree(portdef->bandwidth);
    portdef->bandwidth = bandwidth;
    bandwidth = NULL;

    if (virNetworkPortDefSaveStatus(portdef, dir) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virNetDevBandwidthFree(bandwidth);
    virNetworkObjEndAPI(&obj);
    VIR_FREE(dir);
    return ret;
}


static int
networkPortGetParameters(virNetworkPortPtr port,
                         virTypedParameterPtr *params,
                         int *nparams,
                         unsigned int flags)
{
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    virNetworkPortDefPtr portdef;
    int maxparams = 0;
    int ret = -1;

    virCheckFlags(0, -1);

    *params = NULL;
    *nparams = 0;

    if (!(obj = networkObjFromNetwork(port->net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (!(portdef = virNetworkObjLookupPort(obj, port->uuid)))
        goto cleanup;

    if (virNetworkPortGetParametersEnsureACL(port->net->conn, def, portdef) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    if (portdef->bandwidth) {
        if ((portdef->bandwidth->in != NULL) &&
            (virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_IN_AVERAGE,
                                   portdef->bandwidth->in->average) < 0 ||
             virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_IN_PEAK,
                                   portdef->bandwidth->in->peak) < 0 ||
             virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_IN_FLOOR,
                                   portdef->bandwidth->in->floor) < 0 ||
             virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_IN_BURST,
                                   portdef->bandwidth->in->burst) < 0))
            goto cleanup;

        if ((portdef->bandwidth->out != NULL) &&
            (virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_OUT_AVERAGE,
                                   portdef->bandwidth->out->average) < 0 ||
             virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_OUT_PEAK,
                                   portdef->bandwidth->out->peak) < 0 ||
             virTypedParamsAddUInt(params, nparams, &maxparams,
                                   VIR_NETWORK_PORT_BANDWIDTH_OUT_BURST,
                                   portdef->bandwidth->out->burst) < 0))
            goto cleanup;
    }

    ret = 0;
 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static int
networkListAllPorts(virNetworkPtr net,
                    virNetworkPortPtr **ports,
                    unsigned int flags)
{
    virNetworkObjPtr obj;
    virNetworkDefPtr def;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(obj = networkObjFromNetwork(net)))
        return ret;

    def = virNetworkObjGetDef(obj);

    if (virNetworkListAllPortsEnsureACL(net->conn, def) < 0)
        goto cleanup;

    if (!virNetworkObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("network '%s' is not active"),
                       def->name);
        goto cleanup;
    }

    ret = virNetworkObjPortListExport(net, obj, ports,
                                      virNetworkListAllPortsCheckACL);

 cleanup:
    virNetworkObjEndAPI(&obj);
    return ret;
}


static virNetworkDriver networkDriver = {
    .name = "bridge",
    .connectNumOfNetworks = networkConnectNumOfNetworks, /* 0.2.0 */
    .connectListNetworks = networkConnectListNetworks, /* 0.2.0 */
    .connectNumOfDefinedNetworks = networkConnectNumOfDefinedNetworks, /* 0.2.0 */
    .connectListDefinedNetworks = networkConnectListDefinedNetworks, /* 0.2.0 */
    .connectListAllNetworks = networkConnectListAllNetworks, /* 0.10.2 */
    .connectNetworkEventRegisterAny = networkConnectNetworkEventRegisterAny, /* 1.2.1 */
    .connectNetworkEventDeregisterAny = networkConnectNetworkEventDeregisterAny, /* 1.2.1 */
    .networkLookupByUUID = networkLookupByUUID, /* 0.2.0 */
    .networkLookupByName = networkLookupByName, /* 0.2.0 */
    .networkCreateXML = networkCreateXML, /* 0.2.0 */
    .networkDefineXML = networkDefineXML, /* 0.2.0 */
    .networkUndefine = networkUndefine, /* 0.2.0 */
    .networkUpdate = networkUpdate, /* 0.10.2 */
    .networkCreate = networkCreate, /* 0.2.0 */
    .networkDestroy = networkDestroy, /* 0.2.0 */
    .networkGetXMLDesc = networkGetXMLDesc, /* 0.2.0 */
    .networkGetBridgeName = networkGetBridgeName, /* 0.2.0 */
    .networkGetAutostart = networkGetAutostart, /* 0.2.1 */
    .networkSetAutostart = networkSetAutostart, /* 0.2.1 */
    .networkIsActive = networkIsActive, /* 0.7.3 */
    .networkIsPersistent = networkIsPersistent, /* 0.7.3 */
    .networkGetDHCPLeases = networkGetDHCPLeases, /* 1.2.6 */
    .networkPortLookupByUUID = networkPortLookupByUUID, /* 5.5.0 */
    .networkPortCreateXML = networkPortCreateXML, /* 5.5.0 */
    .networkPortGetXMLDesc = networkPortGetXMLDesc, /* 5.5.0 */
    .networkPortDelete = networkPortDelete, /* 5.5.0 */
    .networkListAllPorts = networkListAllPorts, /* 5.5.0 */
    .networkPortGetParameters = networkPortGetParameters, /* 5.5.0 */
    .networkPortSetParameters = networkPortSetParameters, /* 5.5.0 */
};


static virHypervisorDriver networkHypervisorDriver = {
    .name = "network",
    .connectOpen = networkConnectOpen, /* 4.1.0 */
    .connectClose = networkConnectClose, /* 4.1.0 */
    .connectIsEncrypted = networkConnectIsEncrypted, /* 4.1.0 */
    .connectIsSecure = networkConnectIsSecure, /* 4.1.0 */
    .connectIsAlive = networkConnectIsAlive, /* 4.1.0 */
};


static virConnectDriver networkConnectDriver = {
    .localOnly = true,
    .uriSchemes = (const char *[]){ "network", NULL },
    .hypervisorDriver = &networkHypervisorDriver,
    .networkDriver = &networkDriver,
};


static virStateDriver networkStateDriver = {
    .name = "bridge",
    .stateInitialize  = networkStateInitialize,
    .stateCleanup = networkStateCleanup,
    .stateReload = networkStateReload,
};

int
networkRegister(void)
{
    if (virRegisterConnectDriver(&networkConnectDriver, false) < 0)
        return -1;
    if (virSetSharedNetworkDriver(&networkDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&networkStateDriver) < 0)
        return -1;
    return 0;
}
