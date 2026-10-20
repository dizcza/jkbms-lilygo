/*
 * ui.c
 *
 * Pages of every BMS, in order:
 *   summary, all cells at a glance, pack (temperatures, cycles, balancing
 *   current), one page per cell, active errors, settings (several pages),
 *   logbook (several pages, newest events first).
 * The header blinks red while any BMS reports an active error.
 * The left/right buttons step through all pages of all BMS with wrap-around.
 */

#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "jkbms_ble.h"
#include "lcd.h"

#define COLOR_BG          0x000000
#define COLOR_HEADER      0x202830
#define COLOR_TEXT        0xFFFFFF
#define COLOR_DIM         0x8A949E
#define COLOR_OK          0x3DDC84
#define COLOR_WARN        0xFFA726
#define COLOR_ERROR       0xFF5252
#define COLOR_MAX         0xFF5252
#define COLOR_MIN         0x42A5F5
#define COLOR_BAR         0x45607A
#define COLOR_SELECTED    0xFFEB3B

#define HEADER_H          22
#define CONTENT_H         (LCD_V_RES - HEADER_H)
#define CHART_Y           202
#define CHART_H           34
#define CHART_X           4
#define CHART_W           (LCD_H_RES - 2 * CHART_X)

#define STALE_US          (15 * 1000000LL)

#define LOG_SLOTS         8          // max logbook events per page
#define LOG_TITLE_H       26
#define LOG_TIME_H        17         // line height of the 14 px font
#define LOG_GAP           6          // space between events

#define SETTINGS_ROWS     4          // settings per page, name and value on two lines
#define SETTINGS_ROW_H    47
#define SETTINGS_MAX      32

#define PICKER_ROWS       5
#define PICKER_ROW_H      34
#define PICKER_MAX_AGE_US (20 * 1000000LL)

typedef enum {
    UI_MODE_PICKER,
    UI_MODE_MONITOR,
} ui_mode_t;

typedef enum {
    PAGE_SUMMARY,
    PAGE_PACK,
    PAGE_CELLS,
    PAGE_CELL,
    PAGE_ERRORS,
    PAGE_SETTINGS,
    PAGE_LOG,
    PAGE_NODATA,
} page_kind_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *voltage;
    lv_obj_t *voltage_unit;
    lv_obj_t *current;
    lv_obj_t *soc_bar;
    lv_obj_t *soc;
    lv_obj_t *diff;
    lv_obj_t *charging;
    lv_obj_t *discharging;
    lv_obj_t *balancing;
} summary_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *temp1;
    lv_obj_t *temp2;
    lv_obj_t *temp_mos;
    lv_obj_t *cycles;
    lv_obj_t *balancing;
} pack_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *numbers[JKBMS_MAX_CELLS];
    lv_obj_t *voltages[JKBMS_MAX_CELLS];
} cells_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *title;
    lv_obj_t *voltage;
    lv_obj_t *voltage_unit;
    lv_obj_t *resistance;
    lv_obj_t *resistance_unit;
    lv_obj_t *ohm;
    lv_obj_t *wire_warning;
} cell_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *title;
    lv_obj_t *list;
} errors_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *title;
    lv_obj_t *names[SETTINGS_ROWS];
    lv_obj_t *values[SETTINGS_ROWS];
} settings_page_t;

typedef struct {
    char name[16];
    char value[16];
    uint32_t color;
} setting_row_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *title;
    lv_obj_t *times[LOG_SLOTS];
    lv_obj_t *events[LOG_SLOTS];
} log_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *title;
    lv_obj_t *state;
    lv_obj_t *mac;
} nodata_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *status;
    lv_obj_t *rows[PICKER_ROWS];
    lv_obj_t *names[PICKER_ROWS];
    lv_obj_t *details[PICKER_ROWS];
    lv_obj_t *hint;
} picker_page_t;

static struct {
    lv_obj_t *header;
    lv_obj_t *header_title;
    lv_obj_t *header_state;
    summary_page_t summary;
    pack_page_t pack;
    cells_page_t cells;
    cell_page_t cell_page;
    errors_page_t errors;
    settings_page_t settings;
    log_page_t log;
    nodata_page_t nodata;
    picker_page_t picker;
    lv_obj_t *chart;
    lv_obj_t *bars[JKBMS_MAX_CELLS];
    ui_mode_t mode;
    int bms;             // current BMS index
    int page;            // page index within the current BMS, see page_kind()
    bool blink;          // alarm blink phase
    // picker
    jkbms_found_t found[PICKER_ROWS];
    int found_count;
    int cursor;
    bool picker_touched;
    int64_t picker_start_us;
} m_ui;

extern const lv_image_dsc_t ohm_glyph;

static void create_picker_page(lv_obj_t *scr);
static void ui_fetch_status(void);
static void enter_monitor(void);
static int first_free_slot(void);

static jkbms_status_t m_status[JKBMS_DEVICES_MAX];


static lv_obj_t* make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}


static lv_obj_t* make_container(lv_obj_t *parent, int y, int h) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_pos(cont, 0, y);
    lv_obj_set_size(cont, LCD_H_RES, h);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    return cont;
}


static void set_visible(lv_obj_t *obj, bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}


