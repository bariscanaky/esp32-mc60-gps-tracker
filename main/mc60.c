#include "mc60.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "credentials.h" // IO_USERNAME, IO_KEY, FEED_NAME, SIM_APN, AUTHORIZED_NUMBERS

static const char *TAG = "mc60";

// --- Authorized senders ---
// AUTHORIZED_NUMBERS (admin): full command access, including PWROFF/SETINTERVAL.
// VIEWER_NUMBERS (viewer): can query location/status but not change device
// behavior. VIEWER_NUMBERS is optional -- credentials.example.h documents it
// as `{ }` (empty) by default, so existing credentials.h files without it
// still compile (an empty array has kViewerCount == 0).
static const char *const kAuthorized[] = AUTHORIZED_NUMBERS;
static const size_t kAuthorizedCount = sizeof(kAuthorized) / sizeof(kAuthorized[0]);
static const char *const kViewer[] = VIEWER_NUMBERS;
static const size_t kViewerCount = sizeof(kViewer) / sizeof(kViewer[0]);

// --- Rolling buffer: accumulates raw bytes from the MC60 across the whole
// run, consumed from the front as +CMT: URCs are parsed out of it. Capped at
// MC60_ROLLING_BUF_CAP, matching the original String's `if (length() > 1000)
// mc60Buffer = "";` behavior. ---
static char rolling_buf[MC60_ROLLING_BUF_CAP + 1];
static size_t rolling_len = 0;

// --- Pending SMS command queue (filled by mc60_pump(), drained by the
// caller via mc60_dequeue_sms()) ---
typedef struct {
    char sender[MC60_SMS_SENDER_BUF_SIZE];
    char text[MC60_SMS_TEXT_BUF_SIZE];
    mc60_role_t role;
} pending_sms_t;
static pending_sms_t sms_queue[SMS_QUEUE_CAPACITY];
static size_t sms_queue_head = 0;
static size_t sms_queue_count = 0;

// --- Small string helpers ---

static void str_trim_inplace(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start])) start++;
    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    size_t new_len = end - start;
    if (start > 0) memmove(s, s + start, new_len);
    s[new_len] = '\0';
}

static void last10_digits(const char *number, char out[11])
{
    char digits[32];
    size_t n = 0;
    for (const char *p = number; *p && n < sizeof(digits) - 1; p++) {
        if (isdigit((unsigned char)*p)) digits[n++] = *p;
    }
    digits[n] = '\0';
    const char *tail = (n > 10) ? digits + (n - 10) : digits;
    strlcpy(out, tail, 11);
}

size_t mc60_authorized_count(void)
{
    return kAuthorizedCount + kViewerCount;
}

mc60_role_t mc60_get_role(const char *number)
{
    char num_d[11];
    last10_digits(number, num_d);
    if (num_d[0] == '\0') return MC60_ROLE_NONE;

    for (size_t i = 0; i < kAuthorizedCount; i++) {
        char auth_d[11];
        last10_digits(kAuthorized[i], auth_d);
        if (strcmp(num_d, auth_d) == 0) return MC60_ROLE_ADMIN;
    }
    for (size_t i = 0; i < kViewerCount; i++) {
        char viewer_d[11];
        last10_digits(kViewer[i], viewer_d);
        if (strcmp(num_d, viewer_d) == 0) return MC60_ROLE_VIEWER;
    }
    return MC60_ROLE_NONE;
}

// --- SMS queue helpers ---

static bool mc60_enqueue_sms(const char *sender, const char *text, mc60_role_t role)
{
    if (sms_queue_count >= SMS_QUEUE_CAPACITY) {
        ESP_LOGW(TAG, "SMS queue full, dropping message.");
        return false;
    }
    size_t idx = (sms_queue_head + sms_queue_count) % SMS_QUEUE_CAPACITY;
    strlcpy(sms_queue[idx].sender, sender, sizeof(sms_queue[idx].sender));
    strlcpy(sms_queue[idx].text, text, sizeof(sms_queue[idx].text));
    sms_queue[idx].role = role;
    sms_queue_count++;
    return true;
}

bool mc60_dequeue_sms(char *out_sender, size_t sender_size, char *out_text, size_t text_size,
                      mc60_role_t *out_role)
{
    if (sms_queue_count == 0) return false;
    strlcpy(out_sender, sms_queue[sms_queue_head].sender, sender_size);
    strlcpy(out_text, sms_queue[sms_queue_head].text, text_size);
    if (out_role) *out_role = sms_queue[sms_queue_head].role;
    sms_queue_head = (sms_queue_head + 1) % SMS_QUEUE_CAPACITY;
    sms_queue_count--;
    return true;
}

// --- Rolling buffer management ---

static void rolling_buf_append(const uint8_t *data, size_t len)
{
    size_t space = MC60_ROLLING_BUF_CAP - rolling_len;
    size_t n = (len < space) ? len : space; // truncate rather than overflow
    if (n > 0) {
        memcpy(rolling_buf + rolling_len, data, n);
        rolling_len += n;
    }
    rolling_buf[rolling_len] = '\0';
    if (rolling_len >= MC60_ROLLING_BUF_CAP) { // mirrors `if (length() > 1000) mc60Buffer = "";`
        rolling_len = 0;
        rolling_buf[0] = '\0';
    }
}

