/*
 * config.c: VM configuration management
 *
 * Copyright (C) 2006, 2007 Red Hat, Inc.
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <dirent.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/utsname.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>

#include <libvirt/virterror.h>

#include "qemu_conf.h"
#include "uuid.h"
#include "buf.h"

#define qemudLog(level, msg...) fprintf(stderr, msg)

void qemudReportError(virConnectPtr conn,
                      virDomainPtr dom,
                      virNetworkPtr net,
                      int code, const char *fmt, ...) {
    va_list args;
    char errorMessage[QEMUD_MAX_ERROR_LEN];

    if (fmt) {
        va_start(args, fmt);
        vsnprintf(errorMessage, QEMUD_MAX_ERROR_LEN-1, fmt, args);
        va_end(args);
    } else {
        errorMessage[0] = '\0';
    }
    __virRaiseError(conn, dom, net, VIR_FROM_QEMU, code, VIR_ERR_ERROR,
                    NULL, NULL, NULL, -1, -1, errorMessage);
}

struct qemud_vm *qemudFindVMByID(const struct qemud_driver *driver, int id) {
    struct qemud_vm *vm = driver->vms;

    while (vm) {
        if (qemudIsActiveVM(vm) && vm->id == id)
            return vm;
        vm = vm->next;
    }

    return NULL;
}

struct qemud_vm *qemudFindVMByUUID(const struct qemud_driver *driver,
                                   const unsigned char *uuid) {
    struct qemud_vm *vm = driver->vms;

    while (vm) {
        if (!memcmp(vm->def->uuid, uuid, VIR_UUID_BUFLEN))
            return vm;
        vm = vm->next;
    }

    return NULL;
}

struct qemud_vm *qemudFindVMByName(const struct qemud_driver *driver,
                                   const char *name) {
    struct qemud_vm *vm = driver->vms;

    while (vm) {
        if (!strcmp(vm->def->name, name))
            return vm;
        vm = vm->next;
    }

    return NULL;
}

struct qemud_network *qemudFindNetworkByUUID(const struct qemud_driver *driver,
                                             const unsigned char *uuid) {
    struct qemud_network *network = driver->networks;

    while (network) {
        if (!memcmp(network->def->uuid, uuid, VIR_UUID_BUFLEN))
            return network;
        network = network->next;
    }

    return NULL;
}

struct qemud_network *qemudFindNetworkByName(const struct qemud_driver *driver,
                                             const char *name) {
    struct qemud_network *network = driver->networks;

    while (network) {
        if (!strcmp(network->def->name, name))
            return network;
        network = network->next;
    }

    return NULL;
}


/* Free all memory associated with a struct qemud_vm object */
void qemudFreeVMDef(struct qemud_vm_def *def) {
    struct qemud_vm_disk_def *disk = def->disks;
    struct qemud_vm_net_def *net = def->nets;
    struct qemud_vm_input_def *input = def->inputs;

    while (disk) {
        struct qemud_vm_disk_def *prev = disk;
        disk = disk->next;
        free(prev);
    }
    while (net) {
        struct qemud_vm_net_def *prev = net;
        net = net->next;
        free(prev);
    }
    while (input) {
        struct qemud_vm_input_def *prev = input;
        input = input->next;
        free(prev);
    }
    free(def);
}

void qemudFreeVM(struct qemud_vm *vm) {
    qemudFreeVMDef(vm->def);
    if (vm->newDef)
        qemudFreeVMDef(vm->newDef);
    free(vm);
}

/* Build up a fully qualfiied path for a config file to be
 * associated with a persistent guest or network */
static int
qemudMakeConfigPath(const char *configDir,
                    const char *name,
                    const char *ext,
                    char *buf,
                    unsigned int buflen) {
    if ((strlen(configDir) + 1 + strlen(name) + (ext ? strlen(ext) : 0) + 1) > buflen)
        return -1;

    strcpy(buf, configDir);
    strcat(buf, "/");
    strcat(buf, name);
    if (ext)
        strcat(buf, ext);
    return 0;
}

int
qemudEnsureDir(const char *path)
{
    struct stat st;
    char parent[PATH_MAX];
    char *p;
    int err;

    if (stat(path, &st) >= 0)
        return 0;

    strncpy(parent, path, PATH_MAX);
    parent[PATH_MAX - 1] = '\0';

    if (!(p = strrchr(parent, '/')))
        return EINVAL;

    if (p == parent)
        return EPERM;

    *p = '\0';

    if ((err = qemudEnsureDir(parent)))
        return err;

    if (mkdir(path, 0777) < 0 && errno != EEXIST)
        return errno;

    return 0;
}

/* The list of possible machine types for various architectures,
   as supported by QEMU - taken from 'qemu -M ?' for each arch */
static const char *arch_info_x86_machines[] = {
    "pc", "isapc", NULL
};
static const char *arch_info_mips_machines[] = {
    "mips", NULL
};
static const char *arch_info_sparc_machines[] = {
    "sun4m", NULL
};
static const char *arch_info_ppc_machines[] = {
    "g3bw", "mac99", "prep", NULL
};

/* Feature flags for the architecture info */
struct qemu_feature_flags arch_info_i686_flags [] = {
    { "pae",  1, 1 },
    { "acpi", 1, 1 },
    { "apic", 1, 0 },
    { NULL, -1, -1 }
};

struct qemu_feature_flags arch_info_x86_64_flags [] = {
    { "acpi", 1, 1 },
    { "apic", 1, 0 },
    { NULL, -1, -1 }
};

/* The archicture tables for supported QEMU archs */
struct qemu_arch_info qemudArchs[] = { 
    /* i686 must be in position 0 */
    {  "i686", 32, arch_info_x86_machines, "qemu", arch_info_i686_flags },
    /* x86_64 must be in position 1 */
    {  "x86_64", 64, arch_info_x86_machines, "qemu-system-x86_64", arch_info_x86_64_flags },
    {  "mips", 32, arch_info_mips_machines, "qemu-system-mips", NULL },
    {  "mipsel", 32, arch_info_mips_machines, "qemu-system-mipsel", NULL },
    {  "sparc", 32, arch_info_sparc_machines, "qemu-system-sparc", NULL },
    {  "ppc", 32, arch_info_ppc_machines, "qemu-system-ppc", NULL },
    { NULL, -1, NULL, NULL, NULL }
};

/* Return the default architecture if none is explicitly requested*/
static const char *qemudDefaultArch(void) {
    return qemudArchs[0].arch;
}

/* Return the default machine type for a given architecture */
static const char *qemudDefaultMachineForArch(const char *arch) {
    int i;

    for (i = 0; qemudArchs[i].arch; i++) {
        if (!strcmp(qemudArchs[i].arch, arch)) {
            return qemudArchs[i].machines[0];
        }
    }

    return NULL;
}

/* Return the default binary name for a particular architecture */
static const char *qemudDefaultBinaryForArch(const char *arch) {
    int i;

    for (i = 0 ; qemudArchs[i].arch; i++) {
        if (!strcmp(qemudArchs[i].arch, arch)) {
            return qemudArchs[i].binary;
        }
    }

    return NULL;
}

/* Find the fully qualified path to the binary for an architecture */
static char *qemudLocateBinaryForArch(virConnectPtr conn,
                                      int virtType, const char *arch) {
    const char *name;
    char *path;

    if (virtType == QEMUD_VIRT_KVM)
        name = "qemu-kvm";
    else
        name = qemudDefaultBinaryForArch(arch);

    if (!name) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "cannot determin binary for architecture %s", arch);
        return NULL;
    }

    /* XXX lame. should actually use $PATH ... */
    path = malloc(strlen(name) + strlen("/usr/bin/") + 1);
    if (!path) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "path");
        return NULL;
    }
    strcpy(path, "/usr/bin/");
    strcat(path, name);
    return path;
}


static int qemudExtractVersionInfo(const char *qemu, int *version, int *flags) {
    pid_t child;
    int newstdout[2];

    *flags = 0;
    *version = 0;

    if (pipe(newstdout) < 0) {
        return -1;
    }

    if ((child = fork()) < 0) {
        close(newstdout[0]);
        close(newstdout[1]);
        return -1;
    }

    if (child == 0) { /* Kid */
        if (close(STDIN_FILENO) < 0)
            goto cleanup1;
        if (close(STDERR_FILENO) < 0)
            goto cleanup1;
        if (close(newstdout[0]) < 0)
            goto cleanup1;
        if (dup2(newstdout[1], STDOUT_FILENO) < 0)
            goto cleanup1;

        /* Just in case QEMU is translated someday.. */
        setenv("LANG", "C", 1);
        execl(qemu, qemu, (char*)NULL);

    cleanup1:
        _exit(-1); /* Just in case */
    } else { /* Parent */
        char help[8192]; /* Ought to be enough to hold QEMU help screen */
        int got = 0, ret = -1;
        int major, minor, micro;

        if (close(newstdout[1]) < 0)
            goto cleanup2;

        while (got < (sizeof(help)-1)) {
            int len;
            if ((len = read(newstdout[0], help+got, sizeof(help)-got-1)) <= 0) {
                if (!len)
                    break;
                if (errno == EINTR)
                    continue;
                goto cleanup2;
            }
            got += len;
        }
        help[got] = '\0';

        if (sscanf(help, "QEMU PC emulator version %d.%d.%d", &major,&minor, &micro) != 3) {
            goto cleanup2;
        }

        *version = (major * 1000 * 1000) + (minor * 1000) + micro;
        if (strstr(help, "-no-kqemu"))
            *flags |= QEMUD_CMD_FLAG_KQEMU;
        if (strstr(help, "-no-reboot"))
            *flags |= QEMUD_CMD_FLAG_NO_REBOOT;
        if (*version >= 9000)
            *flags |= QEMUD_CMD_FLAG_VNC_COLON;
        ret = 0;

        qemudDebug("Version %d %d %d  Cooked version: %d, with flags ? %d",
                   major, minor, micro, *version, *flags);

    cleanup2:
        if (close(newstdout[0]) < 0)
            ret = -1;

    rewait:
        if (waitpid(child, &got, 0) != child) {
            if (errno == EINTR) {
                goto rewait;
            }
            qemudLog(QEMUD_ERR, "Unexpected exit status from qemu %d pid %lu", got, (unsigned long)child);
            ret = -1;
        }
        /* Check & log unexpected exit status, but don't fail,
         * as there's really no need to throw an error if we did
         * actually read a valid version number above */
        if (WEXITSTATUS(got) != 1) {
            qemudLog(QEMUD_WARN, "Unexpected exit status '%d', qemu probably failed", got);
        }

        return ret;
    }
}

int qemudExtractVersion(virConnectPtr conn,
                        struct qemud_driver *driver ATTRIBUTE_UNUSED) {
    char *binary = NULL;
    struct stat sb;
    int ignored;

    if (driver->qemuVersion > 0)
        return 0;

    if (!(binary = qemudLocateBinaryForArch(conn, QEMUD_VIRT_QEMU, "i686")))
        return -1;

    if (stat(binary, &sb) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "Cannot find QEMU binary %s: %s", binary,
                         strerror(errno));
        free(binary);
        return -1;
    }

    if (qemudExtractVersionInfo(binary, &driver->qemuVersion, &ignored) < 0) {
        free(binary);
        return -1;
    }

    free(binary);
    return 0;
}