static void set_text(lv_obj_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_text(lv_obj_t *label, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    lv_label_set_text(label, buf);
}


static void set_color(lv_obj_t *label, uint32_t color) {
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}


/*---------------------------------------------------------------
        Page layout
---------------------------------------------------------------*/

static void create_summary_page(lv_obj_t *scr) {
    summary_page_t *p = &m_ui.summary;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    p->voltage = make_label(p->cont, &lv_font_montserrat_40, COLOR_TEXT, 4, 0);
    p->voltage_unit = make_label(p->cont, &lv_font_montserrat_28, COLOR_TEXT, 0, 0);
    lv_label_set_text(p->voltage_unit, "V");
    p->current = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 50);

    p->soc_bar = lv_bar_create(p->cont);
    lv_obj_set_pos(p->soc_bar, 4, 84);
    lv_obj_set_size(p->soc_bar, 76, 16);
    lv_bar_set_range(p->soc_bar, 0, 100);
    lv_obj_set_style_bg_color(p->soc_bar, lv_color_hex(COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(p->soc_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
    p->soc = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 86, 80);

    p->diff = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 108);
    p->charging = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 134);
    p->discharging = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 160);
    p->balancing = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 186);
}


static void create_pack_page(lv_obj_t *scr) {
    pack_page_t *p = &m_ui.pack;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    lv_obj_t **rows[] = { &p->temp1, &p->temp2, &p->temp_mos, &p->cycles, &p->balancing };
    for (int i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        *rows[i] = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 4 + i * 30);
    }
}


static void create_cells_page(lv_obj_t *scr) {
    cells_page_t *p = &m_ui.cells;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    for (int i = 0; i < JKBMS_MAX_CELLS; i++) {
        p->numbers[i] = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 0, 0);
        p->voltages[i] = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 0, 0);
    }
}


static void create_cell_page(lv_obj_t *scr) {
    cell_page_t *p = &m_ui.cell_page;
    p->cont = make_container(scr, HEADER_H, CHART_Y - HEADER_H);
    p->title = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 4);
    p->voltage = make_label(p->cont, &lv_font_montserrat_40, COLOR_TEXT, 2, 36);
    p->voltage_unit = make_label(p->cont, &lv_font_montserrat_28, COLOR_TEXT, 0, 0);
    lv_label_set_text(p->voltage_unit, "V");
    p->resistance = make_label(p->cont, &lv_font_montserrat_40, COLOR_TEXT, 2, 100);
    p->resistance_unit = make_label(p->cont, &lv_font_montserrat_28, COLOR_TEXT, 0, 0);
    lv_label_set_text(p->resistance_unit, "m");
    p->ohm = lv_image_create(p->cont);
    lv_image_set_src(p->ohm, &ohm_glyph);
    lv_obj_set_style_image_recolor(p->ohm, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(p->ohm, LV_OPA_COVER, 0);
    p->wire_warning = make_label(p->cont, &lv_font_montserrat_20, COLOR_ERROR, 4, 150);
    lv_label_set_text(p->wire_warning, LV_SYMBOL_WARNING " Check wire!");
}


static void create_errors_page(lv_obj_t *scr) {
    errors_page_t *p = &m_ui.errors;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    p->title = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 4);
    lv_label_set_text(p->title, "Errors");
    p->list = make_label(p->cont, &lv_font_montserrat_14, COLOR_TEXT, 4, 34);
    lv_obj_set_width(p->list, LCD_H_RES - 8);
    lv_label_set_long_mode(p->list, LV_LABEL_LONG_WRAP);
}


static void create_settings_page(lv_obj_t *scr) {
    settings_page_t *p = &m_ui.settings;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    p->title = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 2);
    for (int i = 0; i < SETTINGS_ROWS; i++) {
        const int y = 28 + i * SETTINGS_ROW_H;
        p->names[i] = make_label(p->cont, &lv_font_montserrat_20, COLOR_DIM, 4, y);
        p->values[i] = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, y + 22);
    }
}


static void create_log_page(lv_obj_t *scr) {
    log_page_t *p = &m_ui.log;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    p->title = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 2);
    // events are stacked in a column; their heights depend on the wrapped text
    lv_obj_t *list = make_container(p->cont, LOG_TITLE_H, CONTENT_H - LOG_TITLE_H);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < LOG_SLOTS; i++) {
        p->times[i] = make_label(list, &lv_font_montserrat_14, COLOR_DIM, 0, 0);
        lv_obj_set_style_pad_left(p->times[i], 4, 0);
        p->events[i] = make_label(list, &lv_font_montserrat_14, COLOR_TEXT, 0, 0);
        lv_obj_set_width(p->events[i], LCD_H_RES);
        lv_obj_set_style_pad_hor(p->events[i], 4, 0);
        lv_obj_set_style_pad_bottom(p->events[i], LOG_GAP, 0);
        lv_label_set_long_mode(p->events[i], LV_LABEL_LONG_WRAP);
    }
}


static void create_nodata_page(lv_obj_t *scr) {
    nodata_page_t *p = &m_ui.nodata;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    p->title = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 20);
    p->state = make_label(p->cont, &lv_font_montserrat_14, COLOR_WARN, 4, 52);
    p->mac = make_label(p->cont, &lv_font_montserrat_14, COLOR_DIM, 4, 76);
}


static void create_chart(lv_obj_t *scr) {
    m_ui.chart = make_container(scr, CHART_Y, CHART_H);
    lv_obj_set_x(m_ui.chart, CHART_X);
    lv_obj_set_width(m_ui.chart, CHART_W);
    for (int i = 0; i < JKBMS_MAX_CELLS; i++) {
        lv_obj_t *bar = lv_obj_create(m_ui.chart);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_BAR), 0);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        m_ui.bars[i] = bar;
    }
}


