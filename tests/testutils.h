/*
 * utils.c: test utils
 *
 * Copyright (C) 2005 Red Hat, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Karel Zak <kzak@redhat.com>
 *
 * $Id: testutils.h,v 1.4 2007/07/18 21:08:22 berrange Exp $
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

#ifdef __cplusplus
}
#endif
#endif /* __VIT_TEST_UTILS_H__ */

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