/* Parse the XML definition for a disk */
static struct qemud_vm_disk_def *qemudParseDiskXML(virConnectPtr conn,
                                                   struct qemud_driver *driver ATTRIBUTE_UNUSED,
                                                   xmlNodePtr node) {
    struct qemud_vm_disk_def *disk = calloc(1, sizeof(struct qemud_vm_disk_def));
    xmlNodePtr cur;
    xmlChar *device = NULL;
    xmlChar *source = NULL;
    xmlChar *target = NULL;
    xmlChar *type = NULL;
    int typ = 0;

    if (!disk) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "disk");
        return NULL;
    }

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "file"))
            typ = QEMUD_DISK_FILE;
        else if (xmlStrEqual(type, BAD_CAST "block"))
            typ = QEMUD_DISK_BLOCK;
        else {
            typ = QEMUD_DISK_FILE;
        }
        xmlFree(type);
        type = NULL;
    }

    device = xmlGetProp(node, BAD_CAST "device");
  
    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {
	
                if (typ == QEMUD_DISK_FILE)
                    source = xmlGetProp(cur, BAD_CAST "file");
                else
                    source = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((target == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                target = xmlGetProp(cur, BAD_CAST "dev");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                disk->readonly = 1;
            }
        }
        cur = cur->next;
    }

    if (source == NULL) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_SOURCE, target ? "%s" : NULL, target);
        goto error;
    }
    if (target == NULL) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_TARGET, source ? "%s" : NULL, source);
        goto error;
    }

    if (device &&
        !strcmp((const char *)device, "floppy") &&
        strcmp((const char *)target, "fda") &&
        strcmp((const char *)target, "fdb")) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "Invalid floppy device name: %s", target);
        goto error;
    }
  
    if (device &&
        !strcmp((const char *)device, "cdrom") &&
        strcmp((const char *)target, "hdc")) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "Invalid cdrom device name: %s", target);
        goto error;
    }

    if (device &&
        !strcmp((const char *)device, "cdrom"))
        disk->readonly = 1;

    if ((!device || !strcmp((const char *)device, "disk")) &&
        strcmp((const char *)target, "hda") &&
        strcmp((const char *)target, "hdb") &&
        strcmp((const char *)target, "hdc") &&
        strcmp((const char *)target, "hdd")) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "Invalid harddisk device name: %s", target);
        goto error;
    }

    strncpy(disk->src, (const char *)source, NAME_MAX-1);
    disk->src[NAME_MAX-1] = '\0';

    strncpy(disk->dst, (const char *)target, NAME_MAX-1);
    disk->dst[NAME_MAX-1] = '\0';
    disk->type = typ;

    if (!device)
        disk->device = QEMUD_DISK_DISK;
    else if (!strcmp((const char *)device, "disk"))
        disk->device = QEMUD_DISK_DISK;
    else if (!strcmp((const char *)device, "cdrom"))
        disk->device = QEMUD_DISK_CDROM;
    else if (!strcmp((const char *)device, "floppy"))
        disk->device = QEMUD_DISK_FLOPPY;
    else {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "Invalid device type: %s", device);
        goto error;
    }

    xmlFree(device);
    xmlFree(target);
    xmlFree(source);

    return disk;

 error:
    if (type)
        xmlFree(type);
    if (target)
        xmlFree(target);
    if (source)
        xmlFree(source);
    if (device)
        xmlFree(device);
    free(disk);
    return NULL;
}

static void qemudRandomMAC(struct qemud_vm_net_def *net) {
    net->mac[0] = 0x52;
    net->mac[1] = 0x54;
    net->mac[2] = 0x00;
    net->mac[3] = 1 + (int)(256*(rand()/(RAND_MAX+1.0)));
    net->mac[4] = 1 + (int)(256*(rand()/(RAND_MAX+1.0)));
    net->mac[5] = 1 + (int)(256*(rand()/(RAND_MAX+1.0)));
}


/* Parse the XML definition for a network interface */
static struct qemud_vm_net_def *qemudParseInterfaceXML(virConnectPtr conn,
                                                       struct qemud_driver *driver ATTRIBUTE_UNUSED,
                                                       xmlNodePtr node) {
    struct qemud_vm_net_def *net = calloc(1, sizeof(struct qemud_vm_net_def));
    xmlNodePtr cur;
    xmlChar *macaddr = NULL;
    xmlChar *type = NULL;
    xmlChar *network = NULL;
    xmlChar *bridge = NULL;
    xmlChar *ifname = NULL;
    xmlChar *script = NULL;
    xmlChar *address = NULL;
    xmlChar *port = NULL;

    if (!net) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "net");
        return NULL;
    }

    net->type = QEMUD_NET_USER;

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "user"))
            net->type = QEMUD_NET_USER;
        else if (xmlStrEqual(type, BAD_CAST "ethernet"))
            net->type = QEMUD_NET_ETHERNET;
        else if (xmlStrEqual(type, BAD_CAST "server"))
            net->type = QEMUD_NET_SERVER;
        else if (xmlStrEqual(type, BAD_CAST "client"))
            net->type = QEMUD_NET_CLIENT;
        else if (xmlStrEqual(type, BAD_CAST "mcast"))
            net->type = QEMUD_NET_MCAST;
        else if (xmlStrEqual(type, BAD_CAST "network"))
            net->type = QEMUD_NET_NETWORK;
        else if (xmlStrEqual(type, BAD_CAST "bridge"))
            net->type = QEMUD_NET_BRIDGE;
        else
            net->type = QEMUD_NET_USER;
        xmlFree(type);
        type = NULL;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((macaddr == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "mac"))) {
                macaddr = xmlGetProp(cur, BAD_CAST "address");
            } else if ((network == NULL) &&
                       (net->type == QEMUD_NET_NETWORK) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                network = xmlGetProp(cur, BAD_CAST "network");
            } else if ((network == NULL) &&
                       (net->type == QEMUD_NET_BRIDGE) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                bridge = xmlGetProp(cur, BAD_CAST "bridge");
            } else if ((network == NULL) &&
                       ((net->type == QEMUD_NET_SERVER) ||
                        (net->type == QEMUD_NET_CLIENT) ||
                        (net->type == QEMUD_NET_MCAST)) &&
                       (xmlStrEqual(cur->name, BAD_CAST "source"))) {
                address = xmlGetProp(cur, BAD_CAST "address");
                port = xmlGetProp(cur, BAD_CAST "port");
            } else if ((ifname == NULL) &&
                       ((net->type == QEMUD_NET_NETWORK) ||
                        (net->type == QEMUD_NET_ETHERNET) ||
                        (net->type == QEMUD_NET_BRIDGE)) &&
                       xmlStrEqual(cur->name, BAD_CAST "target")) {
                ifname = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((script == NULL) &&
                       (net->type == QEMUD_NET_ETHERNET) &&
                       xmlStrEqual(cur->name, BAD_CAST "script")) {
                script = xmlGetProp(cur, BAD_CAST "path");
            }
        }
        cur = cur->next;
    }

    if (macaddr) {
        unsigned int mac[6];
        sscanf((const char *)macaddr, "%02x:%02x:%02x:%02x:%02x:%02x",
               (unsigned int*)&mac[0],
               (unsigned int*)&mac[1],
               (unsigned int*)&mac[2],
               (unsigned int*)&mac[3],
               (unsigned int*)&mac[4],
               (unsigned int*)&mac[5]);
        net->mac[0] = mac[0];
        net->mac[1] = mac[1];
        net->mac[2] = mac[2];
        net->mac[3] = mac[3];
        net->mac[4] = mac[4];
        net->mac[5] = mac[5];

        xmlFree(macaddr);
        macaddr = NULL;
    } else {
        qemudRandomMAC(net);
    }

    if (net->type == QEMUD_NET_NETWORK) {
        int len;

        if (network == NULL) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "No <source> 'network' attribute specified with <interface type='network'/>");
            goto error;
        } else if ((len = xmlStrlen(network)) >= (QEMUD_MAX_NAME_LEN-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Network name '%s' too long", network);
            goto error;
        } else {
            strncpy(net->dst.network.name, (char *)network, len);
            net->dst.network.name[len] = '\0';
        }

        if (network) {
            xmlFree(network);
            network = NULL;
        }

        if (ifname != NULL) {
            if ((len = xmlStrlen(ifname)) >= (BR_IFNAME_MAXLEN-1)) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 "TAP interface name '%s' is too long", ifname);
                goto error;
            } else {
                strncpy(net->dst.network.ifname, (char *)ifname, len);
                net->dst.network.ifname[len] = '\0';
            }
            xmlFree(ifname);
            ifname = NULL;
        }
    } else if (net->type == QEMUD_NET_ETHERNET) {
        int len;

        if (script != NULL) {
            if ((len = xmlStrlen(script)) >= (PATH_MAX-1)) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 "TAP script path '%s' is too long", script);
                goto error;
            } else {
                strncpy(net->dst.ethernet.script, (char *)script, len);
                net->dst.ethernet.script[len] = '\0';
            }
            xmlFree(script);
            script = NULL;
        }
        if (ifname != NULL) {
            if ((len = xmlStrlen(ifname)) >= (BR_IFNAME_MAXLEN-1)) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 "TAP interface name '%s' is too long", ifname);
                goto error;
            } else {
                strncpy(net->dst.ethernet.ifname, (char *)ifname, len);
                net->dst.ethernet.ifname[len] = '\0';
            }
            xmlFree(ifname);
        }
    } else if (net->type == QEMUD_NET_BRIDGE) {
        int len;

        if (bridge == NULL) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "No <source> 'dev' attribute specified with <interface type='bridge'/>");
            goto error;
        } else if ((len = xmlStrlen(bridge)) >= (BR_IFNAME_MAXLEN-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "TAP bridge path '%s' is too long", bridge);
            goto error;
        } else {
            strncpy(net->dst.bridge.brname, (char *)bridge, len);
            net->dst.bridge.brname[len] = '\0';
        }

        xmlFree(bridge);
        bridge = NULL;

        if (ifname != NULL) {
            if ((len = xmlStrlen(ifname)) >= (BR_IFNAME_MAXLEN-1)) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 "TAP interface name '%s' is too long", ifname);
                goto error;
            } else {
                strncpy(net->dst.bridge.ifname, (char *)ifname, len);
                net->dst.bridge.ifname[len] = '\0';
            }
            xmlFree(ifname);
        }
    } else if (net->type == QEMUD_NET_CLIENT ||
               net->type == QEMUD_NET_SERVER ||
               net->type == QEMUD_NET_MCAST) {
        int len = 0;
        char *ret;

        if (port == NULL) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "No <source> 'port' attribute specified with socket interface");
            goto error;
        }
        if (!(net->dst.socket.port = strtol((char*)port, &ret, 10)) &&
            ret == (char*)port) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Cannot parse <source> 'port' attribute with socket interface");
            goto error;
        }
        xmlFree(port);
        port = NULL;

        if (address == NULL) {
            if (net->type == QEMUD_NET_CLIENT ||
                net->type == QEMUD_NET_MCAST) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 "No <source> 'address' attribute specified with socket interface");
                goto error;
            }
        } else if ((len = xmlStrlen(address)) >= (BR_INET_ADDR_MAXLEN)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "IP address '%s' is too long", address);
            goto error;
        }
        if (address == NULL) {
            net->dst.socket.address[0] = '\0';
        } else {
            strncpy(net->dst.socket.address, (char*)address,len);
            net->dst.socket.address[len] = '\0';
        }
        xmlFree(address);
    }

    return net;

 error:
    if (network)
        xmlFree(network);
    if (address)
        xmlFree(address);
    if (port)
        xmlFree(port);
    if (ifname)
        xmlFree(ifname);
    if (script)
        xmlFree(script);
    if (bridge)
        xmlFree(bridge);
    free(net);
    return NULL;
}


