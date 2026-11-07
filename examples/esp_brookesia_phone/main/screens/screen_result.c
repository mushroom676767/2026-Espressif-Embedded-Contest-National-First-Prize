/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen 4: Result Screen
 * Shows borrow/return results after door close.
 * Displays: result card with details, status, cloud push status.
 */

#include "screen_result.h"
#include "ui_theme.h"
#include "ui_touch_layout.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "screen_result";

#define RESULT_CARD_W       760
#define RESULT_CARD_H       500
#define DETAIL_LINE_H       40

// ============================================================================
// Internal Data
// ============================================================================
struct screen_result_data_s {
    lv_obj_t *screen;
    // Card
    lv_obj_t *card;
    lv_obj_t *card_title_label;
    lv_obj_t *card_icon_label;
    // Detail labels
    lv_obj_t *operator_val;
    lv_obj_t *time_val;
    lv_obj_t *borrowed_val;
    lv_obj_t *returned_val;
    lv_obj_t *anomaly_val;
    // Bottom
    lv_obj_t *cloud_status_label;
    lv_obj_t *return_btn;
    // Status border
    lv_obj_t *status_line;
};

static struct screen_result_data_s s_data;

// ============================================================================
// Result type helpers
// ============================================================================
static const char *get_result_title(result_type_t type)
{
    switch (type) {
        case RESULT_BORROW_OK:     return LV_SYMBOL_OK "  借用成功";
        case RESULT_RETURN_OK:    return LV_SYMBOL_OK "  归还成功";
        case RESULT_OPERATION_OK:  return LV_SYMBOL_OK "  操作成功";
        case RESULT_ANOMALY:      return LV_SYMBOL_WARNING "  检测到异常";
        case RESULT_AUTH_FAILED:  return LV_SYMBOL_CLOSE "  验证失败";
        default:                   return "未知结果";
    }
}

static lv_color_t get_result_color(result_type_t type)
{
    switch (type) {
        case RESULT_BORROW_OK:     return UI_COLOR_SUCCESS;
        case RESULT_RETURN_OK:    return UI_COLOR_PRIMARY;
        case RESULT_OPERATION_OK:  return UI_COLOR_SUCCESS;
        case RESULT_ANOMALY:      return UI_COLOR_DANGER;
        case RESULT_AUTH_FAILED:  return UI_COLOR_WARNING;
        default:                   return UI_COLOR_TEXT_SECOND;
    }
}

static const char *get_result_icon(result_type_t type)
{
    switch (type) {
        case RESULT_BORROW_OK:     return LV_SYMBOL_OK;
        case RESULT_RETURN_OK:    return LV_SYMBOL_LOOP;
        case RESULT_OPERATION_OK:  return LV_SYMBOL_OK;
        case RESULT_ANOMALY:      return LV_SYMBOL_WARNING;
        case RESULT_AUTH_FAILED:  return LV_SYMBOL_CLOSE;
        default:                   return LV_SYMBOL_REFRESH;
    }
}

// ============================================================================
// Build tool list string
// ============================================================================
static void build_tool_list(char *buf, size_t buf_len, tool_info_t *list, uint8_t count)
{
    if (count == 0) {
        strncpy(buf, "无", buf_len);
        return;
    }

    buf[0] = '\0';
    size_t pos = 0;
    for (uint8_t i = 0; i < count && pos < buf_len - 1; i++) {
        int written = snprintf(buf + pos, buf_len - pos, "%s%s",
                                (i > 0) ? ", " : "", list[i].name);
        if (written > 0) {
            pos += written;
        }
    }
}

// ============================================================================
// Button callback
// ============================================================================
static void return_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Return to standby");
    ui_manager_goto_screen(SCREEN_STANDBY);
}