static void hide_pages(void) {
    set_visible(m_ui.summary.cont, false);
    set_visible(m_ui.pack.cont, false);
    set_visible(m_ui.cells.cont, false);
    set_visible(m_ui.cell_page.cont, false);
    set_visible(m_ui.errors.cont, false);
    set_visible(m_ui.settings.cont, false);
    set_visible(m_ui.log.cont, false);
    set_visible(m_ui.nodata.cont, false);
    set_visible(m_ui.picker.cont, false);
    set_visible(m_ui.chart, false);
}


void ui_init(lv_display_t *disp) {
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = make_container(scr, 0, HEADER_H);
    m_ui.header = header;
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    m_ui.header_title = make_label(header, &lv_font_montserrat_20, COLOR_TEXT, 6, 0);
    lv_obj_align(m_ui.header_title, LV_ALIGN_LEFT_MID, 6, 0);
    m_ui.header_state = lv_obj_create(header);
    lv_obj_remove_style_all(m_ui.header_state);
    lv_obj_set_size(m_ui.header_state, 14, 14);
    lv_obj_set_style_radius(m_ui.header_state, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m_ui.header_state, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m_ui.header_state, lv_color_hex(COLOR_WARN), 0);
    lv_obj_align(m_ui.header_state, LV_ALIGN_RIGHT_MID, -6, 0);

    create_summary_page(scr);
    create_pack_page(scr);
    create_cells_page(scr);
    create_cell_page(scr);
    create_errors_page(scr);
    create_settings_page(scr);
    create_log_page(scr);
    create_nodata_page(scr);
    create_picker_page(scr);
    create_chart(scr);
    hide_pages();

    ui_fetch_status();
    m_ui.picker_start_us = esp_timer_get_time();
    // skip the picker when a BMS was configured in menuconfig or picked before
    bool any_assigned = false;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        any_assigned |= m_status[i].link != JKBMS_LINK_UNASSIGNED;
    }
    if (any_assigned) {
        enter_monitor();
    } else {
        m_ui.mode = UI_MODE_PICKER;
        jkbms_ble_set_discovery(true);
    }
    lvgl_port_unlock();
}


/*---------------------------------------------------------------
        Page model
---------------------------------------------------------------*/

static bool has_data(int bms) {
    const jkbms_status_t *st = &m_status[bms];
    return st->cells_valid && st->cells.cell_count > 0;
}


static void add_row(setting_row_t *rows, int *count, uint32_t color, const char *name, const char *fmt, ...)
        __attribute__((format(printf, 5, 6)));

static void add_row(setting_row_t *rows, int *count, uint32_t color, const char *name, const char *fmt, ...) {
    if (*count >= SETTINGS_MAX) {
        return;
    }
    setting_row_t *row = &rows[(*count)++];
    snprintf(row->name, sizeof(row->name), "%s", name);
    va_list args;
    va_start(args, fmt);
    vsnprintf(row->value, sizeof(row->value), fmt, args);
    va_end(args);
    row->color = color;
}


static void add_switch(setting_row_t *rows, int *count, const char *name, bool on) {
    // a disabled charge/discharge/balancer switch is a likely cause of trouble
    add_row(rows, count, on ? COLOR_TEXT : COLOR_ERROR, name, "%s", on ? "ON" : "OFF");
}


/**
 * Lists the BMS settings, the ones relevant to discharge cut-offs first.
 */
static int settings_rows(const jkbms_status_t *st, setting_row_t *rows) {
    const jkbms_settings_t *s = &st->settings;
    const bool is_32s = st->proto == JKBMS_PROTO_JK02_32S;
    int n = 0;
    if (!st->settings_valid) {
        return 0;
    }
    add_switch(rows, &n, "Charge", s->charge_enabled);
    add_switch(rows, &n, "Discharge", s->discharge_enabled);
    add_switch(rows, &n, "Balancer", s->balancer_enabled);
    add_row(rows, &n, COLOR_TEXT, "Cells", "%u", s->cell_count);
    add_row(rows, &n, COLOR_TEXT, "Capacity", "%.0f Ah", s->capacity);
    add_row(rows, &n, COLOR_TEXT, "Cell UVP", "%.3f V", s->cell_uvp);
    add_row(rows, &n, COLOR_TEXT, "UVP rec", "%.3f V", s->cell_uvp_recovery);
    if (is_32s) {
        add_row(rows, &n, COLOR_TEXT, "SOC 0%", "%.3f V", s->cell_soc0);
    }
    add_row(rows, &n, COLOR_TEXT, "Power off", "%.3f V", s->power_off);
    add_row(rows, &n, COLOR_TEXT, "Cell OVP", "%.3f V", s->cell_ovp);
    add_row(rows, &n, COLOR_TEXT, "OVP rec", "%.3f V", s->cell_ovp_recovery);
    if (is_32s) {
        add_row(rows, &n, COLOR_TEXT, "SOC 100%", "%.3f V", s->cell_soc100);
        add_row(rows, &n, COLOR_TEXT, "Sleep", "%s", s->smart_sleep_enabled ? "ON" : "OFF");
        add_row(rows, &n, COLOR_TEXT, "Sleep delay", "%u h", s->smart_sleep_delay_h);
    }
    add_row(rows, &n, COLOR_TEXT, "Sleep V", "%.3f V", s->smart_sleep);
    add_row(rows, &n, COLOR_TEXT, "Bal start", "%.3f V", s->balance_start);
    add_row(rows, &n, COLOR_TEXT, "Bal trigger", "%.0f mV", s->balance_trigger * 1000);
    add_row(rows, &n, COLOR_TEXT, "Bal max", "%.1f A", s->max_balance_current);
    add_row(rows, &n, COLOR_TEXT, "Chg max", "%.0f A", s->max_charge_current);
    add_row(rows, &n, COLOR_TEXT, "Dsg max", "%.0f A", s->max_discharge_current);
    add_row(rows, &n, COLOR_TEXT, "Chg OTP", "%.0f/%.0f" "\xC2\xB0" "C", s->charge_otp, s->charge_otp_recovery);
    add_row(rows, &n, COLOR_TEXT, "Chg UTP", "%.0f/%.0f" "\xC2\xB0" "C", s->charge_utp, s->charge_utp_recovery);
    add_row(rows, &n, COLOR_TEXT, "Dsg OTP", "%.0f/%.0f" "\xC2\xB0" "C", s->discharge_otp,
            s->discharge_otp_recovery);
    if (is_32s) {
        add_row(rows, &n, COLOR_TEXT, "Dsg UTP", "%.0f/%.0f" "\xC2\xB0" "C", s->discharge_utp,
                s->discharge_utp_recovery);
    }
    add_row(rows, &n, COLOR_TEXT, "MOS OTP", "%.0f/%.0f" "\xC2\xB0" "C", s->mosfet_otp, s->mosfet_otp_recovery);
    if (is_32s) {
        add_row(rows, &n, COLOR_TEXT, "Heating", "%s", s->heating_enabled ? "ON" : "OFF");
    }
    return n;
}


