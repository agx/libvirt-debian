#include <config.h>

#ifdef WITH_ESX

# include <stdio.h>
# include <string.h>
# include <unistd.h>

# include "internal.h"
# include "memory.h"
# include "testutils.h"
# include "esx/esx_vmx.h"

static char *progname = NULL;
static char *abs_srcdir = NULL;
static virCapsPtr caps = NULL;

# define MAX_FILE 4096

static void
testCapsInit(void)
{
    virCapsGuestPtr guest = NULL;

    caps = virCapabilitiesNew("i686", 1, 1);

    if (caps == NULL) {
        return;
    }

    virCapabilitiesSetMacPrefix(caps, (unsigned char[]){ 0x00, 0x0c, 0x29 });
    virCapabilitiesAddHostMigrateTransport(caps, "esx");

    caps->hasWideScsiBus = true;

    /* i686 guest */
    guest =
      virCapabilitiesAddGuest(caps, "hvm", "i686", 32, NULL, NULL, 0, NULL);

    if (guest == NULL) {
        goto failure;
    }

    if (virCapabilitiesAddGuestDomain(guest, "vmware", NULL, NULL, 0,
                                      NULL) == NULL) {
        goto failure;
    }

    /* x86_64 guest */
    guest =
      virCapabilitiesAddGuest(caps, "hvm", "x86_64", 64, NULL, NULL, 0, NULL);

    if (guest == NULL) {
        goto failure;
    }

    if (virCapabilitiesAddGuestDomain(guest, "vmware", NULL, NULL, 0,
                                      NULL) == NULL) {
        goto failure;
    }

    return;

  failure:
    virCapabilitiesFree(caps);
    caps = NULL;
}

static int
testCompareFiles(const char *vmx, const char *xml,
                 esxVI_ProductVersion productVersion)
{
    int result = -1;
    char vmxData[MAX_FILE];
    char xmlData[MAX_FILE];
    char *formatted = NULL;
    char *vmxPtr = &(vmxData[0]);
    char *xmlPtr = &(xmlData[0]);
    virDomainDefPtr def = NULL;
    virErrorPtr err = NULL;

    if (virtTestLoadFile(vmx, &vmxPtr, MAX_FILE) < 0) {
        goto failure;
    }

    if (virtTestLoadFile(xml, &xmlPtr, MAX_FILE) < 0) {
        goto failure;
    }

    def = esxVMX_ParseConfig(NULL, caps, vmxData, "datastore", "directory",
                             productVersion);

    if (def == NULL) {
        err = virGetLastError();
        fprintf(stderr, "ERROR: %s\n", err != NULL ? err->message : "<unknown>");
        goto failure;
    }

    formatted = virDomainDefFormat(def, VIR_DOMAIN_XML_SECURE);

    if (formatted == NULL) {
        err = virGetLastError();
        fprintf(stderr, "ERROR: %s\n", err != NULL ? err->message : "<unknown>");
        goto failure;
    }

    if (STRNEQ(xmlData, formatted)) {
        virtTestDifference(stderr, xmlData, formatted);
        goto failure;
    }

    result = 0;

  failure:
    VIR_FREE(formatted);
    virDomainDefFree(def);

    return result;
}

struct testInfo {
    const char *input;
    const char *output;
    esxVI_ProductVersion version;
};

static int
testCompareHelper(const void *data)
{
    const struct testInfo *info = data;
    char vmx[PATH_MAX];
    char xml[PATH_MAX];

    snprintf(vmx, PATH_MAX, "%s/vmx2xmldata/vmx2xml-%s.vmx", abs_srcdir,
             info->input);
    snprintf(xml, PATH_MAX, "%s/vmx2xmldata/vmx2xml-%s.xml", abs_srcdir,
             info->output);

    return testCompareFiles(vmx, xml, info->version);
}

