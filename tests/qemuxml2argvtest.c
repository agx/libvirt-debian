#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <fcntl.h>

#ifdef WITH_QEMU

#include "internal.h"
#include "testutils.h"
#include "qemu_conf.h"

#include "testutilsqemu.h"

static char *progname;
static char *abs_srcdir;
static struct qemud_driver driver;

#define MAX_FILE 4096

static int testCompareXMLToArgvFiles(const char *xml,
                                     const char *cmd,
                                     int extraFlags) {
    char argvData[MAX_FILE];
    char *expectargv = &(argvData[0]);
    char *actualargv = NULL;
    const char **argv = NULL;
    const char **qenv = NULL;
    const char **tmp = NULL;
    int ret = -1, len, flags;
    virDomainDefPtr vmdef = NULL;
    virDomainObj vm;

    if (virtTestLoadFile(cmd, &expectargv, MAX_FILE) < 0)
        goto fail;

    if (!(vmdef = virDomainDefParseFile(NULL, driver.caps, xml)))
        goto fail;

    memset(&vm, 0, sizeof vm);
    vm.def = vmdef;
    if (extraFlags & QEMUD_CMD_FLAG_DOMID)
        vm.def->id = 6;
    else
        vm.def->id = -1;
    vm.pid = -1;

    flags = QEMUD_CMD_FLAG_VNC_COLON |
        QEMUD_CMD_FLAG_NO_REBOOT |
        extraFlags;

    if (qemudBuildCommandLine(NULL, &driver,
                              &vm, flags, &argv, &qenv,
                              NULL, NULL, NULL) < 0)
        goto fail;

    len = 1; /* for trailing newline */
    tmp = qenv;
    while (*tmp) {
        len += strlen(*tmp) + 1;
        tmp++;
    }

    tmp = argv;
    while (*tmp) {
        len += strlen(*tmp) + 1;
        tmp++;
    }
    actualargv = malloc(sizeof(*actualargv)*len);
    actualargv[0] = '\0';
    tmp = qenv;
    while (*tmp) {
        if (actualargv[0])
            strcat(actualargv, " ");
        strcat(actualargv, *tmp);
        tmp++;
    }
    tmp = argv;
    while (*tmp) {
        if (actualargv[0])
            strcat(actualargv, " ");
        strcat(actualargv, *tmp);
        tmp++;
    }
    strcat(actualargv, "\n");

    if (STRNEQ(expectargv, actualargv)) {
        virtTestDifference(stderr, expectargv, actualargv);
        goto fail;
    }

    ret = 0;

 fail:
    free(actualargv);
    if (argv) {
        tmp = argv;
        while (*tmp) {
            free(*(char**)tmp);
            tmp++;
        }
        free(argv);
    }
    if (qenv) {
        tmp = qenv;
        while (*tmp) {
            free(*(char**)tmp);
            tmp++;
        }
        free(qenv);
    }
    virDomainDefFree(vmdef);
    return ret;
}


struct testInfo {
    const char *name;
    int extraFlags;
};

static int testCompareXMLToArgvHelper(const void *data) {
    const struct testInfo *info = data;
    char xml[PATH_MAX];
    char args[PATH_MAX];
    snprintf(xml, PATH_MAX, "%s/qemuxml2argvdata/qemuxml2argv-%s.xml",
             abs_srcdir, info->name);
    snprintf(args, PATH_MAX, "%s/qemuxml2argvdata/qemuxml2argv-%s.args",
             abs_srcdir, info->name);
    return testCompareXMLToArgvFiles(xml, args, info->extraFlags);
}



static int
mymain(int argc, char **argv)
{
    int ret = 0;
    char cwd[PATH_MAX];

    progname = argv[0];

    if (argc > 1) {
        fprintf(stderr, "Usage: %s\n", progname);
        return (EXIT_FAILURE);
    }

    abs_srcdir = getenv("abs_srcdir");
    if (!abs_srcdir)
        abs_srcdir = getcwd(cwd, sizeof(cwd));

    if ((driver.caps = testQemuCapsInit()) == NULL)
        return EXIT_FAILURE;

#define DO_TEST(name, extraFlags)                                       \
    do {                                                                \
        struct testInfo info = { name, extraFlags };                    \
        if (virtTestRun("QEMU XML-2-ARGV " name,                        \
                        1, testCompareXMLToArgvHelper, &info) < 0)      \
            ret = -1;                                                   \
    } while (0)

    setenv("PATH", "/bin", 1);
    setenv("USER", "test", 1);
    setenv("LOGNAME", "test", 1);
    setenv("HOME", "/home/test", 1);
    unsetenv("TMPDIR");
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");

    DO_TEST("minimal", QEMUD_CMD_FLAG_NAME);
    DO_TEST("boot-cdrom", 0);
    DO_TEST("boot-network", 0);
    DO_TEST("boot-floppy", 0);
    DO_TEST("bootloader", 0);
    DO_TEST("clock-utc", 0);
    DO_TEST("clock-localtime", 0);
    DO_TEST("disk-cdrom", 0);
    DO_TEST("disk-cdrom-empty", QEMUD_CMD_FLAG_DRIVE);
    DO_TEST("disk-floppy", 0);
    DO_TEST("disk-many", 0);
    DO_TEST("disk-virtio", QEMUD_CMD_FLAG_DRIVE |
            QEMUD_CMD_FLAG_DRIVE_BOOT);
    DO_TEST("disk-xenvbd", QEMUD_CMD_FLAG_DRIVE |
            QEMUD_CMD_FLAG_DRIVE_BOOT);
    DO_TEST("disk-drive-boot-disk", QEMUD_CMD_FLAG_DRIVE |
            QEMUD_CMD_FLAG_DRIVE_BOOT);
    DO_TEST("disk-drive-boot-cdrom", QEMUD_CMD_FLAG_DRIVE |
            QEMUD_CMD_FLAG_DRIVE_BOOT);
    DO_TEST("disk-usb", 0);
    DO_TEST("graphics-vnc", 0);
    DO_TEST("graphics-sdl", 0);
    DO_TEST("input-usbmouse", 0);
    DO_TEST("input-usbtablet", 0);
    DO_TEST("input-xen", 0);
    DO_TEST("misc-acpi", 0);
    DO_TEST("misc-no-reboot", 0);
    DO_TEST("misc-uuid", QEMUD_CMD_FLAG_NAME |
        QEMUD_CMD_FLAG_UUID | QEMUD_CMD_FLAG_DOMID);
    DO_TEST("net-user", 0);
    DO_TEST("net-virtio", 0);

    DO_TEST("serial-vc", 0);
    DO_TEST("serial-pty", 0);
    DO_TEST("serial-dev", 0);
    DO_TEST("serial-file", 0);
    DO_TEST("serial-unix", 0);
    DO_TEST("serial-tcp", 0);
    DO_TEST("serial-udp", 0);
    DO_TEST("serial-tcp-telnet", 0);
    DO_TEST("serial-many", 0);
    DO_TEST("parallel-tcp", 0);
    DO_TEST("console-compat", 0);
    DO_TEST("sound", 0);

    DO_TEST("hostdev-usb-product", 0);
    DO_TEST("hostdev-usb-address", 0);

    virCapabilitiesFree(driver.caps);

    return(ret==0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

VIRT_TEST_MAIN(mymain)

#else

int main (void) { return (77); /* means 'test skipped' for automake */ }

#endif /* WITH_QEMU */