static int settings_pages(int bms) {
    static setting_row_t rows[SETTINGS_MAX];  // static: called under the LVGL lock only
    const int n = settings_rows(&m_status[bms], rows);
    return n > 0 ? (n + SETTINGS_ROWS - 1) / SETTINGS_ROWS : 1;
}


/**
 * Formats logbook event n (0 = newest): its number and age, and its description.
 */
static void log_event_text(const jkbms_status_t *st, int n, char *time, size_t time_size, char *label,
                           size_t label_size) {
    const jkbms_log_entry_t *e = &st->logbook[st->logbook_count - 1 - n];
    const unsigned long number = st->logbook_total - n;
    // timestamps count seconds since the BMS was first powered, like its uptime
    const uint32_t age = (st->info.uptime_s > e->timestamp_s) ? st->info.uptime_s - e->timestamp_s : 0;
    if (age < 3600) {
        snprintf(time, time_size, "#%lu  %lum ago", number, (unsigned long) (age / 60));
    } else if (age < 86400) {
        snprintf(time, time_size, "#%lu  %luh %02lum ago", number, (unsigned long) (age / 3600),
                 (unsigned long) (age % 3600 / 60));
    } else {
        snprintf(time, time_size, "#%lu  %lud %luh ago", number, (unsigned long) (age / 86400),
                 (unsigned long) (age % 86400 / 3600));
    }
    jkbms_log_label(e->code, label, label_size);
}


/**
 * Splits the logbook, newest first, into pages of whole events.
 * `first` receives the index of the first event of every page.
 */
static int log_paginate(const jkbms_status_t *st, uint8_t first[JKBMS_LOGBOOK_FRAME_ENTRIES]) {
    const int available = CONTENT_H - LOG_TITLE_H;
    int pages = 0;
    int used = available;
    int on_page = LOG_SLOTS;
    first[0] = 0;
    for (int n = 0; n < st->logbook_count; n++) {
        char time[32], label[64];
        log_event_text(st, n, time, sizeof(time), label, sizeof(label));
        lv_point_t size;
        lv_text_get_size(&size, label, &lv_font_montserrat_14, 0, 0, LCD_H_RES - 8, LV_TEXT_FLAG_NONE);
        const int height = LOG_TIME_H + size.y + LOG_GAP;
        if (used + height > available || on_page == LOG_SLOTS) {
            first[pages++] = n;
            used = 0;
            on_page = 0;
        }
        used += height;
        on_page++;
    }
    return pages > 0 ? pages : 1;
}


static int log_pages(int bms) {
    uint8_t first[JKBMS_LOGBOOK_FRAME_ENTRIES];
    return log_paginate(&m_status[bms], first);
}


static int page_count(int bms) {
    if (!has_data(bms)) {
        return 1;
    }
    // summary + cells + pack + N cell pages + errors + settings pages + logbook pages
    return 3 + m_status[bms].cells.cell_count + 1 + settings_pages(bms) + log_pages(bms);
}


/**
 * Returns the kind of the page and its index within that kind
 * (cell index or logbook page index).
 */
static page_kind_t page_kind(int bms, int page, int *index) {
    *index = 0;
    if (!has_data(bms)) {
        return PAGE_NODATA;
    }
    const int cells = m_status[bms].cells.cell_count;
    if (page == 0) {
        return PAGE_SUMMARY;
    }
    if (page == 1) {
        return PAGE_CELLS;
    }
    if (page == 2) {
        return PAGE_PACK;
    }
    if (page < 3 + cells) {
        *index = page - 3;
        return PAGE_CELL;
    }
    if (page == 3 + cells) {
        return PAGE_ERRORS;
    }
    const int settings = settings_pages(bms);
    if (page < 4 + cells + settings) {
        *index = page - 4 - cells;
        return PAGE_SETTINGS;
    }
    *index = page - 4 - cells - settings;
    return PAGE_LOG;
}


