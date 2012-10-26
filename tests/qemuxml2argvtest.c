#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <fcntl.h>

#ifdef WITH_QEMU

# include "internal.h"
# include "testutils.h"
# include "util/memory.h"
# include "qemu/qemu_capabilities.h"
# include "qemu/qemu_command.h"
# include "qemu/qemu_domain.h"
# include "datatypes.h"
# include "cpu/cpu_map.h"

# include "testutilsqemu.h"

static const char *abs_top_srcdir;
static struct qemud_driver driver;

static unsigned char *
fakeSecretGetValue(virSecretPtr obj ATTRIBUTE_UNUSED,
                   size_t *value_size,
                   unsigned int fakeflags ATTRIBUTE_UNUSED,
                   unsigned int internalFlags ATTRIBUTE_UNUSED)
{
    char *secret = strdup("AQCVn5hO6HzFAhAAq0NCv8jtJcIcE+HOBlMQ1A");
    *value_size = strlen(secret);
    return (unsigned char *) secret;
}

static virSecretPtr
fakeSecretLookupByUsage(virConnectPtr conn,
                        int usageType ATTRIBUTE_UNUSED,
                        const char *usageID)
{
    unsigned char uuid[VIR_UUID_BUFLEN];
    if (STRNEQ(usageID, "mycluster_myname"))
        return NULL;

    virUUIDGenerate(uuid);
    return virGetSecret(conn, uuid, usageType, usageID);
}

static int
fakeSecretClose(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 0;
}

static virSecretDriver fakeSecretDriver = {
    .name = "fake_secret",
    .open = NULL,
    .close = fakeSecretClose,
    .numOfSecrets = NULL,
    .listSecrets = NULL,
    .lookupByUUID = NULL,
    .lookupByUsage = fakeSecretLookupByUsage,
    .defineXML = NULL,
    .getXMLDesc = NULL,
    .setValue = NULL,
    .getValue = fakeSecretGetValue,
    .undefine = NULL,
};

typedef enum {
    FLAG_EXPECT_ERROR       = 1 << 0,
    FLAG_EXPECT_FAILURE     = 1 << 1,
    FLAG_EXPECT_PARSE_ERROR = 1 << 2,
    FLAG_JSON               = 1 << 3,
} virQemuXML2ArgvTestFlags;

