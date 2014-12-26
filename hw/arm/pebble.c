/*-
 * Copyright (c) 2013
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "stm32f2xx.h"
#include "stm32f4xx.h"
#include "hw/ssi.h"
#include "hw/boards.h"
#include "hw/display/pebble_snowy_display.h"
#include "hw/display/ls013b7dh01.h"
#include "hw/block/flash.h"
#include "sysemu/sysemu.h"
#include "sysemu/blockdev.h"
#include "ui/console.h"

//#define DEBUG_PEBBLE
#ifdef DEBUG_PEBBLE
// NOTE: The usleep() helps the MacOS stdout from freezing when we have a lot of print out
#define DPRINTF(fmt, ...)                                       \
    do { printf("DEBUG_PEBBLE: " fmt , ## __VA_ARGS__); \
         usleep(1000); \
    } while (0)
#else
#define DPRINTF(fmt, ...)
#endif


typedef enum {
  PBL_BUTTON_ID_NONE = -1,
  PBL_BUTTON_ID_BACK = 0,
  PBL_BUTTON_ID_UP = 1,
  PBL_BUTTON_ID_SELECT = 2,
  PBL_BUTTON_ID_DOWN = 3
  } PblButtonID;

typedef struct {
    int gpio;
    int pin;
} PblButtonMap;

const static PblButtonMap s_button_map_bb2_ev1_ev2[] = {
    {STM32_GPIOC_INDEX, 3},   /* back */
    {STM32_GPIOA_INDEX, 2},   /* up */
    {STM32_GPIOC_INDEX, 6},   /* select */
    {STM32_GPIOA_INDEX, 1},   /* down */
};

const static PblButtonMap s_button_map_bigboard[] = {
    {STM32_GPIOA_INDEX, 2},
    {STM32_GPIOA_INDEX, 1},
    {STM32_GPIOA_INDEX, 3},
    {STM32_GPIOC_INDEX, 9}
};

const static PblButtonMap s_button_map_snowy_bb[] = {
    {STM32_GPIOG_INDEX, 4},
    {STM32_GPIOG_INDEX, 3},
    {STM32_GPIOG_INDEX, 1},
    {STM32_GPIOG_INDEX, 2},
};

typedef struct {
    qemu_irq irq;
    bool pressed;
} PblButtonState;

static PblButtonID s_waiting_key_up_id = PBL_BUTTON_ID_NONE;
static QEMUTimer *s_button_timer;


static void prv_send_key_up(void *opaque)
{
    PblButtonState *bs = opaque;
    if (s_waiting_key_up_id == PBL_BUTTON_ID_NONE) {
        /* Should never happen */
        return;
    }

    DPRINTF("button %d released\n", s_waiting_key_up_id);
    qemu_set_irq(bs[s_waiting_key_up_id].irq, true);
    s_waiting_key_up_id = PBL_BUTTON_ID_NONE;
}


// NOTE: When running using a VNC display, we alwqys get a key-up immediately after the key-down,
// even if the user is holding the key down. For long presses, this results in a series of
// quick back to back key-down, key-up callbacks.
static void pebble_key_handler(void *arg, int keycode)
{
    PblButtonState *bs = arg;
    static int prev_keycode;

    int pressed = (keycode & 0x80) == 0;
    int button_id = PBL_BUTTON_ID_NONE;

    switch (keycode & 0x7F) {
    case 16: /* Q */
        button_id = PBL_BUTTON_ID_BACK;
        break;
    case 17: /* W */
        button_id = PBL_BUTTON_ID_UP;
        break;
    case 31: /* S */
        button_id = PBL_BUTTON_ID_SELECT;
        break;
    case 45: /* X */
        button_id = PBL_BUTTON_ID_DOWN;
        break;
    case 72: /* up arrow */
        if (prev_keycode == 224) {
            button_id = PBL_BUTTON_ID_UP;
        }
        break;
    case 80: /* down arrow */
        if (prev_keycode == 224) {
            button_id = PBL_BUTTON_ID_DOWN;
        }
        break;
    case 75: /* left arrow */
        if (prev_keycode == 224) {
            button_id = PBL_BUTTON_ID_BACK;
        }
        break;
    case 77: /* right arrow */
        if (prev_keycode == 224) {
            button_id = PBL_BUTTON_ID_SELECT;
        }
        break;
    default:
        break;
    }

    prev_keycode = keycode;
    if (button_id == PBL_BUTTON_ID_NONE || !pressed) {
        /* Ignore key ups and keys we don't care about */
        return;
    }

    // If this is a different key, and we are waiting for the prior one to key up, send the
    //  key up now
    if (s_waiting_key_up_id != PBL_BUTTON_ID_NONE && button_id != s_waiting_key_up_id) {
        prv_send_key_up(bs);
    }

    if (s_waiting_key_up_id != button_id) {
        DPRINTF("button %d pressed\n", button_id);
        s_waiting_key_up_id = button_id;
        qemu_set_irq(bs[button_id].irq, false);   // Pressed
    }

    /* Set or reschedule the timer to release the key */
    if (!s_button_timer) {
        s_button_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, prv_send_key_up, bs);
    }
    timer_mod(s_button_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 250);
}


