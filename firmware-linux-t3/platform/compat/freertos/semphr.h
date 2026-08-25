/* FreeRTOS semaphore/mutex -> pthreads.
 *
 * This used to be a bare pthread_mutex_t with the timeout argument IGNORED, which
 * was fine while the only user was one always-blocking mutex. It is not fine for
 * the offline relay: that waits on a reply from the Control4 driver with a real
 * timeout, and "ignore the timeout" would park the calling task forever if the
 * reply never arrived. So both flavours are modelled properly -- a recursive-free
 * mutex and a counting/binary semaphore -- and Take honours its deadline.
 */
#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

typedef struct nv_sem {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             count;      /* semaphore token count (mutex: 1 = free) */
    int             is_mutex;
} nv_sem_t;

typedef nv_sem_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t nv_sem_new(int count, int is_mutex) {
    nv_sem_t *s = (nv_sem_t *)calloc(1, sizeof(nv_sem_t));
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = count;
    s->is_mutex = is_mutex;
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)  { return nv_sem_new(1, 1); }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return nv_sem_new(0, 0); }

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait_ms) {
    if (!s) return pdFALSE;
    pthread_mutex_lock(&s->m);
    BaseType_t rc = pdTRUE;
    if (wait_ms == portMAX_DELAY) {
        while (s->count <= 0) pthread_cond_wait(&s->c, &s->m);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(wait_ms / 1000u);
        ts.tv_nsec += (long)(wait_ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (s->count <= 0) {
            if (pthread_cond_timedwait(&s->c, &s->m, &ts) == ETIMEDOUT) { rc = pdFALSE; break; }
        }
    }
    if (rc == pdTRUE) s->count--;
    pthread_mutex_unlock(&s->m);
    return rc;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s) return pdFALSE;
    pthread_mutex_lock(&s->m);
    /* A binary semaphore saturates at one token; a mutex never exceeds one either. */
    if (s->count < 1) s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
