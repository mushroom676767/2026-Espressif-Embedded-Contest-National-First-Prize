/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FOD Smart Tool Cabinet - Dark Industrial Theme
 *
 * Color palette inspired by industrial control systems and aviation displays.
 * Deep blue backgrounds with high-contrast text for readability.
 */

// ============================================================================
// Background Colors
// ============================================================================
#define UI_COLOR_BG_DARK        lv_color_hex(0x0D1B2A)    // Deep navy black
#define UI_COLOR_BG_PANEL       lv_color_hex(0x1B263B)    // Dark blue panel
#define UI_COLOR_BG_CARD        lv_color_hex(0x415A77)    // Medium blue card
#define UI_COLOR_BG_INPUT       lv_color_hex(0x1B263B)    // Input field bg

// ============================================================================
// Primary / Accent Colors
// ============================================================================
#define UI_COLOR_PRIMARY        lv_color_hex(0x00D4FF)    // Tech cyan
#define UI_COLOR_PRIMARY_DIM    lv_color_hex(0x0096B7)    // Dimmed cyan
#define UI_COLOR_SECONDARY      lv_color_hex(0x778DA9)    // Muted blue-gray

// ============================================================================
// Status Colors
// ============================================================================
#define UI_COLOR_SUCCESS        lv_color_hex(0x00E676)    // Bright green
#define UI_COLOR_SUCCESS_DIM    lv_color_hex(0x1B5E20)    // Dimmed green
#define UI_COLOR_WARNING        lv_color_hex(0xFFB300)    // Amber
#define UI_COLOR_WARNING_DIM    lv_color_hex(0xE65100)    // Dimmed orange
#define UI_COLOR_DANGER         lv_color_hex(0xFF1744)    // Alert red
#define UI_COLOR_DANGER_DIM     lv_color_hex(0xB71C1C)    // Dimmed red
#define UI_COLOR_ONLINE         lv_color_hex(0x00E676)    // Online indicator
#define UI_COLOR_OFFLINE        lv_color_hex(0xFF1744)    // Offline indicator

// ============================================================================
// Text Colors
// ============================================================================
#define UI_COLOR_TEXT_PRIMARY   lv_color_hex(0xFFFFFF)    // Primary text
#define UI_COLOR_TEXT_SECOND    lv_color_hex(0xB0BEC5)    // Secondary text
#define UI_COLOR_TEXT_HINT      lv_color_hex(0x607D8B)    // Hint/disabled text
#define UI_COLOR_TEXT_INVERSE   lv_color_hex(0x0D1B2A)    // Text on bright bg

// ============================================================================
// Border / Divider Colors
// ============================================================================
#define UI_COLOR_BORDER         lv_color_hex(0x37474F)    // Default border
#define UI_COLOR_BORDER_FOCUS   lv_color_hex(0x00D4FF)    // Focused border
#define UI_COLOR_DIVIDER        lv_color_hex(0x263238)    // Divider lines

// ============================================================================
// Button Colors
// ============================================================================
#define UI_COLOR_BTN_PRIMARY_BG     UI_COLOR_PRIMARY
#define UI_COLOR_BTN_PRIMARY_TEXT   UI_COLOR_TEXT_INVERSE
#define UI_COLOR_BTN_SUCCESS_BG     UI_COLOR_SUCCESS
#define UI_COLOR_BTN_SUCCESS_TEXT   UI_COLOR_TEXT_INVERSE
#define UI_COLOR_BTN_DANGER_BG      UI_COLOR_DANGER
#define UI_COLOR_BTN_DANGER_TEXT    UI_COLOR_TEXT_PRIMARY
#define UI_COLOR_BTN_SECONDARY_BG   UI_COLOR_BG_CARD
#define UI_COLOR_BTN_SECONDARY_TEXT UI_COLOR_TEXT_PRIMARY
#define UI_COLOR_BTN_DISABLED_BG    lv_color_hex(0x37474F)
#define UI_COLOR_BTN_DISABLED_TEXT  UI_COLOR_TEXT_HINT

// ============================================================================
// Tool Slot Colors
// ============================================================================
#define UI_COLOR_SLOT_AVAILABLE    UI_COLOR_SUCCESS       // Tool present (green)
#define UI_COLOR_SLOT_BORROWED     UI_COLOR_DANGER        // Tool borrowed (red)
#define UI_COLOR_SLOT_WARNING      UI_COLOR_WARNING       // Anomaly (amber)
#define UI_COLOR_SLOT_EMPTY        UI_COLOR_BORDER        // Empty slot outline

// ============================================================================
// Typography
// ============================================================================
const lv_font_t *ui_font_title(void);
const lv_font_t *ui_font_subtitle(void);
const lv_font_t *ui_font_body(void);
const lv_font_t *ui_font_body_bold(void);
const lv_font_t *ui_font_caption(void);
const lv_font_t *ui_font_status(void);
const lv_font_t *ui_font_time(void);
const lv_font_t *ui_font_count(void);

