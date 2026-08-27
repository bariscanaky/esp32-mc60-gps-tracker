#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "credentials.h" // IO_USERNAME, IO_KEY, FEED_NAME, SIM_APN, AUTHORIZED_NUMBERS
#include "mc60.h"
#include "storage.h"

static const char *TAG = "mc60_tracker";

// --- State Machine ---
typedef enum {
    STATE_POWER_ON,
    STATE_WAIT_FOR_BOOT,
    STATE_CHECK_SIM,
    STATE_INIT_SMS_ROUTING,
    STATE_CHECK_NETWORK,
    STATE_ATTACH_GPRS,
    STATE_SET_APN,
    STATE_MQTT_OPEN,
    STATE_MQTT_CONNECT,
    STATE_ENABLE_GPS,
    STATE_WAIT_GPS_FIX,
    STATE_RUNNING,
    STATE_FATAL_ERROR
} module_state_t;

static module_state_t current_state = STATE_POWER_ON;

// --- Timers, Flags & Retry Counters ---
static uint32_t state_timer = 0;
static uint32_t last_upload_time = 0;
static int retry_counter = 0;      // resets on every enter_state() call; only valid for "stay and retry" loops
static bool gps_enabled = false;
static bool pwrkey_pulse_active = false;
static int boot_retry_counter = 0; // survives POWER_ON<->WAIT_FOR_BOOT cycling, unlike retry_counter
static int consecutive_publish_failures = 0;

// Runtime-adjustable upload interval (SMS command SETINTERVAL). Not
// persisted across reboots -- resets to the config.h default on restart.
static uint32_t upload_interval_ms = UPLOAD_INTERVAL_MS;

// --- Central state transition helper ---
// Resets everything that must never leak from one state into the next.
// Do NOT call this for "stay in the same state and retry" branches -- that
// would wipe retry_counter and defeat the retry cap.
static void enter_state(module_state_t new_state)
{
    current_state = new_state;
    retry_counter = 0;
    state_timer = millis();
}

static void pwrkey_gpio_init(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << MC60_PWRKEY_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(MC60_PWRKEY_PIN, 0);
}

static void power_off_module(void)
{
    gpio_set_level(MC60_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1200));
    gpio_set_level(MC60_PWRKEY_PIN, 0);
    ESP_LOGI(TAG, "Module powered off.");
    gps_enabled = false;
    enter_state(STATE_POWER_ON);
}

// --- SMS command dispatch helpers ---

static void str_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static bool str_ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + (ls - lf), suffix) == 0;
}

// Parses "SETINTERVAL <seconds>" (any amount of whitespace/'=' between the
// keyword and the number, and tolerant of a prefix before the keyword, same
// as the other commands' suffix matching). Returns false if the keyword
// isn't present or no digits follow it.
static bool parse_set_interval(const char *cmd, long *out_seconds)
{
    const char *p = strstr(cmd, "SETINTERVAL");
    if (!p) return false;
    p += strlen("SETINTERVAL");
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return false;
    *out_seconds = strtol(p, NULL, 10);
    return true;
}

