/*
 * report.c
 */

#include "report.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "jkbms_ble.h"

// the BMS is read once per upload period: data of the last read counts as online
#define FRESH_US   ((CONFIG_UPLOAD_PERIOD_S + 30) * 1000000LL)


// floats would otherwise be printed as 3.3340001106262207
static double round3(double value) {
    return round(value * 1000) / 1000;
}


static void add_number(cJSON *object, const char *name, double value) {
    cJSON_AddNumberToObject(object, name, round3(value));
}


static const char* error_name(jkbms_error_t error) {
    switch (error) {
    case JKBMS_ERROR_NOT_FOUND:
        return "not found";
    case JKBMS_ERROR_CONNECT:
        return "connection failed";
    case JKBMS_ERROR_NOT_JK:
        return "not a JK BMS";
    case JKBMS_ERROR_NO_DATA:
        return "not responding";
    default:
        return "no data";
    }
}


static void add_cells(cJSON *bms, const jkbms_cell_info_t *ci) {
    cJSON *cells = cJSON_AddArrayToObject(bms, "cells");
    cJSON *resistances = cJSON_AddArrayToObject(bms, "resistances");
    for (int i = 0; i < ci->cell_count; i++) {
        cJSON_AddItemToArray(cells, cJSON_CreateNumber(round3(ci->cell_voltage[i])));
        cJSON_AddItemToArray(resistances, cJSON_CreateNumber(round3(ci->cell_resistance[i])));
    }
    add_number(bms, "wire_warning", ci->wire_resistance_warning);
    add_number(bms, "voltage", ci->total_voltage);
    add_number(bms, "current", ci->current);
    add_number(bms, "power", ci->power);
    add_number(bms, "soc", ci->soc);
    add_number(bms, "diff", ci->delta_cell_voltage);
    cJSON_AddBoolToObject(bms, "charging", ci->charging);
    cJSON_AddBoolToObject(bms, "discharging", ci->discharging);
    cJSON_AddBoolToObject(bms, "balancing", ci->balancer_action == JKBMS_BALANCER_CHARGE
                                            || ci->balancer_action == JKBMS_BALANCER_DISCHARGE);
    add_number(bms, "bal_current", ci->balancing_current);
    add_number(bms, "t1", ci->temperature[0]);
    add_number(bms, "t2", ci->temperature[1]);
    add_number(bms, "t_mos", ci->mosfet_temperature);
    add_number(bms, "cycles", ci->cycles);
    add_number(bms, "capacity_remaining", ci->capacity_remaining);
    add_number(bms, "capacity_nominal", ci->capacity_nominal);
    add_number(bms, "errors", ci->errors);
    cJSON *labels = cJSON_AddArrayToObject(bms, "error_labels");
    for (uint8_t bit = 0; bit < 32; bit++) {
        const char *label = jkbms_error_label(bit);
        if ((ci->errors & (1UL << bit)) && label != NULL) {
            cJSON_AddItemToArray(labels, cJSON_CreateString(label));
        }
    }
}


static void add_settings(cJSON *bms, const jkbms_settings_t *s) {
    cJSON *o = cJSON_AddObjectToObject(bms, "settings");
    cJSON_AddBoolToObject(o, "charge_enabled", s->charge_enabled);
    cJSON_AddBoolToObject(o, "discharge_enabled", s->discharge_enabled);
    cJSON_AddBoolToObject(o, "balancer_enabled", s->balancer_enabled);
    add_number(o, "cell_count", s->cell_count);
    add_number(o, "capacity", s->capacity);
    add_number(o, "cell_uvp", s->cell_uvp);
    add_number(o, "cell_uvp_recovery", s->cell_uvp_recovery);
    add_number(o, "cell_soc0", s->cell_soc0);
    add_number(o, "power_off", s->power_off);
    add_number(o, "cell_ovp", s->cell_ovp);
    add_number(o, "cell_ovp_recovery", s->cell_ovp_recovery);
    add_number(o, "cell_soc100", s->cell_soc100);
    cJSON_AddBoolToObject(o, "smart_sleep_enabled", s->smart_sleep_enabled);
    add_number(o, "smart_sleep", s->smart_sleep);
    add_number(o, "smart_sleep_delay_h", s->smart_sleep_delay_h);
    add_number(o, "balance_start", s->balance_start);
    add_number(o, "balance_trigger", s->balance_trigger);
    add_number(o, "max_balance_current", s->max_balance_current);
    add_number(o, "max_charge_current", s->max_charge_current);
    add_number(o, "max_discharge_current", s->max_discharge_current);
    add_number(o, "charge_otp", s->charge_otp);
    add_number(o, "charge_utp", s->charge_utp);
    add_number(o, "discharge_otp", s->discharge_otp);
    add_number(o, "discharge_utp", s->discharge_utp);
    add_number(o, "mosfet_otp", s->mosfet_otp);
    cJSON_AddBoolToObject(o, "heating_enabled", s->heating_enabled);
}