/* Parse the XML definition for a network interface */
static struct qemud_vm_input_def *qemudParseInputXML(virConnectPtr conn,
                                                   struct qemud_driver *driver ATTRIBUTE_UNUSED,
                                                   xmlNodePtr node) {
    struct qemud_vm_input_def *input = calloc(1, sizeof(struct qemud_vm_input_def));
    xmlChar *type = NULL;
    xmlChar *bus = NULL;

    if (!input) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "input");
        return NULL;
    }

    type = xmlGetProp(node, BAD_CAST "type");
    bus = xmlGetProp(node, BAD_CAST "bus");

    if (!type) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "no type provide for input device");
        goto error;
    }

    if (!strcmp((const char *)type, "mouse")) {
        input->type = QEMU_INPUT_TYPE_MOUSE;
    } else if (!strcmp((const char *)type, "tablet")) {
        input->type = QEMU_INPUT_TYPE_TABLET;
    } else {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "unsupported input device type %s", (const char*)type);
        goto error;
    }

    if (bus) {
        if (!strcmp((const char*)bus, "ps2")) { /* Only allow mouse */
            if (input->type == QEMU_INPUT_TYPE_TABLET) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "ps2 bus does not support %s input device", (const char*)type);
                goto error;
            }
            input->bus = QEMU_INPUT_BUS_PS2;
        } else if (!strcmp((const char *)bus, "usb")) { /* Allow mouse & keyboard */
            input->bus = QEMU_INPUT_BUS_USB;
        } else {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "unsupported input bus %s", (const char*)bus);
            goto error;
        }
    } else {
        if (input->type == QEMU_INPUT_TYPE_MOUSE)
            input->bus = QEMU_INPUT_BUS_PS2;
        else
            input->bus = QEMU_INPUT_BUS_USB;
    }

    if (type)
        xmlFree(type);
    if (bus)
        xmlFree(bus);

    return input;

 error:
    if (type)
        xmlFree(type);
    if (bus)
        xmlFree(bus);

    free(input);
    return NULL;
}


/*
 * Parses a libvirt XML definition of a guest, and populates the
 * the qemud_vm struct with matching data about the guests config
 */
static struct qemud_vm_def *qemudParseXML(virConnectPtr conn,
                                          struct qemud_driver *driver,
                                          xmlDocPtr xml) {
    xmlNodePtr root = NULL;
    xmlChar *prop = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlXPathObjectPtr obj = NULL;
    char *conv = NULL;
    int i;
    struct qemud_vm_def *def;

    if (!(def = calloc(1, sizeof(struct qemud_vm_def)))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "xmlXPathContext");
        return NULL;
    }

    /* Prepare parser / xpath context */
    root = xmlDocGetRootElement(xml);
    if ((root == NULL) || (!xmlStrEqual(root->name, BAD_CAST "domain"))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "incorrect root element");
        goto error;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "xmlXPathContext");
        goto error;
    }


    /* Find out what type of QEMU virtualization to use */
    if (!(prop = xmlGetProp(root, BAD_CAST "type"))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "missing domain type attribute");
        goto error;
    }

    if (!strcmp((char *)prop, "qemu"))
        def->virtType = QEMUD_VIRT_QEMU;
    else if (!strcmp((char *)prop, "kqemu"))
        def->virtType = QEMUD_VIRT_KQEMU;
    else if (!strcmp((char *)prop, "kvm"))
        def->virtType = QEMUD_VIRT_KVM;
    else {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "invalid domain type attribute");
        goto error;
    }
    free(prop);
    prop = NULL;


    /* Extract domain name */
    obj = xmlXPathEval(BAD_CAST "string(/domain/name[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_NAME, NULL);
        goto error;
    }
    if (strlen((const char *)obj->stringval) >= (QEMUD_MAX_NAME_LEN-1)) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "domain name length too long");
        goto error;
    }
    strcpy(def->name, (const char *)obj->stringval);
    xmlXPathFreeObject(obj);


    /* Extract domain uuid */
    obj = xmlXPathEval(BAD_CAST "string(/domain/uuid[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        int err;
        if ((err = virUUIDGenerate(def->uuid))) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Failed to generate UUID: %s", strerror(err));
            goto error;
        }
    } else if (virUUIDParse((const char *)obj->stringval, def->uuid) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "malformed uuid element");
        goto error;
    }
    xmlXPathFreeObject(obj);


    /* Extract domain memory */
    obj = xmlXPathEval(BAD_CAST "string(/domain/memory[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "missing memory element");
        goto error;
    } else {
        conv = NULL;
        def->maxmem = strtoll((const char*)obj->stringval, &conv, 10);
        if (conv == (const char*)obj->stringval) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "malformed memory information");
            goto error;
        }
    }
    if (obj)
        xmlXPathFreeObject(obj);


    /* Extract domain memory */
    obj = xmlXPathEval(BAD_CAST "string(/domain/currentMemory[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        def->memory = def->maxmem;
    } else {
        conv = NULL;
        def->memory = strtoll((const char*)obj->stringval, &conv, 10);
        if (def->memory > def->maxmem)
            def->memory = def->maxmem;
        if (conv == (const char*)obj->stringval) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "malformed memory information");
            goto error;
        }
    }
    if (obj)
        xmlXPathFreeObject(obj);

    /* Extract domain vcpu info */
    obj = xmlXPathEval(BAD_CAST "string(/domain/vcpu[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        def->vcpus = 1;
    } else {
        conv = NULL;
        def->vcpus = strtoll((const char*)obj->stringval, &conv, 10);
        if (conv == (const char*)obj->stringval) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "malformed vcpu information");
            goto error;
        }
    }
    if (obj)
        xmlXPathFreeObject(obj);

    /* See if ACPI feature is requested */
    obj = xmlXPathEval(BAD_CAST "/domain/features/acpi", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr == 1)) {
        def->features |= QEMUD_FEATURE_ACPI;
    }
    xmlXPathFreeObject(obj);


    /* See if we disable reboots */
    obj = xmlXPathEval(BAD_CAST "string(/domain/on_reboot)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        def->noReboot = 0;
    } else {
        if (!strcmp((char*)obj->stringval, "destroy"))
            def->noReboot = 1;
        else
            def->noReboot = 0;
    }
    if (obj)
        xmlXPathFreeObject(obj);

    /* See if we set clock to localtime */
    obj = xmlXPathEval(BAD_CAST "string(/domain/clock/@offset)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        def->localtime = 0;
    } else {
        if (!strcmp((char*)obj->stringval, "localtime"))
            def->localtime = 1;
        else
            def->localtime = 0;
    }
    if (obj)
        xmlXPathFreeObject(obj);


    /* Extract OS type info */
    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_OS_TYPE, NULL);
        goto error;
    }
    if (strcmp((const char *)obj->stringval, "hvm")) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_OS_TYPE, "%s", obj->stringval);
        goto error;
    }
    strcpy(def->os.type, (const char *)obj->stringval);
    xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type[1]/@arch)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        const char *defaultArch = qemudDefaultArch();
        if (strlen(defaultArch) >= (QEMUD_OS_TYPE_MAX_LEN-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "architecture type too long");
            goto error;
        }
        strcpy(def->os.arch, defaultArch);
    } else {
        if (strlen((const char *)obj->stringval) >= (QEMUD_OS_TYPE_MAX_LEN-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "architecture type too long");
            goto error;
        }
        strcpy(def->os.arch, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type[1]/@machine)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        const char *defaultMachine = qemudDefaultMachineForArch(def->os.arch);
        if (!defaultMachine) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "unsupported arch %s", def->os.arch);
            goto error;
        }
        if (strlen(defaultMachine) >= (QEMUD_OS_MACHINE_MAX_LEN-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "machine type too long");
            goto error;
        }
        strcpy(def->os.machine, defaultMachine);
    } else {
        if (strlen((const char *)obj->stringval) >= (QEMUD_OS_MACHINE_MAX_LEN-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "architecture type too long");
            goto error;
        }
        strcpy(def->os.machine, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/kernel[1])", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "kernel path too long");
            goto error;
        }
        strcpy(def->os.kernel, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/initrd[1])", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "initrd path too long");
            goto error;
        }
        strcpy(def->os.initrd, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/cmdline[1])", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "cmdline arguments too long");
            goto error;
        }
        strcpy(def->os.cmdline, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    /* analysis of the disk devices */
    obj = xmlXPathEval(BAD_CAST "/domain/os/boot", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        for (i = 0; i < obj->nodesetval->nodeNr && i < QEMUD_MAX_BOOT_DEVS ; i++) {
            if (!(prop = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "dev")))
                continue;
            if (!strcmp((char *)prop, "hd")) {
                def->os.bootDevs[def->os.nBootDevs++] = QEMUD_BOOT_DISK;
            } else if (!strcmp((char *)prop, "fd")) {
                def->os.bootDevs[def->os.nBootDevs++] = QEMUD_BOOT_FLOPPY;
            } else if (!strcmp((char *)prop, "cdrom")) {
                def->os.bootDevs[def->os.nBootDevs++] = QEMUD_BOOT_CDROM;
            } else if (!strcmp((char *)prop, "network")) {
                def->os.bootDevs[def->os.nBootDevs++] = QEMUD_BOOT_NET;
            } else {
                goto error;
            }
            xmlFree(prop);
            prop = NULL;
        }
    }
    xmlXPathFreeObject(obj);
    if (def->os.nBootDevs == 0) {
        def->os.nBootDevs = 1;
        def->os.bootDevs[0] = QEMUD_BOOT_DISK;
    }


    obj = xmlXPathEval(BAD_CAST "string(/domain/devices/emulator[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        char *tmp = qemudLocateBinaryForArch(conn, def->virtType, def->os.arch);
        if (!tmp) {
            goto error;
        }
        strcpy(def->os.binary, tmp);
        free(tmp);
    } else {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "emulator path too long");
            goto error;
        }
        strcpy(def->os.binary, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "/domain/devices/graphics", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_NODESET) ||
        (obj->nodesetval == NULL) || (obj->nodesetval->nodeNr == 0)) {
        def->graphicsType = QEMUD_GRAPHICS_NONE;
    } else if ((prop = xmlGetProp(obj->nodesetval->nodeTab[0], BAD_CAST "type"))) {
        if (!strcmp((char *)prop, "vnc")) {
            xmlChar *vncport, *vnclisten;
            def->graphicsType = QEMUD_GRAPHICS_VNC;
            vncport = xmlGetProp(obj->nodesetval->nodeTab[0], BAD_CAST "port");
            if (vncport) {
                conv = NULL;
                def->vncPort = strtoll((const char*)vncport, &conv, 10);
            } else {
                def->vncPort = -1;
            }
            vnclisten = xmlGetProp(obj->nodesetval->nodeTab[0], BAD_CAST "listen");
            if (vnclisten && *vnclisten)
                strncpy(def->vncListen, (char *)vnclisten, BR_INET_ADDR_MAXLEN-1);
            else
                strcpy(def->vncListen, "127.0.0.1");
            def->vncListen[BR_INET_ADDR_MAXLEN-1] = '\0';
            xmlFree(vncport);
            xmlFree(vnclisten);
        } else if (!strcmp((char *)prop, "sdl")) {
            def->graphicsType = QEMUD_GRAPHICS_SDL;
        } else {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "Unsupported graphics type %s", prop);
            goto error;
        }
        xmlFree(prop);
        prop = NULL;
    }
    xmlXPathFreeObject(obj);

    /* analysis of the disk devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/disk", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        struct qemud_vm_disk_def *prev = NULL;
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            struct qemud_vm_disk_def *disk;
            if (!(disk = qemudParseDiskXML(conn, driver, obj->nodesetval->nodeTab[i]))) {
                goto error;
            }
            def->ndisks++;
            disk->next = NULL;
            if (i == 0) {
                def->disks = disk;
            } else {
                prev->next = disk;
            }
            prev = disk;
        }
    }
    xmlXPathFreeObject(obj);


    /* analysis of the network devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/interface", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        struct qemud_vm_net_def *prev = NULL;
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            struct qemud_vm_net_def *net;
            if (!(net = qemudParseInterfaceXML(conn, driver, obj->nodesetval->nodeTab[i]))) {
                goto error;
            }
            def->nnets++;
            net->next = NULL;
            if (i == 0) {
                def->nets = net;
            } else {
                prev->next = net;
            }
            prev = net;
        }
    }
    xmlXPathFreeObject(obj);

    /* analysis of the input devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/input", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        struct qemud_vm_input_def *prev = NULL;
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            struct qemud_vm_input_def *input;
            if (!(input = qemudParseInputXML(conn, driver, obj->nodesetval->nodeTab[i]))) {
                goto error;
            }
            /* Mouse + PS/2 is implicit with graphics, so don't store it */
            if (input->bus == QEMU_INPUT_BUS_PS2 &&
                input->type == QEMU_INPUT_TYPE_MOUSE) {
                free(input);
                continue;
            }
            def->ninputs++;
            input->next = NULL;
            if (def->inputs == NULL) {
                def->inputs = input;
            } else {
                prev->next = input;
            }
            prev = input;
        }
    }
    xmlXPathFreeObject(obj);
    obj = NULL;

    /* If graphics are enabled, there's an implicit PS2 mouse */
    if (def->graphicsType != QEMUD_GRAPHICS_NONE) {
        int hasPS2mouse = 0;
        struct qemud_vm_input_def *input = def->inputs;
        while (input) {
            if (input->type == QEMU_INPUT_TYPE_MOUSE &&
                input->bus == QEMU_INPUT_BUS_PS2)
                hasPS2mouse = 1;
            input = input->next;
        }

        if (!hasPS2mouse) {
            input = calloc(1, sizeof(struct qemud_vm_input_def));
            if (!input) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "input");
                goto error;
            }
            input->type = QEMU_INPUT_TYPE_MOUSE;
            input->bus = QEMU_INPUT_BUS_PS2;
            input->next = def->inputs;
            def->inputs = input;
            def->ninputs++;
        }
    }

    xmlXPathFreeContext(ctxt);

    return def;

 error:
    if (prop)
        free(prop);
    if (obj)
        xmlXPathFreeObject(obj);
    if (ctxt)
        xmlXPathFreeContext(ctxt);
    qemudFreeVMDef(def);
    return NULL;
}