// Only ever called from the top level of the main loop, never re-entrantly
// from inside mc60_send_command()'s wait loop -- so it's safe to hit the AT
// engine here.
static void process_sms_command(const char *sender, const char *text, mc60_role_t role)
{
    char cmd[MC60_SMS_TEXT_BUF_SIZE];
    strlcpy(cmd, text, sizeof(cmd));
    str_upper_inplace(cmd);

    long set_interval_seconds;

    if (str_ends_with(cmd, "PWROFF")) {
        if (role != MC60_ROLE_ADMIN) {
            mc60_send_sms(sender, "Not authorized for this command (admin only).");
            return;
        }
        ESP_LOGI(TAG, "Action: Powering Off.");
        mc60_send_sms(sender, "Powering off.");
        power_off_module();
    } else if (str_ends_with(cmd, "LOC") || str_ends_with(cmd, "GPS") || str_ends_with(cmd, "GETGPS")) {
        ESP_LOGI(TAG, "Action: Fetching GPS and replying...");
        float last_lat, last_lon;
        bool have_last_fix = storage_load_last_fix(&last_lat, &last_lon);
        mc60_send_gps_via_sms(sender, have_last_fix, last_lat, last_lon);
    } else if (str_ends_with(cmd, "STATUS")) {
        ESP_LOGI(TAG, "Action: Sending status...");
        char reset_summary[64];
        storage_format_reset_summary(reset_summary, sizeof(reset_summary));
        char msg[160];
        snprintf(msg, sizeof(msg), "State=%d Uptime=%lus Interval=%lus Resets[%s]",
                 (int)current_state, (unsigned long)(millis() / 1000),
                 (unsigned long)(upload_interval_ms / 1000), reset_summary);
        mc60_send_sms(sender, msg);
    } else if (parse_set_interval(cmd, &set_interval_seconds)) {
        if (role != MC60_ROLE_ADMIN) {
            mc60_send_sms(sender, "Not authorized for this command (admin only).");
            return;
        }
        // Bounds-check in the seconds domain, before the *1000 multiply --
        // strtol() can return a value large enough to overflow uint32_t
        // milliseconds and wrap around into the valid range.
        long min_s = UPLOAD_INTERVAL_MIN_MS / 1000;
        long max_s = UPLOAD_INTERVAL_MAX_MS / 1000;
        if (set_interval_seconds < min_s || set_interval_seconds > max_s) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Interval must be %ld-%lds.", min_s, max_s);
            mc60_send_sms(sender, msg);
        } else {
            upload_interval_ms = (uint32_t)set_interval_seconds * 1000;
            char msg[64];
            snprintf(msg, sizeof(msg), "Upload interval set to %lds.", set_interval_seconds);
            mc60_send_sms(sender, msg);
        }
    } else if (str_ends_with(cmd, "HELP")) {
        if (role == MC60_ROLE_ADMIN) {
            mc60_send_sms(sender, "Commands: LOC, STATUS, HELP, SETINTERVAL <sec>, PWROFF");
        } else {
            mc60_send_sms(sender, "Commands: LOC, STATUS, HELP");
        }
    } else {
        mc60_send_sms(sender, "Unknown command. Text HELP for a list.");
    }
}

