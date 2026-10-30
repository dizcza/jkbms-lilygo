/*
 * jkbms_proto.c
 *
 * Frame offsets are taken from esphome-jk-bms (jk_bms_ble.cpp,
 * decode_jk02_cell_info_ and decode_device_info_).
 */

#include "jkbms_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>


static const char *const m_errors_jk02[32] = {
    "Wire resistance",                      // bit 0
    "MOSFET overtemperature",               // bit 1
    "Cell count is not equal to settings",  // bit 2
    NULL,                                   // bit 3
    "Battery is fully charged",             // bit 4
    "Battery pack overvoltage",             // bit 5
    "Charge overcurrent",                   // bit 6
    "Charge short circuit",                 // bit 7
    "Charge overtemperature",               // bit 8
    "Charge undertemperature",              // bit 9
    "Coprocessor communication error",      // bit 10
    "Cell undervoltage",                    // bit 11
    "Battery pack undervoltage",            // bit 12
    "Discharge overcurrent",                // bit 13
    "Discharge short circuit",              // bit 14
    "Discharge overtemperature",            // bit 15
    "Charging MOSFET abnormal",             // bit 16
    "Discharging MOSFET abnormal",          // bit 17
    "GPS disconnected",                     // bit 18
    "Modify password in time",              // bit 19
    "Discharge on failed",                  // bit 20
    "Battery overtemperature",              // bit 21
    "Temperature sensor anomaly",           // bit 22
    "PL module anomaly",                    // bit 23
    "SCP release failed",                   // bit 24
    "Discharge OCP II",                     // bit 25
    "Discharge OCP III",                    // bit 26
    "Discharge undertemperature alarm",     // bit 27
    "GPS remote lock",                      // bit 28
    NULL, NULL, NULL,                       // bits 29-31
};


// logbook event codes 0x00..0x49, from esphome-jk-bms LOGBOOK_CODES
static const char *const m_log_codes[] = {
    "",                                                    // 0x00
    "Boot",                                                // 0x01
    "Shutdown",                                            // 0x02
    "APP close charge",                                    // 0x03
    "APP open charge",                                     // 0x04
    "APP close discharge",                                 // 0x05
    "APP open discharge",                                  // 0x06
    "Remote close charge",                                 // 0x07
    "Remote open charge",                                  // 0x08
    "Remote close discharge",                              // 0x09
    "Remote open discharge",                               // 0x0A
    "MOS over temperature protection",                     // 0x0B
    "MOS over-temperature protection is released",         // 0x0C
    "Abnormal current sensor",                             // 0x0D
    "Abnormal release of current sensor",                  // 0x0E
    "Abnormal coprocessor communication",                  // 0x0F
    "Abnormal cancellation of coprocessor communication",  // 0x10
    "Cell overcharge protection",                          // 0x11
    "Cell overcharge protection is released",              // 0x12
    "Battery overcharge protection",                       // 0x13
    "Battery overcharge protection is released",           // 0x14
    "Charge overcurrent protection",                       // 0x15
    "Charge overcurrent protection is released",           // 0x16
    "Charge short circuit protection",                     // 0x17
    "Charge short circuit protection is released",         // 0x18
    "Charge over temperature protection",                  // 0x19
    "Charge over temperature protection is released",      // 0x1A
    "Charge low temperature protection",                   // 0x1B
    "Charge low temperature protection is released",       // 0x1C
    "Cell undervoltage protection",                        // 0x1D
    "Cell undervoltage protection is released",            // 0x1E
    "Battery undervoltage protection",                     // 0x1F
    "Battery undervoltage protection is released",         // 0x20
    "Discharge overcurrent protection",                    // 0x21
    "Discharge overcurrent protection is released",        // 0x22
    "Discharge short circuit protection",                  // 0x23
    "Discharge short circuit protection released",         // 0x24
    "Discharge over temperature protection",               // 0x25
    "Discharge over-temperature protection is released",   // 0x26
    "Reset Watch-Dog",                                     // 0x27
    "Discharge level 2 short circuit protection",          // 0x28
    "Manually enable the emergency mode",                  // 0x29
    "Manually turn off the emergency mode",                // 0x2A
    "Turn off the emergency mode automatically",           // 0x2B
    "APP to turn it off",                                  // 0x2C
    "Button to turn it off",                               // 0x2D
    "Discharge On Failed",                                 // 0x2E
    "RS485 power off",                                     // 0x2F
    "CAN charge off",                                      // 0x30
    "CAN charge on",                                       // 0x31
    "CAN discharge off",                                   // 0x32
    "CAN discharge on",                                    // 0x33
    "RS485 charge off",                                    // 0x34
    "RS485 charge on",                                     // 0x35
    "RS485 discharge off",                                 // 0x36
    "RS485 discharge on",                                  // 0x37
    "Enter sleep",                                         // 0x38
    "Charge MOS abnormal",                                 // 0x39
    "Discharge MOS abnormal",                              // 0x3A
    "Time calibration",                                    // 0x3B
    "Cells Count Incorrect",                               // 0x3C
    "Button Emergency On",                                 // 0x3D
    "Button Emergency Off",                                // 0x3E
    "Button Forced Heating",                               // 0x3F
    "Discharge OCP II",                                    // 0x40
    "Discharge OCP III",                                   // 0x41
    "SCP Release Failed",                                  // 0x42
    "Factory setting LION",                                // 0x43
    "Factory setting LFP",                                 // 0x44
    "Factory setting LTO",                                 // 0x45
    "Remote Emergency On",                                 // 0x46
    "Remote Emergency Off",                                // 0x47
    "Discharge under temperature protection",              // 0x48
    "Discharge under temperature protection Release",      // 0x49
};