static char *
qemudNetworkIfaceConnect(virConnectPtr conn,
                         struct qemud_driver *driver,
                         struct qemud_vm *vm,
                         struct qemud_vm_net_def *net,
                         int vlan)
{
    struct qemud_network *network = NULL;
    char *brname;
    char *ifname;
    char tapfdstr[4+3+32+7];
    char *retval = NULL;
    int err;
    int tapfd = -1;
    int *tapfds;

    if (net->type == QEMUD_NET_NETWORK) {
        if (!(network = qemudFindNetworkByName(driver, net->dst.network.name))) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Network '%s' not found", net->dst.network.name);
            goto error;
        } else if (network->bridge[0] == '\0') {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Network '%s' not active", net->dst.network.name);
            goto error;
        }
        brname = network->bridge;
        if (net->dst.network.ifname[0] == '\0' ||
            strchr(net->dst.network.ifname, '%')) {
            strcpy(net->dst.network.ifname, "vnet%d");
        }
        ifname = net->dst.network.ifname;
    } else if (net->type == QEMUD_NET_BRIDGE) {
        brname = net->dst.bridge.brname;
        if (net->dst.bridge.ifname[0] == '\0' ||
            strchr(net->dst.bridge.ifname, '%')) {
            strcpy(net->dst.bridge.ifname, "vnet%d");
        }
        ifname = net->dst.bridge.ifname;
    } else {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "Network type %d is not supported", net->type);
        goto error;
    }

    if (!driver->brctl && (err = brInit(&driver->brctl))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot initialize bridge support: %s", strerror(err));
        goto error;
    }

    if ((err = brAddTap(driver->brctl, brname,
                        ifname, BR_IFNAME_MAXLEN, &tapfd))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "Failed to add tap interface '%s' to bridge '%s' : %s",
                         ifname, brname, strerror(err));
        goto error;
    }

    snprintf(tapfdstr, sizeof(tapfdstr), "tap,fd=%d,script=,vlan=%d", tapfd, vlan);

    if (!(retval = strdup(tapfdstr)))
        goto no_memory;

    if (!(tapfds = realloc(vm->tapfds, sizeof(int) * (vm->ntapfds+2))))
        goto no_memory;

    vm->tapfds = tapfds;
    vm->tapfds[vm->ntapfds++] = tapfd;
    vm->tapfds[vm->ntapfds]   = -1;

    return retval;

 no_memory:
    qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "tapfds");
 error:
    if (retval)
        free(retval);
    if (tapfd != -1)
        close(tapfd);
    return NULL;
}

/*
 * Constructs a argv suitable for launching qemu with config defined
 * for a given virtual machine.
 */
