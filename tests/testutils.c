/*
 * testutils.c: basic test utils
 *
 * Copyright (C) 2005-2015 Red Hat, Inc.
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

#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "testutils.h"
#include "internal.h"
#include "viralloc.h"
#include "virutil.h"
#include "virthread.h"
#include "virerror.h"
#include "virbuffer.h"
#include "virlog.h"
#include "vircommand.h"
#include "virrandom.h"
#include "virprocess.h"
#include "virstring.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("tests.testutils");

#include "virbitmap.h"
#include "virfile.h"

static unsigned int testDebug = -1;
static unsigned int testVerbose = -1;
static unsigned int testExpensive = -1;
static unsigned int testRegenerate = -1;


static size_t testCounter;
static virBitmapPtr testBitmap;

virArch virTestHostArch = VIR_ARCH_X86_64;

virArch
virArchFromHost(void)
{
    return virTestHostArch;
}


static int virTestUseTerminalColors(void)
{
    return isatty(STDOUT_FILENO);
}

static unsigned int
virTestGetFlag(const char *name)
{
    char *flagStr;
    unsigned int flag;

    if ((flagStr = getenv(name)) == NULL)
        return 0;

    if (virStrToLong_ui(flagStr, NULL, 10, &flag) < 0)
        return 0;

    return flag;
}


/**
 * virTestPropagateLibvirtError:
 *
 * In cases when a libvirt utility function which reports libvirt errors is
 * used in the test suite outside of the virTestRun call and the failure of such
 * a function would cause an test failure the error message reported by that
 * function will not be propagated to the user as the error callback is not
 * invoked.
 *
 * In cases when the error message may be beneficial in debugging this helper
 * provides means to dispatch the errors including invocation of the error
 * callback.
 */
void
virTestPropagateLibvirtError(void)
{
    if (virGetLastErrorCode() == VIR_ERR_OK)
        return;

    if (virTestGetVerbose() || virTestGetDebug())
        virDispatchError(NULL);
}


/*
 * Runs test
 *
 * returns: -1 = error, 0 = success
 */
int
virTestRun(const char *title,
           int (*body)(const void *data), const void *data)
{
    int ret = 0;

    /* Some test are fragile about environ settings.  If that's
     * the case, don't poison it. */
    if (getenv("VIR_TEST_MOCK_PROGNAME"))
        g_setenv("VIR_TEST_MOCK_TESTNAME", title, TRUE);

    if (testCounter == 0 && !virTestGetVerbose())
        fprintf(stderr, "      ");

    testCounter++;


    /* Skip tests if out of range */
    if (testBitmap && !virBitmapIsBitSet(testBitmap, testCounter))
        return 0;

    if (virTestGetVerbose())
        fprintf(stderr, "%2zu) %-65s ... ", testCounter, title);

    virResetLastError();
    ret = body(data);
    virTestPropagateLibvirtError();

    if (virTestGetVerbose()) {
        if (ret == 0)
            if (virTestUseTerminalColors())
                fprintf(stderr, "\e[32mOK\e[0m\n");  /* green */
            else
                fprintf(stderr, "OK\n");
        else if (ret == EXIT_AM_SKIP)
            if (virTestUseTerminalColors())
                fprintf(stderr, "\e[34m\e[1mSKIP\e[0m\n");  /* bold blue */
            else
                fprintf(stderr, "SKIP\n");
        else
            if (virTestUseTerminalColors())
                fprintf(stderr, "\e[31m\e[1mFAILED\e[0m\n");  /* bold red */
            else
                fprintf(stderr, "FAILED\n");
    } else {
        if (testCounter != 1 &&
            !((testCounter-1) % 40)) {
            fprintf(stderr, " %-3zu\n", (testCounter-1));
            fprintf(stderr, "      ");
        }
        if (ret == 0)
                fprintf(stderr, ".");
        else if (ret == EXIT_AM_SKIP)
            fprintf(stderr, "_");
        else
            fprintf(stderr, "!");
    }

    g_unsetenv("VIR_TEST_MOCK_TESTNAME");
    return ret;
}


/**
 * virTestLoadFile:
 * @file: name of the file to load
 * @buf: buffer to load the file into
 *
 * Allocates @buf to the size of FILE. Reads FILE into buffer BUF.
 * Upon any failure, error is printed to stderr and -1 is returned. 'errno' is
 * not preserved. On success 0 is returned. Caller is responsible for freeing
 * @buf.
 */