#define LOG_CODE_CELL_OVERCHARGE      0x64    // 0x64..0x83: cell 1..32 over charge protection
#define LOG_CODE_CELL_OVERDISCHARGE   0xC8    // 0xC8..0xE7: cell 1..32 over discharge protection


static inline uint16_t get_u16(const uint8_t *frame, int i) {
    return (uint16_t) frame[i] | ((uint16_t) frame[i + 1] << 8);
}

static inline uint32_t get_u32(const uint8_t *frame, int i) {
    return (uint32_t) get_u16(frame, i) | ((uint32_t) get_u16(frame, i + 2) << 16);
}

static void copy_string(char *dst, size_t dst_size, const uint8_t *src, size_t src_len) {
    size_t n = 0;
    while (n < src_len && n < dst_size - 1 && src[n] != '\0') {
        dst[n] = (char) src[n];
        n++;
    }
    dst[n] = '\0';
}


uint8_t jkbms_crc(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0;
    for (uint16_t i = 0; i < len; i++) {
        crc += data[i];
    }
    return crc;
}


void jkbms_build_command(uint8_t command, uint8_t frame[JKBMS_COMMAND_SIZE]) {
    memset(frame, 0, JKBMS_COMMAND_SIZE);
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = 0x90;
    frame[3] = 0xEB;
    frame[4] = command;
    frame[JKBMS_COMMAND_SIZE - 1] = jkbms_crc(frame, JKBMS_COMMAND_SIZE - 1);
}


const uint8_t* jkbms_assemble(jkbms_assembler_t *assembler, const uint8_t *data, uint16_t len) {
    // flush the buffer on every preamble
    if (len >= 4 && data[0] == 0x55 && data[1] == 0xAA && data[2] == 0xEB && data[3] == 0x90) {
        assembler->len = 0;
    }
    if (assembler->len + len > sizeof(assembler->buf)) {
        assembler->len = 0;
        return NULL;
    }
    memcpy(assembler->buf + assembler->len, data, len);
    assembler->len += len;

    if (assembler->len < JKBMS_FRAME_SIZE) {
        return NULL;
    }
    // even if the frame is 320 bytes long, the CRC is at position 299
    const bool crc_ok = jkbms_crc(assembler->buf, JKBMS_FRAME_SIZE - 1) == assembler->buf[JKBMS_FRAME_SIZE - 1];
    assembler->len = 0;
    return crc_ok ? assembler->buf : NULL;
}


void jkbms_decode_device_info(const uint8_t *frame, jkbms_device_info_t *info) {
    copy_string(info->model, sizeof(info->model), frame + 6, 16);
    copy_string(info->hw_version, sizeof(info->hw_version), frame + 22, 8);
    copy_string(info->sw_version, sizeof(info->sw_version), frame + 30, 8);
    info->uptime_s = get_u32(frame, 38);
    info->power_on_count = get_u32(frame, 42);
    copy_string(info->name, sizeof(info->name), frame + 46, 16);
}