int qemudBuildCommandLine(virConnectPtr conn,
                          struct qemud_driver *driver,
                          struct qemud_vm *vm,
                          char ***argv) {
    int len, n = -1, i;
    char memory[50];
    char vcpus[50];
    char boot[QEMUD_MAX_BOOT_DEVS+1];
    struct stat sb;
    struct qemud_vm_disk_def *disk = vm->def->disks;
    struct qemud_vm_net_def *net = vm->def->nets;
    struct qemud_vm_input_def *input = vm->def->inputs;
    struct utsname ut;
    int disableKQEMU = 0;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so its hard to feed back a useful error
     */
    if (stat(vm->def->os.binary, &sb) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "Cannot find QEMU binary %s: %s", vm->def->os.binary,
                         strerror(errno));
        return -1;
    }

    if (vm->qemuVersion == 0) {
        if (qemudExtractVersionInfo(vm->def->os.binary,
                                    &(vm->qemuVersion),
                                    &(vm->qemuCmdFlags)) < 0)
            return -1;
    }

    uname(&ut);

    /* Nasty hack make i?86 look like i686 to simplify next comparison */
    if (ut.machine[0] == 'i' &&
        ut.machine[2] == '8' &&
        ut.machine[3] == '6' &&
        !ut.machine[4])
        ut.machine[1] = '6';

    /* Need to explicitly disable KQEMU if
     * 1. Arch matches host arch
     * 2. Guest is 'qemu'
     * 3. The qemu binary has the -no-kqemu flag
     */
    if ((vm->qemuCmdFlags & QEMUD_CMD_FLAG_KQEMU) &&
        !strcmp(ut.machine, vm->def->os.arch) &&
        vm->def->virtType == QEMUD_VIRT_QEMU)
        disableKQEMU = 1;

    len = 1 + /* qemu */
        2 + /* machine type */
        disableKQEMU + /* Disable kqemu */
        2 * vm->def->ndisks + /* disks*/
        (vm->def->nnets > 0 ? (4 * vm->def->nnets) : 2) + /* networks */
        1 + /* usb */
        2 * vm->def->ninputs + /* input devices */
        2 + /* memory*/
        2 + /* cpus */
        2 + /* boot device */
        2 + /* monitor */
        (vm->def->localtime ? 1 : 0) + /* localtime */
        (vm->qemuCmdFlags & QEMUD_CMD_FLAG_NO_REBOOT &&
         vm->def->noReboot ? 1 : 0) + /* no-reboot */
        (vm->def->features & QEMUD_FEATURE_ACPI ? 0 : 1) + /* acpi */
        (vm->def->os.kernel[0] ? 2 : 0) + /* kernel */
        (vm->def->os.initrd[0] ? 2 : 0) + /* initrd */
        (vm->def->os.cmdline[0] ? 2 : 0) + /* cmdline */
        (vm->def->graphicsType == QEMUD_GRAPHICS_VNC ? 2 :
         (vm->def->graphicsType == QEMUD_GRAPHICS_SDL ? 0 : 1)) + /* graphics */
        (vm->migrateFrom[0] ? 3 : 0); /* migrateFrom */

    snprintf(memory, sizeof(memory), "%d", vm->def->memory/1024);
    snprintf(vcpus, sizeof(vcpus), "%d", vm->def->vcpus);

    if (!(*argv = malloc(sizeof(char *) * (len+1))))
        goto no_memory;
    if (!((*argv)[++n] = strdup(vm->def->os.binary)))
        goto no_memory;
    if (!((*argv)[++n] = strdup("-M")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(vm->def->os.machine)))
        goto no_memory;
    if (disableKQEMU) {
        if (!((*argv)[++n] = strdup("-no-kqemu")))
            goto no_memory;
    }
    if (!((*argv)[++n] = strdup("-m")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(memory)))
        goto no_memory;
    if (!((*argv)[++n] = strdup("-smp")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(vcpus)))
        goto no_memory;

    /*
     * NB, -nographic *MUST* come before any serial, or monitor
     * or parallel port flags due to QEMU craziness, where it
     * decides to change the serial port & monitor to be on stdout
     * if you ask for nographic. So we have to make sure we override
     * these defaults ourselves...
     */
    if (vm->def->graphicsType == QEMUD_GRAPHICS_NONE) {
        if (!((*argv)[++n] = strdup("-nographic")))
            goto no_memory;
    }

    if (!((*argv)[++n] = strdup("-monitor")))
        goto no_memory;
    if (!((*argv)[++n] = strdup("pty")))
        goto no_memory;

    if (vm->def->localtime) {
        if (!((*argv)[++n] = strdup("-localtime")))
            goto no_memory;
    }

    if (vm->qemuCmdFlags & QEMUD_CMD_FLAG_NO_REBOOT &&
        vm->def->noReboot) {
        if (!((*argv)[++n] = strdup("-no-reboot")))
            goto no_memory;
    }

    if (!(vm->def->features & QEMUD_FEATURE_ACPI)) {
        if (!((*argv)[++n] = strdup("-no-acpi")))
            goto no_memory;
    }

    for (i = 0 ; i < vm->def->os.nBootDevs ; i++) {
        switch (vm->def->os.bootDevs[i]) {
        case QEMUD_BOOT_CDROM:
            boot[i] = 'd';
            break;
        case QEMUD_BOOT_FLOPPY:
            boot[i] = 'a';
            break;
        case QEMUD_BOOT_DISK:
            boot[i] = 'c';
            break;
        case QEMUD_BOOT_NET:
            boot[i] = 'n';
            break;
        default:
            boot[i] = 'c';
            break;
        }
    }
    boot[vm->def->os.nBootDevs] = '\0';
    if (!((*argv)[++n] = strdup("-boot")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(boot)))
        goto no_memory;

    if (vm->def->os.kernel[0]) {
        if (!((*argv)[++n] = strdup("-kernel")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->def->os.kernel)))
            goto no_memory;
    }
    if (vm->def->os.initrd[0]) {
        if (!((*argv)[++n] = strdup("-initrd")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->def->os.initrd)))
            goto no_memory;
    }
    if (vm->def->os.cmdline[0]) {
        if (!((*argv)[++n] = strdup("-append")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->def->os.cmdline)))
            goto no_memory;
    }

    while (disk) {
        char dev[NAME_MAX];
        char file[PATH_MAX];
        if (!strcmp(disk->dst, "hdc") &&
            disk->device == QEMUD_DISK_CDROM)
            snprintf(dev, NAME_MAX, "-%s", "cdrom");
        else
            snprintf(dev, NAME_MAX, "-%s", disk->dst);
        snprintf(file, PATH_MAX, "%s", disk->src);

        if (!((*argv)[++n] = strdup(dev)))
            goto no_memory;
        if (!((*argv)[++n] = strdup(file)))
            goto no_memory;

        disk = disk->next;
    }

    if (!net) {
        if (!((*argv)[++n] = strdup("-net")))
            goto no_memory;
        if (!((*argv)[++n] = strdup("none")))
            goto no_memory;
    } else {
        int vlan = 0;
        while (net) {
            char nic[100];

            if (snprintf(nic, sizeof(nic), "nic,macaddr=%02x:%02x:%02x:%02x:%02x:%02x,vlan=%d",
                         net->mac[0], net->mac[1],
                         net->mac[2], net->mac[3],
                         net->mac[4], net->mac[5],
                         vlan) >= sizeof(nic))
                goto error;

            if (!((*argv)[++n] = strdup("-net")))
                goto no_memory;
            if (!((*argv)[++n] = strdup(nic)))
                goto no_memory;

            if (!((*argv)[++n] = strdup("-net")))
                goto no_memory;

            switch (net->type) {
            case QEMUD_NET_NETWORK:
            case QEMUD_NET_BRIDGE:
                if (!((*argv)[++n] = qemudNetworkIfaceConnect(conn, driver, vm, net, vlan)))
                    goto error;
                break;

            case QEMUD_NET_ETHERNET:
                {
                    char arg[PATH_MAX];
                    if (snprintf(arg, PATH_MAX-1, "tap,ifname=%s,script=%s,vlan=%d",
                                 net->dst.ethernet.ifname,
                                 net->dst.ethernet.script,
                                 vlan) >= (PATH_MAX-1))
                        goto error;

                    if (!((*argv)[++n] = strdup(arg)))
                        goto no_memory;
                }
                break;

            case QEMUD_NET_CLIENT:
            case QEMUD_NET_SERVER:
            case QEMUD_NET_MCAST:
                {
                    char arg[PATH_MAX];
                    const char *mode = NULL;
                    switch (net->type) {
                    case QEMUD_NET_CLIENT:
                        mode = "connect";
                        break;
                    case QEMUD_NET_SERVER:
                        mode = "listen";
                        break;
                    case QEMUD_NET_MCAST:
                        mode = "mcast";
                        break;
                    }
                    if (snprintf(arg, PATH_MAX-1, "socket,%s=%s:%d,vlan=%d",
                                 mode,
                                 net->dst.socket.address,
                                 net->dst.socket.port,
                                 vlan) >= (PATH_MAX-1))
                        goto error;

                    if (!((*argv)[++n] = strdup(arg)))
                        goto no_memory;
                }
                break;

            case QEMUD_NET_USER:
            default:
                {
                    char arg[PATH_MAX];
                    if (snprintf(arg, PATH_MAX-1, "user,vlan=%d", vlan) >= (PATH_MAX-1))
                        goto error;

                    if (!((*argv)[++n] = strdup(arg)))
                        goto no_memory;
                }
            }

            net = net->next;
            vlan++;
        }
    }

    if (!((*argv)[++n] = strdup("-usb")))
        goto no_memory;
    while (input) {
        if (input->bus == QEMU_INPUT_BUS_USB) {
            if (!((*argv)[++n] = strdup("-usbdevice")))
                goto no_memory;
            if (!((*argv)[++n] = strdup(input->type == QEMU_INPUT_TYPE_MOUSE ? "mouse" : "tablet")))
                goto no_memory;
        }

        input = input->next;
    }

    if (vm->def->graphicsType == QEMUD_GRAPHICS_VNC) {
        char vncdisplay[BR_INET_ADDR_MAXLEN+20];
        int ret;
        if (vm->qemuCmdFlags & QEMUD_CMD_FLAG_VNC_COLON)
            ret = snprintf(vncdisplay, sizeof(vncdisplay), "%s:%d",
                           vm->def->vncListen,
                           vm->def->vncActivePort - 5900);
        else
            ret = snprintf(vncdisplay, sizeof(vncdisplay), "%d",
                           vm->def->vncActivePort - 5900);
        if (ret < 0 || ret >= (int)sizeof(vncdisplay))
            goto error;

        if (!((*argv)[++n] = strdup("-vnc")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vncdisplay)))
            goto no_memory;
    } else if (vm->def->graphicsType == QEMUD_GRAPHICS_NONE) {
        /* Nada - we added -nographic earlier in this function */
    } else {
        /* SDL is the default. no args needed */
    }

    if (vm->migrateFrom[0]) {
        if (!((*argv)[++n] = strdup("-S")))
            goto no_memory;
        if (!((*argv)[++n] = strdup("-incoming")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->migrateFrom)))
            goto no_memory;
    }

    (*argv)[++n] = NULL;

    return 0;

 no_memory:
    qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "argv");
 error:
    if (vm->tapfds) {
        for (i = 0; vm->tapfds[i] != -1; i++)
            close(vm->tapfds[i]);
        free(vm->tapfds);
        vm->tapfds = NULL;
        vm->ntapfds = 0;
    }
    if (argv) {
        for (i = 0 ; i < n ; i++)
            free((*argv)[i]);
        free(*argv);
    }
    return -1;
}


/* Save a guest's config data into a persistent file */
static int qemudSaveConfig(virConnectPtr conn,
                           struct qemud_driver *driver,
                           struct qemud_vm *vm,
                           struct qemud_vm_def *def) {
    char *xml;
    int fd = -1, ret = -1;
    int towrite;

    if (!(xml = qemudGenerateXML(conn, driver, vm, def, 0)))
        return -1;

    if ((fd = open(vm->configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot create config file %s: %s",
                         vm->configFile, strerror(errno));
        goto cleanup;
    }

    towrite = strlen(xml);
    if (write(fd, xml, towrite) != towrite) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot write config file %s: %s",
                         vm->configFile, strerror(errno));
        goto cleanup;
    }

    if (close(fd) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot save config file %s: %s",
                         vm->configFile, strerror(errno));
        goto cleanup;
    }

    ret = 0;

 cleanup:
    if (fd != -1)
        close(fd);

    free(xml);

    return ret;
}

struct qemud_vm_def *
qemudParseVMDef(virConnectPtr conn,
                struct qemud_driver *driver,
                const char *xmlStr,
                const char *displayName) {
    xmlDocPtr xml;
    struct qemud_vm_def *def = NULL;

    if (!(xml = xmlReadDoc(BAD_CAST xmlStr, displayName ? displayName : "domain.xml", NULL,
                           XML_PARSE_NOENT | XML_PARSE_NONET |
                           XML_PARSE_NOERROR | XML_PARSE_NOWARNING))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_XML_ERROR, NULL);
        return NULL;
    }

    def = qemudParseXML(conn, driver, xml);

    xmlFreeDoc(xml);

    return def;
}

struct qemud_vm *
qemudAssignVMDef(virConnectPtr conn,
                 struct qemud_driver *driver,
                 struct qemud_vm_def *def)
{
    struct qemud_vm *vm = NULL;

    if ((vm = qemudFindVMByName(driver, def->name))) {
        if (!qemudIsActiveVM(vm)) {
            qemudFreeVMDef(vm->def);
            vm->def = def;
        } else {
            if (vm->newDef)
                qemudFreeVMDef(vm->newDef);
            vm->newDef = def;
        }
        /* Reset version, because the emulator path might have changed */
        vm->qemuVersion = 0;
        vm->qemuCmdFlags = 0;
        return vm;
    }

    if (!(vm = calloc(1, sizeof(struct qemud_vm)))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "vm");
        return NULL;
    }

    vm->stdin = -1;
    vm->stdout = -1;
    vm->stderr = -1;
    vm->monitor = -1;
    vm->pid = -1;
    vm->id = -1;
    vm->state = VIR_DOMAIN_SHUTOFF;
    vm->def = def;
    vm->next = driver->vms;

    driver->vms = vm;
    driver->ninactivevms++;

    return vm;
}

void
qemudRemoveInactiveVM(struct qemud_driver *driver,
                      struct qemud_vm *vm)
{
    struct qemud_vm *prev = NULL, *curr;

    curr = driver->vms;
    while (curr != vm) {
        prev = curr;
        curr = curr->next;
    }

    if (curr) {
        if (prev)
            prev->next = curr->next;
        else
            driver->vms = curr->next;

        driver->ninactivevms--;
    }

    qemudFreeVM(vm);
}

int
qemudSaveVMDef(virConnectPtr conn,
               struct qemud_driver *driver,
               struct qemud_vm *vm,
               struct qemud_vm_def *def) {
    if (vm->configFile[0] == '\0') {
        int err;

        if ((err = qemudEnsureDir(driver->configDir))) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "cannot create config directory %s: %s",
                             driver->configDir, strerror(err));
            return -1;
        }

        if (qemudMakeConfigPath(driver->configDir, def->name, ".xml",
                                vm->configFile, PATH_MAX) < 0) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "cannot construct config file path");
            return -1;
        }

        if (qemudMakeConfigPath(driver->autostartDir, def->name, ".xml",
                                vm->autostartLink, PATH_MAX) < 0) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "cannot construct autostart link path");
            vm->configFile[0] = '\0';
            return -1;
        }
    }

    return qemudSaveConfig(conn, driver, vm, def);
}

static int qemudSaveNetworkConfig(virConnectPtr conn,
                                  struct qemud_driver *driver,
                                  struct qemud_network *network,
                                  struct qemud_network_def *def) {
    char *xml;
    int fd, ret = -1;
    int towrite;
    int err;

    if (!(xml = qemudGenerateNetworkXML(conn, driver, network, def))) {
        return -1;
    }

    if ((err = qemudEnsureDir(driver->networkConfigDir))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot create config directory %s: %s",
                         driver->networkConfigDir, strerror(err));
        goto cleanup;
    }

    if ((fd = open(network->configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot create config file %s: %s",
                         network->configFile, strerror(errno));
        goto cleanup;
    }

    towrite = strlen(xml);
    if (write(fd, xml, towrite) != towrite) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot write config file %s: %s",
                         network->configFile, strerror(errno));
        goto cleanup;
    }

    if (close(fd) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                         "cannot save config file %s: %s",
                         network->configFile, strerror(errno));
        goto cleanup;
    }

    ret = 0;

 cleanup:

    free(xml);

    return ret;
}

