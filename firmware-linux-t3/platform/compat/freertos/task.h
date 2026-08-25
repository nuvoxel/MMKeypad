/* FreeRTOS task API -> pthreads. The app creates a handful of long-lived
 * worker threads (net rx, art fetch, sip) and does simple delays. */
#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef pthread_t *TaskHandle_t;

static inline void vTaskDelay(TickType_t ticks) {
    struct timespec ts = {.tv_sec = ticks / 1000,
                          .tv_nsec = (long)(ticks % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* FreeRTOS task fn returns void; pthread wants void*. Trampoline it. */
typedef void (*TaskFunction_t)(void *);

struct _mmk_task_ctx {
    TaskFunction_t fn;
    void *arg;
};

static inline void *_mmk_task_trampoline(void *p) {
    struct _mmk_task_ctx *c = (struct _mmk_task_ctx *)p;
    TaskFunction_t fn = c->fn;
    void *arg = c->arg;
    /* leak the small ctx; tasks are long-lived/never-return in this app */
    fn(arg);
    return NULL;
}

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                                     uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out) {
    (void)name; (void)stack; (void)prio;
    struct _mmk_task_ctx *c = (struct _mmk_task_ctx *)malloc(sizeof(*c));
    c->fn = fn; c->arg = arg;
    pthread_t *th = (pthread_t *)malloc(sizeof(pthread_t));
    if (pthread_create(th, NULL, _mmk_task_trampoline, c) != 0) {
        free(c); free(th);
        return pdFALSE;
    }
    pthread_detach(*th);
    if (out) *out = th;
    return pdPASS;
}

static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn,
        const char *name, uint32_t stack, void *arg, UBaseType_t prio,
        TaskHandle_t *out, BaseType_t core) {
    (void)core;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

static inline void vTaskDelete(TaskHandle_t t) {
    /* Only ever called with NULL (delete self) in this app -> just return
     * from the thread fn, which the trampoline handles. */
    (void)t;
    pthread_exit(NULL);
}

static inline TickType_t xTaskGetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); /* tick == ms */
}