jkbms_proto_t jkbms_proto_from_device_info(const jkbms_device_info_t *info) {
    const int sw_major = atoi(info->sw_version);
    const int hw_major = atoi(info->hw_version);
    if (sw_major >= 11 || hw_major >= 11) {
        return JKBMS_PROTO_JK02_32S;
    }
    if (sw_major > 0 || hw_major > 0) {
        return JKBMS_PROTO_JK02_24S;
    }
    return JKBMS_PROTO_UNKNOWN;
}


static float pack_voltage_mismatch(const uint8_t *frame, jkbms_proto_t proto) {
    const int offset = (proto == JKBMS_PROTO_JK02_32S) ? 16 : 0;
    const int cells = 24 + offset / 2;
    float sum = 0;
    for (int i = 0; i < cells; i++) {
        sum += get_u16(frame, 6 + 2 * i) * 0.001f;
    }
    const float total = get_u32(frame, 118 + 2 * offset) * 0.001f;
    if (sum <= 0) {
        return INFINITY;
    }
    return fabsf(sum - total);
}


jkbms_proto_t jkbms_proto_guess(const uint8_t *frame) {
    const float err_24s = pack_voltage_mismatch(frame, JKBMS_PROTO_JK02_24S);
    const float err_32s = pack_voltage_mismatch(frame, JKBMS_PROTO_JK02_32S);
    return (err_32s <= err_24s) ? JKBMS_PROTO_JK02_32S : JKBMS_PROTO_JK02_24S;
}


bool jkbms_decode_cell_info(const uint8_t *frame, jkbms_proto_t proto, jkbms_cell_info_t *info) {
    if (frame[4] != JKBMS_FRAME_TYPE_CELL_INFO) {
        return false;
    }
    if (proto == JKBMS_PROTO_UNKNOWN) {
        proto = jkbms_proto_guess(frame);
    }
    const bool is_32s = (proto == JKBMS_PROTO_JK02_32S);
    int offset = is_32s ? 16 : 0;
    const int cells_max = 24 + offset / 2;

    // 54+offset: enabled cells bitmask
    const uint32_t enabled_mask = get_u32(frame, 54 + offset);
    int cell_count = __builtin_popcount(enabled_mask);
    if (cell_count == 0 || cell_count > cells_max) {
        // fall back to the highest cell with a non-zero voltage
        cell_count = 0;
        for (int i = 0; i < cells_max; i++) {
            if (get_u16(frame, 6 + 2 * i) != 0) {
                cell_count = i + 1;
            }
        }
    }
    info->cell_count = (uint8_t) cell_count;

    info->min_cell_voltage = 100.0f;
    info->max_cell_voltage = -100.0f;
    info->min_voltage_cell = 0;
    info->max_voltage_cell = 0;
    float sum = 0;
    int cells_enabled = 0;
    for (int i = 0; i < JKBMS_MAX_CELLS; i++) {
        if (i >= cell_count) {
            info->cell_voltage[i] = 0;
            info->cell_resistance[i] = 0;
            continue;
        }
        // 6+2i: cell voltage, mV
        const float voltage = get_u16(frame, 6 + 2 * i) * 0.001f;
        // 64+offset+2i: cell resistance, mOhm
        info->cell_voltage[i] = voltage;
        info->cell_resistance[i] = get_u16(frame, 64 + offset + 2 * i) * 0.001f;
        if (voltage > 0) {
            sum += voltage;
            cells_enabled++;
            if (voltage < info->min_cell_voltage) {
                info->min_cell_voltage = voltage;
                info->min_voltage_cell = i + 1;
            }
        }
        if (voltage > info->max_cell_voltage) {
            info->max_cell_voltage = voltage;
            info->max_voltage_cell = i + 1;
        }
    }
    if (cells_enabled == 0) {
        info->min_cell_voltage = info->max_cell_voltage = 0;
    }
    info->avg_cell_voltage = cells_enabled ? sum / cells_enabled : 0;
    info->delta_cell_voltage = info->max_cell_voltage - info->min_cell_voltage;

    offset *= 2;

    info->mosfet_temperature = (int16_t) get_u16(frame, (is_32s ? 112 : 134) + offset) * 0.1f;
    info->wire_resistance_warning = get_u32(frame, 114 + offset);
    info->total_voltage = get_u32(frame, 118 + offset) * 0.001f;
    info->current = (int32_t) get_u32(frame, 126 + offset) * 0.001f;
    info->power = info->total_voltage * info->current;
    info->temperature[0] = (int16_t) get_u16(frame, 130 + offset) * 0.1f;
    info->temperature[1] = (int16_t) get_u16(frame, 132 + offset) * 0.1f;
    info->errors = is_32s ? get_u32(frame, 134 + offset) : get_u16(frame, 136 + offset);
    info->balancing_current = (int16_t) get_u16(frame, 138 + offset) * 0.001f;
    info->balancer_action = frame[140 + offset];
    info->soc = frame[141 + offset];
    info->capacity_remaining = get_u32(frame, 142 + offset) * 0.001f;
    info->capacity_nominal = get_u32(frame, 146 + offset) * 0.001f;
    info->cycles = get_u32(frame, 150 + offset);
    info->cycle_capacity = get_u32(frame, 154 + offset) * 0.001f;
    info->soh = frame[158 + offset];
    info->runtime_s = get_u32(frame, 162 + offset);
    info->charging = frame[166 + offset] != 0;
    info->discharging = frame[167 + offset] != 0;
    info->precharging = frame[168 + offset] != 0;
    info->heating = frame[183 + offset] != 0;

    return true;
}


