/*
 * testutils.c: basic test utils
 *
 * Copyright (C) 2005-2007 Red Hat, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Karel Zak <kzak@redhat.com>
 *
 * $Id: testutils.c,v 1.22 2008/11/21 12:16:08 berrange Exp $
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include "testutils.h"
#include "internal.h"
#include "memory.h"
#include "util.h"

#if TEST_OOM_TRACE
#include <execinfo.h>
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#ifndef _PATH_DEVNULL
#define	_PATH_DEVNULL	"/dev/null"
#endif

#define GETTIMEOFDAY(T) gettimeofday(T, NULL)
#define DIFF_MSEC(T, U)                                 \
    ((((int) ((T)->tv_sec - (U)->tv_sec)) * 1000000.0 +	\
      ((int) ((T)->tv_usec - (U)->tv_usec))) / 1000.0)

static unsigned int testOOM = 0;
static unsigned int testDebug = 0;
static unsigned int testCounter = 0;

double
virtTestCountAverage(double *items, int nitems)
{
    long double sum = 0;
    int i;

    for (i=1; i < nitems; i++)
        sum += items[i];

    return (double) (sum / nitems);
}

/*
 * Runs test and count average time (if the nloops is grater than 1)
 *
 * returns: -1 = error, 0 = success
 */
int
virtTestRun(const char *title, int nloops, int (*body)(const void *data), const void *data)
{
    int i, ret = 0;
    double *ts = NULL;

    testCounter++;

    if (testOOM < 2) {
        fprintf(stderr, "%2d) %-65s ... ", testCounter, title);
        fflush(stderr);
    }

    if (nloops > 1 && (ts = calloc(nloops,
                                   sizeof(double)))==NULL)
        return -1;

    for (i=0; i < nloops; i++) {
        struct timeval before, after;

        if (ts)
            GETTIMEOFDAY(&before);
        if ((ret = body(data)) != 0)
            break;
        if (ts)	{
            GETTIMEOFDAY(&after);
            ts[i] = DIFF_MSEC(&after, &before);
        }
    }
    if (testOOM < 2) {
        if (ret == 0 && ts)
            fprintf(stderr, "OK     [%.5f ms]\n",
                    virtTestCountAverage(ts, nloops));
        else if (ret == 0)
            fprintf(stderr, "OK\n");
        else
            fprintf(stderr, "FAILED\n");
    }

    free(ts);
    return ret;
}

int virtTestLoadFile(const char *name,
                     char **buf,
                     int buflen) {
    FILE *fp = fopen(name, "r");
    struct stat st;

    if (!fp)
        return -1;

    if (fstat(fileno(fp), &st) < 0) {
        fclose(fp);
        return -1;
    }

    if (st.st_size > (buflen-1)) {
        fclose(fp);
        return -1;
    }

    if (st.st_size) {
        if (fread(*buf, st.st_size, 1, fp) != 1) {
            fclose(fp);
            return -1;
        }
    }
    (*buf)[st.st_size] = '\0';

    fclose(fp);
    return st.st_size;
}

#ifndef WIN32
static
void virtTestCaptureProgramExecChild(const char *const argv[],
                                     int pipefd) {
    int i;
    int open_max;
    int stdinfd = -1;
    int stderrfd = -1;
    const char *const env[] = {
        "LANG=C",
#if WITH_DRIVER_MODULES
        "LIBVIRT_DRIVER_DIR=" TEST_DRIVER_DIR,
#endif
        NULL
    };

    if ((stdinfd = open(_PATH_DEVNULL, O_RDONLY)) < 0)
        goto cleanup;
    if ((stderrfd = open(_PATH_DEVNULL, O_WRONLY)) < 0)
        goto cleanup;

    open_max = sysconf (_SC_OPEN_MAX);
    for (i = 0; i < open_max; i++) {
        if (i != stdinfd &&
            i != stderrfd &&
            i != pipefd)
            close(i);
    }

    if (dup2(stdinfd, STDIN_FILENO) != STDIN_FILENO)
        goto cleanup;
    if (dup2(pipefd, STDOUT_FILENO) != STDOUT_FILENO)
        goto cleanup;
    if (dup2(stderrfd, STDERR_FILENO) != STDERR_FILENO)
        goto cleanup;

    /* SUS is crazy here, hence the cast */
    execve(argv[0], (char *const*)argv, (char *const*)env);

 cleanup:
    if (stdinfd != -1)
        close(stdinfd);
    if (stderrfd != -1)
        close(stderrfd);
}

int virtTestCaptureProgramOutput(const char *const argv[],
                                 char **buf,
                                 int buflen) {
    int pipefd[2];

    if (pipe(pipefd) < 0)
        return -1;

    int pid = fork();
    switch (pid) {
    case 0:
        close(pipefd[0]);
        virtTestCaptureProgramExecChild(argv, pipefd[1]);

        close(pipefd[1]);
        _exit(1);

    case -1:
        return -1;

    default:
        {
            int got = 0;
            int ret = -1;
            int want = buflen-1;

            close(pipefd[1]);

            while (want) {
                if ((ret = read(pipefd[0], (*buf)+got, want)) <= 0)
                    break;
                got += ret;
                want -= ret;
            }
            close(pipefd[0]);

            if (!ret)
                (*buf)[got] = '\0';

            waitpid(pid, NULL, 0);

            return ret;
        }
    }
}
#endif /* !WIN32 */


