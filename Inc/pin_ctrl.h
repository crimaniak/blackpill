#pragma once
#include <stdint.h>

#define MAX_PINS 16

typedef enum {
    PIN_UNSET,
    PIN_LOW,
    PIN_HIGH,
    PIN_TOGGLE,
} PinMode;

/* Set pin mode. freq_hz only used when mode=PIN_TOGGLE.
 * Returns: 0=ok, -1=pin reserved, -2=pin unknown, -3=table full */
int  pin_ctrl_set(const char *name, PinMode mode, uint32_t freq_hz);

/* Release a single pin back to input-floating.
 * Returns: 0=ok, -2=not found / not active */
int  pin_ctrl_reset_pin(const char *name);

/* Release all pins. */
void pin_ctrl_reset_all(void);

/* Print all active pins via print_fn callback. */
void pin_ctrl_list(void (*print_fn)(const char *));
