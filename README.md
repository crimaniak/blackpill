# blackpill-pinconsole

Port of [`../bluepill-pinconsole`](../bluepill-pinconsole) to the Black Pill
(STM32F411CEU6, 512 KiB flash, 128 KiB SRAM, 25 MHz HSE). It is a USB CDC serial
console for driving GPIO pins from a terminal: set a pin low or high, or toggle
it at 1-500 Hz. The board enumerates as `0483:5740` "STM32 Virtual ComPort".

The command set, parser and pin logic are unchanged. See the Blue Pill README
for the full command reference.

| Command | Effect |
|---|---|
| `pin <NAME> 0` / `1` | Drive the pin low / high |
| `pin <NAME> <N>` | Toggle at N Hz (1-500) |
| `pin <NAME> reset` | Release the pin back to input-floating |
| `list` | Show active pins |
| `reset` | Release all pins |
| `help` | Show the command list |

Pins: `PA0`-`PA15`, `PB0`-`PB10`, `PB12`-`PB15`, `PC13`-`PC15`.
`PA11`, `PA12` (USB) and `PA13`, `PA14` (SWD) are reserved. `PC13` is the
onboard LED (active low) and `PA0` is the KEY button.

## Differences from the Blue Pill version

| | Blue Pill | Black Pill (this) |
|---|---|---|
| MCU | STM32F103C8T6, Cortex-M3 | STM32F411CEU6, Cortex-M4F |
| HSE / SYSCLK | 8 MHz / 72 MHz | 25 MHz / 96 MHz (PLL M=25 N=192 P=2 Q=4, USB 48 MHz) |
| USB peripheral | USB FS device (PMA) | OTG_FS in device mode, FIFOs set in `usbd_conf.c`, VBUS sensing off |
| USB IRQ | `USB_LP_CAN1_RX0` | `OTG_FS` |
| FreeRTOS port | `ARM_CM3` | `ARM_CM4F` (hard float) |
| Heap | 10 KiB | 20 KiB |
| Pin table | has `PB11` | `PB11` removed (not bonded out on the 48-pin F411) |
| Flash tool | OpenOCD with clone IDCODE override | `st-flash` |

## Layout

- `Src/main.c`: clocks, USB re-enumeration (PA12 low for 100 ms), RTOS objects, console task
- `Src/console.c`, `Src/pin_ctrl.c`: command line and pin control, same as the Blue Pill
- `Src/usbd_conf.c`, `Src/usbd_desc.c`, `Src/usbd_cdc_if.c`: USB device glue
- `Src/stm32f4xx_it.c`: `SysTick_Handler` and `OTG_FS_IRQHandler`
- `STM32F411CE_FLASH.ld`: linker script
- `original.bin`: dump of the flash as found on the board (512 KiB), kept as a backup

## Dependencies

Except for FreeRTOS, these are sibling directories of this one, found through `EMB` in the `Makefile`:

- `cmsis-device-f4` (clone of STMicroelectronics/cmsis-device-f4)
- `stm32f4xx-hal-driver` (clone of STMicroelectronics/stm32f4xx-hal-driver)
- `STM32CubeF1/Drivers/CMSIS/Include` for the CMSIS core headers
- `stm32_mw_usb_device`
- `Middlewares/FreeRTOS`: FreeRTOS-Kernel V11.3.2, a git submodule (`git submodule update --init`)
- `arm-none-eabi-gcc` and `st-flash`

## Build and flash

```sh
make            # build/firmware.elf and build/firmware.bin
make flash      # st-flash write to 0x08000000, then reset
make clean
```

To restore the original firmware:
`st-flash write original.bin 0x08000000`

## Status

Tested on a Black Pill (STM32F411CEU6): enumerates as `/dev/ttyACM0`, all
console commands work, and pin levels and a 100 Hz toggle were confirmed by
reading the GPIO input registers over SWD. Toggle frequency accuracy was not
measured with a scope or logic analyzer.

Builds to about 26.6 KB flash and 25.7 KB RAM (mostly the FreeRTOS heap and the
USB library's static buffer).

Note: the RTOS queue and semaphore are created after `USBD_Start()`, not before.
A critical section entered before the scheduler starts leaves BASEPRI raised,
which masks SysTick and makes the `HAL_Delay()` calls inside USB start-up hang.