void qemudFreeNetworkDef(struct qemud_network_def *def) {
    struct qemud_dhcp_range_def *range = def->ranges;
    while (range) {
        struct qemud_dhcp_range_def *next = range->next;
        free(range);
        range = next;
    }
    free(def);
}

void qemudFreeNetwork(struct qemud_network *network) {
    qemudFreeNetworkDef(network->def);
    if (network->newDef)
        qemudFreeNetworkDef(network->newDef);
    free(network);
}

static int qemudParseBridgeXML(struct qemud_driver *driver ATTRIBUTE_UNUSED,
                               struct qemud_network_def *def,
                               xmlNodePtr node) {
    xmlChar *name, *stp, *delay;

    name = xmlGetProp(node, BAD_CAST "name");
    if (name != NULL) {
        strncpy(def->bridge, (const char *)name, IF_NAMESIZE-1);
        def->bridge[IF_NAMESIZE-1] = '\0';
        xmlFree(name);
        name = NULL;
    }

    stp = xmlGetProp(node, BAD_CAST "stp");
    if (stp != NULL) {
        if (xmlStrEqual(stp, BAD_CAST "off")) {
            def->disableSTP = 1;
        }
        xmlFree(stp);
        stp = NULL;
    }

    delay = xmlGetProp(node, BAD_CAST "delay");
    if (delay != NULL) {
        def->forwardDelay = strtol((const char *)delay, NULL, 10);
        xmlFree(delay);
        delay = NULL;
    }

    return 1;
}

static int qemudParseDhcpRangesXML(virConnectPtr conn,
                                   struct qemud_driver *driver ATTRIBUTE_UNUSED,
                                   struct qemud_network_def *def,
                                   xmlNodePtr node) {

    xmlNodePtr cur;

    cur = node->children;
    while (cur != NULL) {
        struct qemud_dhcp_range_def *range;
        xmlChar *start, *end;

        if (cur->type != XML_ELEMENT_NODE ||
            !xmlStrEqual(cur->name, BAD_CAST "range")) {
            cur = cur->next;
            continue;
        }

        if (!(range = calloc(1, sizeof(struct qemud_dhcp_range_def)))) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "range");
            return 0;
        }

        start = xmlGetProp(cur, BAD_CAST "start");
        end = xmlGetProp(cur, BAD_CAST "end");

        if (start && start[0] && end && end[0]) {
            strncpy(range->start, (const char *)start, BR_INET_ADDR_MAXLEN-1);
            range->start[BR_INET_ADDR_MAXLEN-1] = '\0';

            strncpy(range->end, (const char *)end, BR_INET_ADDR_MAXLEN-1);
            range->end[BR_INET_ADDR_MAXLEN-1] = '\0';

            range->next = def->ranges;
            def->ranges = range;
            def->nranges++;
        } else {
            free(range);
        }

        if (start)
            xmlFree(start);
        if (end)
            xmlFree(end);

        cur = cur->next;
    }

    return 1;
}

static int qemudParseInetXML(virConnectPtr conn,
                             struct qemud_driver *driver ATTRIBUTE_UNUSED,
                             struct qemud_network_def *def,
                             xmlNodePtr node) {
    xmlChar *address, *netmask;
    xmlNodePtr cur;

    address = xmlGetProp(node, BAD_CAST "address");
    if (address != NULL) {
        strncpy(def->ipAddress, (const char *)address, BR_INET_ADDR_MAXLEN-1);
        def->ipAddress[BR_INET_ADDR_MAXLEN-1] = '\0';
        xmlFree(address);
        address = NULL;
    }

    netmask = xmlGetProp(node, BAD_CAST "netmask");
    if (netmask != NULL) {
        strncpy(def->netmask, (const char *)netmask, BR_INET_ADDR_MAXLEN-1);
        def->netmask[BR_INET_ADDR_MAXLEN-1] = '\0';
        xmlFree(netmask);
        netmask = NULL;
    }

    if (def->ipAddress[0] && def->netmask[0]) {
        struct in_addr inaddress, innetmask;
        char *netaddr;

        inet_aton((const char*)def->ipAddress, &inaddress);
        inet_aton((const char*)def->netmask, &innetmask);

        inaddress.s_addr &= innetmask.s_addr;

        netaddr = inet_ntoa(inaddress);

        snprintf(def->network,sizeof(def->network)-1,
                 "%s/%s", netaddr, (const char *)def->netmask);
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrEqual(cur->name, BAD_CAST "dhcp") &&
            !qemudParseDhcpRangesXML(conn, driver, def, cur))
            return 0;
        cur = cur->next;
    }

    return 1;
}


static struct qemud_network_def *qemudParseNetworkXML(virConnectPtr conn,
                                                      struct qemud_driver *driver,
                                                      xmlDocPtr xml) {
    xmlNodePtr root = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlXPathObjectPtr obj = NULL, tmp = NULL;
    struct qemud_network_def *def;

    if (!(def = calloc(1, sizeof(struct qemud_network_def)))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "network_def");
        return NULL;
    }

    /* Prepare parser / xpath context */
    root = xmlDocGetRootElement(xml);
    if ((root == NULL) || (!xmlStrEqual(root->name, BAD_CAST "network"))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "incorrect root element");
        goto error;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "xmlXPathContext");
        goto error;
    }


    /* Extract network name */
    obj = xmlXPathEval(BAD_CAST "string(/network/name[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_NAME, NULL);
        goto error;
    }
    if (strlen((const char *)obj->stringval) >= (QEMUD_MAX_NAME_LEN-1)) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "network name length too long");
        goto error;
    }
    strcpy(def->name, (const char *)obj->stringval);
    xmlXPathFreeObject(obj);


    /* Extract network uuid */
    obj = xmlXPathEval(BAD_CAST "string(/network/uuid[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        int err;
        if ((err = virUUIDGenerate(def->uuid))) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Failed to generate UUID: %s", strerror(err));
            goto error;
        }
    } else if (virUUIDParse((const char *)obj->stringval, def->uuid) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "%s", "malformed uuid element");
        goto error;
    }
    xmlXPathFreeObject(obj);

    /* Parse bridge information */
    obj = xmlXPathEval(BAD_CAST "/network/bridge[1]", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr > 0)) {
        if (!qemudParseBridgeXML(driver, def, obj->nodesetval->nodeTab[0])) {
            goto error;
        }
    }
    xmlXPathFreeObject(obj);

    /* Parse IP information */
    obj = xmlXPathEval(BAD_CAST "/network/ip[1]", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr > 0)) {
        if (!qemudParseInetXML(conn, driver, def, obj->nodesetval->nodeTab[0])) {
            goto error;
        }
    }
    xmlXPathFreeObject(obj);

    /* IPv4 forwarding setup */
    obj = xmlXPathEval(BAD_CAST "count(/network/forward) > 0", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_BOOLEAN) &&
        obj->boolval) {
        if (!def->ipAddress[0] ||
            !def->netmask[0]) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "Forwarding requested, but no IPv4 address/netmask provided");
            goto error;
        }

        def->forward = 1;
        tmp = xmlXPathEval(BAD_CAST "string(/network/forward[1]/@dev)", ctxt);
        if ((tmp != NULL) && (tmp->type == XPATH_STRING) &&
            (tmp->stringval != NULL) && (tmp->stringval[0] != 0)) {
            int len;
            if ((len = xmlStrlen(tmp->stringval)) >= (BR_IFNAME_MAXLEN-1)) {
                qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                                 "forward device name '%s' is too long",
                                 (char*)tmp->stringval);
                goto error;
            }
            strcpy(def->forwardDev, (char*)tmp->stringval);
        } else {
            def->forwardDev[0] = '\0';
        }
        xmlXPathFreeObject(tmp);
        tmp = NULL;
    } else {
        def->forward = 0;
    }
    xmlXPathFreeObject(obj);

    xmlXPathFreeContext(ctxt);

    return def;

 error:
    /* XXX free all the stuff in the qemud_network struct, or leave it upto
       the caller ? */
    if (obj)
        xmlXPathFreeObject(obj);
    if (tmp)
        xmlXPathFreeObject(tmp);
    if (ctxt)
        xmlXPathFreeContext(ctxt);
    qemudFreeNetworkDef(def);
    return NULL;
}

struct qemud_network_def *
qemudParseNetworkDef(virConnectPtr conn,
                     struct qemud_driver *driver,
                     const char *xmlStr,
                     const char *displayName) {
    xmlDocPtr xml;
    struct qemud_network_def *def;

    if (!(xml = xmlReadDoc(BAD_CAST xmlStr, displayName ? displayName : "network.xml", NULL,
                           XML_PARSE_NOENT | XML_PARSE_NONET |
                           XML_PARSE_NOERROR | XML_PARSE_NOWARNING))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_XML_ERROR, NULL);
        return NULL;
    }

    def = qemudParseNetworkXML(conn, driver, xml);

    xmlFreeDoc(xml);

    return def;
}

struct qemud_network *
qemudAssignNetworkDef(virConnectPtr conn,
                      struct qemud_driver *driver,
                      struct qemud_network_def *def) {
    struct qemud_network *network;

    if ((network = qemudFindNetworkByName(driver, def->name))) {
        if (!qemudIsActiveNetwork(network)) {
            qemudFreeNetworkDef(network->def);
            network->def = def;
        } else {
            if (network->newDef)
                qemudFreeNetworkDef(network->newDef);
            network->newDef = def;
        }

        return network;
    }

    if (!(network = calloc(1, sizeof(struct qemud_network)))) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "network");
        return NULL;
    }

    network->def = def;
    network->next = driver->networks;

    driver->networks = network;
    driver->ninactivenetworks++;

    return network;
}

void
qemudRemoveInactiveNetwork(struct qemud_driver *driver,
                           struct qemud_network *network)
{
    struct qemud_network *prev = NULL, *curr;

    curr = driver->networks;
    while (curr != network) {
        prev = curr;
        curr = curr->next;
    }

    if (curr) {
        if (prev)
            prev->next = curr->next;
        else
            driver->networks = curr->next;

        driver->ninactivenetworks--;
    }

    qemudFreeNetwork(network);
}

int
qemudSaveNetworkDef(virConnectPtr conn,
                    struct qemud_driver *driver,
                    struct qemud_network *network,
                    struct qemud_network_def *def) {

    if (network->configFile[0] == '\0') {
        int err;

        if ((err = qemudEnsureDir(driver->networkConfigDir))) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "cannot create config directory %s: %s",
                             driver->networkConfigDir, strerror(err));
            return -1;
        }

        if (qemudMakeConfigPath(driver->networkConfigDir, def->name, ".xml",
                                network->configFile, PATH_MAX) < 0) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "cannot construct config file path");
            return -1;
        }

        if (qemudMakeConfigPath(driver->networkAutostartDir, def->name, ".xml",
                                network->autostartLink, PATH_MAX) < 0) {
            qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR,
                             "cannot construct autostart link path");
            network->configFile[0] = '\0';
            return -1;
        }
    }

    return qemudSaveNetworkConfig(conn, driver, network, def);
}