/*---------------------------------------------------------------
        Rendering
---------------------------------------------------------------*/

/**
 * Returns true if the BMS reports an active error. "Battery is fully
 * charged" (bit 4) is a status, not an error.
 */
static bool has_error(const jkbms_status_t *st) {
    if (!st->cells_valid) {
        return false;
    }
    for (uint8_t bit = 0; bit < 32; bit++) {
        if ((st->cells.errors & (1UL << bit)) && bit != 4 && jkbms_error_label(bit) != NULL) {
            return true;
        }
    }
    return false;
}


static void update_header(const jkbms_status_t *st, bool stale) {
    // green: fresh data (short reconnects every ~30 s keep it fresh),
    // red: data went stale, orange: never got data yet
    uint32_t color = COLOR_WARN;
    if (st->cells_valid && !stale) {
        color = COLOR_OK;
    } else if (st->cells_valid) {
        color = COLOR_ERROR;
    }
    set_text(m_ui.header_title, "BMS %d", m_ui.bms + 1);
    lv_obj_align(m_ui.header_title, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(m_ui.header_state, lv_color_hex(color), 0);
    set_visible(m_ui.header_state, true);

    bool alarm = false;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        alarm |= has_error(&m_status[i]);
    }
    const bool red = alarm && m_ui.blink;
    lv_obj_set_style_bg_color(m_ui.header, lv_color_hex(red ? COLOR_ERROR : COLOR_HEADER), 0);
}


static uint32_t cell_color(const jkbms_cell_info_t *ci, int cell) {
    if (ci->delta_cell_voltage > 0 && cell == ci->max_voltage_cell) {
        return COLOR_MAX;
    }
    if (ci->delta_cell_voltage > 0 && cell == ci->min_voltage_cell) {
        return COLOR_MIN;
    }
    return COLOR_TEXT;
}


static void update_chart(const jkbms_cell_info_t *ci, int selected) {
    const int n = ci->cell_count;
    set_visible(m_ui.chart, true);
    // stretch the min..max range so that small imbalances stay visible
    float lo = ci->min_cell_voltage;
    float hi = ci->max_cell_voltage;
    const float span_min = 0.020f;
    if (hi - lo < span_min) {
        const float mid = (hi + lo) / 2;
        lo = mid - span_min / 2;
        hi = mid + span_min / 2;
    }
    lo -= (hi - lo) * 0.25f;

    const int gap = (n <= 16) ? 2 : 1;
    const int bar_w = (CHART_W - gap * (n - 1)) / n;
    const int x0 = (CHART_W - (bar_w * n + gap * (n - 1))) / 2;
    for (int i = 0; i < JKBMS_MAX_CELLS; i++) {
        lv_obj_t *bar = m_ui.bars[i];
        if (i >= n) {
            set_visible(bar, false);
            continue;
        }
        float frac = (ci->cell_voltage[i] - lo) / (hi - lo);
        frac = fminf(fmaxf(frac, 0.05f), 1.0f);
        const int h = (int) (frac * CHART_H);
        lv_obj_set_pos(bar, x0 + i * (bar_w + gap), CHART_H - h);
        lv_obj_set_size(bar, bar_w, h);
        lv_obj_set_style_bg_color(bar, lv_color_hex(i == selected ? COLOR_SELECTED : COLOR_BAR), 0);
        set_visible(bar, true);
    }
}


static void set_on_off(lv_obj_t *label, const char *name, bool on) {
    set_color(label, on ? COLOR_OK : COLOR_DIM);
    set_text(label, "%s %s", name, on ? "ON" : "OFF");
}


static void update_summary(const jkbms_status_t *st) {
    summary_page_t *p = &m_ui.summary;
    const jkbms_cell_info_t *ci = &st->cells;

    set_text(p->voltage, "%.2f", ci->total_voltage);
    lv_obj_align_to(p->voltage_unit, p->voltage, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -5);

    uint32_t color = COLOR_TEXT;
    if (ci->current > 0.05f) {
        color = COLOR_OK;
    } else if (ci->current < -0.05f) {
        color = COLOR_WARN;
    }
    set_color(p->current, color);
    // keep "A W" on one line of the 135 px wide screen
    set_text(p->current, fabsf(ci->current) < 10 ? "%+.2fA %+.0fW" : "%+.1fA %+.0fW", ci->current, ci->power);

    lv_bar_set_value(p->soc_bar, ci->soc, LV_ANIM_OFF);
    uint32_t soc_color = ci->soc > 30 ? COLOR_OK : (ci->soc > 10 ? COLOR_WARN : COLOR_ERROR);
    lv_obj_set_style_bg_color(p->soc_bar, lv_color_hex(soc_color), LV_PART_INDICATOR);
    set_text(p->soc, "%u%%", ci->soc);

    set_text(p->diff, "DIFF %.0f mV", ci->delta_cell_voltage * 1000);
    const bool balancing = ci->balancer_action == JKBMS_BALANCER_CHARGE
            || ci->balancer_action == JKBMS_BALANCER_DISCHARGE;
    set_on_off(p->charging, "CHG", ci->charging);
    set_on_off(p->discharging, "DSG", ci->discharging);
    set_on_off(p->balancing, "BAL", balancing);
}


