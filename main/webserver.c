/*
 * webserver.c
 *
 * The OTA upload handler follows webserver_ota.c of esp32-sdpsensor: the image
 * header is checked on the first chunk, before esp_ota_begin() erases the
 * other slot, so a wrong file costs one chunk and nothing else.
 */

#include "webserver.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mdns.h"

#include "ui.h"

#define SCRATCH_SIZE    4096
#define HEADER_SIZE     (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static const char *TAG = "web";
static volatile bool m_ota_active;


static esp_err_t update_page_handler(httpd_req_t *req) {
    extern const char update_html_start[] asm("_binary_update_html_start");
    extern const char update_html_end[] asm("_binary_update_html_end");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, update_html_start, update_html_end - update_html_start);
}


static esp_err_t info_handler(httpd_req_t *req) {
    const esp_app_desc_t *app = esp_app_get_description();
    char json[256];
    // the ELF SHA-256 tells builds apart: the version string and the build date often stay the same
    char elf[9];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    snprintf(json, sizeof(json),
             "{\"project\":\"%s\",\"version\":\"%s\",\"elf\":\"%s\",\"idf\":\"%s\",\"partition\":\"%s\"}",
             app->project_name, app->version, elf, app->idf_ver, esp_ota_get_running_partition()->label);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}


/**
 * Rejects anything but an app image for this chip.
 */
static const char* check_header(const char *data) {
    const esp_image_header_t *header = (const esp_image_header_t*) data;
    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "Not an ESP firmware image";
    }
    if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "Firmware is built for another chip";
    }
    const esp_app_desc_t *app = (const esp_app_desc_t*) (data + sizeof(esp_image_header_t)
                                                         + sizeof(esp_image_segment_header_t));
    if (strcmp(app->project_name, esp_app_get_description()->project_name) != 0) {
        return "Firmware is of another project";
    }
    ESP_LOGI(TAG, "New firmware version %s, running %s", app->version, esp_app_get_description()->version);
    return NULL;
}


static void restart_task(void *args) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}


static esp_err_t upload_firmware_handler(httpd_req_t *req) {
    static char scratch[SCRATCH_SIZE];  // one upload at a time: httpd runs handlers in one task
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    }
    if (req->content_len < HEADER_SIZE || req->content_len > partition->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wrong firmware size");
    }
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned) req->content_len, partition->label);
    m_ota_active = true;

    esp_ota_handle_t handle = 0;
    size_t received = 0;
    size_t header_len = 0;       // the first chunk is collected until the header is complete
    int shown = -1;
    esp_err_t err = ESP_OK;
    const char *error = NULL;
    while (received < req->content_len) {
        const size_t offset = (handle == 0) ? header_len : 0;
        const size_t want = (handle == 0) ? HEADER_SIZE - header_len : SCRATCH_SIZE;
        const int len = httpd_req_recv(req, scratch + offset, MIN(want, req->content_len - received));
        if (len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (len <= 0) {
            error = "File reception failed";
            break;
        }
        received += len;
        if (handle == 0) {
            header_len += len;
            if (header_len < HEADER_SIZE) {
                continue;
            }
            error = check_header(scratch);
            if (error != NULL) {
                break;
            }
            err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) {
                handle = 0;
                error = "esp_ota_begin failed";
                break;
            }
            err = esp_ota_write(handle, scratch, header_len);
        } else {
            err = esp_ota_write(handle, scratch, len);
        }
        if (err != ESP_OK) {
            error = "Flash write failed";
            break;
        }
        const int percent = (int) (100ULL * received / req->content_len);
        if (percent != shown) {
            ui_ota_progress(percent);
            shown = percent;
        }
    }

    if (error == NULL) {
        err = esp_ota_end(handle);
        handle = 0;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            error = "Firmware image is corrupted";
        } else if (err != ESP_OK) {
            error = "esp_ota_end failed";
        } else if (esp_ota_set_boot_partition(partition) != ESP_OK) {
            error = "Cannot set the boot partition";
        }
    }
    if (handle != 0) {
        esp_ota_abort(handle);
    }
    m_ota_active = false;
    if (error != NULL) {
        ESP_LOGW(TAG, "Firmware update failed: %s (%s)", error, esp_err_to_name(err));
        ui_ota_progress(-1);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, error);
    }

    ESP_LOGW(TAG, "Firmware updated, restarting");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "Firmware updated, restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}


bool webserver_ota_active(void) {
    return m_ota_active;
}


esp_err_t webserver_start(void) {
    static httpd_handle_t server = NULL;
    if (server != NULL) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.recv_wait_timeout = 10;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = update_page_handler },
        { .uri = "/info", .method = HTTP_GET, .handler = info_handler },
        { .uri = "/upload_firmware", .method = HTTP_POST, .handler = upload_firmware_handler },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "Firmware update page on port %d", config.server_port);

    // reachable as http://espbms.local/ on the local network
    err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(WEBSERVER_HOSTNAME);
        mdns_instance_name_set("JK BMS monitor");
        mdns_service_add(NULL, "_http", "_tcp", config.server_port, NULL, 0);
        ESP_LOGI(TAG, "mDNS name %s.local", WEBSERVER_HOSTNAME);
    } else {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