static void rolling_buf_consume(size_t n)
{
    if (n > rolling_len) n = rolling_len;
    memmove(rolling_buf, rolling_buf + n, rolling_len - n);
    rolling_len -= n;
    rolling_buf[rolling_len] = '\0';
}

// Parses at most ONE +CMT: URC per call out of the rolling buffer, queueing
// it if the sender is authorized. Re-entrant-safe: called both from
// mc60_pump() (top of the main loop) and from inside mc60_send_command()'s
// wait loop, so it must stay side-effect-free beyond parsing+queueing --
// never call mc60_send_command()/mc60_send_sms() from here.
static void mc60_check_for_sms(void)
{
    char *cmt = strstr(rolling_buf, "+CMT:");
    if (!cmt) return;
    char *header_end = strchr(cmt, '\n');
    if (!header_end) return;
    char *text_end = strchr(header_end + 1, '\n');
    if (!text_end) return;

    char sender[MC60_SMS_SENDER_BUF_SIZE] = "";
    char *q1 = memchr(cmt, '"', (size_t)(header_end - cmt));
    if (q1) {
        char *q2 = memchr(q1 + 1, '"', (size_t)(header_end - (q1 + 1)));
        if (q2) {
            size_t n = (size_t)(q2 - (q1 + 1));
            if (n >= sizeof(sender)) n = sizeof(sender) - 1;
            memcpy(sender, q1 + 1, n);
            sender[n] = '\0';
        }
    }

    char text[MC60_SMS_TEXT_BUF_SIZE];
    size_t text_len = (size_t)(text_end - (header_end + 1));
    if (text_len >= sizeof(text)) text_len = sizeof(text) - 1;
    memcpy(text, header_end + 1, text_len);
    text[text_len] = '\0';
    str_trim_inplace(text);

    // Clear BEFORE acting, to avoid infinite reprocessing -- same ordering
    // as the original.
    size_t consumed = (size_t)(text_end - rolling_buf) + 1;
    rolling_buf_consume(consumed);

    ESP_LOGI(TAG, "*** DIRECT SMS RECEIVED *** From: %s Message: %s", sender, text);

    mc60_role_t role = mc60_get_role(sender);
    if (role == MC60_ROLE_NONE) {
        ESP_LOGI(TAG, "Sender NOT authorized. Ignoring.");
    } else {
        mc60_enqueue_sms(sender, text, role);
    }
}

// --- UART init ---