static void update_pack(const jkbms_status_t *st) {
    pack_page_t *p = &m_ui.pack;
    const jkbms_cell_info_t *ci = &st->cells;
    set_text(p->temp1, "T1  %.1f" "\xC2\xB0" "C", ci->temperature[0]);
    set_text(p->temp2, "T2  %.1f" "\xC2\xB0" "C", ci->temperature[1]);
    set_text(p->temp_mos, "MOS %.1f" "\xC2\xB0" "C", ci->mosfet_temperature);
    set_text(p->cycles, "Cycles %lu", (unsigned long) ci->cycles);
    set_text(p->balancing, "BAL %.2f A", ci->balancing_current);
}


static void update_cells(const jkbms_status_t *st) {
    cells_page_t *p = &m_ui.cells;
    const jkbms_cell_info_t *ci = &st->cells;
    const int n = ci->cell_count;
    const int cols = (n <= 8) ? 1 : (n <= 24 ? 2 : 3);
    const int rows = (n + cols - 1) / cols;
    const lv_font_t *font = (cols == 1) ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
    int row_h = (CONTENT_H - 4) / rows;
    if (row_h > 26) {
        row_h = 26;
    }
    const int col_w = LCD_H_RES / cols;

    // separate labels: Montserrat digits are proportional, so padding with
    // spaces would not line up the voltages
    const int number_w = (cols == 1) ? 36 : 22;

    for (int i = 0; i < JKBMS_MAX_CELLS; i++) {
        lv_obj_t *number = p->numbers[i];
        lv_obj_t *voltage = p->voltages[i];
        if (i >= n) {
            set_visible(number, false);
            set_visible(voltage, false);
            continue;
        }
        const int x = 4 + (i / rows) * col_w;
        const int y = 4 + (i % rows) * row_h;
        const uint32_t color = cell_color(ci, i + 1);
        lv_obj_set_style_text_font(number, font, 0);
        lv_obj_set_style_text_font(voltage, font, 0);
        lv_obj_set_pos(number, x, y);
        lv_obj_set_pos(voltage, x + number_w, y);
        set_color(number, color);
        set_color(voltage, color);
        set_text(number, "%d", i + 1);
        if (cols == 1) {
            set_text(voltage, "%.3f V", ci->cell_voltage[i]);
        } else {
            set_text(voltage, "%.3f", ci->cell_voltage[i]);
        }
        set_visible(number, true);
        set_visible(voltage, true);
    }
}


static void update_cell(const jkbms_status_t *st, int i) {
    cell_page_t *p = &m_ui.cell_page;
    const jkbms_cell_info_t *ci = &st->cells;

    set_text(p->title, "Cell %d/%u", i + 1, ci->cell_count);

    set_color(p->voltage, cell_color(ci, i + 1));
    set_text(p->voltage, "%.3f", ci->cell_voltage[i]);
    lv_obj_align_to(p->voltage_unit, p->voltage, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -5);

    // the BMS flags a cell whose balance wire resistance is too high
    const bool wire_warning = (ci->wire_resistance_warning & (1UL << i)) != 0;
    set_color(p->resistance, wire_warning ? COLOR_ERROR : COLOR_TEXT);
    set_visible(p->wire_warning, wire_warning);
    set_text(p->resistance, "%.0f", ci->cell_resistance[i] * 1000);
    lv_obj_align_to(p->resistance_unit, p->resistance, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -5);
    lv_obj_align_to(p->ohm, p->resistance_unit, LV_ALIGN_OUT_RIGHT_BOTTOM, 1, -7);

    update_chart(ci, i);
}


static void update_errors(const jkbms_status_t *st) {
    char text[512] = "";
    size_t pos = 0;
    for (uint8_t bit = 0; bit < 32; bit++) {
        const char *label = jkbms_error_label(bit);
        if ((st->cells.errors & (1UL << bit)) && label != NULL && pos < sizeof(text)) {
            pos += snprintf(text + pos, sizeof(text) - pos, "%s%s", pos ? "\n" : "", label);
        }
    }
    lv_label_set_text(m_ui.errors.list, text);
}


static void update_settings(const jkbms_status_t *st, int settings_page) {
    settings_page_t *p = &m_ui.settings;
    static setting_row_t rows[SETTINGS_MAX];  // static: called under the LVGL lock only
    const int n = settings_rows(st, rows);
    if (n == 0) {
        lv_label_set_text(p->title, "Settings: loading...");
    } else {
        set_text(p->title, "Settings %d/%d", settings_page + 1, settings_pages(m_ui.bms));
    }
    for (int i = 0; i < SETTINGS_ROWS; i++) {
        const int r = settings_page * SETTINGS_ROWS + i;
        const bool visible = r < n;
        set_visible(p->names[i], visible);
        set_visible(p->values[i], visible);
        if (!visible) {
            continue;
        }
        lv_label_set_text(p->names[i], rows[r].name);
        lv_label_set_text(p->values[i], rows[r].value);
        set_color(p->values[i], rows[r].color);
    }
}


static void update_log(const jkbms_status_t *st, int log_page) {
    log_page_t *p = &m_ui.log;
    uint8_t first[JKBMS_LOGBOOK_FRAME_ENTRIES];
    const int pages = log_paginate(st, first);
    const int count = st->logbook_count;
    if (!st->logbook_valid) {
        lv_label_set_text(p->title, "Log: loading...");
    } else if (count == 0) {
        lv_label_set_text(p->title, "Log: empty");
    } else {
        set_text(p->title, "Log %d/%d", log_page + 1, pages);
    }
    const int start = (log_page < pages) ? first[log_page] : count;
    const int end = (log_page + 1 < pages) ? first[log_page + 1] : count;
    for (int i = 0; i < LOG_SLOTS; i++) {
        const int n = start + i;
        const bool visible = n < end;
        set_visible(p->times[i], visible);
        set_visible(p->events[i], visible);
        if (!visible) {
            continue;
        }
        char time[32], label[64];
        log_event_text(st, n, time, sizeof(time), label, sizeof(label));
        lv_label_set_text(p->times[i], time);
        lv_label_set_text(p->events[i], label);
    }
}