static void pebble_32f2_init(MachineState *machine, const PblButtonMap *map) {
    Stm32Gpio *gpio[STM32F2XX_GPIO_COUNT];
    Stm32Uart *uart[STM32F2XX_UART_COUNT];
    Stm32Timer *timer[STM32F2XX_TIM_COUNT];
    DeviceState *spi_flash;
    SSIBus *spi;
    struct stm32f2xx stm;

    // Note: allow for bigger flash images (4MByte) to aid in development and debugging
    stm32f2xx_init(
        4096 /*flash size KBytes*/,
        128  /* ram size, KBytes */,
        machine->kernel_filename,
        gpio,
        uart,
        timer,
        8000000, /* osc_freq*/
        32768, /* osc2_freq*/
        &stm);

    /* SPI flash */
    spi = (SSIBus *)qdev_get_child_bus(stm.spi_dev[0], "ssi");
    spi_flash = ssi_create_slave_no_init(spi, "n25q032a11");
    qdev_init_nofail(spi_flash);

    qemu_irq cs;
    cs = qdev_get_gpio_in_named(spi_flash, SSI_GPIO_CS, 0);
    qdev_connect_gpio_out((DeviceState *)gpio[STM32_GPIOA_INDEX], 4, cs);

    /* Display */
    spi = (SSIBus *)qdev_get_child_bus(stm.spi_dev[1], "ssi");
    DeviceState *display_dev = ssi_create_slave_no_init(spi, "sm-lcd");
    qdev_init_nofail(display_dev);

    /* UARTs */
    stm32_uart_connect(uart[0], serial_hds[0], 0); /* UART1: not used */
    stm32_uart_connect(uart[1], serial_hds[1], 0); /* UART2: Pebble protocol */
    stm32_uart_connect(uart[2], serial_hds[2], 0); /* UART3: console */

    /* Buttons */
    static PblButtonState bs[4];
    int i;
    for (i = 0; i < 4; i++) {
        bs[i].pressed = 0;
        bs[i].irq = qdev_get_gpio_in((DeviceState *)gpio[map[i].gpio], map[i].pin);
    }
    qemu_add_kbd_event_handler(pebble_key_handler, bs);

    /* Hook up the display brightness to Timer 3's PWM setting */
    qdev_prop_set_ptr((DeviceState *)timer[3-1], "pwm_ratio_callback_arg",
                          (void *)display_dev);
    qdev_prop_set_ptr((DeviceState *)timer[3-1], "pwm_ratio_callback",
                          (void *)sm_lcd_set_brightness);
}

