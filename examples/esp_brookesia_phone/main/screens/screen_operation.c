/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen 3: Operation Screen
 * Displayed after the cabinet door is unlocked.
 * Shows: tool grid with slot status, operator info, confirm button.
 */

#include "screen_operation.h"
#include "cabinet_core.h"
#include "tool_cabinet.h"
#include "ui_theme.h"
#include "ui_touch_layout.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "screen_operation";

#define TOOL_GRID_COLS      3
#define TOOL_GRID_ROWS      3
#define TOOL_SLOT_MAX       TOOL_CABINET_SLOT_COUNT
#define SLOT_W              286
#define SLOT_H              122
#define SLOT_GAP            12

// ============================================================================
// Single slot widget
// ============================================================================
typedef struct {
    lv_obj_t *panel;
    lv_obj_t *icon;
    lv_obj_t *name_label;
    lv_obj_t *id_label;
    lv_obj_t *status_label;
    lv_obj_t *status_dot;
    tool_slot_status_t status;
} slot_widget_t;

// ============================================================================
// Internal Data
// ============================================================================
struct screen_operation_data_s {
    lv_obj_t *screen;
    lv_obj_t *operator_label;
    lv_obj_t *title_label;
    lv_obj_t *records_btn;
    lv_obj_t *confirm_btn;
    lv_obj_t *grace_label;
    slot_widget_t slots[TOOL_SLOT_MAX];
    uint8_t slot_count;
    bool read_only;
};

static struct screen_operation_data_s s_data;

// ============================================================================
// Slot status helpers
// ============================================================================
static lv_color_t get_slot_color(tool_slot_status_t status)
{
    switch (status) {
        case TOOL_SLOT_AVAILABLE: return UI_COLOR_SLOT_AVAILABLE;
        case TOOL_SLOT_BORROWED:  return UI_COLOR_SLOT_BORROWED;
        case TOOL_SLOT_WARNING:   return UI_COLOR_SLOT_WARNING;
        case TOOL_SLOT_EMPTY:     return UI_COLOR_SLOT_EMPTY;
        default:                  return UI_COLOR_SLOT_EMPTY;
    }
}

static lv_color_t get_slot_bg_color(tool_slot_status_t status)
{
    switch (status) {
        case TOOL_SLOT_AVAILABLE: return UI_COLOR_SUCCESS_DIM;
        case TOOL_SLOT_BORROWED:  return UI_COLOR_DANGER_DIM;
        case TOOL_SLOT_WARNING:   return UI_COLOR_WARNING_DIM;
        case TOOL_SLOT_EMPTY:     return UI_COLOR_BG_PANEL;
        default:                  return UI_COLOR_BG_PANEL;
    }
}