static int
mymain(int argc, char **argv)
{
    int result = 0;
    char cwd[PATH_MAX];

    progname = argv[0];

    if (argc > 1) {
        fprintf(stderr, "Usage: %s\n", progname);
        return EXIT_FAILURE;
    }

    abs_srcdir = getenv("abs_srcdir");

    if (abs_srcdir == NULL) {
        abs_srcdir = getcwd(cwd, sizeof(cwd));
    }

    if (argc > 1) {
        fprintf(stderr, "Usage: %s\n", progname);
        return EXIT_FAILURE;
    }

# define DO_TEST(_in, _out, _version)                                         \
        do {                                                                  \
            struct testInfo info = { _in, _out, _version };                   \
            virResetLastError();                                              \
            if (virtTestRun("VMware VMX-2-XML "_in" -> "_out, 1,              \
                            testCompareHelper, &info) < 0) {                  \
                result = -1;                                                  \
            }                                                                 \
        } while (0)

    testCapsInit();

    if (caps == NULL) {
        return EXIT_FAILURE;
    }

    DO_TEST("case-insensitive-1", "case-insensitive-1", esxVI_ProductVersion_ESX35);
    DO_TEST("case-insensitive-2", "case-insensitive-2", esxVI_ProductVersion_ESX35);

    DO_TEST("minimal", "minimal", esxVI_ProductVersion_ESX35);
    DO_TEST("minimal-64bit", "minimal-64bit", esxVI_ProductVersion_ESX35);

    DO_TEST("graphics-vnc", "graphics-vnc", esxVI_ProductVersion_ESX35);

    DO_TEST("scsi-driver", "scsi-driver", esxVI_ProductVersion_ESX35);
    DO_TEST("scsi-writethrough", "scsi-writethrough", esxVI_ProductVersion_ESX35);

    DO_TEST("harddisk-scsi-file", "harddisk-scsi-file", esxVI_ProductVersion_ESX35);
    DO_TEST("harddisk-ide-file", "harddisk-ide-file", esxVI_ProductVersion_ESX35);

    DO_TEST("cdrom-scsi-file", "cdrom-scsi-file", esxVI_ProductVersion_ESX35);
    DO_TEST("cdrom-scsi-device", "cdrom-scsi-device", esxVI_ProductVersion_ESX35);
    DO_TEST("cdrom-ide-file", "cdrom-ide-file", esxVI_ProductVersion_ESX35);
    DO_TEST("cdrom-ide-device", "cdrom-ide-device", esxVI_ProductVersion_ESX35);

    DO_TEST("floppy-file", "floppy-file", esxVI_ProductVersion_ESX35);
    DO_TEST("floppy-device", "floppy-device", esxVI_ProductVersion_ESX35);

    DO_TEST("ethernet-e1000", "ethernet-e1000", esxVI_ProductVersion_ESX35);
    DO_TEST("ethernet-vmxnet2", "ethernet-vmxnet2", esxVI_ProductVersion_ESX35);

    DO_TEST("ethernet-custom", "ethernet-custom", esxVI_ProductVersion_ESX35);
    DO_TEST("ethernet-bridged", "ethernet-bridged", esxVI_ProductVersion_ESX35);

    DO_TEST("ethernet-generated", "ethernet-generated", esxVI_ProductVersion_ESX35);
    DO_TEST("ethernet-static", "ethernet-static", esxVI_ProductVersion_ESX35);
    DO_TEST("ethernet-vpx", "ethernet-vpx", esxVI_ProductVersion_ESX35);
    DO_TEST("ethernet-other", "ethernet-other", esxVI_ProductVersion_ESX35);

    DO_TEST("serial-file", "serial-file", esxVI_ProductVersion_ESX35);
    DO_TEST("serial-device", "serial-device", esxVI_ProductVersion_ESX35);
    DO_TEST("serial-pipe-client-app", "serial-pipe", esxVI_ProductVersion_ESX35);
    DO_TEST("serial-pipe-server-vm", "serial-pipe", esxVI_ProductVersion_ESX35);
    DO_TEST("serial-pipe-client-app", "serial-pipe", esxVI_ProductVersion_ESX35);
    DO_TEST("serial-pipe-server-vm", "serial-pipe", esxVI_ProductVersion_ESX35);

    DO_TEST("parallel-file", "parallel-file", esxVI_ProductVersion_ESX35);
    DO_TEST("parallel-device", "parallel-device", esxVI_ProductVersion_ESX35);

    DO_TEST("esx-in-the-wild-1", "esx-in-the-wild-1", esxVI_ProductVersion_ESX35);
    DO_TEST("esx-in-the-wild-2", "esx-in-the-wild-2", esxVI_ProductVersion_ESX35);
    DO_TEST("esx-in-the-wild-3", "esx-in-the-wild-3", esxVI_ProductVersion_ESX35);
    DO_TEST("esx-in-the-wild-4", "esx-in-the-wild-4", esxVI_ProductVersion_ESX35);

    DO_TEST("gsx-in-the-wild-1", "gsx-in-the-wild-1", esxVI_ProductVersion_ESX35);
    DO_TEST("gsx-in-the-wild-2", "gsx-in-the-wild-2", esxVI_ProductVersion_ESX35);
    DO_TEST("gsx-in-the-wild-3", "gsx-in-the-wild-3", esxVI_ProductVersion_ESX35);
    DO_TEST("gsx-in-the-wild-4", "gsx-in-the-wild-4", esxVI_ProductVersion_ESX35);

    virCapabilitiesFree(caps);

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)

#else

int main (void)
{
    return 77; /* means 'test skipped' for automake */
}

#endif /* WITH_ESX */
