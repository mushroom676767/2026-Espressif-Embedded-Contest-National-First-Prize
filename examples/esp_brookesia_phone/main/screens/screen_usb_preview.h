#ifndef SCREEN_USB_PREVIEW_H
#define SCREEN_USB_PREVIEW_H

#include "ui_manager.h"

void screen_usb_preview_create(void);
lv_obj_t *screen_usb_preview_get_screen(void);
void screen_usb_preview_start(const char *operator_name, auth_type_t auth_type);

#endif // SCREEN_USB_PREVIEW_H
