/* Minimal FreeRTOS -> Linux shim base. The shared MMKeypad app uses only a
 * tiny slice of FreeRTOS (delays, a few tasks, one mutex); map it onto
 * POSIX. Tick == millisecond here. */
#pragma once
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY ((TickType_t)0xFFFFFFFF)
#define portTICK_PERIOD_MS 1
#define configMINIMAL_STACK_SIZE 2048
#define tskNO_AFFINITY (-1)