// ============================================================================
// Detail row creation helper
// ============================================================================
static lv_obj_t *create_detail_row(lv_obj_t *parent, const char *label_text, int32_t y_offset)
{
    // Label
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, y_offset);
    lv_obj_set_style_text_font(label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(label, label_text);

    // Separator colon
    lv_obj_t *colon = lv_label_create(parent);
    lv_obj_align(colon, LV_ALIGN_TOP_LEFT, 190, y_offset);
    lv_obj_set_style_text_font(colon, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(colon, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(colon, ":");

    // Value
    lv_obj_t *value = lv_label_create(parent);
    lv_obj_set_width(value, 500);
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, 220, y_offset);
    lv_obj_set_style_text_font(value, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(value, "---");

    return value;
}

// ============================================================================
// Result screen creation
// ============================================================================
void screen_result_create(void)
{
    ESP_LOGI(TAG, "Creating result screen...");

    memset(&s_data, 0, sizeof(s_data));

    // --- Screen background ---
    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);

    // --- Semi-transparent overlay effect ---
    lv_obj_t *overlay = lv_obj_create(s_data.screen);
    lv_obj_set_size(overlay, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);

    // --- Result card (centered) ---
    s_data.card = lv_obj_create(s_data.screen);
    lv_obj_set_size(s_data.card, RESULT_CARD_W, RESULT_CARD_H);
    lv_obj_align(s_data.card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_data.card, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.card, UI_RADIUS_XL, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_data.card, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_data.card, UI_PADDING_LG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_data.card, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_data.card, lv_color_hex(0x00000040), LV_PART_MAIN);
    lv_obj_clear_flag(s_data.card, LV_OBJ_FLAG_SCROLLABLE);

    // --- Result icon (large, top center) ---
    s_data.card_icon_label = lv_label_create(s_data.card);
    lv_obj_align(s_data.card_icon_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(s_data.card_icon_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.card_icon_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    lv_label_set_text(s_data.card_icon_label, LV_SYMBOL_OK);

    // --- Result title ---
    s_data.card_title_label = lv_label_create(s_data.card);
    lv_obj_align(s_data.card_title_label, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_text_font(s_data.card_title_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.card_title_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    lv_label_set_text(s_data.card_title_label, "借用成功");

    // --- Status accent line ---
    s_data.status_line = lv_obj_create(s_data.card);
    lv_obj_set_size(s_data.status_line, 120, 4);
    lv_obj_align(s_data.status_line, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_color(s_data.status_line, UI_COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_radius(s_data.status_line, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_data.status_line, 0, LV_PART_MAIN);

    // --- Detail rows ---
    int32_t y = 148;
    s_data.operator_val = create_detail_row(s_data.card, "操作员", y);
    y += DETAIL_LINE_H;
    s_data.time_val = create_detail_row(s_data.card, "时间", y);
    y += DETAIL_LINE_H;
    s_data.borrowed_val = create_detail_row(s_data.card, "借出", y);
    y += DETAIL_LINE_H;
    s_data.returned_val = create_detail_row(s_data.card, "归还", y);
    y += DETAIL_LINE_H;
    s_data.anomaly_val = create_detail_row(s_data.card, "异常", y);

    // --- Cloud push status ---
    s_data.cloud_status_label = lv_label_create(s_data.card);
    lv_obj_align(s_data.cloud_status_label, LV_ALIGN_BOTTOM_LEFT, 20, -72);
    lv_obj_set_style_text_font(s_data.cloud_status_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.cloud_status_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(s_data.cloud_status_label, LV_SYMBOL_UPLOAD "  云端:等待同步...");

    // --- Return button ---
    lv_obj_t *return_parent = ui_touch_is_damaged_mode() ? s_data.screen : s_data.card;
    s_data.return_btn = ui_create_btn_primary_large(return_parent);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.return_btn, 0, 500, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.return_btn, 230, UI_BTN_H_LG);
        lv_obj_align(s_data.return_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    lv_obj_t *btn_label = lv_label_create(s_data.return_btn);
    lv_obj_set_style_text_font(btn_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(btn_label, LV_SYMBOL_HOME "  返回待机");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(s_data.return_btn, return_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "Result screen created");
}

// ============================================================================
// Public API
// ============================================================================

lv_obj_t *screen_result_get_screen(void)
{
    return s_data.screen;
}

void screen_result_show(operation_result_t *result)
{
    if (result == NULL) return;

    lv_color_t color = get_result_color(result->result_type);

    // Update icon
    lv_label_set_text(s_data.card_icon_label, get_result_icon(result->result_type));
    lv_obj_set_style_text_color(s_data.card_icon_label, color, LV_PART_MAIN);

    // Update title
    lv_label_set_text(s_data.card_title_label, get_result_title(result->result_type));
    lv_obj_set_style_text_color(s_data.card_title_label, color, LV_PART_MAIN);

    // Update accent line color
    lv_obj_set_style_bg_color(s_data.status_line, color, LV_PART_MAIN);

    // Update card border for anomaly
    if (result->result_type == RESULT_ANOMALY) {
        lv_obj_set_style_border_color(s_data.card, UI_COLOR_DANGER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_data.card, 3, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_color(s_data.card, UI_COLOR_BORDER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_data.card, 2, LV_PART_MAIN);
    }

    // Update detail values
    lv_label_set_text(s_data.operator_val, result->operator_name);
    lv_label_set_text(s_data.time_val, result->timestamp);

    char buf[128];
    build_tool_list(buf, sizeof(buf), result->borrowed, result->borrowed_count);
    lv_label_set_text(s_data.borrowed_val, buf);

    build_tool_list(buf, sizeof(buf), result->returned, result->returned_count);
    lv_label_set_text(s_data.returned_val, buf);

    if (result->anomaly_count > 0) {
        build_tool_list(buf, sizeof(buf), result->anomalous, result->anomaly_count);
        lv_label_set_text(s_data.anomaly_val, buf);
        lv_obj_set_style_text_color(s_data.anomaly_val, UI_COLOR_DANGER, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_data.anomaly_val, "无");
        lv_obj_set_style_text_color(s_data.anomaly_val, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    }

    // Cloud push status
    if (result->cloud_pushed) {
        lv_label_set_text(s_data.cloud_status_label, LV_SYMBOL_OK "  云端:已同步");
        lv_obj_set_style_text_color(s_data.cloud_status_label, UI_COLOR_SUCCESS, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_data.cloud_status_label, LV_SYMBOL_WARNING "  云端:未同步");
        lv_obj_set_style_text_color(s_data.cloud_status_label, UI_COLOR_WARNING, LV_PART_MAIN);
    }

    ESP_LOGI(TAG, "Result shown: type=%d, operator=%s",
             result->result_type, result->operator_name);
}

void screen_result_clear(void)
{
    lv_label_set_text(s_data.card_title_label, "---");
    lv_label_set_text(s_data.operator_val, "---");
    lv_label_set_text(s_data.time_val, "---");
    lv_label_set_text(s_data.borrowed_val, "无");
    lv_label_set_text(s_data.returned_val, "无");
    lv_label_set_text(s_data.anomaly_val, "无");
    lv_label_set_text(s_data.cloud_status_label, LV_SYMBOL_UPLOAD "  云端:等待同步...");
    lv_obj_set_style_text_color(s_data.anomaly_val, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.cloud_status_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
}

struct screen_result_data_s *screen_result_get_data(void)
{
    return &s_data;
}
