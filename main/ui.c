/*
 * ui.c
 *
 * Pages of every BMS, in order:
 *   summary, all cells at a glance, pack (temperatures, cycles, balancing
 *   current), one page per cell, active errors, settings (several pages),
 *   logbook (several pages, newest events first), board (firmware, Wi-Fi and
 *   the address of the firmware update page).
 * The header blinks red while any BMS reports an active error.
 * The left/right buttons step through all pages of all BMS with wrap-around.
 */

#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#if CONFIG_UI_SCREENSHOT_CONSOLE
#include "others/snapshot/lv_snapshot.h"
#endif
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "buttons.h"
#include "jkbms_ble.h"
#include "lcd.h"
#include "remote.h"
#include "uploader.h"
#include "webserver.h"

#define COLOR_BG          0x000000
#define COLOR_HEADER      0x202830
#define COLOR_TEXT        0xFFFFFF
#define COLOR_DIM         0x8A949E
#define COLOR_OK          0x3DDC84
#define COLOR_WARN        0xFFA726
#define COLOR_READ        0xFFD600           // "read", waiting to be sent
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

#define SPINNER_SIZE      100

#define UI_REFRESH_MS     500
#if CONFIG_UPLOAD_COLLECT_FIRST
#define CONFIG_UPLOAD_COLLECT_FIRST_ENABLED  true
#else
#define CONFIG_UPLOAD_COLLECT_FIRST_ENABLED  false
#endif
#define RESULT_SHOW_US    (7 * 1000000LL)   // the first-upload check mark or cross
#define READ_SHOW_US      (5 * 1000000LL)   // the yellow "read" check mark
#define RESULT_RING_D     104
#define RESULT_DISC_D     88
#define RESULT_Y          28                 // top of the ring

// LiFePO4 cell voltage range of the per-cell bar chart before the BMS settings arrive
#define CHART_DEFAULT_LO  2.5f
#define CHART_DEFAULT_HI  3.65f

#define LOG_SLOTS         8          // max logbook events per page
#define LOG_TITLE_H       26
#define LOG_TIME_H        17         // line height of the 14 px font
#define LOG_GAP           6          // space between events

#define SETTINGS_ROWS     4          // settings per page, name and value on two lines
#define SETTINGS_ROW_H    47
#define SETTINGS_MAX      48

#define BOARD_ROWS        3          // firmware, Wi-Fi, update address
#define BOARD_ROW_H       62

#define PICKER_ROWS       5
#define PICKER_HINT_H     22         // the hint line below the list
#define PICKER_MAX_AGE_US (60 * 1000000LL)  // results of a search this recent are reused

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
    PAGE_BOARD,
    PAGE_NODATA,
} page_kind_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *voltage;
    lv_obj_t *voltage_unit;
    lv_obj_t *current;
    lv_obj_t *power;
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
    lv_obj_t *spinner;
    lv_obj_t *message;
    lv_obj_t *hint;
} nodata_page_t;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *spinner;
    lv_obj_t *message;
    lv_obj_t *message_hint;
    lv_obj_t *list;
    lv_obj_t *rows[PICKER_ROWS];
    lv_obj_t *names[PICKER_ROWS];
    lv_obj_t *macs[PICKER_ROWS];
    lv_obj_t *rssis[PICKER_ROWS];
    lv_obj_t *hint;
} picker_page_t;

static struct {
    lv_obj_t *header;
    lv_obj_t *header_title;
    lv_obj_t *header_state;
    lv_obj_t *header_wifi;
    lv_obj_t *header_wifi_cross;
    lv_obj_t *header_bar;    // sending progress, in place of the title, the dot and the Wi-Fi icon
    summary_page_t summary;
    pack_page_t pack;
    cells_page_t cells;
    cell_page_t cell_page;
    errors_page_t errors;
    settings_page_t settings;
    settings_page_t board;
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
    bool picker_manual;      // opened by the user: always show the list
    bool picker_board;       // no BMS found: the board page instead of "no BMS"
    int64_t picker_start_us;
    lv_obj_t *no_bms;        // full-screen "no BMS" image
    // firmware update progress over everything
    lv_obj_t *ota_cont;
    lv_obj_t *ota_label;
    lv_obj_t *ota_bar;
    // first upload result: a green check mark or a red cross in a double circle
    lv_obj_t *result_cont;
    lv_obj_t *result_ring;
    lv_obj_t *result_disc;
    lv_obj_t *result_check;
    lv_obj_t *result_cross[2];
    lv_obj_t *result_text;
    int64_t result_until_us;
    bool anonymize;          // screenshots: show made-up BMS names and MACs
} m_ui;

extern const lv_image_dsc_t ohm_glyph;
extern const lv_image_dsc_t sent_text;
extern const lv_image_dsc_t read_text;

static void create_picker_page(lv_obj_t *scr);
static void ui_fetch_status(void);
static void enter_monitor(void);
static int first_free_slot(void);
static int assigned_count(void);