static void pebble_32f4_init(MachineState *machine, const PblButtonMap *map) {
    Stm32Gpio *gpio[STM32F4XX_GPIO_COUNT];
    Stm32Uart *uart[STM32F4XX_UART_COUNT];
    Stm32Timer *timer[STM32F4XX_TIM_COUNT];
    SSIBus *spi;
    struct stm32f4xx stm;

    // Note: allow for bigger flash images (4MByte) to aid in development and debugging
    stm32f4xx_init(4096 /*flash_size in KBytes */, 256 /*ram_size on KBytes*/,
        machine->kernel_filename, gpio, uart, timer, 8000000 /*osc_freq*/,
        32768 /*osc2_freq*/, &stm);


    /* Storage flash (NOR-flash on Snowy) */
    const uint32_t flash_size_bytes = 16 * 1024 * 1024;  /* 16 MBytes */
    const uint32_t flash_sector_size_bytes = 32 * 1024;  /* 32 KBytes */
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 1);   /* Use the 2nd -pflash drive */
    if (dinfo) {
        pflash_jedec_424_register(
            0x60000000,               /* flash_base*/
            NULL,                     /* qdev, not used */
            "mx29vs128fb",            /* name */
            flash_size_bytes,         /* size */
            dinfo->bdrv,              /* driver state */
            flash_sector_size_bytes,  /* sector size */
            flash_size_bytes / flash_sector_size_bytes, /* number of sectors */
            2,                        /* width in bytes */
            0x00c2, 0x007e, 0x0065, 0x0001, /* id: 0, 1, 2, 3 */
            0                         /* big endian */
        );
    }


    /* --- Display ------------------------------------------------  */
    spi = (SSIBus *)qdev_get_child_bus(stm.spi_dev[5], "ssi");
    DeviceState *display_dev = ssi_create_slave_no_init(spi, "pebble-snowy-display");

    /* Create the outputs that the display will drive and associate them with the correct
     * GPIO input pins on the MCU */
    qemu_irq display_done_irq = qdev_get_gpio_in((DeviceState *)gpio[STM32_GPIOG_INDEX], 9);
    qdev_prop_set_ptr(display_dev, "done_output", display_done_irq);
    qemu_irq display_intn_irq = qdev_get_gpio_in((DeviceState *)gpio[STM32_GPIOG_INDEX], 10);
    qdev_prop_set_ptr(display_dev, "intn_output", display_intn_irq);
    qdev_init_nofail(display_dev);

    /* Connect the correct MCU GPIO outputs to the inputs on the display */
    qemu_irq display_cs;
    display_cs = qdev_get_gpio_in_named(display_dev, SSI_GPIO_CS, 0);
    qdev_connect_gpio_out((DeviceState *)gpio[STM32_GPIOG_INDEX], 8, display_cs);

    qemu_irq display_reset;
    display_reset = qdev_get_gpio_in_named(display_dev, "pebble-snowy-display-reset", 0);
    qdev_connect_gpio_out((DeviceState *)gpio[STM32_GPIOG_INDEX], 15, display_reset);

    qemu_irq display_sclk;
    display_sclk = qdev_get_gpio_in_named(display_dev, "pebble-snowy-display-sclk", 0);
    qdev_connect_gpio_out((DeviceState *)gpio[STM32_GPIOG_INDEX], 13, display_sclk);


    /* UARTs */
    stm32_uart_connect(uart[0], serial_hds[0], 0); /* UART1: not used */
    stm32_uart_connect(uart[1], serial_hds[1], 0); /* UART2: Pebble protocol */
    stm32_uart_connect(uart[2], serial_hds[2], 0); /* UART3: console */

    /* Buttons */
    static PblButtonState bs[4];
    int i;
    for (i = 0; i < 4; i++) {
        bs[i].pressed = 0;
        bs[i].irq = qdev_get_gpio_in((DeviceState *)gpio[map[i].gpio], map[i].pin);
    }
    qemu_add_kbd_event_handler(pebble_key_handler, bs);

    /* Hook up the display brightness to Timer 12's PWM setting */
    qdev_prop_set_ptr((DeviceState *)timer[11], "pwm_ratio_callback_arg",
                          (void *)display_dev);
    qdev_prop_set_ptr((DeviceState *)timer[11], "pwm_ratio_callback",
                          (void *)ps_display_set_brightness);
}

static void
pebble_bb2_init(MachineState *machine) {
    pebble_32f2_init(machine, s_button_map_bb2_ev1_ev2);
}

static void
pebble_bb_init(MachineState *machine) {
    pebble_32f2_init(machine, s_button_map_bigboard);
}

static void
pebble_snowy_init(MachineState *machine) {
    pebble_32f4_init(machine, s_button_map_snowy_bb);
}



static QEMUMachine pebble_bb2_machine = {
    .name = "pebble-bb2",
    .desc = "Pebble smartwatch (bb2/ev1/ev2)",
    .init = pebble_bb2_init
};

static QEMUMachine pebble_bb_machine = {
    .name = "pebble-bb",
    .desc = "Pebble smartwatch (bb)",
    .init = pebble_bb_init
};

static QEMUMachine pebble_snowy_bb_machine = {
    .name = "pebble-snowy-bb",
    .desc = "Pebble smartwatch (snowy)",
    .init = pebble_snowy_init
};


static void pebble_machine_init(void)
{
    qemu_register_machine(&pebble_bb2_machine);
    qemu_register_machine(&pebble_bb_machine);
    qemu_register_machine(&pebble_snowy_bb_machine);
}

machine_init(pebble_machine_init);