int
virTestLoadFile(const char *file, char **buf)
{
    FILE *fp = fopen(file, "r");
    struct stat st;
    char *tmp;
    int len, tmplen, buflen;

    if (!fp) {
        fprintf(stderr, "%s: failed to open: %s\n", file, g_strerror(errno));
        return -1;
    }

    if (fstat(fileno(fp), &st) < 0) {
        fprintf(stderr, "%s: failed to fstat: %s\n", file, g_strerror(errno));
        VIR_FORCE_FCLOSE(fp);
        return -1;
    }

    tmplen = buflen = st.st_size + 1;

    if (VIR_ALLOC_N(*buf, buflen) < 0) {
        VIR_FORCE_FCLOSE(fp);
        return -1;
    }

    tmp = *buf;
    (*buf)[0] = '\0';
    if (st.st_size) {
        /* read the file line by line */
        while (fgets(tmp, tmplen, fp) != NULL) {
            len = strlen(tmp);
            /* stop on an empty line */
            if (len == 0)
                break;
            /* remove trailing backslash-newline pair */
            if (len >= 2 && tmp[len-2] == '\\' && tmp[len-1] == '\n') {
                len -= 2;
                tmp[len] = '\0';
            }
            /* advance the temporary buffer pointer */
            tmp += len;
            tmplen -= len;
        }
        if (ferror(fp)) {
            fprintf(stderr, "%s: read failed: %s\n", file, g_strerror(errno));
            VIR_FORCE_FCLOSE(fp);
            VIR_FREE(*buf);
            return -1;
        }
    }

    VIR_FORCE_FCLOSE(fp);
    return 0;
}


static char *
virTestLoadFileGetPath(const char *p,
                       va_list ap)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char *path = NULL;

    virBufferAddLit(&buf, abs_srcdir "/");

    if (p) {
        virBufferAdd(&buf, p, -1);
        virBufferStrcatVArgs(&buf, ap);
    }

    if (!(path = virBufferContentAndReset(&buf)))
        VIR_TEST_VERBOSE("failed to format file path");

    return path;
}


/**
 * virTestLoadFilePath:
 * @...: file name components terminated with a NULL
 *
 * Constructs the test file path from variable arguments and loads the file.
 * 'abs_srcdir' is automatically prepended.
 */
char *
virTestLoadFilePath(const char *p, ...)
{
    char *path = NULL;
    char *ret = NULL;
    va_list ap;

    va_start(ap, p);

    if (!(path = virTestLoadFileGetPath(p, ap)))
        goto cleanup;

    ignore_value(virTestLoadFile(path, &ret));

 cleanup:
    va_end(ap);
    VIR_FREE(path);

    return ret;
}


/**
 * virTestLoadFileJSON:
 * @...: name components terminated with a NULL
 *
 * Constructs the test file path from variable arguments and loads and parses
 * the JSON file. 'abs_srcdir' is automatically prepended to the path.
 */
virJSONValuePtr
virTestLoadFileJSON(const char *p, ...)
{
    virJSONValuePtr ret = NULL;
    char *jsonstr = NULL;
    char *path = NULL;
    va_list ap;

    va_start(ap, p);

    if (!(path = virTestLoadFileGetPath(p, ap)))
        goto cleanup;

    if (virTestLoadFile(path, &jsonstr) < 0)
        goto cleanup;

    if (!(ret = virJSONValueFromString(jsonstr)))
        VIR_TEST_VERBOSE("failed to parse json from file '%s'", path);

 cleanup:
    va_end(ap);
    VIR_FREE(jsonstr);
    VIR_FREE(path);
    return ret;
}


#ifndef WIN32
static
void virTestCaptureProgramExecChild(const char *const argv[],
                                    int pipefd)
{
    size_t i;
    int open_max;
    int stdinfd = -1;
    const char *const env[] = {
        "LANG=C",
        NULL
    };

    if ((stdinfd = open("/dev/null", O_RDONLY)) < 0)
        goto cleanup;

    open_max = sysconf(_SC_OPEN_MAX);
    if (open_max < 0)
        goto cleanup;

    for (i = 0; i < open_max; i++) {
        if (i != stdinfd &&
            i != pipefd) {
            int tmpfd;
            tmpfd = i;
            VIR_FORCE_CLOSE(tmpfd);
        }
    }

    if (dup2(stdinfd, STDIN_FILENO) != STDIN_FILENO)
        goto cleanup;
    if (dup2(pipefd, STDOUT_FILENO) != STDOUT_FILENO)
        goto cleanup;
    if (dup2(pipefd, STDERR_FILENO) != STDERR_FILENO)
        goto cleanup;

    /* SUS is crazy here, hence the cast */
    execve(argv[0], (char *const*)argv, (char *const*)env);

 cleanup:
    VIR_FORCE_CLOSE(stdinfd);
}

