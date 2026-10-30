/*
 * report.c
 */

#include "report.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "cJSON.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "jkbms_ble.h"
#include "remote.h"

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
    add_number(o, "charge_otp_recovery", s->charge_otp_recovery);
    add_number(o, "charge_utp", s->charge_utp);
    add_number(o, "charge_utp_recovery", s->charge_utp_recovery);
    add_number(o, "discharge_otp", s->discharge_otp);
    add_number(o, "discharge_otp_recovery", s->discharge_otp_recovery);
    add_number(o, "discharge_utp", s->discharge_utp);
    add_number(o, "discharge_utp_recovery", s->discharge_utp_recovery);
    add_number(o, "mosfet_otp", s->mosfet_otp);
    add_number(o, "mosfet_otp_recovery", s->mosfet_otp_recovery);
    cJSON_AddBoolToObject(o, "heating_enabled", s->heating_enabled);
    add_number(o, "heating_start", s->heating_start);
    add_number(o, "heating_stop", s->heating_stop);
    add_number(o, "cell_rcv", s->cell_rcv);
    add_number(o, "cell_rfv", s->cell_rfv);
    add_number(o, "charge_ocp_delay", s->charge_ocp_delay_s);
    add_number(o, "charge_ocp_recovery", s->charge_ocp_recovery_s);
    add_number(o, "discharge_ocp_delay", s->discharge_ocp_delay_s);
    add_number(o, "discharge_ocp_recovery", s->discharge_ocp_recovery_s);
    add_number(o, "scp_delay_us", s->scp_delay_us);
    add_number(o, "scp_recovery", s->scp_recovery_s);
    add_number(o, "device_address", s->device_address);
    add_number(o, "precharge", s->precharge_s);
    cJSON_AddBoolToObject(o, "disable_temp_sensors", s->disable_temp_sensors);
    cJSON_AddBoolToObject(o, "gps_heartbeat", s->gps_heartbeat);
    cJSON_AddBoolToObject(o, "port_rs485", s->port_rs485);
    cJSON_AddBoolToObject(o, "display_always_on", s->display_always_on);
    cJSON_AddBoolToObject(o, "special_charger", s->special_charger);
    cJSON_AddBoolToObject(o, "disable_pcl_module", s->disable_pcl_module);
    cJSON_AddBoolToObject(o, "timed_stored_data", s->timed_stored_data);
    cJSON_AddBoolToObject(o, "charging_float_mode", s->charging_float_mode);
    cJSON *wires = cJSON_AddArrayToObject(o, "wire_resistance_mohm");
    for (int i = 0; i < s->wire_count; i++) {
        cJSON_AddItemToArray(wires, cJSON_CreateNumber(s->wire_resistance_mohm[i]));
    }
}


#define LOGBOOK_CHUNK   10   // events per report: a whole logbook does not fit in memory at once

// logbook event numbers: the last delivered and the last in the report being sent
static uint32_t m_logbook_sent[JKBMS_DEVICES_MAX];
static uint32_t m_logbook_pending[JKBMS_DEVICES_MAX];


/**
 * A report with the board ID and one entry per assigned BMS (slot and MAC);
 * `part` names a partial report ("settings", "logbook"), NULL for the status.
 */
static cJSON* report_start(const char *part, uint32_t wake) {
    cJSON *root = cJSON_CreateObject();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char device[13];
    snprintf(device, sizeof(device), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device", device);
    add_number(root, "wake", wake);
    if (part != NULL) {
        cJSON_AddStringToObject(root, "part", part);
    }
    cJSON_AddArrayToObject(root, "bms");
    return root;
}


static cJSON* report_add_bms(cJSON *root, int slot, const jkbms_status_t *st) {
    cJSON *bms = cJSON_CreateObject();
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", st->mac[0], st->mac[1], st->mac[2],
             st->mac[3], st->mac[4], st->mac[5]);
    add_number(bms, "slot", slot + 1);
    cJSON_AddStringToObject(bms, "mac", mac_str);
    cJSON_AddItemToArray(cJSON_GetObjectItem(root, "bms"), bms);
    return bms;
}


