#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"

// --- Pin Definitions ---
#define MC60_PWRKEY_PIN     GPIO_NUM_45
#define MC60_RX_PIN         18   // ESP32's own RX GPIO (MC60's TX)
#define MC60_TX_PIN         17   // ESP32's own TX GPIO (MC60's RX)

// --- MC60 UART ---
#define MC60_UART_NUM        UART_NUM_1
#define MC60_UART_BAUD       115200
#define MC60_UART_RX_BUF_SIZE 1024

// --- Timers & Intervals (ms) ---
#define UPLOAD_INTERVAL_MS        10000  // default; changeable at runtime via the SETINTERVAL SMS command
#define UPLOAD_INTERVAL_MIN_MS    10000  // floor -- below this, mc60_send_command()'s fixed AT-wait overhead dominates the cycle anyway
#define UPLOAD_INTERVAL_MAX_MS  3600000  // ceiling (1 hour)
#define GPS_FIX_TIMEOUT_MS        60000
#define PWRKEY_PULSE_MS            1000
#define FATAL_ERROR_RESTART_MS    30000

// --- Adafruit IO ---
#define ADAFRUIT_SERVER  "io.adafruit.com"
#define ADAFRUIT_PORT    1883

// --- Retry limits ---
#define MAX_RETRIES            5
#define MAX_PUBLISH_FAILURES   5

// --- SMS queue ---
#define SMS_QUEUE_CAPACITY 6  // was 3; a burst of authorized commands (or carrier redelivery) could overrun that

// --- Buffer sizes ---
#define MC60_RESP_BUF_SIZE          512  // generic AT response buffer
#define MC60_SMS_SENDER_BUF_SIZE     32  // E.164 max ~16 chars, plus margin
#define MC60_SMS_TEXT_BUF_SIZE      256  // single-part SMS max 160 GSM-7 chars, plus margin
#define MC60_ROLLING_BUF_CAP        1000 // matches the original String cap
#define MC60_JSON_PAYLOAD_BUF_SIZE  160
#define MC60_FEED_PATH_BUF_SIZE     128
#define MC60_AT_CMD_BUF_SIZE        192
#define MC60_GGA_LINE_BUF_SIZE       96
#define MC60_GGA_MAX_FIELDS          16

// millis()-equivalent: microseconds-since-boot / 1000, same computation
// Arduino's own millis() uses internally. uint32_t wraps at ~49.7 days;
// all timer comparisons in this codebase use rollover-safe unsigned
// subtraction (now - stateTimer >= INTERVAL), so that's not a problem.
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