static int
qemudReadFile(const char *path,
              char *buf,
              int maxlen) {
    FILE *fh;
    struct stat st;
    int ret = 0;

    if (!(fh = fopen(path, "r"))) {
        qemudLog(QEMUD_WARN, "Failed to open file '%s': %s",
                 path, strerror(errno));
        goto error;
    }

    if (fstat(fileno(fh), &st) < 0) {
        qemudLog(QEMUD_WARN, "Failed to stat file '%s': %s",
                 path, strerror(errno));
        goto error;
    }

    if (S_ISDIR(st.st_mode)) {
        qemudDebug("Ignoring directory '%s' - clearly not a config file", path);
        goto error;
    }

    if (st.st_size >= maxlen) {
        qemudLog(QEMUD_WARN, "File '%s' is too large", path);
        goto error;
    }

    if ((ret = fread(buf, st.st_size, 1, fh)) != 1) {
        qemudLog(QEMUD_WARN, "Failed to read config file '%s': %s",
                 path, strerror(errno));
        goto error;
    }

    buf[st.st_size] = '\0';

    ret = 1;

 error:
    if (fh)
        fclose(fh);

    return ret;
}

static int
compareFileToNameSuffix(const char *file,
                        const char *name,
                        const char *suffix) {
    int filelen = strlen(file);
    int namelen = strlen(name);
    int suffixlen = strlen(suffix);

    if (filelen == (namelen + suffixlen) &&
        !strncmp(file, name, namelen) &&
        !strncmp(file + namelen, suffix, suffixlen))
        return 1;
    else
        return 0;
}

static int
hasSuffix(const char *str,
          const char *suffix)
{
    int len = strlen(str);
    int suffixlen = strlen(suffix);

    if (len < suffixlen)
        return 0;

    return strcmp(str + len - suffixlen, suffix) == 0;
}

static int
checkLinkPointsTo(const char *checkLink,
                  const char *checkDest)
{
    char dest[PATH_MAX];
    char real[PATH_MAX];
    char checkReal[PATH_MAX];
    int n;
    int passed = 0;

    /* read the link destination */
    if ((n = readlink(checkLink, dest, PATH_MAX)) < 0) {
        switch (errno) {
        case ENOENT:
        case ENOTDIR:
            break;

        case EINVAL:
            qemudLog(QEMUD_WARN, "Autostart file '%s' is not a symlink",
                     checkLink);
            break;

        default:
            qemudLog(QEMUD_WARN, "Failed to read autostart symlink '%s': %s",
                     checkLink, strerror(errno));
            break;
        }

        goto failed;
    } else if (n >= PATH_MAX) {
        qemudLog(QEMUD_WARN, "Symlink '%s' contents too long to fit in buffer",
                 checkLink);
        goto failed;
    }

    dest[n] = '\0';

    /* make absolute */
    if (dest[0] != '/') {
        char dir[PATH_MAX];
        char tmp[PATH_MAX];
        char *p;

        strncpy(dir, checkLink, PATH_MAX);
        dir[PATH_MAX] = '\0';

        if (!(p = strrchr(dir, '/'))) {
            qemudLog(QEMUD_WARN, "Symlink path '%s' is not absolute", checkLink);
            goto failed;
        }

        if (p == dir) /* handle unlikely root dir case */
            p++;

        *p = '\0';

        if (qemudMakeConfigPath(dir, dest, NULL, tmp, PATH_MAX) < 0) {
            qemudLog(QEMUD_WARN, "Path '%s/%s' is too long", dir, dest);
            goto failed;
        }

        strncpy(dest, tmp, PATH_MAX);
        dest[PATH_MAX] = '\0';
    }

    /* canonicalize both paths */
    if (!realpath(dest, real)) {
        qemudLog(QEMUD_WARN, "Failed to expand path '%s' :%s",
                 dest, strerror(errno));
        strncpy(real, dest, PATH_MAX);
        real[PATH_MAX] = '\0';
    }

    if (!realpath(checkDest, checkReal)) {
        qemudLog(QEMUD_WARN, "Failed to expand path '%s' :%s",
                 checkDest, strerror(errno));
        strncpy(checkReal, checkDest, PATH_MAX);
        checkReal[PATH_MAX] = '\0';
    }

    /* compare */
    if (strcmp(checkReal, real) != 0) {
        qemudLog(QEMUD_WARN, "Autostart link '%s' is not a symlink to '%s', ignoring",
                 checkLink, checkReal);
        goto failed;
    }

    passed = 1;

 failed:
    return passed;
}

static struct qemud_vm *
qemudLoadConfig(struct qemud_driver *driver,
                const char *file,
                const char *path,
                const char *xml,
                const char *autostartLink) {
    struct qemud_vm_def *def;
    struct qemud_vm *vm;

    if (!(def = qemudParseVMDef(NULL, driver, xml, file))) {
        virErrorPtr err = virGetLastError();
        qemudLog(QEMUD_WARN, "Error parsing QEMU guest config '%s' : %s",
                 path, err->message);
        return NULL;
    }

    if (!compareFileToNameSuffix(file, def->name, ".xml")) {
        qemudLog(QEMUD_WARN, "QEMU guest config filename '%s' does not match guest name '%s'",
                 path, def->name);
        qemudFreeVMDef(def);
        return NULL;
    }

    if (!(vm = qemudAssignVMDef(NULL, driver, def))) {
        qemudLog(QEMUD_WARN, "Failed to load QEMU guest config '%s': out of memory", path);
        qemudFreeVMDef(def);
        return NULL;
    }

    strncpy(vm->configFile, path, PATH_MAX);
    vm->configFile[PATH_MAX-1] = '\0';

    strncpy(vm->autostartLink, autostartLink, PATH_MAX);
    vm->autostartLink[PATH_MAX-1] = '\0';

    vm->autostart = checkLinkPointsTo(vm->autostartLink, vm->configFile);

    return vm;
}

static struct qemud_network *
qemudLoadNetworkConfig(struct qemud_driver *driver,
                       const char *file,
                       const char *path,
                       const char *xml,
                       const char *autostartLink) {
    struct qemud_network_def *def;
    struct qemud_network *network;

    if (!(def = qemudParseNetworkDef(NULL, driver, xml, file))) {
        virErrorPtr err = virGetLastError();
        qemudLog(QEMUD_WARN, "Error parsing network config '%s' : %s",
                 path, err->message);
        return NULL;
    }

    if (!compareFileToNameSuffix(file, def->name, ".xml")) {
        qemudLog(QEMUD_WARN, "Network config filename '%s' does not match network name '%s'",
                 path, def->name);
        qemudFreeNetworkDef(def);
        return NULL;
    }

    if (!(network = qemudAssignNetworkDef(NULL, driver, def))) {
        qemudLog(QEMUD_WARN, "Failed to load network config '%s': out of memory", path);
        qemudFreeNetworkDef(def);
        return NULL;
    }

    strncpy(network->configFile, path, PATH_MAX);
    network->configFile[PATH_MAX-1] = '\0';

    strncpy(network->autostartLink, autostartLink, PATH_MAX);
    network->autostartLink[PATH_MAX-1] = '\0';

    network->autostart = checkLinkPointsTo(network->autostartLink, network->configFile);

    return network;
}

static
int qemudScanConfigDir(struct qemud_driver *driver,
                       const char *configDir,
                       const char *autostartDir,
                       int isGuest) {
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(configDir))) {
        if (errno == ENOENT)
            return 0;
        qemudLog(QEMUD_ERR, "Failed to open dir '%s': %s",
                 configDir, strerror(errno));
        return -1;
    }

    while ((entry = readdir(dir))) {
        char xml[QEMUD_MAX_XML_LEN];
        char path[PATH_MAX];
        char autostartLink[PATH_MAX];

        if (entry->d_name[0] == '.')
            continue;

        if (!hasSuffix(entry->d_name, ".xml"))
            continue;

        if (qemudMakeConfigPath(configDir, entry->d_name, NULL, path, PATH_MAX) < 0) {
            qemudLog(QEMUD_WARN, "Config filename '%s/%s' is too long",
                     configDir, entry->d_name);
            continue;
        }

        if (qemudMakeConfigPath(autostartDir, entry->d_name, NULL, autostartLink, PATH_MAX) < 0) {
            qemudLog(QEMUD_WARN, "Autostart link path '%s/%s' is too long",
                     autostartDir, entry->d_name);
            continue;
        }

        if (!qemudReadFile(path, xml, QEMUD_MAX_XML_LEN))
            continue;

        if (isGuest)
            qemudLoadConfig(driver, entry->d_name, path, xml, autostartLink);
        else
            qemudLoadNetworkConfig(driver, entry->d_name, path, xml, autostartLink);
    }

    closedir(dir);
 
    return 0;
}

/* Scan for all guest and network config files */
int qemudScanConfigs(struct qemud_driver *driver) {
    if (qemudScanConfigDir(driver, driver->configDir, driver->autostartDir, 1) < 0)
        return -1;

    if (qemudScanConfigDir(driver, driver->networkConfigDir, driver->networkAutostartDir, 0) < 0)
        return -1;

    return 0;
}