/**
 * @param stream: output stream write to differences to
 * @param expect: expected output text
 * @param actual: actual output text
 *
 * Display expected and actual output text, trimmed to
 * first and last characters at which differences occur
 */
int virtTestDifference(FILE *stream,
                       const char *expect,
                       const char *actual)
{
    const char *expectStart = expect;
    const char *expectEnd = expect + (strlen(expect)-1);
    const char *actualStart = actual;
    const char *actualEnd = actual + (strlen(actual)-1);

    if (!testDebug)
        return 0;

    if (testDebug < 2) {
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
    fprintf(stream, "\nExpect [");
    if ((expectEnd - expectStart + 1) &&
        fwrite(expectStart, (expectEnd-expectStart+1), 1, stream) != 1)
        return -1;
    fprintf(stream, "]\n");
    fprintf(stream, "Actual [");
    if ((actualEnd - actualStart + 1) &&
        fwrite(actualStart, (actualEnd-actualStart+1), 1, stream) != 1)
        return -1;
    fprintf(stream, "]\n");

    /* Pad to line up with test name ... in virTestRun */
    fprintf(stream, "                                                                      ... ");

    return 0;
}

#if TEST_OOM
static void
virtTestErrorFuncQuiet(void *data ATTRIBUTE_UNUSED,
                       virErrorPtr err ATTRIBUTE_UNUSED)
{ }
#endif

#if TEST_OOM_TRACE
static void
virtTestErrorHook(int n, void *data ATTRIBUTE_UNUSED)
{
    void *trace[30];
    int ntrace = ARRAY_CARDINALITY(trace);
    int i;
    char **symbols = NULL;

    ntrace = backtrace(trace, ntrace);
    symbols = backtrace_symbols(trace, ntrace);
    if (symbols) {
        fprintf(stderr, "Failing allocation %d at:\n", n);
        for (i = 0 ; i < ntrace ; i++) {
            if (symbols[i])
                fprintf(stderr, "  TRACE:  %s\n", symbols[i]);
        }
        free(symbols);
    }
}
#endif


int virtTestMain(int argc,
                 char **argv,
                 int (*func)(int, char **))
{
    char *debugStr;
#if TEST_OOM
    int ret;
    int approxAlloc = 0;
    int n;
    char *oomStr = NULL;
    int oomCount;
    int mp = 0;
    pid_t *workers;
    int worker = 0;
#endif

    if ((debugStr = getenv("VIR_TEST_DEBUG")) != NULL) {
        if (virStrToLong_ui(debugStr, NULL, 10, &testDebug) < 0)
            testDebug = 0;
    }

#if TEST_OOM
    if ((oomStr = getenv("VIR_TEST_OOM")) != NULL) {
        if (virStrToLong_i(oomStr, NULL, 10, &oomCount) < 0)
            oomCount = 0;

        if (oomCount < 0)
            oomCount = 0;
        if (oomCount)
            testOOM = 1;
    }

    if (getenv("VIR_TEST_MP") != NULL) {
        mp = sysconf(_SC_NPROCESSORS_ONLN);
        fprintf(stderr, "Using %d worker processes\n", mp);
        if (VIR_ALLOC_N(workers, mp) < 0)
            return EXIT_FAILURE;
    }

    if (testOOM)
        virAllocTestInit();

    /* Run once to count allocs, and ensure it passes :-) */
    ret = (func)(argc, argv);
    if (ret != EXIT_SUCCESS)
        return EXIT_FAILURE;

#if TEST_OOM_TRACE
    if (testDebug)
        virAllocTestHook(virtTestErrorHook, NULL);
#endif

    if (testOOM) {
        /* Makes next test runs quiet... */
        testOOM++;
        virSetErrorFunc(NULL, virtTestErrorFuncQuiet);

        approxAlloc = virAllocTestCount();
        testCounter++;
        if (testDebug)
            fprintf(stderr, "%d) OOM...\n", testCounter);
        else
            fprintf(stderr, "%d) OOM of %d allocs ", testCounter, approxAlloc);

        if (mp) {
            int i;
            for (i = 0 ; i < mp ; i++) {
                workers[i] = fork();
                if (workers[i] == 0) {
                    worker = i + 1;
                    break;
                }
            }
        }

        /* Run once for each alloc, failing a different one
           and validating that the test case failed */
        for (n = 0; n < approxAlloc && (!mp || worker) ; n++) {
            if ((n % mp) != (worker - 1))
                continue;
            if (!testDebug) {
                if (mp)
                    fprintf(stderr, "%d", worker);
                else
                    fprintf(stderr, ".");
                fflush(stderr);
            }
            virAllocTestOOM(n+1, oomCount);

            if (((func)(argc, argv)) != EXIT_FAILURE) {
                ret = EXIT_FAILURE;
                break;
            }
        }

        if (mp) {
            if (worker) {
                _exit(ret);
            } else {
                int i, status;
                for (i = 0 ; i < mp ; i++) {
                    waitpid(workers[i], &status, 0);
                    if (WEXITSTATUS(status) != EXIT_SUCCESS)
                        ret = EXIT_FAILURE;
                }
                VIR_FREE(workers);
            }
        }

        if (testDebug)
            fprintf(stderr, " ... OOM of %d allocs", approxAlloc);

        if (ret == EXIT_SUCCESS)
            fprintf(stderr, " OK\n");
        else
            fprintf(stderr, " FAILED\n");
    }
    return ret;

#else
    return (func)(argc, argv);
#endif
}
