#ifndef UI_TOUCH_LAYOUT_H
#define UI_TOUCH_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UI_DAMAGED_TOUCH_LEFT_QUARTER
#define UI_DAMAGED_TOUCH_LEFT_QUARTER 0
#endif

#define UI_TOUCH_SAFE_X          0
#define UI_TOUCH_SAFE_Y          0
#define UI_TOUCH_SAFE_W          (UI_SCREEN_WIDTH / 4)
#define UI_TOUCH_SAFE_H          UI_SCREEN_HEIGHT
#define UI_TOUCH_SAFE_PAD        10
#define UI_TOUCH_SAFE_GAP        8
#define UI_TOUCH_SAFE_CONTENT_W  (UI_TOUCH_SAFE_W - UI_TOUCH_SAFE_PAD * 2)

bool ui_touch_is_damaged_mode(void);
void ui_touch_set_safe_btn_size(lv_obj_t *obj, int32_t height);
void ui_touch_place_stack(lv_obj_t *obj, uint8_t index, int32_t y_start, int32_t height);
void ui_touch_place_xy(lv_obj_t *obj, int32_t x, int32_t y, int32_t w, int32_t h);

#ifdef __cplusplus
}
#endif

#endif // UI_TOUCH_LAYOUT_H
