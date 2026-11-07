#include "screen_tool_records.h"

#include <stdio.h>
#include <string.h>
#include "cabinet_core.h"
#include "esp_err.h"
#include "esp_log.h"
#include "tool_record_store.h"
#include "ui_touch_layout.h"
#include "ui_manager.h"
#include "ui_theme.h"

#define RECORD_ROW_MAX 20
#define RECORD_TABLE_X 20
#define RECORD_TABLE_Y 78
#define RECORD_TABLE_W 984
#define RECORD_TABLE_H 462
#define RECORD_HEADER_H 70
#define RECORD_ROW_H 76
#define RECORD_SAFE_BUTTON_H UI_BTN_H_SAFE

typedef struct {
    const char *title;
    int32_t width;
} record_column_t;

struct screen_tool_records_data_s {
    lv_obj_t *screen;
    lv_obj_t *rows;
    lv_obj_t *empty_label;
};

static const char *TAG = "screen_tool_records";
static struct screen_tool_records_data_s s_data;

static const record_column_t s_columns[] = {
    {"事件编号", 220},
    {"设备", 126},
    {"操作者", 130},
    {"结果", 86},
    {"开柜", 156},
    {"关柜", 156},
    {"同步", 82},
};

static const char *result_label(const char *result_type)
{
    if (result_type == NULL) {
        return "---";
    }
    if (strcmp(result_type, "borrow_ok") == 0) {
        return "借用";
    }
    if (strcmp(result_type, "return_ok") == 0) {
        return "归还";
    }
    if (strcmp(result_type, "anomaly") == 0) {
        return "异常";
    }
    if (strcmp(result_type, "no_change") == 0) {
        return "无变化";
    }
    return result_type;
}

static int32_t records_table_y(void)
{
    return ui_touch_is_damaged_mode() ? 192 : RECORD_TABLE_Y;
}

static int32_t records_table_h(void)
{
    return ui_touch_is_damaged_mode() ? (UI_SCREEN_HEIGHT - records_table_y() - 20) : RECORD_TABLE_H;
}

static lv_obj_t *create_cell(lv_obj_t *parent,
                             const char *text,
                             int32_t x,
                             int32_t y,
                             int32_t w,
                             lv_color_t color,
                             const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_size(label, w, RECORD_ROW_H);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 8, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text != NULL && text[0] != '\0' ? text : "---");
    return label;
}

static void refresh_btn_event_cb(lv_event_t *e)
{
    (void)e;
    screen_tool_records_refresh();
}

static void back_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_goto_screen(SCREEN_OPERATION);
}

static void reset_btn_event_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t ret = cabinet_core_reset_all();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Reset tool records failed: %s", esp_err_to_name(ret));
    }
    screen_tool_records_refresh();
}

static void create_header_row(lv_obj_t *parent)
{
    int32_t x = 0;
    for (size_t i = 0; i < sizeof(s_columns) / sizeof(s_columns[0]); i++) {
        create_cell(parent, s_columns[i].title, x, 0, s_columns[i].width,
                    UI_COLOR_TEXT_HINT, UI_FONT_STATUS);
        x += s_columns[i].width;
    }
}

static void create_record_row(lv_obj_t *parent, const tool_record_t *record, int32_t y)
{
    int32_t x = 0;
    char event_short[48];
    snprintf(event_short, sizeof(event_short), "%s", record->event_id);

    create_cell(parent, event_short, x, y, s_columns[0].width, UI_COLOR_TEXT_PRIMARY, UI_FONT_STATUS);
    x += s_columns[0].width;
    create_cell(parent, record->device_code, x, y, s_columns[1].width, UI_COLOR_TEXT_SECOND, UI_FONT_STATUS);
    x += s_columns[1].width;
    create_cell(parent, record->operator_name, x, y, s_columns[2].width, UI_COLOR_TEXT_SECOND, UI_FONT_STATUS);
    x += s_columns[2].width;
    const char *detail = record->door_state == TOOL_RECORD_DOOR_ALERTED
                             ? "未关门"
                             : (record->door_state == TOOL_RECORD_DOOR_CLOSED_AFTER_ALERT
                                    ? "已补关"
                                    : result_label(record->result_type));
    create_cell(parent, detail, x, y, s_columns[3].width, UI_COLOR_PRIMARY, UI_FONT_STATUS);
    x += s_columns[3].width;
    create_cell(parent, record->opened_at, x, y, s_columns[4].width, UI_COLOR_TEXT_HINT, UI_FONT_STATUS);
    x += s_columns[4].width;
    create_cell(parent, record->closed_at, x, y, s_columns[5].width, UI_COLOR_TEXT_HINT, UI_FONT_STATUS);
    x += s_columns[5].width;
    create_cell(parent, tool_record_sync_label(record->sync_status), x, y, s_columns[6].width,
                record->sync_status == TOOL_RECORD_SYNC_SYNCED ? UI_COLOR_SUCCESS :
                (record->sync_status == TOOL_RECORD_SYNC_FAILED ? UI_COLOR_WARNING : UI_COLOR_TEXT_HINT),
                UI_FONT_STATUS);
}