// logbook totals of the last delivered report and of the one being sent
static uint32_t m_logbook_sent[JKBMS_DEVICES_MAX];
static uint32_t m_logbook_pending[JKBMS_DEVICES_MAX];


/**
 * Adds the newest logbook events that were not delivered yet (the dashboard
 * keeps the older ones), oldest first.
 */
static void add_logbook(cJSON *bms, int slot, const jkbms_status_t *st) {
    // the last entry is event number `total`
    const uint32_t sent = m_logbook_sent[slot];
    int first = 0;
    if (sent <= st->logbook_total && st->logbook_total - sent < st->logbook_count) {
        first = st->logbook_count - (st->logbook_total - sent);
    }
    m_logbook_pending[slot] = st->logbook_total;
    if (first >= st->logbook_count) {
        return;  // nothing new
    }
    cJSON *o = cJSON_AddObjectToObject(bms, "logbook");
    add_number(o, "total", st->logbook_total);
    add_number(o, "uptime", jkbms_bms_now_s(st));
    cJSON *events = cJSON_AddArrayToObject(o, "events");
    for (int i = first; i < st->logbook_count; i++) {
        char label[64];
        jkbms_log_label(st->logbook[i].code, label, sizeof(label));
        cJSON *event = cJSON_CreateArray();
        cJSON_AddItemToArray(event, cJSON_CreateNumber(st->logbook[i].timestamp_s));
        cJSON_AddItemToArray(event, cJSON_CreateNumber(st->logbook[i].code));
        cJSON_AddItemToArray(event, cJSON_CreateString(label));
        cJSON_AddItemToArray(events, event);
    }
}


char* report_build(uint32_t wake, bool with_extras) {
    cJSON *root = cJSON_CreateObject();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char device[13];
    snprintf(device, sizeof(device), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device", device);
    add_number(root, "wake", wake);
    cJSON *list = cJSON_AddArrayToObject(root, "bms");

    static jkbms_status_t st;  // large: keep it off the stack
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        if (!jkbms_ble_get_status(i, &st) || st.link == JKBMS_LINK_UNASSIGNED) {
            continue;
        }
        cJSON *bms = cJSON_CreateObject();
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", st.mac[0], st.mac[1], st.mac[2],
                 st.mac[3], st.mac[4], st.mac[5]);
        add_number(bms, "slot", i + 1);
        cJSON_AddStringToObject(bms, "mac", mac_str);
        const bool online = st.cells_valid && esp_timer_get_time() - st.cells_updated_us < FRESH_US;
        cJSON_AddBoolToObject(bms, "online", online);
        if (online) {
            cJSON_AddNullToObject(bms, "error");
        } else {
            cJSON_AddStringToObject(bms, "error", error_name(st.error));
        }
        if (st.info_valid) {
            cJSON_AddStringToObject(bms, "name", st.info.name);
            cJSON_AddStringToObject(bms, "model", st.info.model);
            cJSON_AddStringToObject(bms, "hw", st.info.hw_version);
            cJSON_AddStringToObject(bms, "sw", st.info.sw_version);
        }
        if (online) {
            add_cells(bms, &st.cells);
        }
        if (with_extras && st.settings_valid) {
            add_settings(bms, &st.settings);
        }
        if (st.logbook_valid && st.info_valid) {
            add_logbook(bms, i, &st);
        }
        cJSON_AddItemToArray(list, bms);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}


void report_mark_sent(void) {
    memcpy(m_logbook_sent, m_logbook_pending, sizeof(m_logbook_sent));
}


void report_reset(void) {
    memset(m_logbook_sent, 0, sizeof(m_logbook_sent));
    memset(m_logbook_pending, 0, sizeof(m_logbook_pending));
}