int
virTestCaptureProgramOutput(const char *const argv[], char **buf, int maxlen)
{
    int pipefd[2];
    int len;

    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    switch (pid) {
    case 0:
        VIR_FORCE_CLOSE(pipefd[0]);
        virTestCaptureProgramExecChild(argv, pipefd[1]);

        VIR_FORCE_CLOSE(pipefd[1]);
        _exit(EXIT_FAILURE);

    case -1:
        return -1;

    default:
        VIR_FORCE_CLOSE(pipefd[1]);
        len = virFileReadLimFD(pipefd[0], maxlen, buf);
        VIR_FORCE_CLOSE(pipefd[0]);
        if (virProcessWait(pid, NULL, false) < 0)
            return -1;

        return len;
    }
}
#else /* !WIN32 */
int
virTestCaptureProgramOutput(const char *const argv[] G_GNUC_UNUSED,
                            char **buf G_GNUC_UNUSED,
                            int maxlen G_GNUC_UNUSED)
{
    return -1;
}
#endif /* !WIN32 */

static int
virTestRewrapFile(const char *filename)
{
    int ret = -1;
    char *script = NULL;
    virCommandPtr cmd = NULL;

    if (!(virStringHasSuffix(filename, ".args") ||
          virStringHasSuffix(filename, ".ldargs")))
        return 0;

    script = g_strdup_printf("%s/scripts/test-wrap-argv.py", abs_top_srcdir);

    cmd = virCommandNewArgList(PYTHON, script, "--in-place", filename, NULL);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(script);
    virCommandFree(cmd);
    return ret;
}

/**
 * @param stream: output stream to write differences to
 * @param expect: expected output text
 * @param expectName: name designator of the expected text
 * @param actual: actual output text
 * @param actualName: name designator of the actual text
 * @param regenerate: enable or disable regenerate functionality
 *
 * Display expected and actual output text, trimmed to first and last
 * characters at which differences occur. Displays names of the text strings if
 * non-NULL.
 */
static int
virTestDifferenceFullInternal(FILE *stream,
                              const char *expect,
                              const char *expectName,
                              const char *actual,
                              const char *actualName,
                              bool regenerate)
{
    const char *expectStart;
    const char *expectEnd;
    const char *actualStart;
    const char *actualEnd;

    if (!expect)
        expect = "";
    if (!actual)
        actual = "";

    expectStart = expect;
    expectEnd = expect + (strlen(expect)-1);
    actualStart = actual;
    actualEnd = actual + (strlen(actual)-1);

    if (expectName && regenerate && (virTestGetRegenerate() > 0)) {
        if (virFileWriteStr(expectName, actual, 0666) < 0) {
            virDispatchError(NULL);
            return -1;
        }

        if (virTestRewrapFile(expectName) < 0) {
            virDispatchError(NULL);
            return -1;
        }
    }

    if (!virTestGetDebug())
        return 0;

    if (virTestGetDebug() < 2) {
        /* Skip to first character where they differ */
        while (*expectStart && *actualStart &&
               *actualStart == *expectStart) {
            actualStart++;
            expectStart++;
        }

        /* Work backwards to last character where they differ */
        while (actualEnd > actualStart &&
               expectEnd > expectStart &&
               *actualEnd == *expectEnd) {
            actualEnd--;
            expectEnd--;
        }
    }

    /* Show the trimmed differences */
    if (expectName)
        fprintf(stream, "\nIn '%s':", expectName);
    fprintf(stream, "\nOffset %d\nExpect [", (int) (expectStart - expect));
    if ((expectEnd - expectStart + 1) &&
        fwrite(expectStart, (expectEnd-expectStart+1), 1, stream) != 1)
        return -1;
    fprintf(stream, "]\n");
    if (actualName)
        fprintf(stream, "In '%s':\n", actualName);
    fprintf(stream, "Actual [");
    if ((actualEnd - actualStart + 1) &&
        fwrite(actualStart, (actualEnd-actualStart+1), 1, stream) != 1)
        return -1;
    fprintf(stream, "]\n");

    /* Pad to line up with test name ... in virTestRun */
    fprintf(stream, "                                                                      ... ");

    return 0;
}