static int testCompareXMLToArgvFiles(const char *xml,
                                     const char *cmdline,
                                     qemuCapsPtr extraFlags,
                                     const char *migrateFrom,
                                     int migrateFd,
                                     virQemuXML2ArgvTestFlags flags)
{
    char *expectargv = NULL;
    int len;
    char *actualargv = NULL;
    int ret = -1;
    virDomainDefPtr vmdef = NULL;
    virDomainChrSourceDef monitor_chr;
    virConnectPtr conn;
    char *log = NULL;
    char *emulator = NULL;
    virCommandPtr cmd = NULL;

    if (!(conn = virGetConnect()))
        goto out;
    conn->secretDriver = &fakeSecretDriver;

    if (!(vmdef = virDomainDefParseFile(driver.caps, xml,
                                        QEMU_EXPECTED_VIRT_TYPES,
                                        VIR_DOMAIN_XML_INACTIVE))) {
        if (flags & FLAG_EXPECT_PARSE_ERROR)
            goto ok;
        goto out;
    }

    if (qemuCapsGet(extraFlags, QEMU_CAPS_DOMID))
        vmdef->id = 6;
    else
        vmdef->id = -1;

    memset(&monitor_chr, 0, sizeof(monitor_chr));
    monitor_chr.type = VIR_DOMAIN_CHR_TYPE_UNIX;
    monitor_chr.data.nix.path = (char *)"/tmp/test-monitor";
    monitor_chr.data.nix.listen = true;

    qemuCapsSetList(extraFlags,
                    QEMU_CAPS_VNC_COLON,
                    QEMU_CAPS_NO_REBOOT,
                    QEMU_CAPS_NO_ACPI,
                    QEMU_CAPS_LAST);

    if (STREQ(vmdef->os.machine, "pc") &&
        STREQ(vmdef->emulator, "/usr/bin/qemu-system-x86_64")) {
        VIR_FREE(vmdef->os.machine);
        if (!(vmdef->os.machine = strdup("pc-0.11")))
            goto out;
    }

    if (qemuCapsGet(extraFlags, QEMU_CAPS_DEVICE)) {
        if (qemuDomainAssignAddresses(vmdef, extraFlags, NULL)) {
            if (flags & FLAG_EXPECT_ERROR)
                goto ok;
            goto out;
        }
    }

    log = virtTestLogContentAndReset();
    VIR_FREE(log);
    virResetLastError();

    if (STREQLEN(vmdef->os.arch, "x86_64", 6) ||
        STREQLEN(vmdef->os.arch, "i686", 4)) {
        qemuCapsSet(extraFlags, QEMU_CAPS_PCI_MULTIBUS);
    }

    if (qemuAssignDeviceAliases(vmdef, extraFlags) < 0)
        goto out;

    if (!(cmd = qemuBuildCommandLine(conn, &driver, vmdef, &monitor_chr,
                                     (flags & FLAG_JSON), extraFlags,
                                     migrateFrom, migrateFd, NULL,
                                     VIR_NETDEV_VPORT_PROFILE_OP_NO_OP))) {
        if (flags & FLAG_EXPECT_FAILURE) {
            ret = 0;
            virResetLastError();
        }
        goto out;
    } else if (flags & FLAG_EXPECT_FAILURE) {
        if (virTestGetDebug())
            fprintf(stderr, "qemuBuildCommandLine should have failed\n");
        goto out;
    }

    if (!!virGetLastError() != !!(flags & FLAG_EXPECT_ERROR)) {
        if (virTestGetDebug() && (log = virtTestLogContentAndReset()))
            fprintf(stderr, "\n%s", log);
        goto out;
    }

    if (!(actualargv = virCommandToString(cmd)))
        goto out;

    if (emulator) {
        /* Skip the abs_srcdir portion of replacement emulator.  */
        char *start_skip = strstr(actualargv, abs_srcdir);
        char *end_skip = strstr(actualargv, emulator);
        if (!start_skip || !end_skip)
            goto out;
        memmove(start_skip, end_skip, strlen(end_skip) + 1);
    }

    len = virtTestLoadFile(cmdline, &expectargv);
    if (len < 0)
        goto out;
    if (len && expectargv[len - 1] == '\n')
        expectargv[len - 1] = '\0';

    if (STRNEQ(expectargv, actualargv)) {
        virtTestDifference(stderr, expectargv, actualargv);
        goto out;
    }

 ok:
    if (flags & FLAG_EXPECT_ERROR) {
        /* need to suppress the errors */
        virResetLastError();
    }

    ret = 0;

out:
    VIR_FREE(log);
    VIR_FREE(emulator);
    VIR_FREE(expectargv);
    VIR_FREE(actualargv);
    virCommandFree(cmd);
    virDomainDefFree(vmdef);
    virObjectUnref(conn);
    return ret;
}


struct testInfo {
    const char *name;
    qemuCapsPtr extraFlags;
    const char *migrateFrom;
    int migrateFd;
    unsigned int flags;
};

static int
testCompareXMLToArgvHelper(const void *data)
{
    int result = -1;
    const struct testInfo *info = data;
    char *xml = NULL;
    char *args = NULL;
    unsigned int flags = info->flags;

    if (virAsprintf(&xml, "%s/qemuxml2argvdata/qemuxml2argv-%s.xml",
                    abs_srcdir, info->name) < 0 ||
        virAsprintf(&args, "%s/qemuxml2argvdata/qemuxml2argv-%s.args",
                    abs_srcdir, info->name) < 0)
        goto cleanup;

    if (qemuCapsGet(info->extraFlags, QEMU_CAPS_MONITOR_JSON))
        flags |= FLAG_JSON;

    result = testCompareXMLToArgvFiles(xml, args, info->extraFlags,
                                       info->migrateFrom, info->migrateFd,
                                       flags);

cleanup:
    VIR_FREE(xml);
    VIR_FREE(args);
    return result;
}


static int
testAddCPUModels(qemuCapsPtr caps, bool skipLegacy)
{
    const char *newModels[] = {
        "Opteron_G3", "Opteron_G2", "Opteron_G1",
        "Nehalem", "Penryn", "Conroe",
    };
    const char *legacyModels[] = {
        "n270", "athlon", "pentium3", "pentium2", "pentium",
        "486", "coreduo", "kvm32", "qemu32", "kvm64",
        "core2duo", "phenom", "qemu64",
    };
    size_t i;

    for (i = 0 ; i < ARRAY_CARDINALITY(newModels) ; i++) {
        if (qemuCapsAddCPUDefinition(caps, newModels[i]) < 0)
            return -1;
    }
    if (skipLegacy)
        return 0;
    for (i = 0 ; i < ARRAY_CARDINALITY(legacyModels) ; i++) {
        if (qemuCapsAddCPUDefinition(caps, legacyModels[i]) < 0)
            return -1;
    }
    return 0;
}