static void update_nodata(const jkbms_status_t *st) {
    nodata_page_t *p = &m_ui.nodata;
    set_text(p->title, "BMS %d", m_ui.bms + 1);
    set_text(p->state, "%s...", jkbms_link_name(st->link));
    if (st->mac_known) {
        set_text(p->mac, "%02X:%02X:%02X:%02X:%02X:%02X", st->mac[0], st->mac[1], st->mac[2], st->mac[3],
                 st->mac[4], st->mac[5]);
    } else {
        lv_label_set_text(p->mac, "");
    }
}


static void ui_render(void) {
    const jkbms_status_t *st = &m_status[m_ui.bms];
    const bool stale = (esp_timer_get_time() - st->cells_updated_us) > STALE_US;
    if (m_ui.page >= page_count(m_ui.bms)) {
        m_ui.page = 0;
    }

    hide_pages();
    update_header(st, stale);
    int index;
    switch (page_kind(m_ui.bms, m_ui.page, &index)) {
    case PAGE_SUMMARY:
        update_summary(st);
        set_visible(m_ui.summary.cont, true);
        break;
    case PAGE_PACK:
        update_pack(st);
        set_visible(m_ui.pack.cont, true);
        break;
    case PAGE_CELLS:
        update_cells(st);
        set_visible(m_ui.cells.cont, true);
        break;
    case PAGE_CELL:
        update_cell(st, index);
        set_visible(m_ui.cell_page.cont, true);
        break;
    case PAGE_ERRORS:
        update_errors(st);
        set_visible(m_ui.errors.cont, true);
        break;
    case PAGE_SETTINGS:
        update_settings(st, index);
        set_visible(m_ui.settings.cont, true);
        break;
    case PAGE_LOG:
        update_log(st, index);
        set_visible(m_ui.log.cont, true);
        break;
    case PAGE_NODATA:
        update_nodata(st);
        set_visible(m_ui.nodata.cont, true);
        break;
    }
}


/*---------------------------------------------------------------
        Navigation
---------------------------------------------------------------*/

static void ui_fetch_status(void) {
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_ble_get_status(i, &m_status[i]);
    }
}


static bool slot_assigned(int bms) {
    return m_status[bms].link != JKBMS_LINK_UNASSIGNED;
}


/**
 * Moves to the next/previous assigned BMS.
 */
static void step_bms(int step) {
    for (int n = 0; n < JKBMS_DEVICES_MAX; n++) {
        m_ui.bms = (m_ui.bms + JKBMS_DEVICES_MAX + step) % JKBMS_DEVICES_MAX;
        if (slot_assigned(m_ui.bms)) {
            return;
        }
    }
}


static void monitor_navigate(int step) {
    m_ui.page += step;
    if (m_ui.page >= page_count(m_ui.bms)) {
        step_bms(+1);
        m_ui.page = 0;
    } else if (m_ui.page < 0) {
        step_bms(-1);
        m_ui.page = page_count(m_ui.bms) - 1;
    }
    int index;
    if (page_kind(m_ui.bms, m_ui.page, &index) == PAGE_LOG && index == 0 && step > 0) {
        // refresh the logbook whenever it is opened
        jkbms_ble_request_logbook(m_ui.bms);
    }
}


/*---------------------------------------------------------------
        Picker: select the BMS to monitor
---------------------------------------------------------------*/

static void create_picker_page(lv_obj_t *scr) {
    picker_page_t *p = &m_ui.picker;
    p->cont = make_container(scr, HEADER_H, LCD_V_RES - HEADER_H);
    p->status = make_label(p->cont, &lv_font_montserrat_14, COLOR_DIM, 4, 2);
    for (int i = 0; i < PICKER_ROWS; i++) {
        lv_obj_t *row = make_container(p->cont, 22 + i * PICKER_ROW_H, PICKER_ROW_H - 2);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_HEADER), 0);
        lv_obj_set_style_radius(row, 4, 0);
        p->rows[i] = row;
        p->names[i] = make_label(row, &lv_font_montserrat_14, COLOR_TEXT, 4, 1);
        lv_obj_set_width(p->names[i], LCD_H_RES - 8);
        lv_label_set_long_mode(p->names[i], LV_LABEL_LONG_DOT);
        p->details[i] = make_label(row, &lv_font_montserrat_12, COLOR_DIM, 4, 18);
    }
    p->hint = make_label(p->cont, &lv_font_montserrat_12, COLOR_DIM, 4, LCD_V_RES - HEADER_H - 18);
    lv_label_set_text(p->hint, LV_SYMBOL_LEFT " next   select " LV_SYMBOL_RIGHT);
}


static int assigned_count(void) {
    int count = 0;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        count += slot_assigned(i);
    }
    return count;
}


static int first_free_slot(void) {
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        if (!slot_assigned(i)) {
            return i;
        }
    }
    return -1;
}


/**
 * Refreshes the list of found BMS that are not assigned yet.
 */