/**
 * @param stream: output stream to write differences to
 * @param expect: expected output text
 * @param expectName: name designator of the expected text
 * @param actual: actual output text
 * @param actualName: name designator of the actual text
 *
 * Display expected and actual output text, trimmed to first and last
 * characters at which differences occur. Displays names of the text strings if
 * non-NULL. If VIR_TEST_REGENERATE_OUTPUT is used, this function will
 * regenerate the expected file.
 */
int
virTestDifferenceFull(FILE *stream,
                      const char *expect,
                      const char *expectName,
                      const char *actual,
                      const char *actualName)
{
    return virTestDifferenceFullInternal(stream, expect, expectName,
                                         actual, actualName, true);
}

/**
 * @param stream: output stream to write differences to
 * @param expect: expected output text
 * @param expectName: name designator of the expected text
 * @param actual: actual output text
 * @param actualName: name designator of the actual text
 *
 * Display expected and actual output text, trimmed to first and last
 * characters at which differences occur. Displays names of the text strings if
 * non-NULL. If VIR_TEST_REGENERATE_OUTPUT is used, this function will not
 * regenerate the expected file.
 */
int
virTestDifferenceFullNoRegenerate(FILE *stream,
                                  const char *expect,
                                  const char *expectName,
                                  const char *actual,
                                  const char *actualName)
{
    return virTestDifferenceFullInternal(stream, expect, expectName,
                                         actual, actualName, false);
}

/**
 * @param stream: output stream to write differences to
 * @param expect: expected output text
 * @param actual: actual output text
 *
 * Display expected and actual output text, trimmed to
 * first and last characters at which differences occur
 */
int
virTestDifference(FILE *stream,
                  const char *expect,
                  const char *actual)
{
    return virTestDifferenceFullNoRegenerate(stream,
                                             expect, NULL,
                                             actual, NULL);
}


/**
 * @param stream: output stream to write differences to
 * @param expect: expected output text
 * @param actual: actual output text
 *
 * Display expected and actual output text, trimmed to
 * first and last characters at which differences occur
 */
int virTestDifferenceBin(FILE *stream,
                         const char *expect,
                         const char *actual,
                         size_t length)
{
    size_t start = 0, end = length;
    ssize_t i;

    if (!virTestGetDebug())
        return 0;

    if (virTestGetDebug() < 2) {
        /* Skip to first character where they differ */
        for (i = 0; i < length; i++) {
            if (expect[i] != actual[i]) {
                start = i;
                break;
            }
        }

        /* Work backwards to last character where they differ */
        for (i = (length -1); i >= 0; i--) {
            if (expect[i] != actual[i]) {
                end = i;
                break;
            }
        }
    }
    /* Round to nearest boundary of 4, except that last word can be short */
    start -= (start % 4);
    end += 4 - (end % 4);
    if (end >= length)
        end = length - 1;

    /* Show the trimmed differences */
    fprintf(stream, "\nExpect [ Region %d-%d", (int)start, (int)end);
    for (i = start; i < end; i++) {
        if ((i % 4) == 0)
            fprintf(stream, "\n    ");
        fprintf(stream, "0x%02x, ", ((int)expect[i])&0xff);
    }
    fprintf(stream, "]\n");
    fprintf(stream, "Actual [ Region %d-%d", (int)start, (int)end);
    for (i = start; i < end; i++) {
        if ((i % 4) == 0)
            fprintf(stream, "\n    ");
        fprintf(stream, "0x%02x, ", ((int)actual[i])&0xff);
    }
    fprintf(stream, "]\n");

    /* Pad to line up with test name ... in virTestRun */
    fprintf(stream, "                                                                      ... ");

    return 0;
}

/*
 * @param actual: String input content
 * @param filename: File to compare @actual against
 *
 * If @actual is NULL, it's treated as an empty string.
 */