static jkbms_status_t m_status[JKBMS_DEVICES_MAX];
// the settings and the logbook, read while their pages are shown and freed when left
static jkbms_extras_t *m_extras;
static int m_extras_bms = -1;


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
    p->current = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 44);
    p->power = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 66);

    p->soc_bar = lv_bar_create(p->cont);
    lv_obj_set_pos(p->soc_bar, 4, 96);
    lv_obj_set_size(p->soc_bar, 76, 16);
    lv_bar_set_range(p->soc_bar, 0, 100);
    lv_obj_set_style_bg_color(p->soc_bar, lv_color_hex(COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(p->soc_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
    p->soc = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 86, 92);

    p->diff = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 116);
    p->charging = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 139);
    p->discharging = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 162);
    p->balancing = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 185);
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


static void create_board_page(lv_obj_t *scr) {
    settings_page_t *p = &m_ui.board;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    p->title = make_label(p->cont, &lv_font_montserrat_20, COLOR_TEXT, 4, 2);
#if CONFIG_UPLOAD_COLLECT_FIRST
    // the second OTA slot holds the collect-first firmware
    lv_label_set_text(p->title, "Board: collect");
#else
    lv_label_set_text(p->title, "Board");
#endif
    for (int i = 0; i < BOARD_ROWS; i++) {
        const int y = 30 + i * BOARD_ROW_H;
        p->names[i] = make_label(p->cont, &lv_font_montserrat_16, COLOR_DIM, 4, y);
        p->values[i] = make_label(p->cont, &lv_font_montserrat_16, COLOR_TEXT, 4, y + 20);
        lv_obj_set_width(p->values[i], LCD_H_RES - 8);
        lv_label_set_long_mode(p->values[i], LV_LABEL_LONG_WRAP);
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


/**
 * Spinner with an optional error message and hint below it.
 */
static void create_waiting(lv_obj_t *parent, lv_obj_t **spinner, lv_obj_t **message, lv_obj_t **hint) {
    *spinner = lv_spinner_create(parent);
    lv_spinner_set_anim_params(*spinner, 1000, 270);
    lv_obj_set_size(*spinner, SPINNER_SIZE, SPINNER_SIZE);
    lv_obj_set_style_arc_color(*spinner, lv_color_hex(COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(*spinner, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(*spinner, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(*spinner, 12, LV_PART_INDICATOR);

    *message = make_label(parent, &lv_font_montserrat_20, COLOR_ERROR, 4, SPINNER_SIZE + 12);
    *hint = make_label(parent, &lv_font_montserrat_14, COLOR_DIM, 4, SPINNER_SIZE + 40);
    lv_obj_t *labels[] = { *message, *hint };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(labels[i], LCD_H_RES - 8);
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
}


static void create_nodata_page(lv_obj_t *scr) {
    nodata_page_t *p = &m_ui.nodata;
    p->cont = make_container(scr, HEADER_H, CONTENT_H);
    create_waiting(p->cont, &p->spinner, &p->message, &p->hint);
}


/**
 * Full-screen sad page (folded sheet, crossed eyes, frown) with "NO BMS" below,
 * shown when no BMS is found. Drawn with LVGL lines and an arc.
 */
static void create_no_bms(lv_obj_t *scr) {
    lv_obj_t *cont = make_container(scr, 0, LCD_V_RES);
    lv_obj_set_style_bg_color(cont, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);

    static const lv_point_precise_t sheet[] = {
        { 27, 40 }, { 85, 40 }, { 107, 62 }, { 107, 146 }, { 27, 146 }, { 27, 40 },
    };
    static const lv_point_precise_t fold[] = { { 85, 40 }, { 85, 62 }, { 107, 62 } };
    static const lv_point_precise_t eyes[][2] = {
        { { 45, 75 }, { 57, 87 } }, { { 57, 75 }, { 45, 87 } },
        { { 77, 75 }, { 89, 87 } }, { { 89, 75 }, { 77, 87 } },
    };
    const struct { const lv_point_precise_t *points; int count; } lines[] = {
        { sheet, 6 }, { fold, 3 },
        { eyes[0], 2 }, { eyes[1], 2 }, { eyes[2], 2 }, { eyes[3], 2 },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *line = lv_line_create(cont);
        lv_line_set_points(line, lines[i].points, lines[i].count);
        lv_obj_set_style_line_width(line, 5, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }

    // the frown: the top of a circle centred below the mouth
    lv_obj_t *mouth = lv_arc_create(cont);
    lv_obj_set_size(mouth, 44, 44);
    lv_obj_set_pos(mouth, 67 - 22, 106);
    lv_arc_set_bg_angles(mouth, 225, 315);
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(mouth, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(mouth, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(mouth, true, LV_PART_MAIN);
    lv_obj_remove_flag(mouth, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = make_label(cont, &lv_font_montserrat_28, COLOR_TEXT, 0, 0);
    lv_label_set_text(label, "NO BMS");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 166);
    m_ui.no_bms = cont;
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
    set_visible(m_ui.board.cont, false);
    set_visible(m_ui.nodata.cont, false);
    set_visible(m_ui.picker.cont, false);
    set_visible(m_ui.chart, false);
    set_visible(m_ui.no_bms, false);
}


static void refresh_timer_cb(lv_timer_t *timer) {
    ui_refresh();
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
    m_ui.header_wifi = make_label(header, &lv_font_montserrat_16, COLOR_TEXT, 0, 0);
    lv_label_set_text(m_ui.header_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(m_ui.header_wifi, LV_ALIGN_RIGHT_MID, -28, 0);
    // a red slash over the icon while Wi-Fi is down
    static const lv_point_precise_t cross[] = { { 2, 16 }, { 18, 1 } };
    m_ui.header_wifi_cross = lv_line_create(m_ui.header_wifi);
    lv_line_set_points(m_ui.header_wifi_cross, cross, 2);
    lv_obj_set_style_line_width(m_ui.header_wifi_cross, 2, 0);
    lv_obj_set_style_line_color(m_ui.header_wifi_cross, lv_color_hex(COLOR_ERROR), 0);
    lv_obj_set_style_line_rounded(m_ui.header_wifi_cross, true, 0);
    m_ui.header_bar = lv_bar_create(header);
    lv_obj_set_size(m_ui.header_bar, LCD_H_RES - 12, 10);
    lv_obj_center(m_ui.header_bar);
    lv_bar_set_range(m_ui.header_bar, 0, 1000);
    lv_obj_set_style_bg_color(m_ui.header_bar, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m_ui.header_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_ui.header_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
    set_visible(m_ui.header_bar, false);

    create_summary_page(scr);
    create_pack_page(scr);
    create_cells_page(scr);
    create_cell_page(scr);
    create_errors_page(scr);
    create_settings_page(scr);
    create_log_page(scr);
    create_board_page(scr);
    create_nodata_page(scr);
    create_picker_page(scr);
    create_chart(scr);
    create_no_bms(scr);
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
    // refreshed by the LVGL task itself: uploads on a slow network never hold the screen
    lv_timer_create(refresh_timer_cb, UI_REFRESH_MS, NULL);
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
static int settings_rows(jkbms_proto_t proto, const jkbms_settings_t *s, setting_row_t *rows) {
    const bool is_32s = proto == JKBMS_PROTO_JK02_32S;
    int n = 0;
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
    add_row(rows, &n, COLOR_TEXT, "Chg OCP delay", "%lu s", (unsigned long) s->charge_ocp_delay_s);
    add_row(rows, &n, COLOR_TEXT, "Chg OCP rec", "%lu s", (unsigned long) s->charge_ocp_recovery_s);
    add_row(rows, &n, COLOR_TEXT, "Dsg max", "%.0f A", s->max_discharge_current);
    add_row(rows, &n, COLOR_TEXT, "Dsg OCP delay", "%lu s", (unsigned long) s->discharge_ocp_delay_s);
    add_row(rows, &n, COLOR_TEXT, "Dsg OCP rec", "%lu s", (unsigned long) s->discharge_ocp_recovery_s);
    add_row(rows, &n, COLOR_TEXT, "SCP delay", "%lu us", (unsigned long) s->scp_delay_us);
    add_row(rows, &n, COLOR_TEXT, "SCP rec", "%lu s", (unsigned long) s->scp_recovery_s);
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
        add_row(rows, &n, COLOR_TEXT, "RCV / RFV", "%.2f/%.2f V", s->cell_rcv, s->cell_rfv);
        add_row(rows, &n, COLOR_TEXT, "Stored data", "%s", s->timed_stored_data ? "ON" : "OFF");
        add_row(rows, &n, COLOR_TEXT, "Float mode", "%s", s->charging_float_mode ? "ON" : "OFF");
        add_row(rows, &n, COLOR_TEXT, "Precharge", "%u s", s->precharge_s);
    }
    return n;
}


static int settings_pages(int bms) {
    // the number of rows depends on the protocol only, not on the values
    static setting_row_t rows[SETTINGS_MAX];  // static: called under the LVGL lock only
    static const jkbms_settings_t none;
    const int n = settings_rows(m_status[bms].proto, &none, rows);
    return n > 0 ? (n + SETTINGS_ROWS - 1) / SETTINGS_ROWS : 1;
}


static const jkbms_extras_t* extras_of(int bms) {
    return (m_extras != NULL && m_extras_bms == bms) ? m_extras : NULL;
}


/**
 * Reads the settings or the logbook while their pages are shown, and frees
 * them when another page is shown: memory is short.
 */
static void extras_for_page(int bms, page_kind_t kind) {
    const bool wanted = kind == PAGE_SETTINGS || kind == PAGE_LOG;
    if (m_extras != NULL && (!wanted || m_extras_bms != bms)) {
        jkbms_ble_cancel_extras(m_extras);
        free(m_extras);
        m_extras = NULL;
        m_extras_bms = -1;
    }
    if (!wanted) {
        return;
    }
    if (m_extras == NULL) {
        m_extras = calloc(1, sizeof(*m_extras));
        if (m_extras == NULL) {
            return;
        }
        m_extras_bms = bms;
    }
    // each is read once per visit; the BMS beeps on every request
    const bool settings = kind == PAGE_SETTINGS && !m_extras->settings_valid && !m_extras->want_settings;
    const bool logbook = kind == PAGE_LOG && !m_extras->logbook_valid && !m_extras->want_logbook;
    if (settings || logbook) {
        m_extras->want_settings |= settings;
        m_extras->want_logbook |= logbook;
        jkbms_ble_fetch_extras(bms, m_extras);
    }
}


static void format_age(uint32_t age, char *buf, size_t size) {
    if (age < 3600) {
        snprintf(buf, size, "%lum ago", (unsigned long) (age / 60));
    } else if (age < 86400) {
        snprintf(buf, size, "%luh %02lum ago", (unsigned long) (age / 3600), (unsigned long) (age % 3600 / 60));
    } else {
        snprintf(buf, size, "%lud %luh ago", (unsigned long) (age / 86400), (unsigned long) (age % 86400 / 3600));
    }
}


/**
 * Formats logbook event n (0 = newest): its age and its description.
 */
static void log_event_text(const jkbms_status_t *st, const jkbms_extras_t *x, int n, char *time, size_t time_size,
                           char *label, size_t label_size) {
    const jkbms_log_entry_t *e = &x->logbook[x->logbook_count - 1 - n];
    // timestamps count seconds since the BMS was first powered, like its uptime
    const uint32_t now = jkbms_bms_now_s(st);
    format_age(now > e->timestamp_s ? now - e->timestamp_s : 0, time, time_size);
    jkbms_log_label(e->code, label, label_size);
}


/**
 * Splits the logbook, newest first, into pages of whole events.
 * `first` receives the index of the first event of every page.
 */
static int log_paginate(const jkbms_status_t *st, const jkbms_extras_t *x, uint8_t first[JKBMS_LOGBOOK_FRAME_ENTRIES]) {
    const int available = CONTENT_H - LOG_TITLE_H;
    int pages = 0;
    int used = available;
    int on_page = LOG_SLOTS;
    first[0] = 0;
    const int count = (x != NULL && x->logbook_valid) ? x->logbook_count : 0;
    for (int n = 0; n < count; n++) {
        char time[32], label[64];
        log_event_text(st, x, n, time, sizeof(time), label, sizeof(label));
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
    return log_paginate(&m_status[bms], extras_of(bms), first);
}


static int page_count(int bms) {
    if (!has_data(bms)) {
        return 2;  // waiting or "no BMS", board
    }
    // summary + cells + pack + N cell pages + errors + settings pages + logbook pages + board
    return 3 + m_status[bms].cells.cell_count + 1 + settings_pages(bms) + log_pages(bms) + 1;
}


/**
 * Returns the kind of the page and its index within that kind
 * (cell index or logbook page index).
 */
static page_kind_t page_kind(int bms, int page, int *index) {
    *index = 0;
    if (!has_data(bms)) {
        // the board page shows where to update the firmware, BMS or not
        return page == 0 ? PAGE_NODATA : PAGE_BOARD;
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
    int first = 3;
    if (page < first + cells) {
        *index = page - first;
        return PAGE_CELL;
    }
    first += cells;
    if (page == first) {
        return PAGE_ERRORS;
    }
    first++;
    const int settings = settings_pages(bms);
    if (page < first + settings) {
        *index = page - first;
        return PAGE_SETTINGS;
    }
    first += settings;
    if (page < first + log_pages(bms)) {
        *index = page - first;
        return PAGE_LOG;
    }
    return PAGE_BOARD;
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


static void update_wifi_icon(void) {
    char ssid[33];
    char ip[16];
    const bool online = uploader_get_network(ssid, sizeof(ssid), ip, sizeof(ip));
    set_color(m_ui.header_wifi, online ? COLOR_TEXT : COLOR_DIM);
    set_visible(m_ui.header_wifi_cross, !online);
}


static volatile int m_send_permille = -1;


void ui_set_send_progress(int permille) {
    m_send_permille = permille;
}


/**
 * The progress replaces the title, the state dot and the Wi-Fi icon: of
 * sending, while the data goes out over Wi-Fi from a connected BMS, or, on a
 * collect-first board, of reading the BMS, which needs no Wi-Fi.
 */
static bool show_send_progress(const jkbms_status_t *st) {
    const int permille = m_send_permille;
    const bool show = permille >= 0
            && (CONFIG_UPLOAD_COLLECT_FIRST_ENABLED || (st->link == JKBMS_LINK_STREAMING && uploader_connected()));
    set_visible(m_ui.header_bar, show);
    set_visible(m_ui.header_title, !show);
    set_visible(m_ui.header_wifi, !show);
    if (show) {
        lv_bar_set_value(m_ui.header_bar, permille, LV_ANIM_OFF);
        set_visible(m_ui.header_state, false);
    }
    return show;
}


static void update_header(const jkbms_status_t *st) {
    // green: data was read (the BMS is read once per wakeup), red: connection
    // error, orange: no data yet
    uint32_t color = COLOR_WARN;
    if (st->error != JKBMS_ERROR_NONE) {
        color = COLOR_ERROR;
    } else if (st->cells_valid) {
        color = COLOR_OK;
    }
    set_text(m_ui.header_title, "BMS %d", m_ui.bms + 1);
    lv_obj_align(m_ui.header_title, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(m_ui.header_state, lv_color_hex(color), 0);
    set_visible(m_ui.header_state, !show_send_progress(st));

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


static void update_chart(const jkbms_status_t *st, int selected) {
    const jkbms_cell_info_t *ci = &st->cells;
    const int n = ci->cell_count;
    set_visible(m_ui.chart, true);
    // fixed scale, so that the bars do not jump with every few mV of change
    const float lo = CHART_DEFAULT_LO;
    const float hi = CHART_DEFAULT_HI;

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
    set_color(p->power, color);
    set_text(p->current, "%+.2f A", ci->current);
    set_text(p->power, "%+.0f W", ci->power);

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

    update_chart(st, i);
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
    const jkbms_extras_t *x = extras_of(m_ui.bms);
    const int n = (x != NULL && x->settings_valid) ? settings_rows(st->proto, &x->settings, rows) : 0;
    if (n == 0) {
        lv_label_set_text(p->title, "Settings: reading BMS...");
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
    const jkbms_extras_t *x = extras_of(m_ui.bms);
    const int pages = log_paginate(st, x, first);
    const int count = (x != NULL && x->logbook_valid) ? x->logbook_count : 0;
    if (x == NULL || !x->logbook_valid) {
        lv_label_set_text(p->title, "Log: reading BMS...");
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
        log_event_text(st, x, n, time, sizeof(time), label, sizeof(label));
        lv_label_set_text(p->times[i], time);
        lv_label_set_text(p->events[i], label);
    }
}


/**
 * Shows the spinner alone in the middle of the blank screen (no header), or
 * moved up with a message below it. Errors are red, other messages neutral.
 */
static void show_waiting(lv_obj_t *spinner, lv_obj_t *message_label, lv_obj_t *hint_label, const char *message,
                         const char *hint, bool error) {
    set_visible(m_ui.header, false);
    set_visible(spinner, true);
    // the page containers start below the header, which is hidden now
    const int center_y = (LCD_V_RES - SPINNER_SIZE) / 2 - HEADER_H;
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, message[0] ? 0 : center_y);
    lv_label_set_text(message_label, message);
    lv_label_set_text(hint_label, hint);
    set_color(message_label, error ? COLOR_ERROR : COLOR_TEXT);
}


#define HINT_PICK_ANOTHER   "Hold " LV_SYMBOL_LEFT " to pick another BMS."

static void update_nodata(const jkbms_status_t *st) {
    nodata_page_t *p = &m_ui.nodata;
    const char *message = "";
    const char *hint = "";
    bool error = true;
    switch (st->error) {
    case JKBMS_ERROR_NONE:
        break;
    case JKBMS_ERROR_NOT_FOUND:
        // the saved BMS is off or away: not an error
        set_visible(m_ui.header, false);
        set_visible(m_ui.no_bms, true);
        return;
    case JKBMS_ERROR_CONNECT:
        message = "Connection failed";
        hint = "Retrying. " HINT_PICK_ANOTHER;
        break;
    case JKBMS_ERROR_NOT_JK:
        message = "Not a JK BMS";
        hint = HINT_PICK_ANOTHER;
        break;
    case JKBMS_ERROR_NO_DATA:
        message = "BMS not responding";
        hint = "Retrying. " HINT_PICK_ANOTHER;
        break;
    }
    show_waiting(p->spinner, p->message, p->hint, message, hint, error);
}


static void update_board(void) {
    settings_page_t *p = &m_ui.board;
    char ssid[33];
    char ip[16];
    const bool online = uploader_get_network(ssid, sizeof(ssid), ip, sizeof(ip));
    lv_label_set_text(p->names[0], "Firmware");
    lv_label_set_text(p->values[0], esp_app_get_description()->version);
    lv_label_set_text(p->names[1], "Wi-Fi");
    lv_label_set_text(p->values[1], online ? ssid : "not connected");
    set_color(p->values[1], online ? COLOR_TEXT : COLOR_WARN);
    lv_label_set_text(p->names[2], "Update at");
    if (online) {
        set_text(p->values[2], WEBSERVER_HOSTNAME ".local\n%s", ip);
    } else {
        lv_label_set_text(p->values[2], "-");
    }
}


static void ui_render(void) {
    const jkbms_status_t *st = &m_status[m_ui.bms];
    if (m_ui.page >= page_count(m_ui.bms)) {
        m_ui.page = 0;
    }

    hide_pages();
    set_visible(m_ui.header, true);
    update_header(st);
    update_wifi_icon();
    int index;
    const page_kind_t kind = page_kind(m_ui.bms, m_ui.page, &index);
    extras_for_page(m_ui.bms, kind);
    switch (kind) {
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
    case PAGE_BOARD:
        update_board();
        set_visible(m_ui.board.cont, true);
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
}


/*---------------------------------------------------------------
        Picker: select the BMS to monitor
---------------------------------------------------------------*/

static void create_picker_page(lv_obj_t *scr) {
    picker_page_t *p = &m_ui.picker;
    p->cont = make_container(scr, HEADER_H, LCD_V_RES - HEADER_H);
    create_waiting(p->cont, &p->spinner, &p->message, &p->message_hint);

    // rows grow with the wrapped text; the list scrolls to the selected row
    p->list = make_container(p->cont, 0, CONTENT_H - PICKER_HINT_H);
    lv_obj_add_flag(p->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(p->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(p->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p->list, 4, 0);
    for (int i = 0; i < PICKER_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(p->list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_HEADER), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COLOR_SELECTED), 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        p->rows[i] = row;
        // name and MAC on one line each: the full MAC fits the screen in the 14 px font
        p->names[i] = make_label(row, &lv_font_montserrat_16, COLOR_TEXT, 0, 0);
        p->macs[i] = make_label(row, &lv_font_montserrat_14, COLOR_DIM, 0, 0);
        p->rssis[i] = make_label(row, &lv_font_montserrat_16, COLOR_DIM, 0, 0);
        lv_obj_set_width(p->names[i], LV_PCT(100));
        lv_label_set_long_mode(p->names[i], LV_LABEL_LONG_DOT);
    }

    p->hint = make_label(p->cont, &lv_font_montserrat_14, COLOR_DIM, 0, 0);
    lv_label_set_text(p->hint, LV_SYMBOL_LEFT " next     select " LV_SYMBOL_RIGHT);
    lv_obj_align(p->hint, LV_ALIGN_BOTTOM_MID, 0, -2);
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
        // nothing to pick: either button toggles the board page
        m_ui.picker_board = !m_ui.picker_board;
        return;
    }
    m_ui.picker_board = false;
    if (m_ui.cursor >= items) {
        m_ui.cursor = 0;
    }
    if (step == BUTTON_PREV) {
        m_ui.cursor = (m_ui.cursor + 1) % items;
    } else if (m_ui.cursor < m_ui.found_count) {
        picker_select(&m_ui.found[m_ui.cursor]);
    } else {
        enter_monitor();
    }
}


/**
 * True while the first scan window runs; the found devices are collected
 * before deciding between auto-select and the list.
 */
static bool picker_scanning(void) {
    const int64_t elapsed = esp_timer_get_time() - m_ui.picker_start_us;
    return !m_ui.picker_touched && elapsed < CONFIG_UI_AUTO_SELECT_S * 1000000LL;
}


static void picker_tick(void) {
    picker_fetch();
    // the only BMS around is selected without asking, unless the user asked for the list
    if (!picker_scanning() && !m_ui.picker_touched && !m_ui.picker_manual && assigned_count() == 0
            && m_ui.found_count == 1) {
        picker_select(&m_ui.found[0]);
    }
}


static void picker_render(void) {
    picker_page_t *p = &m_ui.picker;
    hide_pages();
    set_visible(p->cont, true);
    set_visible(m_ui.header_state, false);
    set_visible(m_ui.header_bar, false);
    set_visible(m_ui.header_title, true);
    set_visible(m_ui.header_wifi, true);

    const int items = picker_items();
    const bool show_list = !picker_scanning() && items > 0;
    if (show_list) {
        lv_label_set_text(m_ui.header_title, "Pick BMS");
    } else {
        lv_label_set_text(m_ui.header_title, "JK BMS");
    }
    lv_obj_align(m_ui.header_title, LV_ALIGN_LEFT_MID, 6, 0);

    set_visible(m_ui.header, true);
    update_wifi_icon();
    set_visible(p->list, show_list);
    set_visible(p->hint, show_list);
    if (show_list) {
        set_visible(p->spinner, false);
        lv_label_set_text(p->message, "");
        lv_label_set_text(p->message_hint, "");
    } else if (picker_scanning()) {
        show_waiting(p->spinner, p->message, p->message_hint, "", "", false);
    } else if (m_ui.picker_board) {
        set_visible(p->spinner, false);
        lv_label_set_text(p->message, "");
        lv_label_set_text(p->message_hint, "");
        update_board();
        set_visible(m_ui.board.cont, true);
    } else {
        set_visible(m_ui.header, false);
        set_visible(m_ui.no_bms, true);
    }

    if (m_ui.cursor >= items) {
        m_ui.cursor = 0;
    }
    for (int i = 0; i < PICKER_ROWS; i++) {
        const bool visible = show_list && i < items;
        set_visible(p->rows[i], visible);
        if (!visible) {
            continue;
        }
        lv_obj_set_style_bg_opa(p->rows[i], i == m_ui.cursor ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(p->rows[i], i == m_ui.cursor ? 2 : 0, 0);
        if (i < m_ui.found_count) {
            const jkbms_found_t *f = &m_ui.found[i];
            set_text(p->names[i], "%s", f->name[0] ? f->name : "(no name)");
            set_text(p->macs[i], "%02X:%02X:%02X:%02X:%02X:%02X", f->mac[0], f->mac[1], f->mac[2], f->mac[3],
                     f->mac[4], f->mac[5]);
#if CONFIG_UI_SCREENSHOT_CONSOLE
            if (m_ui.anonymize) {
                static const char *names[] = { "JK-B1A8S20P", "JK-B2A24S15P" };
                static const uint8_t macs[][6] = { { 0xA4, 0xC1, 0x38, 0x5E, 0x92, 0x07 },
                                                   { 0x3C, 0x71, 0xBF, 0x0D, 0x4A, 0xE6 } };
                const uint8_t *mac = macs[i % 2];
                lv_label_set_text(p->names[i], names[i % 2]);
                set_text(p->macs[i], "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
#endif
            set_text(p->rssis[i], "%d dBm", f->rssi);
            set_visible(p->rssis[i], true);
        } else {
            lv_label_set_text(p->names[i], LV_SYMBOL_OK " Done");
            set_text(p->macs[i], "monitor %d BMS", assigned_count());
            set_visible(p->rssis[i], false);
        }
    }
    if (show_list && m_ui.cursor < items) {
        lv_obj_update_layout(p->list);
        lv_obj_scroll_to_view(p->rows[m_ui.cursor], LV_ANIM_OFF);
    }
}


/*---------------------------------------------------------------
        Public API
---------------------------------------------------------------*/

/**
 * Full screen: a filled disc inside a ring with a check mark (sent) or a
 * cross (failed), and "Відправлено" below the check mark.
 */
static void create_result_screen(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(cont, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    m_ui.result_ring = lv_obj_create(cont);
    lv_obj_remove_style_all(m_ui.result_ring);
    lv_obj_set_size(m_ui.result_ring, RESULT_RING_D, RESULT_RING_D);
    lv_obj_align(m_ui.result_ring, LV_ALIGN_TOP_MID, 0, RESULT_Y);
    lv_obj_set_style_radius(m_ui.result_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(m_ui.result_ring, 4, 0);

    m_ui.result_disc = lv_obj_create(cont);
    lv_obj_remove_style_all(m_ui.result_disc);
    lv_obj_set_size(m_ui.result_disc, RESULT_DISC_D, RESULT_DISC_D);
    lv_obj_align(m_ui.result_disc, LV_ALIGN_TOP_MID, 0, RESULT_Y + (RESULT_RING_D - RESULT_DISC_D) / 2);
    lv_obj_set_style_radius(m_ui.result_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m_ui.result_disc, LV_OPA_COVER, 0);

    // the marks are drawn in the disc's coordinates, centre at (44, 44)
    static const lv_point_precise_t check[] = { { 23, 46 }, { 38, 60 }, { 65, 30 } };
    static const lv_point_precise_t cross[2][2] = { { { 28, 28 }, { 60, 60 } }, { { 60, 28 }, { 28, 60 } } };
    lv_obj_t *marks[3];
    marks[0] = m_ui.result_check = lv_line_create(m_ui.result_disc);
    lv_line_set_points(m_ui.result_check, check, 3);
    for (int i = 0; i < 2; i++) {
        marks[1 + i] = m_ui.result_cross[i] = lv_line_create(m_ui.result_disc);
        lv_line_set_points(m_ui.result_cross[i], cross[i], 2);
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_line_width(marks[i], 9, 0);
        lv_obj_set_style_line_color(marks[i], lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_line_rounded(marks[i], true, 0);
    }

    m_ui.result_text = lv_image_create(cont);
    lv_image_set_src(m_ui.result_text, &sent_text);
    lv_obj_set_style_image_recolor(m_ui.result_text, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(m_ui.result_text, LV_OPA_COVER, 0);
    lv_obj_align(m_ui.result_text, LV_ALIGN_TOP_MID, 0, RESULT_Y + RESULT_RING_D + 18);
    m_ui.result_cont = cont;
}


/**
 * A check mark with `text` below it, or a cross without text.
 */
static void show_result(uint32_t rgb, bool ok, const lv_image_dsc_t *text, int64_t show_us) {
    lvgl_port_lock(0);
    if (m_ui.result_cont == NULL) {
        create_result_screen();
    }
    const lv_color_t color = lv_color_hex(rgb);
    lv_image_set_src(m_ui.result_text, text);
    // the cross has no text below it: centre it on the screen
    const int y = ok ? RESULT_Y : (LCD_V_RES - RESULT_RING_D) / 2;
    lv_obj_align(m_ui.result_ring, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_align(m_ui.result_disc, LV_ALIGN_TOP_MID, 0, y + (RESULT_RING_D - RESULT_DISC_D) / 2);
    lv_obj_set_style_border_color(m_ui.result_ring, color, 0);
    lv_obj_set_style_bg_color(m_ui.result_disc, color, 0);
    set_visible(m_ui.result_check, ok);
    set_visible(m_ui.result_cross[0], !ok);
    set_visible(m_ui.result_cross[1], !ok);
    set_visible(m_ui.result_text, ok);
    set_visible(m_ui.result_cont, true);
    m_ui.result_until_us = esp_timer_get_time() + show_us;
    lvgl_port_unlock();
}


void ui_show_upload_result(bool ok) {
    show_result(ok ? COLOR_OK : COLOR_ERROR, ok, &sent_text, RESULT_SHOW_US);
}


void ui_show_read_result(void) {
    show_result(COLOR_READ, true, &read_text, READ_SHOW_US);
}


static bool result_visible(void) {
    return m_ui.result_cont != NULL && !lv_obj_has_flag(m_ui.result_cont, LV_OBJ_FLAG_HIDDEN);
}


/**
 * A short text over everything for a few seconds.
 */
static void show_notice(const char *text) {
    lv_obj_t *label = make_label(lv_layer_top(), &lv_font_montserrat_20, COLOR_TEXT, 0, 0);
    lv_obj_set_width(label, LCD_H_RES);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(label, 20, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_delete_delayed(label, 4000);
}


void ui_on_button(int step) {
    if (step == BUTTON_NEXT_LONG) {
        // works with a stuck display too: the previous firmware is the way out
        const char *error = remote_boot_previous();
        if (lvgl_port_lock(500)) {
            show_notice(error);
            lvgl_port_unlock();
        }
        return;
    }
    // a stuck display must not block the buttons task, see BUTTON_NEXT_LONG
    if (!lvgl_port_lock(500)) {
        return;
    }
    if (result_visible()) {
        // the press only closes the upload result
        set_visible(m_ui.result_cont, false);
        lvgl_port_unlock();
        return;
    }
    if (step == BUTTON_PREV_LONG) {
        // forget the saved BMS and show the list of BMS around
        jkbms_ble_forget_saved();
        jkbms_ble_unassign_all();
        m_ui.mode = UI_MODE_PICKER;
        m_ui.picker_manual = true;
        m_ui.picker_touched = false;
        m_ui.picker_start_us = esp_timer_get_time();
        m_ui.cursor = 0;
        jkbms_ble_set_discovery(true);
        ui_fetch_status();
        picker_fetch();
        if (m_ui.found_count > 0) {
            // searched within the last minute: show the list without searching again
            m_ui.picker_start_us -= CONFIG_UI_AUTO_SELECT_S * 1000000LL;
        }
        picker_render();
        lvgl_port_unlock();
        return;
    }
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


/**
 * Full-screen "Updating firmware" with a progress bar, on the top layer.
 */
static void create_ota_screen(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(cont, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(cont, &lv_font_montserrat_20, COLOR_TEXT, 0, 0);
    lv_obj_set_width(title, LCD_H_RES);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "Updating\nfirmware");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    m_ui.ota_bar = lv_bar_create(cont);
    lv_obj_set_size(m_ui.ota_bar, LCD_H_RES - 20, 16);
    lv_obj_align(m_ui.ota_bar, LV_ALIGN_TOP_MID, 0, 126);
    lv_bar_set_range(m_ui.ota_bar, 0, 100);
    lv_obj_set_style_bg_color(m_ui.ota_bar, lv_color_hex(COLOR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m_ui.ota_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m_ui.ota_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);

    m_ui.ota_label = make_label(cont, &lv_font_montserrat_28, COLOR_TEXT, 0, 0);
    lv_obj_align(m_ui.ota_label, LV_ALIGN_TOP_MID, 0, 152);
    m_ui.ota_cont = cont;
}


void ui_ota_progress(int percent) {
    // a firmware update must never wait for a stuck display
    if (!lvgl_port_lock(100)) {
        return;
    }
    if (m_ui.ota_cont == NULL) {
        create_ota_screen();
    }
    set_visible(m_ui.ota_cont, percent >= 0);
    if (percent >= 0) {
        lv_bar_set_value(m_ui.ota_bar, percent, LV_ANIM_OFF);
        set_text(m_ui.ota_label, "%d %%", percent);
        lv_obj_align(m_ui.ota_label, LV_ALIGN_TOP_MID, 0, 152);
    }
    lvgl_port_unlock();
}


#if CONFIG_UI_SCREENSHOT_CONSOLE
void ui_debug_anonymize(bool enable) {
    lvgl_port_lock(0);
    m_ui.anonymize = enable;
    lvgl_port_unlock();
}


/**
 * Prints the screen as "SHOT <w> <h>", one hex RGB565 row per line, "END".
 */
void ui_screenshot(void) {
    lvgl_port_lock(0);
    const bool ota = m_ui.ota_cont != NULL && !lv_obj_has_flag(m_ui.ota_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *top = ota ? m_ui.ota_cont : result_visible() ? m_ui.result_cont : lv_screen_active();
    lv_draw_buf_t *snap = lv_snapshot_take(top, LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();
    if (snap == NULL) {
        printf("SHOT ERROR\n");
        return;
    }
    const int w = snap->header.w;
    const int h = snap->header.h;
    printf("SHOT %d %d\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = snap->data + y * snap->header.stride;
        for (int x = 0; x < w * 2; x++) {
            printf("%02x", row[x]);
        }
        printf("\n");
    }
    printf("END\n");
    lv_draw_buf_destroy(snap);
}
#endif


void ui_refresh(void) {
    lvgl_port_lock(0);
    if (result_visible() && esp_timer_get_time() > m_ui.result_until_us) {
        set_visible(m_ui.result_cont, false);
    }
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