static void picker_fetch(void) {
    jkbms_found_t found[PICKER_ROWS + JKBMS_DEVICES_MAX];
    const int n = jkbms_ble_get_found(found, sizeof(found) / sizeof(found[0]), PICKER_MAX_AGE_US);
    m_ui.found_count = 0;
    for (int i = 0; i < n && m_ui.found_count < PICKER_ROWS; i++) {
        bool taken = false;
        for (int s = 0; s < JKBMS_DEVICES_MAX; s++) {
            taken |= slot_assigned(s) && memcmp(m_status[s].mac, found[i].mac, 6) == 0;
        }
        if (!taken) {
            m_ui.found[m_ui.found_count++] = found[i];
        }
    }
}


static int picker_items(void) {
    // the extra "Done" item finishes the selection once at least one BMS is picked
    return m_ui.found_count + (assigned_count() > 0 ? 1 : 0);
}


static void enter_monitor(void) {
    m_ui.mode = UI_MODE_MONITOR;
    jkbms_ble_set_discovery(false);
    m_ui.bms = 0;
    m_ui.page = 0;
    if (!slot_assigned(0)) {
        step_bms(+1);
    }
}


static void picker_select(const jkbms_found_t *found) {
    const int slot = first_free_slot();
    if (slot < 0 || !jkbms_ble_assign(slot, found->mac)) {
        return;
    }
    ui_fetch_status();
    picker_fetch();
    m_ui.cursor = 0;
    if (first_free_slot() < 0 || m_ui.found_count == 0) {
        enter_monitor();
    }
}


static void picker_button(int step) {
    m_ui.picker_touched = true;
    picker_fetch();
    const int items = picker_items();
    if (items == 0) {
        return;
    }
    if (m_ui.cursor >= items) {
        m_ui.cursor = 0;
    }
    if (step < 0) {
        m_ui.cursor = (m_ui.cursor + 1) % items;
    } else if (m_ui.cursor < m_ui.found_count) {
        picker_select(&m_ui.found[m_ui.cursor]);
    } else {
        enter_monitor();
    }
}


static void picker_tick(void) {
    picker_fetch();
    const int64_t elapsed = esp_timer_get_time() - m_ui.picker_start_us;
    if (!m_ui.picker_touched && assigned_count() == 0 && m_ui.found_count == 1
            && elapsed > CONFIG_UI_AUTO_SELECT_S * 1000000LL) {
        picker_select(&m_ui.found[0]);
    }
}


static void picker_render(void) {
    picker_page_t *p = &m_ui.picker;
    hide_pages();
    set_visible(p->cont, true);

    const int slot = first_free_slot();
    set_text(m_ui.header_title, "Pick BMS %d", slot + 1);
    lv_obj_align(m_ui.header_title, LV_ALIGN_LEFT_MID, 6, 0);
    set_visible(m_ui.header_state, false);

    if (m_ui.found_count == 0) {
        lv_label_set_text(p->status, "Searching for JK BMS...");
    } else if (!m_ui.picker_touched && assigned_count() == 0 && m_ui.found_count == 1) {
        const int64_t elapsed = esp_timer_get_time() - m_ui.picker_start_us;
        const int left = CONFIG_UI_AUTO_SELECT_S - (int) (elapsed / 1000000LL);
        set_text(p->status, "Auto-select in %d s", left > 0 ? left : 0);
    } else {
        set_text(p->status, "Found %d BMS", m_ui.found_count);
    }

    const int items = picker_items();
    if (m_ui.cursor >= items) {
        m_ui.cursor = 0;
    }
    for (int i = 0; i < PICKER_ROWS; i++) {
        const bool visible = i < items;
        set_visible(p->rows[i], visible);
        if (!visible) {
            continue;
        }
        lv_obj_set_style_bg_opa(p->rows[i], i == m_ui.cursor ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(p->rows[i], i == m_ui.cursor ? 1 : 0, 0);
        lv_obj_set_style_border_color(p->rows[i], lv_color_hex(COLOR_SELECTED), 0);
        if (i < m_ui.found_count) {
            const jkbms_found_t *f = &m_ui.found[i];
            set_text(p->names[i], "%s", f->name[0] ? f->name : "(no name)");
            set_text(p->details[i], "%02X:%02X:%02X:%02X:%02X:%02X %d", f->mac[0], f->mac[1], f->mac[2], f->mac[3],
                     f->mac[4], f->mac[5], f->rssi);
        } else {
            lv_label_set_text(p->names[i], LV_SYMBOL_OK " Done");
            set_text(p->details[i], "monitor %d BMS", assigned_count());
        }
    }
}


/*---------------------------------------------------------------
        Public API
---------------------------------------------------------------*/

void ui_on_button(int step) {
    if (step == 0) {
        // both buttons held: forget the saved BMS and pick again
        jkbms_ble_forget_saved();
        esp_restart();
    }
    lvgl_port_lock(0);
    ui_fetch_status();
    if (m_ui.mode == UI_MODE_PICKER) {
        picker_button(step);
    } else {
        monitor_navigate(step);
    }
    if (m_ui.mode == UI_MODE_PICKER) {
        picker_render();
    } else {
        ui_render();
    }
    lvgl_port_unlock();
}


void ui_refresh(void) {
    lvgl_port_lock(0);
    m_ui.blink = !m_ui.blink;
    ui_fetch_status();
    if (m_ui.mode == UI_MODE_PICKER) {
        picker_tick();
    }
    if (m_ui.mode == UI_MODE_PICKER) {
        picker_render();
    } else {
        ui_render();
    }
    lvgl_port_unlock();
}
