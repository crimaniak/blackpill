#include "pin_ctrl.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "timers.h"
#include <string.h>

/* Compile-time table of every exposed GPIO pin on the Black Pill
   (STM32F411CEU6, UFQFPN48).  Reserved pins produce an error message
   instead of being driven.                                           */
static const struct {
    char          name[6];
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       reserved;   /* 1 = USB or SWD — must not be driven */
} s_pin_defs[] = {
    {"PA0",  GPIOA, GPIO_PIN_0,  0},   /* KEY button (active LOW) */
    {"PA1",  GPIOA, GPIO_PIN_1,  0},
    {"PA2",  GPIOA, GPIO_PIN_2,  0},
    {"PA3",  GPIOA, GPIO_PIN_3,  0},
    {"PA4",  GPIOA, GPIO_PIN_4,  0},
    {"PA5",  GPIOA, GPIO_PIN_5,  0},
    {"PA6",  GPIOA, GPIO_PIN_6,  0},
    {"PA7",  GPIOA, GPIO_PIN_7,  0},
    {"PA8",  GPIOA, GPIO_PIN_8,  0},
    {"PA9",  GPIOA, GPIO_PIN_9,  0},
    {"PA10", GPIOA, GPIO_PIN_10, 0},
    {"PA11", GPIOA, GPIO_PIN_11, 1},   /* USB D- */
    {"PA12", GPIOA, GPIO_PIN_12, 1},   /* USB D+ */
    {"PA13", GPIOA, GPIO_PIN_13, 1},   /* SWDIO  */
    {"PA14", GPIOA, GPIO_PIN_14, 1},   /* SWCLK  */
    {"PA15", GPIOA, GPIO_PIN_15, 0},
    {"PB0",  GPIOB, GPIO_PIN_0,  0},
    {"PB1",  GPIOB, GPIO_PIN_1,  0},
    {"PB2",  GPIOB, GPIO_PIN_2,  0},
    {"PB3",  GPIOB, GPIO_PIN_3,  0},
    {"PB4",  GPIOB, GPIO_PIN_4,  0},
    {"PB5",  GPIOB, GPIO_PIN_5,  0},
    {"PB6",  GPIOB, GPIO_PIN_6,  0},
    {"PB7",  GPIOB, GPIO_PIN_7,  0},
    {"PB8",  GPIOB, GPIO_PIN_8,  0},
    {"PB9",  GPIOB, GPIO_PIN_9,  0},
    {"PB10", GPIOB, GPIO_PIN_10, 0},
    {"PB12", GPIOB, GPIO_PIN_12, 0},
    {"PB13", GPIOB, GPIO_PIN_13, 0},
    {"PB14", GPIOB, GPIO_PIN_14, 0},
    {"PB15", GPIOB, GPIO_PIN_15, 0},
    {"PC13", GPIOC, GPIO_PIN_13, 0},   /* Onboard LED (active LOW) */
    {"PC14", GPIOC, GPIO_PIN_14, 0},
    {"PC15", GPIOC, GPIO_PIN_15, 0},
};
#define PIN_DEFS_COUNT ((int)(sizeof(s_pin_defs) / sizeof(s_pin_defs[0])))

typedef struct {
    uint8_t       active;
    GPIO_TypeDef *port;
    uint16_t      pin_mask;
    PinMode       mode;
    uint32_t      freq_hz;
    TimerHandle_t timer;
    char          name[6];
} PinEntry;

static PinEntry s_pins[MAX_PINS];

/* Software timer callback — called from timer daemon task context. */
static void toggle_cb(TimerHandle_t t) {
    PinEntry *e = (PinEntry *)pvTimerGetTimerID(t);
    HAL_GPIO_TogglePin(e->port, e->pin_mask);
}

static int find_def(const char *name) {
    for (int i = 0; i < PIN_DEFS_COUNT; i++)
        if (strcmp(s_pin_defs[i].name, name) == 0)
            return i;
    return -1;
}

static PinEntry *find_entry(const char *name) {
    for (int i = 0; i < MAX_PINS; i++)
        if (s_pins[i].active && strcmp(s_pins[i].name, name) == 0)
            return &s_pins[i];
    return NULL;
}