static char* report_finish(cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}


char* report_build(uint32_t wake) {
    cJSON *root = report_start(NULL, wake);
    static jkbms_status_t st;  // large: keep it off the stack
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        if (!jkbms_ble_get_status(i, &st) || st.link == JKBMS_LINK_UNASSIGNED) {
            continue;
        }
        cJSON *bms = report_add_bms(root, i, &st);
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
    }
    remote_add_diag(root);
    return report_finish(root);
}


char* report_build_settings(int slot, const jkbms_settings_t *settings) {
    static jkbms_status_t st;
    if (!jkbms_ble_get_status(slot, &st) || st.link == JKBMS_LINK_UNASSIGNED) {
        return NULL;
    }
    cJSON *root = report_start("settings", 0);
    add_settings(report_add_bms(root, slot, &st), settings);
    return report_finish(root);
}


char* report_build_logbook(int slot, const jkbms_extras_t *x) {
    static jkbms_status_t st;
    if (!x->logbook_valid || !jkbms_ble_get_status(slot, &st) || st.link == JKBMS_LINK_UNASSIGNED) {
        return NULL;
    }
    // the last entry is event number `logbook_total`
    const uint32_t sent = m_logbook_sent[slot];
    int first = 0;
    if (sent <= x->logbook_total && x->logbook_total - sent < x->logbook_count) {
        first = x->logbook_count - (x->logbook_total - sent);
    }
    if (first >= x->logbook_count) {
        return NULL;  // nothing new
    }
    const int end = MIN(x->logbook_count, first + LOGBOOK_CHUNK);
    // "total" is the number of the last event in the list
    const uint32_t last = x->logbook_total - (x->logbook_count - end);
    m_logbook_pending[slot] = last;

    cJSON *root = report_start("logbook", 0);
    cJSON *o = cJSON_AddObjectToObject(report_add_bms(root, slot, &st), "logbook");
    add_number(o, "total", last);
    // the BMS uptime now: the events may be sent long after they were read
    add_number(o, "uptime", x->logbook_uptime_s + (esp_timer_get_time() - x->logbook_read_us) / 1000000);
    cJSON *events = cJSON_AddArrayToObject(o, "events");
    for (int i = first; i < end; i++) {
        char label[64];
        jkbms_log_label(x->logbook[i].code, label, sizeof(label));
        cJSON *event = cJSON_CreateArray();
        cJSON_AddItemToArray(event, cJSON_CreateNumber(x->logbook[i].timestamp_s));
        cJSON_AddItemToArray(event, cJSON_CreateNumber(x->logbook[i].code));
        cJSON_AddItemToArray(event, cJSON_CreateString(label));
        cJSON_AddItemToArray(events, event);
    }
    return report_finish(root);
}


char* report_add_age(const char *json, uint32_t age_s) {
    // {"device":... -> {"age":N,"device":...
    const size_t len = strlen(json) + 24;
    char *aged = malloc(len);
    if (aged != NULL) {
        snprintf(aged, len, "{\"age\":%lu,%s", (unsigned long) age_s, json + 1);
    }
    return aged;
}


int report_logbook_unsent(int slot, const jkbms_extras_t *x) {
    const uint32_t sent = m_logbook_sent[slot];
    if (!x->logbook_valid || sent >= x->logbook_total) {
        return 0;
    }
    return MIN(x->logbook_total - sent, (uint32_t) x->logbook_count);
}


void report_logbook_sent(int slot) {
    m_logbook_sent[slot] = m_logbook_pending[slot];
}


void report_reset(void) {
    memset(m_logbook_sent, 0, sizeof(m_logbook_sent));
    memset(m_logbook_pending, 0, sizeof(m_logbook_pending));
}
