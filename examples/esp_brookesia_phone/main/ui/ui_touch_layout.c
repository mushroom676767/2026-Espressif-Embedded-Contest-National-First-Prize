#include "ui_touch_layout.h"

bool ui_touch_is_damaged_mode(void)
{
    return UI_DAMAGED_TOUCH_LEFT_QUARTER != 0;
}

void ui_touch_set_safe_btn_size(lv_obj_t *obj, int32_t height)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_size(obj, UI_TOUCH_SAFE_CONTENT_W, height);
}

void ui_touch_place_stack(lv_obj_t *obj, uint8_t index, int32_t y_start, int32_t height)
{
    if (obj == NULL) {
        return;
    }

    ui_touch_set_safe_btn_size(obj, height);
    lv_obj_align(obj,
                 LV_ALIGN_TOP_LEFT,
                 UI_TOUCH_SAFE_PAD,
                 y_start + index * (height + UI_TOUCH_SAFE_GAP));
}

void ui_touch_place_xy(lv_obj_t *obj, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_size(obj, w, h);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
}