static int
mymain(void)
{
    int ret = 0;
    char *map = NULL;
    bool skipLegacyCPUs = false;

    abs_top_srcdir = getenv("abs_top_srcdir");
    if (!abs_top_srcdir)
        abs_top_srcdir = "..";

    if ((driver.caps = testQemuCapsInit()) == NULL)
        return EXIT_FAILURE;
    if ((driver.stateDir = strdup("/nowhere")) == NULL)
        return EXIT_FAILURE;
    if ((driver.hugetlbfs_mount = strdup("/dev/hugepages")) == NULL)
        return EXIT_FAILURE;
    if ((driver.hugepage_path = strdup("/dev/hugepages/libvirt/qemu")) == NULL)
        return EXIT_FAILURE;
    driver.spiceTLS = 1;
    if (!(driver.spiceTLSx509certdir = strdup("/etc/pki/libvirt-spice")))
        return EXIT_FAILURE;
    if (!(driver.spicePassword = strdup("123456")))
        return EXIT_FAILURE;
    if (virAsprintf(&map, "%s/src/cpu/cpu_map.xml", abs_top_srcdir) < 0 ||
        cpuMapOverride(map) < 0) {
        VIR_FREE(map);
        return EXIT_FAILURE;
    }

# define DO_TEST_FULL(name, migrateFrom, migrateFd, flags, ...)         \
    do {                                                                \
        static struct testInfo info = {                                 \
            name, NULL, migrateFrom, migrateFd, (flags)                 \
        };                                                              \
        if (!(info.extraFlags = qemuCapsNew()))                         \
            return EXIT_FAILURE;                                        \
        if (testAddCPUModels(info.extraFlags, skipLegacyCPUs) < 0)      \
            return EXIT_FAILURE;                                        \
        qemuCapsSetList(info.extraFlags, __VA_ARGS__, QEMU_CAPS_LAST);  \
        if (virtTestRun("QEMU XML-2-ARGV " name,                        \
                        1, testCompareXMLToArgvHelper, &info) < 0)      \
            ret = -1;                                                   \
        virObjectUnref(info.extraFlags);                                \
    } while (0)

# define DO_TEST(name, ...)                                             \
    DO_TEST_FULL(name, NULL, -1, 0, __VA_ARGS__)

# define DO_TEST_ERROR(name, ...)                                       \
    DO_TEST_FULL(name, NULL, -1, FLAG_EXPECT_ERROR, __VA_ARGS__)

# define DO_TEST_FAILURE(name, ...)                                     \
    DO_TEST_FULL(name, NULL, -1, FLAG_EXPECT_FAILURE, __VA_ARGS__)

# define DO_TEST_PARSE_ERROR(name, ...)                                 \
    DO_TEST_FULL(name, NULL, -1,                                        \
                 FLAG_EXPECT_PARSE_ERROR | FLAG_EXPECT_ERROR,           \
                 __VA_ARGS__)

# define NONE QEMU_CAPS_LAST

    /* Unset or set all envvars here that are copied in qemudBuildCommandLine
     * using ADD_ENV_COPY, otherwise these tests may fail due to unexpected
     * values for these envvars */
    setenv("PATH", "/bin", 1);
    setenv("USER", "test", 1);
    setenv("LOGNAME", "test", 1);
    setenv("HOME", "/home/test", 1);
    unsetenv("TMPDIR");
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("QEMU_AUDIO_DRV");
    unsetenv("SDL_AUDIODRIVER");

    DO_TEST("minimal", QEMU_CAPS_NAME);
    DO_TEST("minimal-s390", QEMU_CAPS_NAME);
    DO_TEST("machine-aliases1", NONE);
    DO_TEST("machine-aliases2", QEMU_CAPS_KVM);
    DO_TEST("machine-core-on", QEMU_CAPS_DUMP_GUEST_CORE);
    DO_TEST("machine-core-off", QEMU_CAPS_DUMP_GUEST_CORE);
    DO_TEST_FAILURE("machine-core-on", NONE);
    DO_TEST("boot-cdrom", NONE);
    DO_TEST("boot-network", NONE);
    DO_TEST("boot-floppy", NONE);
    DO_TEST("boot-multi", QEMU_CAPS_BOOT_MENU);
    DO_TEST("boot-menu-enable",
            QEMU_CAPS_BOOT_MENU, QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE);
    DO_TEST("boot-menu-enable",
            QEMU_CAPS_BOOT_MENU, QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE,
            QEMU_CAPS_BOOTINDEX);
    DO_TEST("boot-menu-disable", QEMU_CAPS_BOOT_MENU);
    DO_TEST("boot-menu-disable-drive",
            QEMU_CAPS_BOOT_MENU, QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE);
    DO_TEST("boot-menu-disable-drive-bootindex",
            QEMU_CAPS_BOOT_MENU, QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE,
            QEMU_CAPS_BOOTINDEX);
    DO_TEST_PARSE_ERROR("boot-dev+order",
            QEMU_CAPS_BOOTINDEX, QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("boot-order",
            QEMU_CAPS_BOOTINDEX, QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("boot-complex",
            QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("boot-complex-bootindex",
            QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT,
            QEMU_CAPS_BOOTINDEX,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("bootloader", QEMU_CAPS_DOMID, QEMU_CAPS_KVM);

    DO_TEST("reboot-timeout-disabled", QEMU_CAPS_REBOOT_TIMEOUT);
    DO_TEST("reboot-timeout-enabled", QEMU_CAPS_REBOOT_TIMEOUT);
    DO_TEST_FAILURE("reboot-timeout-enabled", NONE);

    DO_TEST("bios", QEMU_CAPS_DEVICE, QEMU_CAPS_SGA);
    DO_TEST("clock-utc", NONE);
    DO_TEST("clock-localtime", NONE);
    /*
     * Can't be enabled since the absolute timestamp changes every time
    DO_TEST("clock-variable", QEMU_CAPS_RTC);
    */
    DO_TEST("clock-france", QEMU_CAPS_RTC);
    DO_TEST("cpu-kvmclock", QEMU_CAPS_ENABLE_KVM);
    DO_TEST("cpu-host-kvmclock", QEMU_CAPS_ENABLE_KVM, QEMU_CAPS_CPU_HOST);
    DO_TEST("kvmclock", QEMU_CAPS_KVM);

    DO_TEST("cpu-eoi-disabled", QEMU_CAPS_ENABLE_KVM);
    DO_TEST("cpu-eoi-enabled", QEMU_CAPS_ENABLE_KVM);
    DO_TEST("eoi-disabled", NONE);
    DO_TEST("eoi-enabled", NONE);
    DO_TEST("kvmclock+eoi-disabled", QEMU_CAPS_ENABLE_KVM);

    DO_TEST("hyperv", NONE);

    DO_TEST("hugepages", QEMU_CAPS_MEM_PATH);
    DO_TEST("disk-cdrom", NONE);
    DO_TEST("disk-cdrom-empty", QEMU_CAPS_DRIVE);
    DO_TEST("disk-cdrom-tray",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_VIRTIO_TX_ALG);
    DO_TEST("disk-cdrom-tray-no-device-cap", NONE);
    DO_TEST("disk-floppy", NONE);
    DO_TEST("disk-floppy-tray-no-device-cap", NONE);
    DO_TEST("disk-floppy-tray",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE);
    DO_TEST("disk-virtio-s390", QEMU_CAPS_DRIVE,
            QEMU_CAPS_DEVICE, QEMU_CAPS_VIRTIO_S390);
    DO_TEST("disk-many", NONE);
    DO_TEST("disk-virtio", QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT);
    DO_TEST("disk-order",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_DRIVE_BOOT,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("disk-xenvbd", QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT);
    DO_TEST("disk-drive-boot-disk",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT);
    DO_TEST("disk-drive-boot-cdrom",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT);
    DO_TEST("floppy-drive-fat",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-fat",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-readonly-disk",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_READONLY,
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("disk-drive-readonly-no-device",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_READONLY, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("disk-drive-fmt-qcow",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_BOOT, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-shared",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT, QEMU_CAPS_DRIVE_SERIAL);
    DO_TEST("disk-drive-cache-v1-wt",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-v1-wb",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-v1-none",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-error-policy-stop",
            QEMU_CAPS_DRIVE, QEMU_CAPS_MONITOR_JSON, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-error-policy-enospace",
            QEMU_CAPS_DRIVE, QEMU_CAPS_MONITOR_JSON, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-error-policy-wreport-rignore",
            QEMU_CAPS_DRIVE, QEMU_CAPS_MONITOR_JSON, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-v2-wt",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_CACHE_V2, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-v2-wb",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_CACHE_V2, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-v2-none",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_CACHE_V2, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-directsync",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_CACHE_V2,
            QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-cache-unsafe",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_CACHE_V2,
            QEMU_CAPS_DRIVE_CACHE_UNSAFE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-network-nbd",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-network-rbd",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-network-sheepdog",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-network-rbd-auth",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-drive-no-boot",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_BOOTINDEX);
    DO_TEST("disk-usb",  NONE);
    DO_TEST("disk-usb-device",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("disk-scsi-device",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_SCSI_LSI);
    DO_TEST("disk-scsi-device-auto",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_SCSI_LSI);
    DO_TEST("disk-scsi-disk-split",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_SCSI_CD, QEMU_CAPS_SCSI_LSI, QEMU_CAPS_VIRTIO_SCSI_PCI);
    DO_TEST("disk-scsi-disk-wwn",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_SCSI_CD, QEMU_CAPS_SCSI_LSI, QEMU_CAPS_VIRTIO_SCSI_PCI,
            QEMU_CAPS_SCSI_DISK_WWN);
    DO_TEST("disk-scsi-vscsi",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("disk-scsi-virtio-scsi",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_VIRTIO_SCSI_PCI);
    DO_TEST("disk-sata-device",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE,
            QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_ICH9_AHCI);
    DO_TEST("disk-aio",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_AIO,
            QEMU_CAPS_DRIVE_CACHE_V2, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("disk-ioeventfd",
            QEMU_CAPS_DRIVE, QEMU_CAPS_VIRTIO_IOEVENTFD,
            QEMU_CAPS_VIRTIO_TX_ALG, QEMU_CAPS_DEVICE,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("disk-copy_on_read",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_COPY_ON_READ,
            QEMU_CAPS_VIRTIO_TX_ALG, QEMU_CAPS_DEVICE,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("disk-snapshot",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_CACHE_V2, QEMU_CAPS_DRIVE_FORMAT);
    DO_TEST("event_idx",
            QEMU_CAPS_DRIVE,
            QEMU_CAPS_VIRTIO_BLK_EVENT_IDX,
            QEMU_CAPS_VIRTIO_NET_EVENT_IDX,
            QEMU_CAPS_DEVICE,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("virtio-lun",
            QEMU_CAPS_DRIVE,
            QEMU_CAPS_DEVICE,
            QEMU_CAPS_VIRTIO_BLK_SCSI, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    DO_TEST("disk-scsi-lun-passthrough",
            QEMU_CAPS_DRIVE,
            QEMU_CAPS_DEVICE,
            QEMU_CAPS_SCSI_BLOCK, QEMU_CAPS_VIRTIO_BLK_SG_IO,
            QEMU_CAPS_SCSI_LSI, QEMU_CAPS_VIRTIO_SCSI_PCI);

    DO_TEST("graphics-vnc", QEMU_CAPS_VNC);
    DO_TEST("graphics-vnc-socket", QEMU_CAPS_VNC);

    driver.vncSASL = 1;
    driver.vncSASLdir = strdup("/root/.sasl2");
    DO_TEST("graphics-vnc-sasl", QEMU_CAPS_VNC, QEMU_CAPS_VGA);
    driver.vncTLS = 1;
    driver.vncTLSx509verify = 1;
    driver.vncTLSx509certdir = strdup("/etc/pki/tls/qemu");
    DO_TEST("graphics-vnc-tls", QEMU_CAPS_VNC);
    driver.vncSASL = driver.vncTLSx509verify = driver.vncTLS = 0;
    VIR_FREE(driver.vncSASLdir);
    VIR_FREE(driver.vncTLSx509certdir);
    driver.vncSASLdir = driver.vncTLSx509certdir = NULL;

    DO_TEST("graphics-sdl", NONE);
    DO_TEST("graphics-sdl-fullscreen", NONE);
    DO_TEST("nographics", QEMU_CAPS_VGA);
    DO_TEST("nographics-vga",
            QEMU_CAPS_VGA, QEMU_CAPS_VGA_NONE);
    DO_TEST("graphics-spice",
            QEMU_CAPS_VGA, QEMU_CAPS_VGA_QXL,
            QEMU_CAPS_DEVICE, QEMU_CAPS_SPICE);
    DO_TEST("graphics-spice-agentmouse",
            QEMU_CAPS_VGA, QEMU_CAPS_VGA_QXL,
            QEMU_CAPS_DEVICE, QEMU_CAPS_SPICE,
            QEMU_CAPS_CHARDEV_SPICEVMC,
            QEMU_CAPS_NODEFCONFIG);
    DO_TEST("graphics-spice-compression",
            QEMU_CAPS_VGA, QEMU_CAPS_VGA_QXL,
            QEMU_CAPS_DEVICE, QEMU_CAPS_SPICE);
    DO_TEST("graphics-spice-timeout",
            QEMU_CAPS_DRIVE,
            QEMU_CAPS_VGA, QEMU_CAPS_VGA_QXL,
            QEMU_CAPS_DEVICE, QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_QXL_VGA);
    DO_TEST("graphics-spice-qxl-vga",
            QEMU_CAPS_VGA, QEMU_CAPS_VGA_QXL,
            QEMU_CAPS_DEVICE, QEMU_CAPS_SPICE,
            QEMU_CAPS_DEVICE_QXL_VGA);
    DO_TEST("graphics-spice-usb-redir",
            QEMU_CAPS_VGA, QEMU_CAPS_SPICE,
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1, QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_CHARDEV_SPICEVMC);

    DO_TEST("input-usbmouse", NONE);
    DO_TEST("input-usbtablet", NONE);
    DO_TEST("input-xen", QEMU_CAPS_DOMID, QEMU_CAPS_KVM, QEMU_CAPS_VNC);
    DO_TEST("misc-acpi", NONE);
    DO_TEST("misc-disable-s3", QEMU_CAPS_DISABLE_S3);
    DO_TEST("misc-disable-suspends", QEMU_CAPS_DISABLE_S3, QEMU_CAPS_DISABLE_S4);
    DO_TEST("misc-enable-s4", QEMU_CAPS_DISABLE_S4);
    DO_TEST_FAILURE("misc-enable-s4", NONE);
    DO_TEST("misc-no-reboot", NONE);
    DO_TEST("misc-uuid", QEMU_CAPS_NAME, QEMU_CAPS_UUID);
    DO_TEST("net-user", NONE);
    DO_TEST("net-virtio", NONE);
    DO_TEST("net-virtio-device",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_VIRTIO_TX_ALG);
    DO_TEST("net-virtio-netdev",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NETDEV, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("net-virtio-s390",
            QEMU_CAPS_DEVICE, QEMU_CAPS_VIRTIO_S390);
    DO_TEST("net-eth", NONE);
    DO_TEST("net-eth-ifname", NONE);
    DO_TEST("net-eth-names", QEMU_CAPS_NET_NAME);
    DO_TEST("net-client", NONE);
    DO_TEST("net-server", NONE);
    DO_TEST("net-mcast", NONE);
    DO_TEST("net-hostdev",
            QEMU_CAPS_PCIDEVICE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);

    DO_TEST("serial-vc", NONE);
    DO_TEST("serial-pty", NONE);
    DO_TEST("serial-dev", NONE);
    DO_TEST("serial-file", NONE);
    DO_TEST("serial-unix", NONE);
    DO_TEST("serial-tcp", NONE);
    DO_TEST("serial-udp", NONE);
    DO_TEST("serial-tcp-telnet", NONE);
    DO_TEST("serial-many", NONE);
    DO_TEST("parallel-tcp", NONE);
    DO_TEST("console-compat", NONE);
    DO_TEST("console-compat-auto", NONE);

    DO_TEST("serial-vc-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-pty-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-dev-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-file-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-unix-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-tcp-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-udp-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-tcp-telnet-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("serial-many-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("parallel-tcp-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("parallel-parport-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("console-compat-chardev",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);

    DO_TEST("channel-guestfwd",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("channel-virtio",
            QEMU_CAPS_DEVICE, QEMU_CAPS_CHARDEV, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("channel-virtio-auto",
            QEMU_CAPS_DEVICE, QEMU_CAPS_CHARDEV, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("console-virtio",
            QEMU_CAPS_DEVICE, QEMU_CAPS_CHARDEV, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("console-virtio-many",
            QEMU_CAPS_DEVICE, QEMU_CAPS_CHARDEV, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("console-virtio-s390",
            QEMU_CAPS_DEVICE, QEMU_CAPS_CHARDEV, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_DRIVE,  QEMU_CAPS_VIRTIO_S390);
    DO_TEST("channel-spicevmc",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_SPICE, QEMU_CAPS_CHARDEV_SPICEVMC);
    DO_TEST("channel-spicevmc-old",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_SPICE, QEMU_CAPS_DEVICE_SPICEVMC);

    DO_TEST("smartcard-host",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE,
            QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_CCID_EMULATED);
    DO_TEST("smartcard-host-certificates",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE,
            QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_CCID_EMULATED);
    DO_TEST("smartcard-passthrough-tcp",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE,
            QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_CCID_PASSTHRU);
    DO_TEST("smartcard-passthrough-spicevmc",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_CCID_PASSTHRU, QEMU_CAPS_CHARDEV_SPICEVMC);
    DO_TEST("smartcard-controller",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE,
            QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_CCID_EMULATED);

    DO_TEST("usb-controller",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE,
            QEMU_CAPS_NODEFCONFIG);
    DO_TEST("usb-piix3-controller",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("usb-ich9-ehci-addr",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST("input-usbmouse-addr",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("usb-ich9-companion",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST("usb-hub",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_USB_HUB,
            QEMU_CAPS_NODEFCONFIG);
    DO_TEST("usb-ports",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_USB_HUB,
            QEMU_CAPS_NODEFCONFIG);
    DO_TEST("usb-redir",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1, QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_SPICE, QEMU_CAPS_CHARDEV_SPICEVMC);
    DO_TEST("usb-redir-filter",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_USB_HUB,
            QEMU_CAPS_ICH9_USB_EHCI1, QEMU_CAPS_USB_REDIR,
            QEMU_CAPS_SPICE, QEMU_CAPS_CHARDEV_SPICEVMC,
            QEMU_CAPS_USB_REDIR_FILTER);
    DO_TEST("usb1-usb2",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_PIIX3_USB_UHCI,
            QEMU_CAPS_USB_HUB, QEMU_CAPS_ICH9_USB_EHCI1);
    DO_TEST("usb-none",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST_PARSE_ERROR("usb-none-other",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST_PARSE_ERROR("usb-none-hub",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_USB_HUB);
    DO_TEST_PARSE_ERROR("usb-none-usbtablet",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);


    DO_TEST("smbios", QEMU_CAPS_SMBIOS_TYPE);

    DO_TEST("watchdog", NONE);
    DO_TEST("watchdog-device", QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("watchdog-dump", NONE);
    DO_TEST("balloon-device", QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("balloon-device-auto",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("sound", NONE);
    DO_TEST("sound-device",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_HDA_DUPLEX, QEMU_CAPS_HDA_MICRO);
    DO_TEST("fs9p",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG, QEMU_CAPS_FSDEV,
            QEMU_CAPS_FSDEV_WRITEOUT);

    DO_TEST("hostdev-usb-address", NONE);
    DO_TEST("hostdev-usb-address-device",
            QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("hostdev-pci-address", QEMU_CAPS_PCIDEVICE);
    DO_TEST("hostdev-pci-address-device",
            QEMU_CAPS_PCIDEVICE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("pci-rom",
            QEMU_CAPS_PCIDEVICE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_ROMBAR);

    DO_TEST_FULL("restore-v1", "stdio", 7, 0, QEMU_CAPS_MIGRATE_KVM_STDIO);
    DO_TEST_FULL("restore-v2", "stdio", 7, 0, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    DO_TEST_FULL("restore-v2", "exec:cat", 7, 0, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    DO_TEST_FULL("restore-v2-fd", "stdio", 7, 0, QEMU_CAPS_MIGRATE_QEMU_FD);
    DO_TEST_FULL("restore-v2-fd", "fd:7", 7, 0, QEMU_CAPS_MIGRATE_QEMU_FD);
    DO_TEST_FULL("migrate", "tcp:10.0.0.1:5000", -1, 0,
            QEMU_CAPS_MIGRATE_QEMU_TCP);

    DO_TEST("qemu-ns", NONE);

    DO_TEST("smp", QEMU_CAPS_SMP_TOPOLOGY);

    DO_TEST("cpu-topology1", QEMU_CAPS_SMP_TOPOLOGY);
    DO_TEST("cpu-topology2", QEMU_CAPS_SMP_TOPOLOGY);
    DO_TEST("cpu-topology3", NONE);
    DO_TEST("cpu-minimum1", NONE);
    DO_TEST("cpu-minimum2", NONE);
    DO_TEST("cpu-exact1", NONE);
    DO_TEST("cpu-exact2", NONE);
    DO_TEST("cpu-exact2-nofallback", NONE);
    DO_TEST("cpu-fallback", NONE);
    DO_TEST_FAILURE("cpu-nofallback", NONE);
    DO_TEST("cpu-strict1", NONE);
    DO_TEST("cpu-numa1", NONE);
    DO_TEST("cpu-numa2", QEMU_CAPS_SMP_TOPOLOGY);
    DO_TEST("cpu-host-model", NONE);
    skipLegacyCPUs = true;
    DO_TEST("cpu-host-model-fallback", NONE);
    DO_TEST_FAILURE("cpu-host-model-nofallback", NONE);
    skipLegacyCPUs = false;
    DO_TEST("cpu-host-passthrough", QEMU_CAPS_KVM, QEMU_CAPS_CPU_HOST);
    DO_TEST_FAILURE("cpu-host-passthrough", NONE);
    DO_TEST_FAILURE("cpu-qemu-host-passthrough",
                    QEMU_CAPS_KVM, QEMU_CAPS_CPU_HOST);

    DO_TEST("memtune", QEMU_CAPS_NAME);
    DO_TEST("blkiotune", QEMU_CAPS_NAME);
    DO_TEST("blkiotune-device", QEMU_CAPS_NAME);
    DO_TEST("cputune", QEMU_CAPS_NAME);
    DO_TEST("numatune-memory", NONE);
    DO_TEST("numad", NONE);
    DO_TEST("numad-auto-vcpu-static-numatune", NONE);
    DO_TEST("numad-auto-memory-vcpu-cpuset", NONE);
    DO_TEST("numad-auto-memory-vcpu-no-cpuset-and-placement", NONE);
    DO_TEST("numad-static-memory-auto-vcpu", NONE);
    DO_TEST("blkdeviotune", QEMU_CAPS_NAME, QEMU_CAPS_DEVICE,
            QEMU_CAPS_DRIVE, QEMU_CAPS_DRIVE_IOTUNE);

    DO_TEST("multifunction-pci-device",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_PCI_MULTIFUNCTION, QEMU_CAPS_SCSI_LSI);

    DO_TEST("monitor-json", QEMU_CAPS_DEVICE,
            QEMU_CAPS_CHARDEV, QEMU_CAPS_MONITOR_JSON, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("no-shutdown", QEMU_CAPS_DEVICE,
            QEMU_CAPS_CHARDEV, QEMU_CAPS_MONITOR_JSON, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_NO_SHUTDOWN);

    DO_TEST("seclabel-dynamic", QEMU_CAPS_NAME);
    DO_TEST("seclabel-dynamic-baselabel", QEMU_CAPS_NAME);
    DO_TEST("seclabel-dynamic-override", QEMU_CAPS_NAME);
    DO_TEST("seclabel-static", QEMU_CAPS_NAME);
    DO_TEST("seclabel-static-relabel", QEMU_CAPS_NAME);
    DO_TEST("seclabel-none", QEMU_CAPS_NAME);

    DO_TEST("pseries-basic",
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("pseries-vio", QEMU_CAPS_DRIVE,
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("pseries-vio-user-assigned", QEMU_CAPS_DRIVE,
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST_ERROR("pseries-vio-address-clash", QEMU_CAPS_DRIVE,
            QEMU_CAPS_CHARDEV, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG);
    DO_TEST("disk-ide-drive-split",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_IDE_CD);
    DO_TEST("disk-ide-wwn",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_IDE_CD,
            QEMU_CAPS_DRIVE_SERIAL, QEMU_CAPS_IDE_DRIVE_WWN);

    DO_TEST("disk-geometry", QEMU_CAPS_DRIVE);
    DO_TEST("disk-blockio",
            QEMU_CAPS_DRIVE, QEMU_CAPS_DEVICE, QEMU_CAPS_NODEFCONFIG,
            QEMU_CAPS_IDE_CD, QEMU_CAPS_BLOCKIO);

    VIR_FREE(driver.stateDir);
    virCapabilitiesFree(driver.caps);
    VIR_FREE(map);

    return ret==0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)

#else
# include "testutils.h"

int main(void)
{
    return EXIT_AM_SKIP;
}

#endif /* WITH_QEMU */
