/*
 * main.c
 *
 * JK BMS monitor for the LILYGO TTGO T-Display (ESP32 + ST7789V 135x240).
 * Connects to JK BMS units over BLE and shows the pack summary and
 * per-cell voltage and resistance. Left/right buttons switch pages.
 * Always on: the status is uploaded over Wi-Fi every minute, and the
 * firmware can be updated from the board's web page or by the dashboard.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "buttons.h"
#include "console.h"
#include "jkbms_ble.h"
#include "lcd.h"
#include "remote.h"
#include "report.h"
#include "ui.h"
#include "uploader.h"
#include "webserver.h"

#define LOOP_MS          500

#define FIRST_WIFI_WAIT_US    (30 * 1000000LL)  // several networks are tried one by one
#define PART_RETRY_US         (30 * 1000000LL)  // a failed settings or logbook report is sent again
#define READ_SHOW_US          (5 * 1000000LL)   // "read" shows this long before sending starts
#define FALLBACK_PERIOD_US    ((CONFIG_UPLOAD_PERIOD_S + 60) * 1000000LL)

static const char *TAG = "main";


static void nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}


/**
 * A new firmware counts as working once the dashboard accepted a report: it
 * can then be updated again from there. If it resets before that, the
 * bootloader rolls back.
 */
static void mark_app_valid(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK
            && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New firmware reached the dashboard: marking it valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}


/**
 * True once per new stream of any BMS, i.e. after every (re)connection.
 */
static bool stream_started(void) {
    static jkbms_link_t links[JKBMS_DEVICES_MAX];
    static jkbms_status_t st;  // large: keep it off the stack
    bool started = false;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_ble_get_status(i, &st);
        started |= st.link == JKBMS_LINK_STREAMING && links[i] != JKBMS_LINK_STREAMING;
        links[i] = st.link;
    }
    return started;
}


/**
 * True when the set of chosen BMS differs from the last call (and on the
 * first call): a newly chosen BMS gets a full first report again.
 */
static bool selection_changed(void) {
    static uint8_t macs[JKBMS_DEVICES_MAX][6];
    static bool first_call = true;
    static jkbms_status_t st;
    bool changed = first_call;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        uint8_t mac[6] = { 0 };
        if (jkbms_ble_get_status(i, &st) && st.link != JKBMS_LINK_UNASSIGNED) {
            memcpy(mac, st.mac, 6);
        }
        changed |= memcmp(mac, macs[i], 6) != 0;
        memcpy(macs[i], mac, 6);
    }
    first_call = false;
    return changed;
}


/**
 * Builds and POSTs the status report.
 * Returns ESP_ERR_NOT_FINISHED when skipped during a firmware update.
 */
static esp_err_t post_status(const char *json) {
    // the dashboard's reply carries no commands: it is not read
    const esp_err_t err = uploader_send(json, NULL, 0);
    if (err == ESP_OK) {
        mark_app_valid();
    }
    return err;
}


static uint32_t m_uploads;