int
virTestCompareToFile(const char *actual,
                     const char *filename)
{
    int ret = -1;
    char *filecontent = NULL;
    char *fixedcontent = NULL;
    const char *cmpcontent = actual;

    if (!cmpcontent)
        cmpcontent = "";

    if (virTestLoadFile(filename, &filecontent) < 0 && !virTestGetRegenerate())
        goto failure;

    if (filecontent) {
        size_t filecontentLen = strlen(filecontent);
        size_t cmpcontentLen = strlen(cmpcontent);

        if (filecontentLen > 0 &&
            filecontent[filecontentLen - 1] == '\n' &&
            (cmpcontentLen == 0 || cmpcontent[cmpcontentLen - 1] != '\n')) {
            fixedcontent = g_strdup_printf("%s\n", cmpcontent);
            cmpcontent = fixedcontent;
        }
    }

    if (STRNEQ_NULLABLE(cmpcontent, filecontent)) {
        virTestDifferenceFull(stderr,
                              filecontent, filename,
                              cmpcontent, NULL);
        goto failure;
    }

    ret = 0;
 failure:
    VIR_FREE(fixedcontent);
    VIR_FREE(filecontent);
    return ret;
}

int
virTestCompareToULL(unsigned long long expect,
                    unsigned long long actual)
{
    g_autofree char *expectStr = NULL;
    g_autofree char *actualStr = NULL;

    expectStr = g_strdup_printf("%llu", expect);

    actualStr = g_strdup_printf("%llu", actual);

    return virTestCompareToString(expectStr, actualStr);
}

int
virTestCompareToString(const char *expect,
                       const char *actual)
{
    if (STRNEQ_NULLABLE(expect, actual)) {
        virTestDifference(stderr, expect, actual);
        return -1;
    }

    return 0;
}

static void
virTestErrorFuncQuiet(void *data G_GNUC_UNUSED,
                      virErrorPtr err G_GNUC_UNUSED)
{ }


/* register an error handler in tests when using connections */
void
virTestQuiesceLibvirtErrors(bool always)
{
    if (always || !virTestGetVerbose())
        virSetErrorFunc(NULL, virTestErrorFuncQuiet);
}

struct virtTestLogData {
    virBuffer buf;
};

static struct virtTestLogData testLog = { VIR_BUFFER_INITIALIZER };

static void
virtTestLogOutput(virLogSourcePtr source G_GNUC_UNUSED,
                  virLogPriority priority G_GNUC_UNUSED,
                  const char *filename G_GNUC_UNUSED,
                  int lineno G_GNUC_UNUSED,
                  const char *funcname G_GNUC_UNUSED,
                  const char *timestamp,
                  virLogMetadataPtr metadata G_GNUC_UNUSED,
                  const char *rawstr G_GNUC_UNUSED,
                  const char *str,
                  void *data)
{
    struct virtTestLogData *log = data;
    virBufferAsprintf(&log->buf, "%s: %s", timestamp, str);
}

static void
virtTestLogClose(void *data)
{
    struct virtTestLogData *log = data;

    virBufferFreeAndReset(&log->buf);
}

/* Return a malloc'd string (possibly with strlen of 0) of all data
 * logged since the last call to this function, or NULL on failure.  */
char *
virTestLogContentAndReset(void)
{
    char *ret;

    ret = virBufferContentAndReset(&testLog.buf);
    if (!ret)
        ret = g_strdup("");
    return ret;
}


unsigned int
virTestGetDebug(void)
{
    if (testDebug == -1)
        testDebug = virTestGetFlag("VIR_TEST_DEBUG");
    return testDebug;
}

unsigned int
virTestGetVerbose(void)
{
    if (testVerbose == -1)
        testVerbose = virTestGetFlag("VIR_TEST_VERBOSE");
    return testVerbose || virTestGetDebug();
}

unsigned int
virTestGetExpensive(void)
{
    if (testExpensive == -1)
        testExpensive = virTestGetFlag("VIR_TEST_EXPENSIVE");
    return testExpensive;
}

unsigned int
virTestGetRegenerate(void)
{
    if (testRegenerate == -1)
        testRegenerate = virTestGetFlag("VIR_TEST_REGENERATE_OUTPUT");
    return testRegenerate;
}