bool jkbms_decode_settings(const uint8_t *frame, jkbms_proto_t proto, jkbms_settings_t *settings) {
    if (frame[4] != JKBMS_FRAME_TYPE_SETTINGS) {
        return false;
    }
    memset(settings, 0, sizeof(*settings));
    settings->smart_sleep = get_u32(frame, 6) * 0.001f;
    settings->cell_uvp = get_u32(frame, 10) * 0.001f;
    settings->cell_uvp_recovery = get_u32(frame, 14) * 0.001f;
    settings->cell_ovp = get_u32(frame, 18) * 0.001f;
    settings->cell_ovp_recovery = get_u32(frame, 22) * 0.001f;
    settings->balance_trigger = get_u32(frame, 26) * 0.001f;
    settings->cell_soc100 = get_u32(frame, 30) * 0.001f;
    settings->cell_soc0 = get_u32(frame, 34) * 0.001f;
    settings->cell_rcv = get_u32(frame, 38) * 0.001f;
    settings->cell_rfv = get_u32(frame, 42) * 0.001f;
    settings->power_off = get_u32(frame, 46) * 0.001f;
    settings->max_charge_current = get_u32(frame, 50) * 0.001f;
    settings->charge_ocp_delay_s = get_u32(frame, 54);
    settings->charge_ocp_recovery_s = get_u32(frame, 58);
    settings->max_discharge_current = get_u32(frame, 62) * 0.001f;
    settings->discharge_ocp_delay_s = get_u32(frame, 66);
    settings->discharge_ocp_recovery_s = get_u32(frame, 70);
    settings->scp_recovery_s = get_u32(frame, 74);
    settings->max_balance_current = get_u32(frame, 78) * 0.001f;
    settings->charge_otp = (int32_t) get_u32(frame, 82) * 0.1f;
    settings->charge_otp_recovery = (int32_t) get_u32(frame, 86) * 0.1f;
    settings->discharge_otp = (int32_t) get_u32(frame, 90) * 0.1f;
    settings->discharge_otp_recovery = (int32_t) get_u32(frame, 94) * 0.1f;
    settings->charge_utp = (int32_t) get_u32(frame, 98) * 0.1f;
    settings->charge_utp_recovery = (int32_t) get_u32(frame, 102) * 0.1f;
    settings->mosfet_otp = (int32_t) get_u32(frame, 106) * 0.1f;
    settings->mosfet_otp_recovery = (int32_t) get_u32(frame, 110) * 0.1f;
    settings->cell_count = frame[114];
    settings->charge_enabled = frame[118] != 0;
    settings->discharge_enabled = frame[122] != 0;
    settings->balancer_enabled = frame[126] != 0;
    settings->capacity = get_u32(frame, 130) * 0.001f;
    settings->scp_delay_us = get_u32(frame, 134);
    settings->balance_start = get_u32(frame, 138) * 0.001f;
    // balance lead resistances (mOhm): 32 from offset 142 (JK02_32S), 24 from 158 (JK02_24S)
    const bool is_32s = proto == JKBMS_PROTO_JK02_32S;
    settings->wire_count = MIN(settings->cell_count, is_32s ? 32 : 24);
    for (int i = 0; i < settings->wire_count; i++) {
        settings->wire_resistance_mohm[i] = (uint16_t) MIN(get_u32(frame, (is_32s ? 142 : 158) + 4 * i), UINT16_MAX);
    }
    if (is_32s) {
        settings->device_address = frame[270];
        settings->precharge_s = frame[274];
        const uint8_t flags = frame[282];
        settings->heating_enabled = (flags & 0x01) != 0;
        settings->disable_temp_sensors = (flags & 0x02) != 0;
        settings->gps_heartbeat = (flags & 0x04) != 0;
        settings->port_rs485 = (flags & 0x08) != 0;
        settings->display_always_on = (flags & 0x10) != 0;
        settings->special_charger = (flags & 0x20) != 0;
        settings->smart_sleep_enabled = (flags & 0x40) != 0;
        settings->disable_pcl_module = (flags & 0x80) != 0;
        settings->timed_stored_data = (frame[283] & 0x01) != 0;
        settings->charging_float_mode = (frame[283] & 0x02) != 0;
        settings->heating_start = (int8_t) frame[284];
        settings->heating_stop = (int8_t) frame[285];
        settings->smart_sleep_delay_h = frame[286];
        settings->discharge_utp = (int8_t) frame[296];
        settings->discharge_utp_recovery = (int8_t) frame[297];
    }
    return true;
}


