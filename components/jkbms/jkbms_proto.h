/*
 * jkbms_proto.h
 *
 * JK BMS BLE frame assembly and decoding (JK02_24S and JK02_32S frame formats).
 * Ported from https://github.com/syssi/esphome-jk-bms (components/jk_bms_ble).
 * No ESP-IDF dependencies so that it can be unit-tested on the host.
 */

#ifndef COMPONENTS_JKBMS_JKBMS_PROTO_H_
#define COMPONENTS_JKBMS_JKBMS_PROTO_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JKBMS_MAX_CELLS          32
#define JKBMS_FRAME_SIZE         300         // CRC is at byte 299
#define JKBMS_FRAME_MAX_SIZE     (384 + 16)
#define JKBMS_COMMAND_SIZE       20

#define JKBMS_COMMAND_CELL_INFO     0x96
#define JKBMS_COMMAND_DEVICE_INFO   0x97
#define JKBMS_COMMAND_LOGBOOK       0xA1

#define JKBMS_FRAME_TYPE_SETTINGS     0x01
#define JKBMS_FRAME_TYPE_CELL_INFO    0x02
#define JKBMS_FRAME_TYPE_DEVICE_INFO  0x03
#define JKBMS_FRAME_TYPE_LOGBOOK      0x05

#define JKBMS_LOGBOOK_FRAME_ENTRIES   50

#define JKBMS_BALANCER_OFF        0
#define JKBMS_BALANCER_CHARGE     1
#define JKBMS_BALANCER_DISCHARGE  2


typedef enum {
    JKBMS_PROTO_UNKNOWN = 0,
    JKBMS_PROTO_JK02_24S,      // hardware/software < v11
    JKBMS_PROTO_JK02_32S,      // hardware/software >= v11
} jkbms_proto_t;


typedef struct {
    char model[17];            // vendor ID, e.g. "JK-B2A24S20P"
    char hw_version[9];
    char sw_version[9];
    char name[17];             // user-defined device name
    uint32_t uptime_s;
    uint32_t power_on_count;
} jkbms_device_info_t;


typedef struct {
    uint8_t cell_count;
    float cell_voltage[JKBMS_MAX_CELLS];      // V
    float cell_resistance[JKBMS_MAX_CELLS];   // Ohm (balance wire + cell)
    uint32_t wire_resistance_warning;         // bit per cell
    float min_cell_voltage;
    float max_cell_voltage;
    float avg_cell_voltage;
    float delta_cell_voltage;
    uint8_t min_voltage_cell;                 // 1-based
    uint8_t max_voltage_cell;                 // 1-based

    float total_voltage;                      // V
    float current;                            // A, positive = charging
    float power;                              // W
    float temperature[2];                     // °C, T1 and T2
    float mosfet_temperature;                 // °C
    float balancing_current;                  // A
    uint8_t balancer_action;                  // JKBMS_BALANCER_*
    uint8_t soc;                              // %
    uint8_t soh;                              // %
    float capacity_remaining;                 // Ah
    float capacity_nominal;                   // Ah
    float cycle_capacity;                     // Ah
    uint32_t cycles;
    uint32_t runtime_s;
    uint32_t errors;                          // see jkbms_error_label()
    bool charging;
    bool discharging;
    bool precharging;
    bool heating;
} jkbms_cell_info_t;


typedef struct {
    uint8_t cell_count;
    float capacity;                 // Ah
    float cell_ovp;                 // V, cell overvoltage protection
    float cell_ovp_recovery;        // V
    float cell_uvp;                 // V, cell undervoltage protection
    float cell_uvp_recovery;        // V
    float cell_soc100;              // V, cell voltage treated as SOC 100% (JK02_32S)
    float cell_soc0;                // V, cell voltage treated as SOC 0% (JK02_32S)
    float power_off;                // V, cell voltage at which the BMS shuts down
    float smart_sleep;              // V
    float balance_start;            // V, minimum cell voltage to balance
    float balance_trigger;          // V, cell voltage difference to start balancing
    float max_balance_current;      // A
    float max_charge_current;       // A
    float max_discharge_current;    // A
    float charge_otp;               // °C
    float charge_otp_recovery;
    float charge_utp;
    float charge_utp_recovery;
    float discharge_otp;
    float discharge_otp_recovery;
    float discharge_utp;            // JK02_32S
    float discharge_utp_recovery;   // JK02_32S
    float mosfet_otp;
    float mosfet_otp_recovery;
    bool charge_enabled;
    bool discharge_enabled;
    bool balancer_enabled;
    bool heating_enabled;           // JK02_32S
    bool smart_sleep_enabled;       // JK02_32S
    uint8_t smart_sleep_delay_h;    // JK02_32S
} jkbms_settings_t;


typedef struct {
    uint32_t timestamp_s;      // seconds since the BMS was first powered
    uint8_t code;              // see jkbms_log_label()
} jkbms_log_entry_t;


typedef struct {
    uint8_t buf[JKBMS_FRAME_MAX_SIZE];
    uint16_t len;
} jkbms_assembler_t;


uint8_t jkbms_crc(const uint8_t *data, uint16_t len);

void jkbms_build_command(uint8_t command, uint8_t frame[JKBMS_COMMAND_SIZE]);

/**
 * Feed one BLE notification. Returns a pointer to a complete CRC-checked frame
 * (at least JKBMS_FRAME_SIZE bytes) or NULL. The returned frame stays valid
 * until the next call.
 */
const uint8_t* jkbms_assemble(jkbms_assembler_t *assembler, const uint8_t *data, uint16_t len);

void jkbms_decode_device_info(const uint8_t *frame, jkbms_device_info_t *info);

/**
 * Selects the frame format from the device info: firmware or hardware v11+
 * uses JK02_32S, older devices use JK02_24S.
 */
jkbms_proto_t jkbms_proto_from_device_info(const jkbms_device_info_t *info);

/**
 * Guesses the frame format from a cell info frame by checking which format
 * makes the sum of the cell voltages match the pack voltage.
 */
jkbms_proto_t jkbms_proto_guess(const uint8_t *frame);

bool jkbms_decode_cell_info(const uint8_t *frame, jkbms_proto_t proto, jkbms_cell_info_t *info);

/**
 * Decodes a settings frame (sent by the BMS after JKBMS_COMMAND_CELL_INFO).
 * Fields marked JK02_32S stay zero for JK02_24S devices.
 */
bool jkbms_decode_settings(const uint8_t *frame, jkbms_proto_t proto, jkbms_settings_t *settings);

/**
 * Decodes a logbook frame (response to JKBMS_COMMAND_LOGBOOK): the BMS keeps
 * only the last JKBMS_LOGBOOK_FRAME_ENTRIES events. Returns the number of
 * entries written to `entries`, oldest first, and the total number of events
 * ever logged in `total`.
 */
int jkbms_decode_logbook(const uint8_t *frame, uint32_t *total, jkbms_log_entry_t *entries, int max);

/**
 * Writes a description of the logbook event code to `buf`.
 */
void jkbms_log_label(uint8_t code, char *buf, size_t size);

/**
 * Returns a label of the error bit or NULL if the bit is unused.
 */
const char* jkbms_error_label(uint8_t bit);

const char* jkbms_proto_name(jkbms_proto_t proto);


#ifdef __cplusplus
}
#endif

#endif /* COMPONENTS_JKBMS_JKBMS_PROTO_H_ */
