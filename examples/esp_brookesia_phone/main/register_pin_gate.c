#include "register_pin_gate.h"

#include <string.h>

#define REGISTER_PIN_CODE "000000"

bool register_pin_gate_is_valid(const char *pin)
{
    return pin != NULL && strcmp(pin, REGISTER_PIN_CODE) == 0;
}