#define UI_FONT_TITLE          ui_font_title()            // Main titles
#define UI_FONT_SUBTITLE       ui_font_subtitle()         // Subtitles
#define UI_FONT_BODY           ui_font_body()             // Body text
#define UI_FONT_BODY_BOLD      ui_font_body_bold()        // Bold body
#define UI_FONT_CAPTION        ui_font_caption()          // Captions
#define UI_FONT_STATUS         ui_font_status()           // Status text
#define UI_FONT_TIME           ui_font_time()             // Large time display
#define UI_FONT_COUNT          ui_font_count()            // Count/number display

// ============================================================================
// Layout Constants
// ============================================================================
#define UI_SCREEN_WIDTH        1024
#define UI_SCREEN_HEIGHT       600

#define UI_PADDING_SM          8
#define UI_PADDING_MD          16
#define UI_PADDING_LG          24
#define UI_PADDING_XL          32

#define UI_RADIUS_SM            4
#define UI_RADIUS_MD            8
#define UI_RADIUS_LG            12
#define UI_RADIUS_XL            16

#define UI_BORDER_WIDTH         1
#define UI_BORDER_WIDTH_THICK   2

#define UI_HEADER_H             64
#define UI_BTN_H_MD             46
#define UI_BTN_H_LG             58
#define UI_BTN_H_SAFE           52
#define UI_TILE_H_LG            132
#define UI_ROW_H_LG             72

#define UI_ICON_SIZE_SM         24
#define UI_ICON_SIZE_MD         32
#define UI_ICON_SIZE_LG         48
#define UI_ICON_SIZE_XL         64

// ============================================================================
// Global Style Initializers
// ============================================================================

/**
 * @brief Initialize global theme styles
 *
 * Call this once at app startup after LVGL display is ready.
 * Creates reusable style objects for common element types.
 */
void ui_theme_init(void);

/**
 * @brief Get the screen background style
 */
const lv_style_t *ui_style_get_bg(void);

/**
 * @brief Get the panel style (cards, containers)
 */
const lv_style_t *ui_style_get_panel(void);

/**
 * @brief Get the primary button style
 */
const lv_style_t *ui_style_get_btn_primary(void);
const lv_style_t *ui_style_get_btn_primary_large(void);

/**
 * @brief Get the secondary button style
 */
const lv_style_t *ui_style_get_btn_secondary(void);
const lv_style_t *ui_style_get_btn_secondary_large(void);

/**
 * @brief Get the danger button style
 */
const lv_style_t *ui_style_get_btn_danger(void);
const lv_style_t *ui_style_get_btn_danger_large(void);

/**
 * @brief Get a compact status badge style
 */
const lv_style_t *ui_style_get_status_badge(void);

/**
 * @brief Get the title label style
 */
const lv_style_t *ui_style_get_title(void);

/**
 * @brief Get the body label style
 */
const lv_style_t *ui_style_get_body(void);

/**
 * @brief Get the caption style
 */
const lv_style_t *ui_style_get_caption(void);

/**
 * @brief Create a flat container with rounded corners
 *
 * @param parent Parent LVGL object
 * @return lv_obj_t* The created container
 */
lv_obj_t *ui_create_container(lv_obj_t *parent);

/**
 * @brief Create a primary button with standard styling
 *
 * @param parent Parent LVGL object
 * @return lv_obj_t* The created button
 */
lv_obj_t *ui_create_btn_primary(lv_obj_t *parent);
lv_obj_t *ui_create_btn_primary_large(lv_obj_t *parent);

/**
 * @brief Create a secondary button with standard styling
 *
 * @param parent Parent LVGL object
 * @return lv_obj_t* The created button
 */
lv_obj_t *ui_create_btn_secondary(lv_obj_t *parent);
lv_obj_t *ui_create_btn_secondary_large(lv_obj_t *parent);

/**
 * @brief Create a danger button with large touch-friendly styling
 */
lv_obj_t *ui_create_btn_danger_large(lv_obj_t *parent);

/**
 * @brief Create a standard full-width header bar
 */
lv_obj_t *ui_screen_create_header(lv_obj_t *parent, const char *title, lv_obj_t **title_label_out);

/**
 * @brief Apply font and color to a label
 */
void ui_label_set_font_color(lv_obj_t *label, const lv_font_t *font, lv_color_t color);

/**
 * @brief Create a title label
 *
 * @param parent Parent LVGL object
 * @return lv_obj_t* The created label
 */
lv_obj_t *ui_create_title(lv_obj_t *parent);

/**
 * @brief Create a body label
 *
 * @param parent Parent LVGL object
 * @return lv_obj_t* The created label
 */
lv_obj_t *ui_create_body(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif // UI_THEME_H