static void apply_slot_status(slot_widget_t *slot, tool_slot_status_t status)
{
    if (slot == NULL) {
        return;
    }

    slot->status = status;
    lv_color_t status_color = get_slot_color(status);
    lv_color_t bg_color = get_slot_bg_color(status);

    lv_obj_set_style_bg_color(slot->panel, bg_color, LV_PART_MAIN);
    lv_obj_set_style_border_color(slot->panel, status_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(slot->panel, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot->status_dot, status_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot->status_label, status_color, LV_PART_MAIN);
    lv_obj_set_style_border_color(slot->status_label, status_color, LV_PART_MAIN);
    lv_label_set_text(slot->status_label, tool_cabinet_status_label(status));

    if (status == TOOL_SLOT_AVAILABLE) {
        lv_label_set_text(slot->icon, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_SUCCESS, LV_PART_MAIN);
    } else if (status == TOOL_SLOT_BORROWED) {
        lv_label_set_text(slot->icon, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_DANGER, LV_PART_MAIN);
    } else if (status == TOOL_SLOT_WARNING) {
        lv_label_set_text(slot->icon, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_WARNING, LV_PART_MAIN);
    } else {
        lv_label_set_text(slot->icon, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    }
}

static void slot_event_cb(lv_event_t *e)
{
    if (s_data.read_only) {
        ESP_LOGI(TAG, "Ignore slot edit: vision inventory is read-only");
        return;
    }
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= TOOL_SLOT_MAX) {
        return;
    }

    slot_widget_t *slot = &s_data.slots[idx];
    tool_slot_status_t next = tool_cabinet_next_touch_status(slot->status);
    screen_operation_update_slot((uint8_t)idx, next);
    cabinet_core_set_slot_status((uint8_t)(idx + 1), next);
}

// ============================================================================
// Create single slot widget
// ============================================================================
static void create_slot_widget(uint8_t idx, lv_obj_t *parent, int32_t col, int32_t row)
{
    slot_widget_t *slot = &s_data.slots[idx];

    slot->panel = lv_obj_create(parent);
    lv_obj_set_size(slot->panel, SLOT_W, SLOT_H);
    lv_obj_set_style_bg_color(slot->panel, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(slot->panel, UI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_border_width(slot->panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(slot->panel, UI_COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slot->panel, UI_PADDING_MD, LV_PART_MAIN);
    lv_obj_clear_flag(slot->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slot->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot->panel, slot_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    // Grid placement
    lv_obj_set_grid_cell(slot->panel, LV_GRID_ALIGN_STRETCH, col, 1,
                          LV_GRID_ALIGN_STRETCH, row, 1);

    // Status dot (top-right)
    slot->status_dot = lv_obj_create(slot->panel);
    lv_obj_set_size(slot->status_dot, 16, 16);
    lv_obj_align(slot->status_dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(slot->status_dot, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot->status_dot, UI_COLOR_SLOT_EMPTY, LV_PART_MAIN);
    lv_obj_set_style_border_width(slot->status_dot, 0, LV_PART_MAIN);

    // Icon placeholder (LV_SYMBOL settings / wrench)
    slot->icon = lv_label_create(slot->panel);
    lv_obj_align(slot->icon, LV_ALIGN_LEFT_MID, 2, -10);
    lv_obj_set_style_text_font(slot->icon, &lv_font_montserrat_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(slot->icon, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_label_set_text(slot->icon, LV_SYMBOL_SETTINGS);

    // Tool name
    slot->name_label = lv_label_create(slot->panel);
    lv_obj_set_width(slot->name_label, 176);
    lv_obj_align(slot->name_label, LV_ALIGN_LEFT_MID, 52, -16);
    lv_obj_set_style_text_font(slot->name_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(slot->name_label, UI_COLOR_TEXT_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(slot->name_label, "---");

    // Tool ID
    slot->id_label = lv_label_create(slot->panel);
    lv_obj_align(slot->id_label, LV_ALIGN_LEFT_MID, 52, 18);
    lv_obj_set_style_text_font(slot->id_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(slot->id_label, UI_COLOR_TEXT_HINT, LV_PART_MAIN);
    lv_label_set_text(slot->id_label, "空位");

    slot->status_label = lv_label_create(slot->panel);
    lv_obj_add_style(slot->status_label, (lv_style_t *)ui_style_get_status_badge(), LV_PART_MAIN);
    lv_obj_align(slot->status_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_font(slot->status_label, UI_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(slot->status_label, UI_COLOR_TEXT_INVERSE, LV_PART_MAIN);
    lv_label_set_text(slot->status_label, "---");

    slot->status = TOOL_SLOT_EMPTY;
}

// ============================================================================
// Button event callbacks
// ============================================================================
static void confirm_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Confirm door close - user triggered");
    esp_err_t ret = cabinet_core_confirm_close_async();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Confirm close ignored: %s", esp_err_to_name(ret));
    } else {
        lv_obj_add_state(s_data.confirm_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_data.grace_label, "正在执行关门动作并同步数据...");
    }
}

static void grace_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_data.grace_label == NULL || ui_manager_get_current_screen() != SCREEN_OPERATION) {
        return;
    }

    bool alerted = false;
    uint32_t remain_ms = cabinet_core_get_close_grace_remaining_ms(&alerted);
    if (alerted) {
        lv_label_set_text(s_data.grace_label, "箱门未确认关闭，已发送告警，请立即关门");
        lv_obj_set_style_text_color(s_data.grace_label, UI_COLOR_DANGER, LV_PART_MAIN);
    } else if (remain_ms > 0) {
        uint32_t seconds = (remain_ms + 999) / 1000;
        lv_label_set_text_fmt(s_data.grace_label, "请在 %u 秒内确认关门", (unsigned int)seconds);
        lv_obj_set_style_text_color(s_data.grace_label, UI_COLOR_WARNING, LV_PART_MAIN);
    }
}

static void records_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_goto_screen(SCREEN_TOOL_RECORDS);
}

// ============================================================================
// Operation screen creation
// ============================================================================
void screen_operation_create(void)
{
    ESP_LOGI(TAG, "Creating operation screen...");

    memset(&s_data, 0, sizeof(s_data));

    // --- Screen background ---
    s_data.screen = lv_obj_create(NULL);
    lv_obj_add_style(s_data.screen, (lv_style_t *)ui_style_get_bg(), LV_PART_MAIN);
    lv_obj_set_size(s_data.screen, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_clear_flag(s_data.screen, LV_OBJ_FLAG_SCROLLABLE);

    // --- Header bar ---
    lv_obj_t *header = lv_obj_create(s_data.screen);
    lv_obj_set_size(header, UI_SCREEN_WIDTH, UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    s_data.title_label = lv_label_create(header);
    lv_obj_align(s_data.title_label, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_text_font(s_data.title_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.title_label, UI_COLOR_PRIMARY, LV_PART_MAIN);
    lv_label_set_text(s_data.title_label, LV_SYMBOL_EYE_OPEN "  工具柜操作");
    if (ui_touch_is_damaged_mode()) {
        lv_obj_align(s_data.title_label, LV_ALIGN_LEFT_MID, UI_TOUCH_SAFE_W + 20, 0);
    }

    // Records button
    s_data.records_btn = ui_create_btn_secondary_large(header);
    lv_obj_t *records_label = lv_label_create(s_data.records_btn);
    lv_obj_set_style_text_font(records_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(records_label, LV_SYMBOL_LIST "  记录");
    lv_obj_center(records_label);
    lv_obj_add_event_cb(s_data.records_btn, records_btn_event_cb, LV_EVENT_CLICKED, NULL);
    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_xy(s_data.records_btn, UI_TOUCH_SAFE_PAD, 8, UI_TOUCH_SAFE_CONTENT_W, UI_BTN_H_SAFE);
    } else {
        lv_obj_set_size(s_data.records_btn, 118, UI_BTN_H_MD);
        lv_obj_align(s_data.records_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    }

    // Operator name (right side)
    s_data.operator_label = lv_label_create(header);
    lv_obj_set_width(s_data.operator_label, 340);
    lv_obj_align(s_data.operator_label, LV_ALIGN_RIGHT_MID, -130, 0);
    lv_obj_set_style_text_font(s_data.operator_label, UI_FONT_SUBTITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.operator_label, UI_COLOR_TEXT_SECOND, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.operator_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(s_data.operator_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_data.operator_label, "操作员:---");

    // --- Tool Grid Container ---
    lv_obj_t *grid_container = lv_obj_create(s_data.screen);
    int32_t grid_w = TOOL_GRID_COLS * SLOT_W + (TOOL_GRID_COLS - 1) * SLOT_GAP + 2 * UI_PADDING_MD;
    int32_t grid_h = TOOL_GRID_ROWS * SLOT_H + (TOOL_GRID_ROWS - 1) * SLOT_GAP + 2 * UI_PADDING_MD;
    lv_obj_set_size(grid_container, grid_w, grid_h);
    lv_obj_align(grid_container, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_color(grid_container, UI_COLOR_BG_DARK, LV_PART_MAIN);
    lv_obj_set_style_radius(grid_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid_container, UI_PADDING_MD, LV_PART_MAIN);
    lv_obj_clear_flag(grid_container, LV_OBJ_FLAG_SCROLLABLE);

    // Set up grid layout
    static lv_coord_t col_dsc[TOOL_GRID_COLS + 1];
    static lv_coord_t row_dsc[TOOL_GRID_ROWS + 1];
    for (int i = 0; i < TOOL_GRID_COLS; i++) col_dsc[i] = SLOT_W;
    col_dsc[TOOL_GRID_COLS] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < TOOL_GRID_ROWS; i++) row_dsc[i] = SLOT_H;
    row_dsc[TOOL_GRID_ROWS] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_style_grid_column_dsc_array(grid_container, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(grid_container, row_dsc, 0);
    lv_obj_set_style_pad_column(grid_container, SLOT_GAP, 0);
    lv_obj_set_style_pad_row(grid_container, SLOT_GAP, 0);
    lv_obj_set_layout(grid_container, LV_LAYOUT_GRID);

    // Create all slot widgets
    for (uint8_t i = 0; i < TOOL_SLOT_MAX; i++) {
        uint8_t col = i % TOOL_GRID_COLS;
        uint8_t row = i / TOOL_GRID_COLS;
        create_slot_widget(i, grid_container, col, row);
    }
    s_data.slot_count = TOOL_SLOT_MAX;

    // --- Bottom button bar ---
    lv_obj_t *btn_bar = lv_obj_create(s_data.screen);
    lv_obj_set_size(btn_bar, UI_SCREEN_WIDTH, 78);
    lv_obj_align(btn_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_bar, UI_COLOR_BG_PANEL, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_bar, 20, 0);

    s_data.grace_label = lv_label_create(btn_bar);
    lv_obj_set_width(s_data.grace_label, 430);
    lv_obj_set_style_text_font(s_data.grace_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_data.grace_label, UI_COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_data.grace_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_data.grace_label, "请在 15 秒内确认关门");

    lv_obj_t *confirm_parent = ui_touch_is_damaged_mode() ? s_data.screen : btn_bar;
    s_data.confirm_btn = ui_create_btn_primary_large(confirm_parent);
    lv_obj_t *confirm_label = lv_label_create(s_data.confirm_btn);
    lv_obj_set_style_text_font(confirm_label, UI_FONT_BODY_BOLD, LV_PART_MAIN);
    lv_label_set_text(confirm_label, LV_SYMBOL_OK "  确认关门");
    lv_obj_center(confirm_label);

    if (ui_touch_is_damaged_mode()) {
        ui_touch_place_stack(s_data.confirm_btn, 0, 448, 84);
        lv_obj_add_event_cb(s_data.confirm_btn, confirm_btn_event_cb, LV_EVENT_PRESSED, NULL);
    } else {
        lv_obj_set_size(s_data.confirm_btn, 250, UI_BTN_H_LG);
        lv_obj_align(s_data.confirm_btn, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_event_cb(s_data.confirm_btn, confirm_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }

    ESP_LOGI(TAG, "Operation screen created (%d slots)", TOOL_SLOT_MAX);
    lv_timer_create(grace_timer_cb, 250, NULL);
}

// ============================================================================
// Public API
// ============================================================================

lv_obj_t *screen_operation_get_screen(void)
{
    return s_data.screen;
}

void screen_operation_init(tool_info_t *tools, uint8_t count)
{
    (void)tools;
    (void)count;

    tool_slot_runtime_t states[TOOL_CABINET_SLOT_COUNT];
    size_t state_count = 0;
    if (cabinet_core_get_states(states, TOOL_CABINET_SLOT_COUNT, &state_count) != ESP_OK ||
        state_count != TOOL_CABINET_SLOT_COUNT) {
        tool_cabinet_init_states(states, TOOL_CABINET_SLOT_COUNT);
        state_count = TOOL_CABINET_SLOT_COUNT;
    }

    const tool_catalog_entry_t *catalog = tool_cabinet_catalog();
    lv_obj_clear_state(s_data.confirm_btn, LV_STATE_DISABLED);
    uint8_t max = (uint8_t)tool_cabinet_catalog_count();
    for (uint8_t i = 0; i < TOOL_SLOT_MAX; i++) {
        slot_widget_t *slot = &s_data.slots[i];
        if (i < max) {
            lv_label_set_text(slot->name_label, catalog[i].tool_name);
            lv_label_set_text(slot->id_label, catalog[i].tool_code);
            apply_slot_status(slot, states[i].status);
        } else {
            lv_label_set_text(slot->name_label, "---");
            lv_label_set_text(slot->id_label, "空位");
            apply_slot_status(slot, TOOL_SLOT_EMPTY);
        }
    }

    ESP_LOGI(TAG, "Operation grid initialized: %u tools", max);
}

void screen_operation_update_slot(uint8_t slot_id, tool_slot_status_t status)
{
    if (slot_id >= TOOL_SLOT_MAX) return;

    slot_widget_t *slot = &s_data.slots[slot_id];
    apply_slot_status(slot, status);
}

void screen_operation_set_operator(const char *name)
{
    if (name != NULL) {
        lv_label_set_text_fmt(s_data.operator_label, "操作员:%s", name);
    }
}

void screen_operation_set_read_only(bool read_only)
{
    s_data.read_only = read_only;
    for (uint8_t i = 0; i < TOOL_SLOT_MAX; ++i) {
        if (s_data.slots[i].panel == NULL) {
            continue;
        }
        if (read_only) {
            lv_obj_clear_flag(s_data.slots[i].panel, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_flag(s_data.slots[i].panel, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    ESP_LOGI(TAG, "Operation slots are now %s", read_only ? "read-only" : "editable");
}

struct screen_operation_data_s *screen_operation_get_data(void)
{
    return &s_data;
}
