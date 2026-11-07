#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void screen_tool_records_create(void);
lv_obj_t *screen_tool_records_get_screen(void);
void screen_tool_records_refresh(void);

#ifdef __cplusplus
}
#endif