/* Generate an XML document describing the guest's configuration */
char *qemudGenerateXML(virConnectPtr conn,
                       struct qemud_driver *driver ATTRIBUTE_UNUSED,
                       struct qemud_vm *vm,
                       struct qemud_vm_def *def,
                       int live) {
    virBufferPtr buf = 0;
    unsigned char *uuid;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    struct qemud_vm_disk_def *disk;
    struct qemud_vm_net_def *net;
    struct qemud_vm_input_def *input;
    const char *type = NULL;
    int n;

    buf = virBufferNew (QEMUD_MAX_XML_LEN);
    if (!buf)
        goto no_memory;

    switch (def->virtType) {
    case QEMUD_VIRT_QEMU:
        type = "qemu";
        break;
    case QEMUD_VIRT_KQEMU:
        type = "kqemu";
        break;
    case QEMUD_VIRT_KVM:
        type = "kvm";
        break;
    }
    if (!type) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "unexpected domain type %d", def->virtType);
        goto cleanup;
    }

    if (qemudIsActiveVM(vm) && live) {
        if (virBufferVSprintf(buf, "<domain type='%s' id='%d'>\n", type, vm->id) < 0)
            goto no_memory;
    } else {
        if (virBufferVSprintf(buf, "<domain type='%s'>\n", type) < 0)
            goto no_memory;
    }

    if (virBufferVSprintf(buf, "  <name>%s</name>\n", def->name) < 0)
        goto no_memory;

    uuid = def->uuid;
    virUUIDFormat(uuid, uuidstr);
    if (virBufferVSprintf(buf, "  <uuid>%s</uuid>\n", uuidstr) < 0)
        goto no_memory;
    if (virBufferVSprintf(buf, "  <memory>%d</memory>\n", def->maxmem) < 0)
        goto no_memory;
    if (virBufferVSprintf(buf, "  <currentMemory>%d</currentMemory>\n", def->memory) < 0)
        goto no_memory;
    if (virBufferVSprintf(buf, "  <vcpu>%d</vcpu>\n", def->vcpus) < 0)
        goto no_memory;

    if (virBufferAdd(buf, "  <os>\n", -1) < 0)
        goto no_memory;

    if (def->virtType == QEMUD_VIRT_QEMU) {
        if (virBufferVSprintf(buf, "    <type arch='%s' machine='%s'>%s</type>\n",
                              def->os.arch, def->os.machine, def->os.type) < 0)
            goto no_memory;
    } else {
        if (virBufferVSprintf(buf, "    <type>%s</type>\n", def->os.type) < 0)
            goto no_memory;
    }

    if (def->os.kernel[0])
        if (virBufferVSprintf(buf, "    <kernel>%s</kernel>\n", def->os.kernel) < 0)
            goto no_memory;
    if (def->os.initrd[0])
        if (virBufferVSprintf(buf, "    <initrd>%s</initrd>\n", def->os.initrd) < 0)
            goto no_memory;
    if (def->os.cmdline[0])
        if (virBufferVSprintf(buf, "    <cmdline>%s</cmdline>\n", def->os.cmdline) < 0)
            goto no_memory;

    for (n = 0 ; n < def->os.nBootDevs ; n++) {
        const char *boottype = "hd";
        switch (def->os.bootDevs[n]) {
        case QEMUD_BOOT_FLOPPY:
            boottype = "fd";
            break;
        case QEMUD_BOOT_DISK:
            boottype = "hd";
            break;
        case QEMUD_BOOT_CDROM:
            boottype = "cdrom";
            break;
        case QEMUD_BOOT_NET:
            boottype = "network";
            break;
        }
        if (virBufferVSprintf(buf, "    <boot dev='%s'/>\n", boottype) < 0)
            goto no_memory;
    }

    if (virBufferAdd(buf, "  </os>\n", -1) < 0)
        goto no_memory;

    if (def->features & QEMUD_FEATURE_ACPI) {
        if (virBufferAdd(buf, "  <features>\n", -1) < 0)
            goto no_memory;
        if (virBufferAdd(buf, "    <acpi/>\n", -1) < 0)
            goto no_memory;
        if (virBufferAdd(buf, "  </features>\n", -1) < 0)
            goto no_memory;
    }

    virBufferVSprintf(buf, "  <clock offset='%s'/>\n", def->localtime ? "localtime" : "utc");

    if (virBufferAdd(buf, "  <on_poweroff>destroy</on_poweroff>\n", -1) < 0)
        goto no_memory;
    if (def->noReboot) {
        if (virBufferAdd(buf, "  <on_reboot>destroy</on_reboot>\n", -1) < 0)
            goto no_memory;
    } else {
        if (virBufferAdd(buf, "  <on_reboot>restart</on_reboot>\n", -1) < 0)
            goto no_memory;
    }
    if (virBufferAdd(buf, "  <on_crash>destroy</on_crash>\n", -1) < 0)
        goto no_memory;

    if (virBufferAdd(buf, "  <devices>\n", -1) < 0)
        goto no_memory;

    if (virBufferVSprintf(buf, "    <emulator>%s</emulator>\n", def->os.binary) < 0)
        goto no_memory;

    disk = def->disks;
    while (disk) {
        const char *types[] = {
            "block",
            "file",
        };
        const char *typeAttrs[] = {
            "dev",
            "file",
        };
        const char *devices[] = {
            "disk",
            "cdrom",
            "floppy",
        };
        if (virBufferVSprintf(buf, "    <disk type='%s' device='%s'>\n",
                              types[disk->type], devices[disk->device]) < 0)
            goto no_memory;

        if (virBufferVSprintf(buf, "      <source %s='%s'/>\n", typeAttrs[disk->type], disk->src) < 0)
            goto no_memory;

        if (virBufferVSprintf(buf, "      <target dev='%s'/>\n", disk->dst) < 0)
            goto no_memory;

        if (disk->readonly)
            if (virBufferAdd(buf, "      <readonly/>\n", -1) < 0)
                goto no_memory;

        if (virBufferVSprintf(buf, "    </disk>\n") < 0)
            goto no_memory;

        disk = disk->next;
    }

    net = def->nets;
    while (net) {
        const char *types[] = {
            "user",
            "ethernet",
            "server",
            "client",
            "mcast",
            "network",
            "bridge",
        };
        if (virBufferVSprintf(buf, "    <interface type='%s'>\n",
                              types[net->type]) < 0)
            goto no_memory;

        if (virBufferVSprintf(buf, "      <mac address='%02x:%02x:%02x:%02x:%02x:%02x'/>\n",
                              net->mac[0], net->mac[1], net->mac[2],
                              net->mac[3], net->mac[4], net->mac[5]) < 0)
            goto no_memory;

        switch (net->type) {
        case QEMUD_NET_NETWORK:
            if (virBufferVSprintf(buf, "      <source network='%s'/>\n", net->dst.network.name) < 0)
                goto no_memory;

            if (net->dst.network.ifname[0] != '\0') {
                if (virBufferVSprintf(buf, "      <target dev='%s'/>\n", net->dst.network.ifname) < 0)
                    goto no_memory;
            }
            break;

        case QEMUD_NET_ETHERNET:
            if (net->dst.ethernet.ifname[0] != '\0') {
                if (virBufferVSprintf(buf, "      <target dev='%s'/>\n", net->dst.ethernet.ifname) < 0)
                    goto no_memory;
            }
            if (net->dst.ethernet.script[0] != '\0') {
                if (virBufferVSprintf(buf, "      <script path='%s'/>\n", net->dst.ethernet.script) < 0)
                    goto no_memory;
            }
            break;

        case QEMUD_NET_BRIDGE:
            if (virBufferVSprintf(buf, "      <source bridge='%s'/>\n", net->dst.bridge.brname) < 0)
                goto no_memory;
            if (net->dst.bridge.ifname[0] != '\0') {
                if (virBufferVSprintf(buf, "      <target dev='%s'/>\n", net->dst.bridge.ifname) < 0)
                    goto no_memory;
            }
            break;

        case QEMUD_NET_SERVER:
        case QEMUD_NET_CLIENT:
        case QEMUD_NET_MCAST:
            if (net->dst.socket.address[0] != '\0') {
                if (virBufferVSprintf(buf, "      <source address='%s' port='%d'/>\n",
                                      net->dst.socket.address, net->dst.socket.port) < 0)
                    goto no_memory;
            } else {
                if (virBufferVSprintf(buf, "      <source port='%d'/>\n",
                                      net->dst.socket.port) < 0)
                    goto no_memory;
            }
        }

        if (virBufferVSprintf(buf, "    </interface>\n") < 0)
            goto no_memory;

        net = net->next;
    }

    input = def->inputs;
    while (input) {
        if (input->bus != QEMU_INPUT_BUS_PS2 &&
            virBufferVSprintf(buf, "    <input type='%s' bus='usb'/>\n",
                              input->type == QEMU_INPUT_TYPE_MOUSE ? "mouse" : "tablet") < 0)
            goto no_memory;
        input = input->next;
    }
    /* If graphics is enable, add implicit mouse */
    if (def->graphicsType != QEMUD_GRAPHICS_NONE)
        if (virBufferAdd(buf, "    <input type='mouse' bus='ps2'/>\n", -1) < 0)
            goto no_memory;

    switch (def->graphicsType) {
    case QEMUD_GRAPHICS_VNC:
        if (virBufferAdd(buf, "    <graphics type='vnc'", -1) < 0)
            goto no_memory;

        if (def->vncPort &&
            virBufferVSprintf(buf, " port='%d'",
                              qemudIsActiveVM(vm) && live ? def->vncActivePort : def->vncPort) < 0)
            goto no_memory;

        if (def->vncListen[0] &&
            virBufferVSprintf(buf, " listen='%s'",
                              def->vncListen) < 0)
            goto no_memory;

        if (virBufferAdd(buf, "/>\n", -1) < 0)
            goto no_memory;
        break;

    case QEMUD_GRAPHICS_SDL:
        if (virBufferAdd(buf, "    <graphics type='sdl'/>\n", -1) < 0)
            goto no_memory;
        break;

    case QEMUD_GRAPHICS_NONE:
    default:
        break;
    }

    if (def->graphicsType == QEMUD_GRAPHICS_VNC) {
    }

    if (virBufferAdd(buf, "  </devices>\n", -1) < 0)
        goto no_memory;


    if (virBufferAdd(buf, "</domain>\n", -1) < 0)
        goto no_memory;

    return virBufferContentAndFree (buf);

 no_memory:
    qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "xml");
 cleanup:
    if (buf) virBufferFree (buf);
    return NULL;
}


char *qemudGenerateNetworkXML(virConnectPtr conn,
                              struct qemud_driver *driver ATTRIBUTE_UNUSED,
                              struct qemud_network *network,
                              struct qemud_network_def *def) {
    virBufferPtr buf = 0;
    unsigned char *uuid;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    buf = virBufferNew (QEMUD_MAX_XML_LEN);
    if (!buf)
        goto no_memory;

    if (virBufferVSprintf(buf, "<network>\n") < 0)
        goto no_memory;

    if (virBufferVSprintf(buf, "  <name>%s</name>\n", def->name) < 0)
        goto no_memory;

    uuid = def->uuid;
    virUUIDFormat(uuid, uuidstr);
    if (virBufferVSprintf(buf, "  <uuid>%s</uuid>\n", uuidstr) < 0)
        goto no_memory;

    if (def->forward) {
        if (def->forwardDev[0]) {
            virBufferVSprintf(buf, "  <forward dev='%s'/>\n",
                              def->forwardDev);
        } else {
            virBufferAdd(buf, "  <forward/>\n", -1);
        }
    }

    virBufferAdd(buf, "  <bridge", -1);
    if (qemudIsActiveNetwork(network)) {
        if (virBufferVSprintf(buf, " name='%s'", network->bridge) < 0)
            goto no_memory;
    } else if (def->bridge[0]) {
        if (virBufferVSprintf(buf, " name='%s'", def->bridge) < 0)
            goto no_memory;
    }
    if (virBufferVSprintf(buf, " stp='%s' forwardDelay='%d' />\n",
                       def->disableSTP ? "off" : "on",
                       def->forwardDelay) < 0)
        goto no_memory;

    if (def->ipAddress[0] || def->netmask[0]) {
        if (virBufferAdd(buf, "  <ip", -1) < 0)
            goto no_memory;

        if (def->ipAddress[0] &&
            virBufferVSprintf(buf, " address='%s'", def->ipAddress) < 0)
            goto no_memory;

        if (def->netmask[0] &&
            virBufferVSprintf(buf, " netmask='%s'", def->netmask) < 0)
            goto no_memory;

        if (virBufferAdd(buf, ">\n", -1) < 0)
            goto no_memory;

        if (def->ranges) {
            struct qemud_dhcp_range_def *range = def->ranges;
            if (virBufferAdd(buf, "    <dhcp>\n", -1) < 0)
                goto no_memory;
            while (range) {
                if (virBufferVSprintf(buf, "      <range start='%s' end='%s' />\n",
                                      range->start, range->end) < 0)
                    goto no_memory;
                range = range->next;
            }
            if (virBufferAdd(buf, "    </dhcp>\n", -1) < 0)
                goto no_memory;
        }

        if (virBufferAdd(buf, "  </ip>\n", -1) < 0)
            goto no_memory;
    }

    if (virBufferAdd(buf, "</network>\n", -1) < 0)
        goto no_memory;

    return virBufferContentAndFree (buf);

 no_memory:
    qemudReportError(conn, NULL, NULL, VIR_ERR_NO_MEMORY, "xml");
    if (buf) virBufferFree (buf);
    return NULL;
}


int qemudDeleteConfig(virConnectPtr conn,
                      struct qemud_driver *driver ATTRIBUTE_UNUSED,
                      const char *configFile,
                      const char *name) {
    if (!configFile[0]) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "no config file for %s", name);
        return -1;
    }

    if (unlink(configFile) < 0) {
        qemudReportError(conn, NULL, NULL, VIR_ERR_INTERNAL_ERROR, "cannot remove config for %s", name);
        return -1;
    }

    return 0;
}


/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
