/*
 * history.c
 *
 * The partition is a ring of 32-byte records with increasing sequence numbers.
 * A flash sector is erased right before its first record is written, which
 * drops the oldest records. The write position is kept in RTC memory across
 * deep sleep and found by scanning the sequence numbers after a power-on.
 */

#include "history.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"

#define HISTORY_PARTITION_SUBTYPE  0x40
#define SECTOR_SIZE                4096
#define RECORDS_PER_SECTOR         (SECTOR_SIZE / sizeof(history_record_t))
#define SEQ_EMPTY                  0xFFFFFFFF

_Static_assert(sizeof(history_record_t) == 32, "history record must stay 32 bytes");

typedef struct {
    bool valid;
    uint32_t next_index;       // record slot to write next
    uint32_t next_seq;
} history_head_t;

static RTC_DATA_ATTR history_head_t m_head;
static const esp_partition_t *m_partition;
static uint32_t m_capacity;
static const char *TAG = "history";


static uint32_t read_seq(uint32_t index) {
    uint32_t seq = SEQ_EMPTY;
    esp_partition_read(m_partition, index * sizeof(history_record_t), &seq, sizeof(seq));
    return seq;
}


/**
 * Finds the newest record: the sector whose first record has the highest
 * sequence number, then the last written record in it.
 */
static void find_head(void) {
    const uint32_t sectors = m_capacity / RECORDS_PER_SECTOR;
    uint32_t best_sector = 0;
    uint32_t best_seq = 0;
    bool found = false;
    for (uint32_t s = 0; s < sectors; s++) {
        const uint32_t seq = read_seq(s * RECORDS_PER_SECTOR);
        if (seq != SEQ_EMPTY && (!found || seq > best_seq)) {
            best_seq = seq;
            best_sector = s;
            found = true;
        }
    }
    m_head = (history_head_t) { .valid = true, .next_index = 0, .next_seq = 1 };
    if (!found) {
        return;
    }
    uint32_t last = best_sector * RECORDS_PER_SECTOR;
    for (uint32_t i = 1; i < RECORDS_PER_SECTOR; i++) {
        const uint32_t seq = read_seq(best_sector * RECORDS_PER_SECTOR + i);
        if (seq == SEQ_EMPTY || seq <= best_seq) {
            break;
        }
        best_seq = seq;
        last = best_sector * RECORDS_PER_SECTOR + i;
    }
    m_head.next_index = (last + 1) % m_capacity;
    m_head.next_seq = best_seq + 1;
}


esp_err_t history_init(void) {
    m_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, HISTORY_PARTITION_SUBTYPE, "history");
    if (m_partition == NULL) {
        ESP_LOGE(TAG, "No history partition");
        return ESP_ERR_NOT_FOUND;
    }
    m_capacity = m_partition->size / sizeof(history_record_t);
    if (!m_head.valid) {
        find_head();
        ESP_LOGI(TAG, "%lu records recorded so far", (unsigned long) (m_head.next_seq - 1));
    }
    return ESP_OK;
}


void history_add(int slot, const jkbms_status_t *status) {
    if (m_partition == NULL || !status->cells_valid || !status->info_valid) {
        return;
    }
    const jkbms_cell_info_t *ci = &status->cells;
    history_record_t record = {
        .seq = m_head.next_seq,
        .time_s = jkbms_bms_now_s(status),
        .voltage_cv = (uint16_t) (ci->total_voltage * 100 + 0.5f),
        .current_ca = (int16_t) (ci->current * 100),
        .slot = (uint8_t) slot,
        .soc = ci->soc,
        .flags = (ci->charging ? HISTORY_FLAG_CHARGING : 0) | (ci->discharging ? HISTORY_FLAG_DISCHARGING : 0)
                 | (ci->balancer_action != JKBMS_BALANCER_OFF ? HISTORY_FLAG_BALANCING : 0),
        .min_cell = ci->min_voltage_cell,
        .max_cell = ci->max_voltage_cell,
        .min_mv = (uint16_t) (ci->min_cell_voltage * 1000 + 0.5f),
        .max_mv = (uint16_t) (ci->max_cell_voltage * 1000 + 0.5f),
        .errors = ci->errors,
    };
    memset(record.padding, 0xFF, sizeof(record.padding));
    const uint32_t index = m_head.next_index;
    if (index % RECORDS_PER_SECTOR == 0) {
        esp_partition_erase_range(m_partition, index * sizeof(history_record_t), SECTOR_SIZE);
    }
    if (esp_partition_write(m_partition, index * sizeof(history_record_t), &record, sizeof(record)) != ESP_OK) {
        ESP_LOGW(TAG, "write failed");
        return;
    }
    m_head.next_index = (index + 1) % m_capacity;
    m_head.next_seq++;
}


void history_foreach(history_cb_t callback, void *ctx) {
    if (m_partition == NULL || m_head.next_seq == 1) {
        return;
    }
    static history_record_t sector[RECORDS_PER_SECTOR];  // 4 KB: keep it off the stack
    uint32_t index = (m_head.next_index + m_capacity - 1) % m_capacity;
    uint32_t expected = m_head.next_seq - 1;
    uint32_t loaded = UINT32_MAX;
    for (uint32_t n = 0; n < m_capacity; n++) {
        const uint32_t sector_index = index / RECORDS_PER_SECTOR;
        if (sector_index != loaded) {
            esp_partition_read(m_partition, sector_index * SECTOR_SIZE, sector, SECTOR_SIZE);
            loaded = sector_index;
        }
        const history_record_t *record = &sector[index % RECORDS_PER_SECTOR];
        // stop at erased space or where an older lap of the ring was overwritten
        if (record->seq != expected || !callback(record, ctx)) {
            return;
        }
        if (--expected == 0) {
            return;
        }
        index = (index + m_capacity - 1) % m_capacity;
    }
}


uint32_t history_last_seq(void) {
    return m_head.next_seq - 1;
}