static int
virTestSetEnvPath(void)
{
    int ret = -1;
    const char *path = getenv("PATH");
    char *new_path = NULL;

    if (path) {
        if (strstr(path, abs_builddir) != path)
            new_path = g_strdup_printf("%s:%s", abs_builddir, path);
    } else {
        new_path = g_strdup(abs_builddir);
    }

    if (new_path &&
        g_setenv("PATH", new_path, TRUE) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    VIR_FREE(new_path);
    return ret;
}

int virTestMain(int argc,
                char **argv,
                int (*func)(void),
                ...)
{
    const char *lib;
    va_list ap;
    int ret;
    char *testRange = NULL;
    size_t noutputs = 0;
    virLogOutputPtr output = NULL;
    virLogOutputPtr *outputs = NULL;
    g_autofree char *baseprogname = NULL;
    const char *progname;
    g_autofree const char **preloads = NULL;
    size_t npreloads = 0;
    g_autofree char *mock = NULL;

    if (getenv("VIR_TEST_FILE_ACCESS")) {
        preloads = g_renew(const char *, preloads, npreloads + 2);
        preloads[npreloads++] = VIR_TEST_MOCK("virtest");
        preloads[npreloads] = NULL;
    }

    va_start(ap, func);
    while ((lib = va_arg(ap, const char *))) {
        if (!virFileIsExecutable(lib)) {
            perror(lib);
            return EXIT_FAILURE;
        }

        preloads = g_renew(const char *, preloads, npreloads + 2);
        preloads[npreloads++] = lib;
        preloads[npreloads] = NULL;
    }
    va_end(ap);

    if (preloads) {
        mock = g_strjoinv(":", (char **)preloads);
        VIR_TEST_PRELOAD(mock);
    }

    progname = baseprogname = g_path_get_basename(argv[0]);
    if (STRPREFIX(progname, "lt-"))
        progname += 3;

    g_setenv("VIR_TEST_MOCK_PROGNAME", progname, TRUE);

    virFileActivateDirOverrideForProg(argv[0]);

    if (virTestSetEnvPath() < 0)
        return EXIT_AM_HARDFAIL;

    if (!virFileExists(abs_srcdir))
        return EXIT_AM_HARDFAIL;

    if (argc > 1) {
        fprintf(stderr, "Usage: %s\n", argv[0]);
        fputs("effective environment variables:\n"
              "VIR_TEST_VERBOSE set to show names of individual tests\n"
              "VIR_TEST_DEBUG set to show information for debugging failures",
              stderr);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "TEST: %s\n", progname);

    if (virErrorInitialize() < 0)
        return EXIT_FAILURE;

    virLogSetFromEnv();
    if (!getenv("LIBVIRT_DEBUG") && !virLogGetNbOutputs()) {
        if (!(output = virLogOutputNew(virtTestLogOutput, virtTestLogClose,
                                       &testLog, VIR_LOG_DEBUG,
                                       VIR_LOG_TO_STDERR, NULL)) ||
            VIR_APPEND_ELEMENT(outputs, noutputs, output) < 0 ||
            virLogDefineOutputs(outputs, noutputs) < 0) {
            virLogOutputFree(output);
            virLogOutputListFree(outputs, noutputs);
            return EXIT_FAILURE;
        }
    }

    if ((testRange = getenv("VIR_TEST_RANGE")) != NULL) {
        if (!(testBitmap = virBitmapParseUnlimited(testRange))) {
            fprintf(stderr, "Cannot parse range %s\n", testRange);
            return EXIT_FAILURE;
        }
    }

    ret = (func)();

    virResetLastError();
    if (!virTestGetVerbose() && ret != EXIT_AM_SKIP) {
        if (testCounter == 0 || testCounter % 40)
            fprintf(stderr, "%*s", 40 - (int)(testCounter % 40), "");
        fprintf(stderr, " %-3zu %s\n", testCounter, ret == 0 ? "OK" : "FAIL");
    }
    virLogReset();
    return ret;
}


/*
 * @cmdset contains a list of command line args, eg
 *
 * "/usr/sbin/iptables --table filter --insert INPUT --in-interface virbr0 --protocol tcp --destination-port 53 --jump ACCEPT
 *  /usr/sbin/iptables --table filter --insert INPUT --in-interface virbr0 --protocol udp --destination-port 53 --jump ACCEPT
 *  /usr/sbin/iptables --table filter --insert FORWARD --in-interface virbr0 --jump REJECT
 *  /usr/sbin/iptables --table filter --insert FORWARD --out-interface virbr0 --jump REJECT
 *  /usr/sbin/iptables --table filter --insert FORWARD --in-interface virbr0 --out-interface virbr0 --jump ACCEPT"
 *
 * And we're munging it in-place to strip the path component
 * of the command line, to produce
 *
 * "iptables --table filter --insert INPUT --in-interface virbr0 --protocol tcp --destination-port 53 --jump ACCEPT
 *  iptables --table filter --insert INPUT --in-interface virbr0 --protocol udp --destination-port 53 --jump ACCEPT
 *  iptables --table filter --insert FORWARD --in-interface virbr0 --jump REJECT
 *  iptables --table filter --insert FORWARD --out-interface virbr0 --jump REJECT
 *  iptables --table filter --insert FORWARD --in-interface virbr0 --out-interface virbr0 --jump ACCEPT"
 */
void virTestClearCommandPath(char *cmdset)
{
    size_t offset = 0;
    char *lineStart = cmdset;
    char *lineEnd = strchr(lineStart, '\n');

    while (lineStart) {
        char *dirsep;
        char *movestart;
        size_t movelen;
        dirsep = strchr(lineStart, ' ');
        if (dirsep) {
            while (dirsep > lineStart && *dirsep != '/')
                dirsep--;
            if (*dirsep == '/')
                dirsep++;
            movestart = dirsep;
        } else {
            movestart = lineStart;
        }
        movelen = lineEnd ? lineEnd - movestart : strlen(movestart);

        if (movelen) {
            memmove(cmdset + offset, movestart, movelen + 1);
            offset += movelen + 1;
        }
        lineStart = lineEnd ? lineEnd + 1 : NULL;
        lineEnd = lineStart ? strchr(lineStart, '\n') : NULL;
    }
    cmdset[offset] = '\0';
}


virCapsPtr virTestGenericCapsInit(void)
{
    virCapsPtr caps;
    virCapsGuestPtr guest;

    if ((caps = virCapabilitiesNew(VIR_ARCH_X86_64,
                                   false, false)) == NULL)
        return NULL;

    if ((guest = virCapabilitiesAddGuest(caps, VIR_DOMAIN_OSTYPE_HVM, VIR_ARCH_I686,
                                         "/usr/bin/acme-virt", NULL,
                                         0, NULL)) == NULL)
        goto error;

    if (!virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_TEST, NULL, NULL, 0, NULL))
        goto error;
    if (!virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_QEMU,
                                       NULL, NULL, 0, NULL))
        goto error;
    if (!virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_KVM,
                                       NULL, NULL, 0, NULL))
        goto error;

    if ((guest = virCapabilitiesAddGuest(caps, VIR_DOMAIN_OSTYPE_HVM, VIR_ARCH_X86_64,
                                         "/usr/bin/acme-virt", NULL,
                                         0, NULL)) == NULL)
        goto error;

    if (!virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_TEST, NULL, NULL, 0, NULL))
        goto error;
    if (!virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_QEMU,
                                       NULL, NULL, 0, NULL))
        goto error;
    if (!virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_KVM,
                                       NULL, NULL, 0, NULL))
        goto error;


    if (virTestGetDebug() > 1) {
        char *caps_str;

        caps_str = virCapabilitiesFormatXML(caps);
        if (!caps_str)
            goto error;

        VIR_TEST_DEBUG("Generic driver capabilities:\n%s", caps_str);

        VIR_FREE(caps_str);
    }

    return caps;

 error:
    virObjectUnref(caps);
    return NULL;
}


