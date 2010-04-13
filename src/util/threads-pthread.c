/*
 * threads-pthread.c: basic thread synchronization primitives
 *
 * Copyright (C) 2009-2010 Red Hat, Inc.
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
 */

#include <config.h>


/* Nothing special required for pthreads */
int virThreadInitialize(void)
{
    return 0;
}

void virThreadOnExit(void)
{
}


int virMutexInit(virMutexPtr m)
{
    int ret;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    if ((ret = pthread_mutex_init(&m->lock, &attr)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

int virMutexInitRecursive(virMutexPtr m)
{
    int ret;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if ((ret = pthread_mutex_init(&m->lock, &attr)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

void virMutexDestroy(virMutexPtr m)
{
    pthread_mutex_destroy(&m->lock);
}

void virMutexLock(virMutexPtr m){
    pthread_mutex_lock(&m->lock);
}

void virMutexUnlock(virMutexPtr m)
{
    pthread_mutex_unlock(&m->lock);
}


int virCondInit(virCondPtr c)
{
    int ret;
    if ((ret = pthread_cond_init(&c->cond, NULL)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

int virCondDestroy(virCondPtr c)
{
    int ret;
    if ((ret = pthread_cond_destroy(&c->cond)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

int virCondWait(virCondPtr c, virMutexPtr m)
{
    int ret;
    if ((ret = pthread_cond_wait(&c->cond, &m->lock)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

int virCondWaitUntil(virCondPtr c, virMutexPtr m, unsigned long long whenms)
{
    int ret;
    struct timespec ts;

    ts.tv_sec = whenms / 1000;
    ts.tv_nsec = (whenms % 1000) * 1000;

    if ((ret = pthread_cond_timedwait(&c->cond, &m->lock, &ts)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

void virCondSignal(virCondPtr c)
{
    pthread_cond_signal(&c->cond);
}

void virCondBroadcast(virCondPtr c)
{
    pthread_cond_broadcast(&c->cond);
}


int virThreadLocalInit(virThreadLocalPtr l,
                       virThreadLocalCleanup c)
{
    int ret;
    if ((ret = pthread_key_create(&l->key, c)) != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}

void *virThreadLocalGet(virThreadLocalPtr l)
{
    return pthread_getspecific(l->key);
}

void virThreadLocalSet(virThreadLocalPtr l, void *val)
{
    pthread_setspecific(l->key, val);
}