static PinEntry *alloc_entry(void) {
    for (int i = 0; i < MAX_PINS; i++)
        if (!s_pins[i].active)
            return &s_pins[i];
    return NULL;
}

static void release_entry(PinEntry *e) {
    if (e->timer) {
        xTimerStop(e->timer, 0);
        xTimerDelete(e->timer, 0);
        e->timer = NULL;
    }
    HAL_GPIO_DeInit(e->port, e->pin_mask);
    memset(e, 0, sizeof(*e));
}

int pin_ctrl_set(const char *name, PinMode mode, uint32_t freq_hz) {
    int di = find_def(name);
    if (di < 0)                    return -2;   /* unknown */
    if (s_pin_defs[di].reserved)   return -1;   /* reserved */

    PinEntry *e = find_entry(name);
    if (!e) {
        e = alloc_entry();
        if (!e) return -3;   /* table full */
        e->active   = 1;
        e->port     = s_pin_defs[di].port;
        e->pin_mask = s_pin_defs[di].pin;
        e->timer    = NULL;
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
    }

    /* Stop previous toggle timer if switching mode */
    if (e->timer) {
        xTimerStop(e->timer, 0);
        xTimerDelete(e->timer, 0);
        e->timer = NULL;
    }

    GPIO_InitTypeDef g = {
        .Pin   = e->pin_mask,
        .Mode  = GPIO_MODE_OUTPUT_PP,
        .Speed = GPIO_SPEED_FREQ_LOW,
    };
    HAL_GPIO_Init(e->port, &g);

    e->mode    = mode;
    e->freq_hz = freq_hz;

    switch (mode) {
    case PIN_LOW:
        HAL_GPIO_WritePin(e->port, e->pin_mask, GPIO_PIN_RESET);
        break;
    case PIN_HIGH:
        HAL_GPIO_WritePin(e->port, e->pin_mask, GPIO_PIN_SET);
        break;
    case PIN_TOGGLE: {
        /* Period = half-period of requested frequency (50% duty cycle).
           Minimum 1 ms tick → maximum 500 Hz. */
        TickType_t period = pdMS_TO_TICKS(1000u / (2u * freq_hz));
        if (period < 1) period = 1;
        e->timer = xTimerCreate("tog", period, pdTRUE, e, toggle_cb);
        if (e->timer)
            xTimerStart(e->timer, 0);
        break;
    }
    default:
        break;
    }

    return 0;
}

int pin_ctrl_reset_pin(const char *name) {
    int di = find_def(name);
    if (di < 0) return -2;

    PinEntry *e = find_entry(name);
    if (!e) return -2;

    release_entry(e);
    return 0;
}

void pin_ctrl_reset_all(void) {
    for (int i = 0; i < MAX_PINS; i++)
        if (s_pins[i].active)
            release_entry(&s_pins[i]);
}

/* Print decimal representation of v via print_fn. */
static void print_u32(void (*fn)(const char*), uint32_t v) {
    char tmp[12];
    int k = 11;
    tmp[k] = '\0';
    if (v == 0) { tmp[--k] = '0'; }
    else { while (v) { tmp[--k] = '0' + (v % 10); v /= 10; } }
    fn(tmp + k);
}

void pin_ctrl_list(void (*print_fn)(const char *)) {
    int any = 0;
    for (int i = 0; i < MAX_PINS; i++) {
        PinEntry *e = &s_pins[i];
        if (!e->active) continue;
        any = 1;
        /* Pad name to 5 chars: "  PA0  " → "  PA0    " */
        char padded[8] = "  ";
        int j = 2, nlen = (int)strlen(e->name);
        for (int k = 0; k < nlen; k++) padded[j++] = e->name[k];
        while (j < 7) padded[j++] = ' ';
        padded[j] = '\0';
        print_fn(padded);
        if (e->mode == PIN_LOW)       { print_fn("LOW\r\n"); }
        else if (e->mode == PIN_HIGH) { print_fn("HIGH\r\n"); }
        else {
            print_fn("TOGGLE ");
            print_u32(print_fn, e->freq_hz);
            print_fn(" Hz\r\n");
        }
    }
    if (!any)
        print_fn("  (no pins configured)\r\n");
}