#define MAX_CELLS 4
#define MAX_CPUS_IN_CELL 2
#define MAX_MEM_IN_CELL 2097152

/*
 * Build NUMA topology with cell id starting from (0 + seq)
 * for testing
 */
virCapsHostNUMAPtr
virTestCapsBuildNUMATopology(int seq)
{
    virCapsHostNUMAPtr caps = virCapabilitiesHostNUMANew();
    virCapsHostNUMACellCPUPtr cell_cpus = NULL;
    int core_id, cell_id;
    int id;

    id = 0;
    for (cell_id = 0; cell_id < MAX_CELLS; cell_id++) {
        if (VIR_ALLOC_N(cell_cpus, MAX_CPUS_IN_CELL) < 0)
            goto error;

        for (core_id = 0; core_id < MAX_CPUS_IN_CELL; core_id++) {
            cell_cpus[core_id].id = id + core_id;
            cell_cpus[core_id].socket_id = cell_id + seq;
            cell_cpus[core_id].core_id = id + core_id;
            if (!(cell_cpus[core_id].siblings =
                  virBitmapNew(MAX_CPUS_IN_CELL)))
                goto error;
            ignore_value(virBitmapSetBit(cell_cpus[core_id].siblings, id));
        }
        id++;

        virCapabilitiesHostNUMAAddCell(caps, cell_id + seq,
                                       MAX_MEM_IN_CELL,
                                       MAX_CPUS_IN_CELL, cell_cpus,
                                       VIR_ARCH_NONE, NULL,
                                       VIR_ARCH_NONE, NULL);

        cell_cpus = NULL;
    }

    return caps;

 error:
    virCapabilitiesHostNUMAUnref(caps);
    VIR_FREE(cell_cpus);
    return NULL;
}

