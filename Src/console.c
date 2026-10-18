#include "console.h"
#include "pin_ctrl.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdlib.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

/* Transmit s over USB CDC, waiting for any previous transmit to finish.
   The pointer must remain valid until TransmitCplt fires — string literals
   (in flash) and static buffers are safe; stack variables are not.         */
static void cdc_write_raw(const uint8_t *buf, uint32_t len) {
    if (!len) return;
    /* Wait for previous transfer to complete (100 ms max). */
    if (xSemaphoreTake(g_cdc_tx_done, pdMS_TO_TICKS(100)) == pdTRUE) {
        USBD_CDC_SetTxBuffer(&hUsbDeviceFS, (uint8_t *)buf, len);
        if (USBD_CDC_TransmitPacket(&hUsbDeviceFS) != USBD_OK)
            xSemaphoreGive(g_cdc_tx_done);   /* restore if transmit failed */
    }
}

static void cdc_write(const char *s) {
    cdc_write_raw((const uint8_t *)s, (uint32_t)strlen(s));
}

/* Echo a single character — uses a static buffer (valid until TransmitCplt) */
static void cdc_write_char(char c) {
    static uint8_t echo_buf;
    echo_buf = (uint8_t)c;
    cdc_write_raw(&echo_buf, 1);
}

static void process_command(const char *line) {
    if (line[0] == '\0') return;

    if (strcmp(line, "help") == 0) {
        cdc_write("Commands:\r\n"
                  "  pin <NAME> 0         drive pin low\r\n"
                  "  pin <NAME> 1         drive pin high\r\n"
                  "  pin <NAME> <N>       toggle at N Hz  (1-500)\r\n"
                  "  pin <NAME> reset     release pin to input-floating\r\n"
                  "  list                 show all active pins\r\n"
                  "  reset                release ALL pins\r\n"
                  "  help                 this help\r\n");
        return;
    }

    if (strcmp(line, "list") == 0) {
        pin_ctrl_list(cdc_write);
        return;
    }

    if (strcmp(line, "reset") == 0) {
        pin_ctrl_reset_all();
        cdc_write("OK\r\n");
        return;
    }

    if (strncmp(line, "pin ", 4) == 0) {
        char name[8]      = {0};
        char mode_str[16] = {0};

        const char *p = line + 4;
        while (*p == ' ') p++;
        int i = 0;
        while (*p && *p != ' ' && i < 7)  name[i++] = *p++;
        name[i] = '\0';
        while (*p == ' ') p++;
        i = 0;
        while (*p && *p != ' ' && i < 15) mode_str[i++] = *p++;
        mode_str[i] = '\0';

        if (name[0] == '\0' || mode_str[0] == '\0') {
            cdc_write("Usage: pin <NAME> 0|1|<Hz>|reset\r\n");
            return;
        }

        if (strcmp(mode_str, "reset") == 0) {
            int r = pin_ctrl_reset_pin(name);
            cdc_write(r == 0 ? "OK\r\n" : "Error: pin not active\r\n");
            return;
        }

        PinMode mode;
        uint32_t freq = 0;
        if (strcmp(mode_str, "0") == 0) {
            mode = PIN_LOW;
        } else if (strcmp(mode_str, "1") == 0) {
            mode = PIN_HIGH;
        } else {
            char *end;
            long val = strtol(mode_str, &end, 10);
            if (*end != '\0' || val < 1 || val > 500) {
                cdc_write("Error: MODE must be 0, 1, reset, or 1-500 (Hz)\r\n");
                return;
            }
            mode = PIN_TOGGLE;
            freq = (uint32_t)val;
        }

        int r = pin_ctrl_set(name, mode, freq);
        if (r == 0) {
            cdc_write("OK\r\n");
        } else if (r == -1) {
            cdc_write("Error: ");
            cdc_write(name);
            cdc_write(" is reserved (USB/SWD)\r\n");
        } else if (r == -2) {
            cdc_write("Error: ");
            cdc_write(name);
            cdc_write(" unknown\r\n");
        } else if (r == -3) {
            cdc_write("Error: too many pins active (max 16)\r\n");
        }
        return;
    }

    cdc_write("Unknown command. Type 'help'.\r\n");
}

void ConsoleTask(void *pvParam) {
    (void)pvParam;

    /* Wait for the host to open the terminal (DTR assert) */
    while (!g_cdc_connected)
        vTaskDelay(pdMS_TO_TICKS(100));

    cdc_write("\r\nBlackpill PinConsole\r\nType 'help' for commands.\r\n> ");

    char line[64];
    int  pos = 0;
    int  last_cr = 0;   /* previous byte was '\r': swallow a following '\n' (CRLF) */

    for (;;) {
        uint8_t ch;
        /* Block up to 200 ms between bytes (handles slow typists and
           gives reconnect logic a chance to run).                   */
        if (xQueueReceive(g_cdc_rx_queue, &ch, pdMS_TO_TICKS(200)) != pdTRUE) {
            /* Timeout — check if host disconnected and reconnected */
            if (!g_cdc_connected) {
                while (!g_cdc_connected)
                    vTaskDelay(pdMS_TO_TICKS(100));
                pos = 0;
                cdc_write("\r\nBlackpill PinConsole\r\n> ");
            }
            continue;
        }

        int was_cr = last_cr;
        last_cr = (ch == '\r');

        if (ch == '\n' && was_cr) {
            continue;   /* second half of CRLF */
        } else if (ch == '\r' || ch == '\n') {
            cdc_write("\r\n");
            line[pos] = '\0';
            process_command(line);
            pos = 0;
            cdc_write("> ");
        } else if ((ch == '\b' || ch == 127) && pos > 0) {
            pos--;
            cdc_write("\b \b");
        } else if (ch >= 0x20 && pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
            cdc_write_char((char)ch);
        }
    }
}