void mc60_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = MC60_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MC60_UART_NUM, MC60_UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MC60_UART_NUM, &uart_config));
    // uart_set_pin takes (tx, rx) -- NOT (rx, tx) like Arduino's
    // HardwareSerial::begin(). MC60_RX_PIN/MC60_TX_PIN name the ESP32's own
    // pins, so this order (tx first) is correct; swapping it compiles fine
    // and just silently never talks to the modem.
    ESP_ERROR_CHECK(uart_set_pin(MC60_UART_NUM, MC60_TX_PIN, MC60_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void mc60_pump(void)
{
    uint8_t chunk[64];
    int n;
    while ((n = uart_read_bytes(MC60_UART_NUM, chunk, sizeof(chunk), 0)) > 0) {
        rolling_buf_append(chunk, (size_t)n);
    }
    mc60_check_for_sms();
}

size_t mc60_send_command(const char *cmd, uint32_t timeout_ms, bool print,
                          char *out_resp, size_t out_resp_size)
{
    if (out_resp && out_resp_size > 0) out_resp[0] = '\0';
    size_t resp_len = 0;

    if (cmd[0] != '\0') {
        uart_write_bytes(MC60_UART_NUM, cmd, strlen(cmd));
        uart_write_bytes(MC60_UART_NUM, "\r\n", 2); // HardwareSerial::println() appends CRLF
        if (print) ESP_LOGI(TAG, ">> %s", cmd);
    }

    int64_t start_us = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    uint8_t chunk[64];

    // Deliberately waits the FULL timeout regardless of whether OK/ERROR has
    // already arrived -- preserved from the original, not a bug introduced
    // here. The 20ms per-iteration block is a real FreeRTOS yield (not a
    // busy-spin), which is what keeps this from starving the idle task/task
    // watchdog during long timeouts.
    while ((esp_timer_get_time() - start_us) < timeout_us) {
        int n = uart_read_bytes(MC60_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n > 0) {
            if (out_resp && out_resp_size > 0) {
                size_t space = out_resp_size - 1 - resp_len;
                size_t copy_n = ((size_t)n < space) ? (size_t)n : space;
                if (copy_n > 0) {
                    memcpy(out_resp + resp_len, chunk, copy_n);
                    resp_len += copy_n;
                    out_resp[resp_len] = '\0';
                }
            }
            rolling_buf_append(chunk, (size_t)n);
        }
    }

    if (print && resp_len > 0) ESP_LOGI(TAG, "<< %s", out_resp);

    // Only extracts+queues any SMS that arrived while we were waiting; it
    // does NOT dispatch/act on it, so this is safe to call while mid-transaction.
    mc60_check_for_sms();
    return resp_len;
}

void mc60_send_sms(const char *number, const char *text)
{
    ESP_LOGI(TAG, "Sending SMS to: %s", number);
    char cmd[MC60_AT_CMD_BUF_SIZE];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
    mc60_send_command(cmd, 2000, true, NULL, 0);
    uart_write_bytes(MC60_UART_NUM, text, strlen(text));
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t ctrl_z = 0x1A; // Ctrl+Z ends the SMS body and sends it
    uart_write_bytes(MC60_UART_NUM, &ctrl_z, 1);
    mc60_send_command("", 10000, true, NULL, 0); // wait for the network to send it
    ESP_LOGI(TAG, "SMS Sent.");
}

void mc60_send_gps_via_sms(const char *reply_to, bool have_last_fix, float last_lat, float last_lon)
{
    float lat, lon;
    char msg[128];
    if (mc60_get_gps_coordinates(&lat, &lon)) {
        snprintf(msg, sizeof(msg), "Location: https://maps.google.com/?q=%.6f,%.6f", lat, lon);
    } else if (have_last_fix) {
        snprintf(msg, sizeof(msg),
                 "No live fix. Last known (may be stale): https://maps.google.com/?q=%.6f,%.6f",
                 last_lat, last_lon);
    } else {
        strlcpy(msg, "No GPS fix available right now. Try again later.", sizeof(msg));
    }
    mc60_send_sms(reply_to, msg);
}

bool mc60_get_gps_coordinates(float *lat, float *lon)
{
    char resp[MC60_RESP_BUF_SIZE];
    mc60_send_command("AT+QGNSSRD=\"NMEA/GGA\"", 2000, false, resp, sizeof(resp));

    char *gga_start = strstr(resp, "$GNGGA");
    if (!gga_start) return false;
    char *line_end = strchr(gga_start, '\n');
    size_t line_len = line_end ? (size_t)(line_end - gga_start) : strlen(gga_start);
    if (line_len >= MC60_GGA_LINE_BUF_SIZE) line_len = MC60_GGA_LINE_BUF_SIZE - 1;

    char gga[MC60_GGA_LINE_BUF_SIZE];
    memcpy(gga, gga_start, line_len);
    gga[line_len] = '\0';

    // Manual comma split preserving empty fields -- strtok collapses
    // consecutive commas and shifts field indices on a weak fix. Fields:
    // 0=id,1=time,2=lat,3=NS,4=lon,5=EW,6=fixQuality,...
    char *fields[MC60_GGA_MAX_FIELDS];
    int count = 0;
    char *p = gga;
    fields[count++] = p;
    for (; *p && count < MC60_GGA_MAX_FIELDS; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[count++] = p + 1;
        }
    }

    if (count < 7 || fields[6][0] == '\0' || strcmp(fields[6], "0") == 0) return false; // no fix
    if (fields[2][0] == '\0' || fields[4][0] == '\0') return false;                     // lat/lon empty

    float lat_val = strtof(fields[2], NULL);
    *lat = floorf(lat_val / 100.0f) + fmodf(lat_val, 100.0f) / 60.0f;
    if (fields[3][0] == 'S') *lat = -*lat;

    float lon_val = strtof(fields[4], NULL);
    *lon = floorf(lon_val / 100.0f) + fmodf(lon_val, 100.0f) / 60.0f;
    if (fields[5][0] == 'W') *lon = -*lon;

    return true;
}

bool mc60_publish_to_adafruit_io(float lat, float lon)
{
    char feed_path[MC60_FEED_PATH_BUF_SIZE];
    snprintf(feed_path, sizeof(feed_path), "%s/feeds/%s", IO_USERNAME, FEED_NAME);

    char payload[MC60_JSON_PAYLOAD_BUF_SIZE];
    // "value" key required for Adafruit IO's map dashboard block to render it.
    snprintf(payload, sizeof(payload), "{\"value\":\"%.6f,%.6f\",\"lat\":%.6f,\"lon\":%.6f}",
              lat, lon, lat, lon);

    char cmd[MC60_AT_CMD_BUF_SIZE];
    snprintf(cmd, sizeof(cmd), "AT+QMTPUB=0,0,0,0,\"%s\"", feed_path);

    char resp[MC60_RESP_BUF_SIZE];
    mc60_send_command(cmd, 5000, true, resp, sizeof(resp));
    if (strstr(resp, ">") != NULL) {
        uart_write_bytes(MC60_UART_NUM, payload, strlen(payload));
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t ctrl_z = 0x1A;
        uart_write_bytes(MC60_UART_NUM, &ctrl_z, 1);
        mc60_send_command("", 10000, true, NULL, 0); // wait for final OK
        ESP_LOGI(TAG, "Published to Adafruit IO!");
        return true;
    }
    ESP_LOGW(TAG, "Failed MQTT publish.");
    return false;
}