static virDomainDefParserConfig virTestGenericDomainDefParserConfig = {
    .features = VIR_DOMAIN_DEF_FEATURE_INDIVIDUAL_VCPUS,
};

virDomainXMLOptionPtr virTestGenericDomainXMLConfInit(void)
{
    return virDomainXMLOptionNew(&virTestGenericDomainDefParserConfig,
                                 NULL, NULL, NULL, NULL);
}


int
testCompareDomXML2XMLFiles(virCapsPtr caps G_GNUC_UNUSED,
                           virDomainXMLOptionPtr xmlopt,
                           const char *infile, const char *outfile, bool live,
                           unsigned int parseFlags,
                           testCompareDomXML2XMLResult expectResult)
{
    char *actual = NULL;
    int ret = -1;
    testCompareDomXML2XMLResult result;
    virDomainDefPtr def = NULL;
    unsigned int parse_flags = live ? 0 : VIR_DOMAIN_DEF_PARSE_INACTIVE;
    unsigned int format_flags = VIR_DOMAIN_DEF_FORMAT_SECURE;

    parse_flags |= parseFlags;

    if (!virFileExists(infile)) {
        VIR_TEST_DEBUG("Test input file '%s' is missing", infile);
        return -1;
    }

    if (!live)
        format_flags |= VIR_DOMAIN_DEF_FORMAT_INACTIVE;

    if (!(def = virDomainDefParseFile(infile, xmlopt, NULL, parse_flags))) {
        result = TEST_COMPARE_DOM_XML2XML_RESULT_FAIL_PARSE;
        goto out;
    }

    if (!virDomainDefCheckABIStability(def, def, xmlopt)) {
        VIR_TEST_DEBUG("ABI stability check failed on %s", infile);
        result = TEST_COMPARE_DOM_XML2XML_RESULT_FAIL_STABILITY;
        goto out;
    }

    if (!(actual = virDomainDefFormat(def, xmlopt, format_flags))) {
        result = TEST_COMPARE_DOM_XML2XML_RESULT_FAIL_FORMAT;
        goto out;
    }

    if (virTestCompareToFile(actual, outfile) < 0) {
        result = TEST_COMPARE_DOM_XML2XML_RESULT_FAIL_COMPARE;
        goto out;
    }

    result = TEST_COMPARE_DOM_XML2XML_RESULT_SUCCESS;

 out:
    if (result == expectResult) {
        ret = 0;
        if (expectResult != TEST_COMPARE_DOM_XML2XML_RESULT_SUCCESS) {
            VIR_TEST_DEBUG("Got expected failure code=%d msg=%s",
                           result, virGetLastErrorMessage());
        }
    } else {
        ret = -1;
        VIR_TEST_DEBUG("Expected result code=%d but received code=%d",
                       expectResult, result);
    }

    VIR_FREE(actual);
    virDomainDefFree(def);
    return ret;
}


static int virtTestCounter;
static char virtTestCounterStr[128];
static char *virtTestCounterPrefixEndOffset;


/**
 * virTestCounterReset:
 * @prefix: name of the test group
 *
 * Resets the counter and sets up the test group name to use with
 * virTestCounterNext(). This function is not thread safe.
 *
 * Note: The buffer for the assembled message is 128 bytes long. Longer test
 * case names (including the number index) will be silently truncated.
 */
void
virTestCounterReset(const char *prefix)
{
    virtTestCounter = 0;

    ignore_value(virStrcpyStatic(virtTestCounterStr, prefix));
    virtTestCounterPrefixEndOffset = strchrnul(virtTestCounterStr, '\0');
}


/**
 * virTestCounterNext:
 *
 * This function is designed to ease test creation and reordering by adding
 * a way to do automagic test case numbering.
 *
 * Returns string consisting of test name prefix configured via
 * virTestCounterReset() and a number that increments in every call of this
 * function. This function is not thread safe.
 *
 * Note: The buffer for the assembled message is 128 bytes long. Longer test
 * case names (including the number index) will be silently truncated.
 */
const char
*virTestCounterNext(void)
{
    size_t len = G_N_ELEMENTS(virtTestCounterStr);

    /* calculate length of the rest of the string */
    len -= (virtTestCounterPrefixEndOffset - virtTestCounterStr);

    g_snprintf(virtTestCounterPrefixEndOffset, len, "%d", ++virtTestCounter);

    return virtTestCounterStr;
}