void screen_tool_records_create(void)
{
    ESP_LOGI(TAG, "Creating tool records screen...");
    memset(&s_data, 0, sizeof(s_data));

    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(s_data.screen);
    lv_obj_set_size(header, UI_SCREEN_WIDTH, UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_font(title, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_LIST "  借还记录");
    if (ui_touch_is_damaged_mode()) {
        lv_obj_align(title, LV_ALIGN_LEFT_MID, UI_TOUCH_SAFE_W + 20, 0);
    }

    lv_obj_t *button_parent = ui_touch_is_damaged_mode() ? s_data.screen : header;

    lv_obj_t *back_btn = lv_btn_create(button_parent);
    lv_obj_add_style(back_btn, (lv_style_t *)ui_style_get_btn_secondary_large(), LV_PART_MAIN);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_obj_set_style_text_font(back_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT "  返回");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *refresh_btn = lv_btn_create(button_parent);
    lv_obj_add_style(refresh_btn, (lv_style_t *)ui_style_get_btn_secondary_large(), LV_PART_MAIN);
    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_obj_set_style_text_font(refresh_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH "  刷新");
    lv_obj_center(refresh_label);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset_btn = lv_btn_create(button_parent);
    lv_obj_add_style(reset_btn, (lv_style_t *)ui_style_get_btn_danger_large(), LV_PART_MAIN);
    lv_obj_t *reset_label = lv_label_create(reset_btn);
    lv_obj_set_style_text_font(reset_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(reset_label, LV_SYMBOL_TRASH "  重置");
    lv_obj_center(reset_label);
    lv_obj_add_event_cb(reset_btn, reset_btn_event_cb, LV_EVENT_CLICKED, NULL);

    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(back_btn, 0, 8, RECORD_SAFE_BUTTON_H);
        ui_touch_place_stack(refresh_btn, 1, 8, RECORD_SAFE_BUTTON_H);
        ui_touch_place_stack(reset_btn, 2, 8, RECORD_SAFE_BUTTON_H);
    } else {
        lv_obj_set_size(back_btn, 110, UI_BTN_H_MD);
        lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_set_size(refresh_btn, 110, UI_BTN_H_MD);
        lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -122, 0);
        lv_obj_set_size(reset_btn, 110, UI_BTN_H_MD);
        lv_obj_align(reset_btn, LV_ALIGN_RIGHT_MID, -224, 0);
    }

    lv_obj_t *table = lv_obj_create(s_data.screen);
    lv_obj_set_size(table, RECORD_TABLE_W, records_table_h());
    lv_obj_set_pos(table, RECORD_TABLE_X, records_table_y());
    lv_obj_set_style_bg_color(table, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_width(table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(table, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(table, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table, 12, LV_PART_MAIN);
    lv_obj_clear_flag(table, LV_OBJ_FLAG_SCROLLABLE);

    create_header_row(table);

    s_data.rows = lv_obj_create(table);
    lv_obj_set_size(s_data.rows, RECORD_TABLE_W - 24, records_table_h() - RECORD_HEADER_H - 24);
    lv_obj_set_pos(s_data.rows, 0, RECORD_HEADER_H);
    lv_obj_set_style_bg_opa(s_data.rows, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.rows, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.rows, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_data.rows, LV_DIR_VER);

    s_data.empty_label = lv_label_create(table);
    lv_obj_align(s_data.empty_label, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_text_font(s_data.empty_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.empty_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(s_data.empty_label, "暂无借还记录");
}

lv_obj_t *screen_tool_records_get_screen(void)
{
    return s_data.screen;
}

void screen_tool_records_refresh(void)
{
    if (s_data.rows == NULL) {
        return;
    }

    lv_obj_clean(s_data.rows);

    static tool_record_t records[RECORD_ROW_MAX];
    size_t count = 0;
    esp_err_t ret = tool_record_store_list_latest(records, RECORD_ROW_MAX, &count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "List tool records failed: %s", esp_err_to_name(ret));
        count = 0;
    }

    if (count == 0) {
        lv_obj_clear_flag(s_data.empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_data.empty_label, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < count; i++) {
        int32_t y = (int32_t)i * RECORD_ROW_H;
        create_record_row(s_data.rows, &records[i], y);
    }
}