int jkbms_decode_logbook(const uint8_t *frame, uint32_t *total, jkbms_log_entry_t *entries, int max) {
    // 6: total number of events ever logged, 11+5i: u32 timestamp, u8 event code.
    // The 50 slots are a ring buffer: once full, the oldest event is in slot total % 50.
    *total = get_u32(frame, 6);
    const int slots = JKBMS_LOGBOOK_FRAME_ENTRIES;
    int count = (*total < slots) ? (int) *total : slots;
    const int oldest = (*total < slots) ? 0 : (int) (*total % slots);
    if (count > max) {
        count = max;
    }
    for (int i = 0; i < count; i++) {
        const int slot = (oldest + i) % slots;
        entries[i].timestamp_s = get_u32(frame, 11 + 5 * slot);
        entries[i].code = frame[11 + 5 * slot + 4];
    }
    return count;
}


void jkbms_log_label(uint8_t code, char *buf, size_t size) {
    const uint8_t cells = JKBMS_MAX_CELLS;
    if (code < sizeof(m_log_codes) / sizeof(m_log_codes[0]) && m_log_codes[code][0] != '\0') {
        snprintf(buf, size, "%s", m_log_codes[code]);
    } else if (code >= LOG_CODE_CELL_OVERCHARGE && code < LOG_CODE_CELL_OVERCHARGE + cells) {
        snprintf(buf, size, "Cell %d over charge protection", code - LOG_CODE_CELL_OVERCHARGE + 1);
    } else if (code >= LOG_CODE_CELL_OVERDISCHARGE && code < LOG_CODE_CELL_OVERDISCHARGE + cells) {
        snprintf(buf, size, "Cell %d over discharge protection", code - LOG_CODE_CELL_OVERDISCHARGE + 1);
    } else {
        snprintf(buf, size, "Unknown event 0x%02X", code);
    }
}


const char* jkbms_error_label(uint8_t bit) {
    if (bit >= 32) {
        return NULL;
    }
    return m_errors_jk02[bit];
}


const char* jkbms_proto_name(jkbms_proto_t proto) {
    switch (proto) {
    case JKBMS_PROTO_JK02_24S:
        return "JK02_24S";
    case JKBMS_PROTO_JK02_32S:
        return "JK02_32S";
    default:
        return "unknown";
    }
}
