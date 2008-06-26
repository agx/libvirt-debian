/*
 * utils.c: test utils
 *
 * Copyright (C) 2005 Red Hat, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Karel Zak <kzak@redhat.com>
 *
 * $Id: testutils.h,v 1.8 2008/05/29 15:21:45 berrange Exp $
 */

#ifndef __VIT_TEST_UTILS_H__
#define __VIT_TEST_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif


    double virtTestCountAverage(double *items,
                                int nitems);

    int	virtTestRun(const char *title,
                    int nloops,
                    int (*body)(const void *data),
                    const void *data);
    int virtTestLoadFile(const char *name,
                         char **buf,
                         int buflen);
    int virtTestCaptureProgramOutput(const char *const argv[],
                                     char **buf,
                                     int buflen);


    int virtTestDifference(FILE *stream,
                           const char *expect,
                           const char *actual);

    int virtTestMain(int argc,
                     char **argv,
                     int (*func)(int, char **));

#define VIRT_TEST_MAIN(func)                    \
    int main(int argc, char **argv)  {          \
        return virtTestMain(argc,argv, func);   \
    }

#ifdef __cplusplus
}
#endif
#endif /* __VIT_TEST_UTILS_H__ */