static void loop_once(void)
{
    uint32_t now = millis();

    mc60_pump();

    switch (current_state) {
        case STATE_POWER_ON:
            // Non-blocking PWRKEY pulse: rising edge this tick, falling edge
            // once PWRKEY_PULSE_MS has elapsed, instead of delaying inside
            // the handler.
            if (!pwrkey_pulse_active) {
                ESP_LOGI(TAG, "State: POWER_ON");
                gpio_set_level(MC60_PWRKEY_PIN, 1);
                pwrkey_pulse_active = true;
                state_timer = now;
            } else if (now - state_timer >= PWRKEY_PULSE_MS) {
                gpio_set_level(MC60_PWRKEY_PIN, 0);
                pwrkey_pulse_active = false;
                enter_state(STATE_WAIT_FOR_BOOT);
            }
            break;

        case STATE_WAIT_FOR_BOOT:
            if (now - state_timer > 3000) {
                char resp[MC60_RESP_BUF_SIZE];
                mc60_send_command("AT", 1000, true, resp, sizeof(resp));
                if (strstr(resp, "OK") != NULL) {
                    ESP_LOGI(TAG, "Module responsive.");
                    boot_retry_counter = 0;
                    enter_state(STATE_CHECK_SIM);
                } else {
                    boot_retry_counter++;
                    if (boot_retry_counter >= MAX_RETRIES) {
                        ESP_LOGW(TAG, "Module unresponsive after repeated power cycles.");
                        enter_state(STATE_FATAL_ERROR);
                    } else {
                        ESP_LOGI(TAG, "No response, retry power-on...");
                        enter_state(STATE_POWER_ON);
                    }
                }
            }
            break;

        case STATE_CHECK_SIM: {
            char resp[MC60_RESP_BUF_SIZE];
            mc60_send_command("AT+CPIN?", 3000, true, resp, sizeof(resp));
            if (strstr(resp, "+CPIN: READY") != NULL) {
                ESP_LOGI(TAG, "SIM ready.");
                enter_state(STATE_INIT_SMS_ROUTING);
            } else {
                ESP_LOGI(TAG, "Waiting for SIM...");
                retry_counter++;
                state_timer = now;
                if (retry_counter >= MAX_RETRIES) enter_state(STATE_FATAL_ERROR);
            }
            break;
        }

        case STATE_INIT_SMS_ROUTING:
            mc60_send_command("AT+CMGF=1", 2000, true, NULL, 0);          // text mode
            mc60_send_command("AT+CNMI=2,2,0,0,0", 2000, true, NULL, 0);  // don't save SMS to SIM
            ESP_LOGI(TAG, "Direct SMS Routing Enabled. Messages will not be saved on SIM.");
            enter_state(STATE_CHECK_NETWORK);
            break;

        case STATE_CHECK_NETWORK: {
            char resp[MC60_RESP_BUF_SIZE];
            mc60_send_command("AT+CREG?", 2000, true, resp, sizeof(resp));
            if (strstr(resp, ",1") != NULL || strstr(resp, ",5") != NULL) {
                ESP_LOGI(TAG, "Network registered.");
                enter_state(STATE_ATTACH_GPRS);
            } else {
                ESP_LOGI(TAG, "Waiting for network...");
                retry_counter++;
                state_timer = now;
                if (retry_counter >= MAX_RETRIES * 2) enter_state(STATE_FATAL_ERROR);
            }
            break;
        }

        case STATE_ATTACH_GPRS: {
            mc60_send_command("AT+CGATT=1", 3000, true, NULL, 0);
            char resp[MC60_RESP_BUF_SIZE];
            mc60_send_command("AT+CGATT?", 2000, true, resp, sizeof(resp));
            if (strstr(resp, "+CGATT: 1") != NULL) {
                ESP_LOGI(TAG, "GPRS attached.");
                enter_state(STATE_SET_APN);
            } else {
                ESP_LOGI(TAG, "Waiting for GPRS attachment...");
                retry_counter++;
                state_timer = now;
                if (retry_counter >= MAX_RETRIES) enter_state(STATE_FATAL_ERROR);
            }
            break;
        }

        case STATE_SET_APN: {
            char cmd[MC60_AT_CMD_BUF_SIZE];
            snprintf(cmd, sizeof(cmd), "AT+QIREGAPP=\"%s\",\"\",\"\"", SIM_APN);
            char resp[MC60_RESP_BUF_SIZE];
            mc60_send_command(cmd, 5000, true, resp, sizeof(resp));
            if (strstr(resp, "OK") != NULL) {
                ESP_LOGI(TAG, "APN set.");
                enter_state(STATE_MQTT_OPEN);
            } else {
                ESP_LOGI(TAG, "Failed to set APN.");
                retry_counter++;
                state_timer = now;
                if (retry_counter >= MAX_RETRIES) enter_state(STATE_FATAL_ERROR);
            }
            break;
        }

        case STATE_MQTT_OPEN: {
            char cmd[MC60_AT_CMD_BUF_SIZE];
            snprintf(cmd, sizeof(cmd), "AT+QMTOPEN=0,\"%s\",%d", ADAFRUIT_SERVER, ADAFRUIT_PORT);
            char resp[MC60_RESP_BUF_SIZE];
            mc60_send_command(cmd, 10000, true, resp, sizeof(resp));
            if (strstr(resp, "+QMTOPEN: 0,0") != NULL) {
                ESP_LOGI(TAG, "MQTT socket opened.");
                enter_state(STATE_MQTT_CONNECT);
            } else {
                ESP_LOGI(TAG, "MQTT open failed, retrying...");
                mc60_send_command("AT+QMTCLOSE=0", 5000, true, NULL, 0);
                retry_counter++;
                state_timer = now;
                if (retry_counter >= MAX_RETRIES) enter_state(STATE_FATAL_ERROR);
            }
            break;
        }

        case STATE_MQTT_CONNECT: {
            char cmd[MC60_AT_CMD_BUF_SIZE];
            snprintf(cmd, sizeof(cmd), "AT+QMTCONN=0,\"gps-tracker\",\"%s\",\"%s\"", IO_USERNAME, IO_KEY);
            char resp[MC60_RESP_BUF_SIZE];
            mc60_send_command(cmd, 10000, true, resp, sizeof(resp));
            if (strstr(resp, "+QMTCONN: 0,0,0") != NULL) {
                ESP_LOGI(TAG, "MQTT connected.");
                consecutive_publish_failures = 0;
                enter_state(STATE_ENABLE_GPS);
            } else {
                ESP_LOGI(TAG, "MQTT connect failed.");
                mc60_send_command("AT+QMTCLOSE=0", 5000, true, NULL, 0);
                retry_counter++;
                state_timer = now;
                if (retry_counter >= MAX_RETRIES) enter_state(STATE_FATAL_ERROR);
            }
            break;
        }

        case STATE_ENABLE_GPS:
            mc60_send_command("AT+QGNSSC=1", 1000, true, NULL, 0);
            gps_enabled = true;
            ESP_LOGI(TAG, "GPS enabled.");
            enter_state(STATE_WAIT_GPS_FIX);
            break;

        case STATE_WAIT_GPS_FIX: {
            float lat, lon;
            if (mc60_get_gps_coordinates(&lat, &lon)) {
                ESP_LOGI(TAG, "Valid GPS fix acquired.");
                storage_save_last_fix(lat, lon);
                last_upload_time = now;
                enter_state(STATE_RUNNING);
            } else if (now - state_timer > GPS_FIX_TIMEOUT_MS) {
                ESP_LOGI(TAG, "GPS fix timeout, continuing anyway.");
                last_upload_time = now;
                enter_state(STATE_RUNNING);
            } else {
                ESP_LOGI(TAG, "Waiting for valid GPS fix...");
            }
            break;
        }

        case STATE_RUNNING:
            // Only upload to Adafruit IO at the configured interval.
            if (gps_enabled && now - last_upload_time >= upload_interval_ms) {
                float lat, lon;
                if (mc60_get_gps_coordinates(&lat, &lon)) {
                    storage_save_last_fix(lat, lon);
                    if (mc60_publish_to_adafruit_io(lat, lon)) {
                        consecutive_publish_failures = 0;
                    } else {
                        consecutive_publish_failures++;
                    }
                }
                last_upload_time = now;

                if (consecutive_publish_failures >= MAX_PUBLISH_FAILURES) {
                    ESP_LOGW(TAG, "Too many failed publishes, reconnecting MQTT...");
                    mc60_send_command("AT+QMTCLOSE=0", 5000, true, NULL, 0);
                    enter_state(STATE_MQTT_OPEN);
                }
            }
            break;

        case STATE_FATAL_ERROR: {
            static uint32_t last_fatal_print = 0;
            if (now - last_fatal_print > 5000) {
                uint32_t remaining = (now - state_timer < FATAL_ERROR_RESTART_MS)
                                          ? (FATAL_ERROR_RESTART_MS - (now - state_timer)) / 1000
                                          : 0;
                ESP_LOGW(TAG, "FATAL ERROR! Restarting in %lus", (unsigned long)remaining);
                last_fatal_print = now;
            }
            if (now - state_timer > FATAL_ERROR_RESTART_MS) {
                ESP_LOGW(TAG, "Restarting ESP32 to recover...");
                esp_restart();
            }
            break;
        }
    }

    // Process at most one queued SMS command per loop iteration. This runs
    // only here, never from inside mc60_send_command()'s wait loop, so it
    // can never re-enter the AT engine mid-transaction.
    char sender[MC60_SMS_SENDER_BUF_SIZE];
    char text[MC60_SMS_TEXT_BUF_SIZE];
    mc60_role_t role;
    if (mc60_dequeue_sms(sender, sizeof(sender), text, sizeof(text), &role)) {
        process_sms_command(sender, text, role);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "--- MC60 Direct SMS & GPS Tracker ---");

    if (strcmp(IO_USERNAME, "your_adafruit_username") == 0) {
        ESP_LOGE(TAG, "Set your credentials in credentials.h");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    storage_init();
    mc60_init();
    pwrkey_gpio_init();

    state_timer = millis();
    ESP_LOGI(TAG, "Authorized senders: %u", (unsigned)mc60_authorized_count());

    while (1) {
        loop_once();
        // ESP-IDF's task watchdog (default 5s) watches the idle tasks, not
        // app_main -- without a yield here, STATE_RUNNING's non-blocking
        // mc60_pump() would spin for up to UPLOAD_INTERVAL_MS (10s) without
        // ever letting the idle task run, tripping the watchdog and
        // rebooting roughly every upload cycle. Arduino's loopTask wrapper
        // handled this invisibly; a raw app_main() loop must do it itself.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