static esp_err_t upload(void) {
    if (webserver_ota_active()) {
        ESP_LOGI(TAG, "Firmware update in progress, upload skipped");
        return ESP_ERR_NOT_FINISHED;
    }
    char *json = report_build(++m_uploads);
    if (json == NULL) {
        ESP_LOGW(TAG, "No memory for the report: %u bytes free, largest block %u",
                 (unsigned) esp_get_free_heap_size(), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Upload #%lu, %u bytes free", (unsigned long) m_uploads, (unsigned) esp_get_free_heap_size());
    const esp_err_t err = post_status(json);
    free(json);
    return err;
}


/**
 * POSTs a partial report (settings or logbook events) and frees it.
 */
static esp_err_t upload_part(char *json) {
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (webserver_ota_active()) {
        free(json);
        return ESP_ERR_NOT_FINISHED;
    }
    const esp_err_t err = uploader_send(json, NULL, 0);
    free(json);
    return err;
}


/*
 * After a BMS is chosen (or at power-on) its data goes out step by step,
 * each report small enough for the little memory there is: the status, the
 * settings, the logbook 10 events at a time. "Sent" shows once all arrived.
 * Then the status follows with every new stream, and the new logbook events
 * every CONFIG_UPLOAD_EXTRAS_EVERY uploads; the settings are not sent again.
 *
 * With CONFIG_UPLOAD_COLLECT_FIRST everything is read before anything is
 * sent: "Read" shows for 5 s, then the same steps follow once there is Wi-Fi,
 * with the status as it was read, so that the board can be carried from the
 * BMS to the router.
 */
typedef enum {
    STAGE_COLLECT,          // reading the status, the settings and the logbook
    STAGE_READ,             // "Read" shows
    STAGE_READ_STATUS,      // sending the status that was read
    STAGE_STATUS,
    STAGE_SETTINGS,
    STAGE_LOGBOOK,
    STAGE_SENT,
} stage_t;

#if CONFIG_UPLOAD_COLLECT_FIRST
#define STAGE_FIRST     STAGE_COLLECT
#else
#define STAGE_FIRST     STAGE_STATUS
#endif

static stage_t m_stage = STAGE_FIRST;
static char *m_read_status;             // the status report as read, sent later
static int64_t m_read_us;
static jkbms_extras_t *m_extras;        // the settings and the logbook being read: only while needed
static int m_extras_slot = -1;
static bool m_failed;                   // the red cross is shown


static void extras_free(void) {
    if (m_extras != NULL) {
        jkbms_ble_cancel_extras(m_extras);
        free(m_extras);
        m_extras = NULL;
    }
    m_extras_slot = -1;
}


/**
 * Starts reading the settings and/or the logbook of the first streaming BMS.
 */
static void extras_request(bool settings, bool logbook) {
    static jkbms_status_t st;
    for (int i = 0; i < JKBMS_DEVICES_MAX && m_extras == NULL; i++) {
        if (!jkbms_ble_get_status(i, &st) || st.link != JKBMS_LINK_STREAMING) {
            continue;
        }
        m_extras = calloc(1, sizeof(*m_extras));
        if (m_extras == NULL) {
            return;
        }
        m_extras->want_settings = settings;
        m_extras->want_logbook = logbook;
        m_extras_slot = i;
        if (!jkbms_ble_fetch_extras(i, m_extras)) {
            extras_free();
        }
    }
}


static void show_result(bool ok) {
    if (ok || !m_failed) {
        // a green check mark once, or a red cross until the whole first report is delivered
        ui_show_upload_result(ok);
    }
    m_failed = !ok;
}


// the header progress: a third for the status, a third for the settings, a third for the logbook,
// sent (or, collecting first, read from the BMS)
#define PROGRESS_SETTINGS   333
#define PROGRESS_LOGBOOK    667
#define PROGRESS_FULL       1000
#define LOGBOOK_CHUNK       10

static int m_logbook_chunks;            // logbook reports of this round: for the progress


static void show_progress(void) {
    int permille = -1;
#if CONFIG_UPLOAD_COLLECT_FIRST
    // how much was read from the BMS: the status (its stream runs), the settings, the logbook
    if (m_stage == STAGE_COLLECT) {
        permille = (m_extras != NULL ? PROGRESS_SETTINGS : 0)
                 + (m_extras != NULL && m_extras->settings_valid ? PROGRESS_LOGBOOK - PROGRESS_SETTINGS : 0)
                 + (m_extras != NULL && m_extras->logbook_valid ? PROGRESS_FULL - PROGRESS_LOGBOOK : 0);
    }
    ui_set_send_progress(permille);
    return;
#endif
    switch (m_stage) {
    case STAGE_READ_STATUS:
    case STAGE_STATUS:
        permille = 0;
        break;
    case STAGE_SETTINGS:
        permille = PROGRESS_SETTINGS;
        break;
    case STAGE_LOGBOOK: {
        // the last third fills with every logbook report
        const int left = (m_extras != NULL && m_extras->logbook_valid)
                ? (report_logbook_unsent(m_extras_slot, m_extras) + LOGBOOK_CHUNK - 1) / LOGBOOK_CHUNK : 1;
        const int total = MAX(m_logbook_chunks, left);
        permille = PROGRESS_LOGBOOK + (total > 0 ? (PROGRESS_FULL - PROGRESS_LOGBOOK) * (total - left) / total : 0);
        break;
    }
    default:
        break;
    }
    ui_set_send_progress(permille);
}


/**
 * Sends the logbook events not delivered yet. True when all are delivered.
 */
static bool send_logbook(void) {
    m_logbook_chunks = (report_logbook_unsent(m_extras_slot, m_extras) + LOGBOOK_CHUNK - 1) / LOGBOOK_CHUNK;
    while (1) {
        show_progress();
        char *json = report_build_logbook(m_extras_slot, m_extras);
        if (json == NULL) {
            return true;  // nothing new left (or no memory: the next round tries again)
        }
        if (upload_part(json) != ESP_OK) {
            return false;
        }
        report_logbook_sent(m_extras_slot);
    }
}


/**
 * Moves the settings and logbook steps on; called from the main loop.
 */
static void stages_run(int64_t now) {
    static int64_t next_try_us;
    if (m_stage == STAGE_COLLECT) {
        if (m_extras == NULL) {
            extras_request(true, true);
        } else if (m_extras->settings_valid && m_extras->logbook_valid) {
            // the status of this moment goes with the settings and the logbook
            m_read_status = report_build(++m_uploads);
            if (m_read_status != NULL) {
                ESP_LOGI(TAG, "Everything read: %u logbook events", m_extras->logbook_count);
                m_read_us = now;
                ui_show_read_result();
                m_stage = STAGE_READ;
            }
        }
        return;
    }
    if (m_stage == STAGE_READ && now - m_read_us >= READ_SHOW_US) {
        m_stage = STAGE_READ_STATUS;
    }
    if (now < next_try_us || !uploader_connected()) {
        return;
    }
    switch (m_stage) {
    case STAGE_COLLECT:
    case STAGE_READ:
    case STAGE_STATUS:
        break;
    case STAGE_READ_STATUS: {
        char *json = report_add_age(m_read_status, (uint32_t) ((now - m_read_us) / 1000000));
        const esp_err_t err = (json != NULL) ? post_status(json) : ESP_ERR_NO_MEM;
        free(json);
        if (err == ESP_OK) {
            free(m_read_status);
            m_read_status = NULL;
            m_stage = STAGE_SETTINGS;   // the settings and the logbook were read already
        } else {
            show_result(false);
            next_try_us = now + PART_RETRY_US;
        }
        break;
    }
    case STAGE_SETTINGS:
        if (m_extras == NULL) {
            // both in the same connection: fewer beeps
            extras_request(true, true);
        } else if (m_extras->settings_valid) {
            ESP_LOGI(TAG, "Sending the settings, %u bytes free", (unsigned) esp_get_free_heap_size());
            if (upload_part(report_build_settings(m_extras_slot, &m_extras->settings)) == ESP_OK) {
                m_stage = STAGE_LOGBOOK;
            } else {
                show_result(false);
                next_try_us = now + PART_RETRY_US;
            }
        }
        break;
    case STAGE_LOGBOOK:
        if (m_extras == NULL) {
            extras_request(false, true);
        } else if (m_extras->logbook_valid) {
            ESP_LOGI(TAG, "Sending the logbook, %u bytes free", (unsigned) esp_get_free_heap_size());
            if (send_logbook()) {
                extras_free();
                m_stage = STAGE_SENT;
                show_result(true);
            } else {
                show_result(false);
                next_try_us = now + PART_RETRY_US;
            }
        }
        break;
    case STAGE_SENT:
        // new logbook events, asked for every CONFIG_UPLOAD_EXTRAS_EVERY uploads
        if (m_extras != NULL && m_extras->logbook_valid) {
            if (send_logbook()) {
                extras_free();
            } else {
                next_try_us = now + PART_RETRY_US;
            }
        }
        break;
    }
}


void app_main(void) {
    nvs_init();
    remote_boot_check();
    lv_display_t *disp = lcd_init();
    // reconnect for fresh data so that a new stream starts about once per upload period;
    // the BMS streams for about 30 s and beeps on every connection
    jkbms_ble_set_refresh_period_s(CONFIG_UPLOAD_PERIOD_S > 30 ? CONFIG_UPLOAD_PERIOD_S - 30 : CONFIG_UPLOAD_PERIOD_S);
    ESP_ERROR_CHECK(jkbms_ble_init());
    ui_init(disp);
    ui_refresh();
    lcd_backlight(true);

    buttons_init(ui_on_button);
    console_init();
    if (uploader_start(NULL) == ESP_OK) {
        webserver_start();
    }
    ESP_LOGI(TAG, "Monitoring %d JK BMS", JKBMS_DEVICES_MAX);

    int64_t last_upload_us = 0;
    int64_t due_us = -1;            // a new stream started: upload
    bool retry_on_wifi = false;     // the first report failed without Wi-Fi: send once connected
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
        if (CONFIG_UPLOAD_PERIOD_S == 0) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (selection_changed()) {
            m_stage = STAGE_FIRST;
            retry_on_wifi = false;
            extras_free();
            free(m_read_status);
            m_read_status = NULL;
            report_reset();
        }
        if (m_stage < STAGE_STATUS) {
            // collect first: nothing goes out until everything was read
            stages_run(now);
            show_progress();
            continue;
        }
        if (stream_started() && due_us < 0) {
            due_us = now;
        }
        if (retry_on_wifi && m_stage == STAGE_STATUS && due_us < 0 && uploader_connected()) {
            due_us = now;
        }
        if (due_us >= 0) {
            // the first report waits for Wi-Fi, which may still be trying the networks
            if (uploader_connected() || now - due_us > FIRST_WIFI_WAIT_US) {
                const esp_err_t err = upload();
                if (m_stage == STAGE_STATUS && err != ESP_ERR_NOT_FINISHED) {
                    if (err == ESP_OK) {
                        m_stage = STAGE_SETTINGS;
                    } else {
                        show_result(false);
                    }
                    // Wi-Fi was down or dropped during the upload: resend once it is back
                    retry_on_wifi = err != ESP_OK && (err == ESP_ERR_INVALID_STATE || !uploader_connected());
                } else if (m_stage == STAGE_SENT && err == ESP_OK && m_extras == NULL
                           && m_uploads % CONFIG_UPLOAD_EXTRAS_EVERY == 0) {
                    extras_request(false, true);
                }
                last_upload_us = now;
                due_us = -1;
            }
        } else if (now - last_upload_us >= FALLBACK_PERIOD_US) {
            // no BMS streams: still report, so the dashboard shows it offline
            upload();
            last_upload_us = now;
        }
        stages_run(now);
        show_progress();
    }
}
