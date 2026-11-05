/*
 * BBK 9288S research machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "exec/log.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "system/system.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/pixel_ops.h"

#define TYPE_XP3200S_MACHINE   MACHINE_TYPE_NAME("xp3200s")
#define TYPE_XP3200_MACHINE    MACHINE_TYPE_NAME("xp3200")
OBJECT_DECLARE_SIMPLE_TYPE(XP3200SMachineState, XP3200S_MACHINE)

#define XP3200S_SDRAM_BASE     0x02000000u
#define XP3200S_DEFAULT_RAM    (8 * MiB)
#define XP3200S_IRAM_SIZE      (16 * KiB)
#define XP3200S_IRAM_RTC_COOKIE_OFFSET (XP3200S_IRAM_SIZE - 4)
#define XP3200S_BOOT_STACK_TOP 0x00003f80u
#define XP3200S_BOARD_IO_BASE  0x01000000u
#define XP3200S_BOARD_IO_SIZE  0x00010000u
#define XP3200S_BOARD_USB_STATUS 0x22
#define XP3200S_BOARD_USB_CONNECTED_BIT 0x80
#define XP3200S_BOARD_CTRL     0x82
#define XP3200S_BOARD_DATA     0x83
#define XP3200S_BOARD_STATUS_H 0x84
#define XP3200S_BOARD_STATUS_L 0x85
#define XP3200S_IO_BASE        0x00040000u
#define XP3200S_IO_SIZE        0x00010000u
#define XP3200S_TOUCH_IO_BASE  0x00300000u
#define XP3200S_TOUCH_IO_SIZE  0x00001000u
#define XP3200S_TOUCH_SERIAL   0x20
#define XP3200S_TOUCH_CONFIG   0x49
#define XP3200S_TOUCH_CLK      0x80
#define XP3200S_TOUCH_MISO     0x10
#define XP3200S_TOUCH_MOSI     0x08
#define XP3200S_AUDIO_IO_BASE  0x003a0000u
#define XP3200S_AUDIO_IO_SIZE  0x00000010u
#define XP3200_AUDIO_IO_SIZE   0x00001000u
#define XP3200S_AUDIO_DATA_IN  0x0000
#define XP3200S_AUDIO_DATA_OUT 0x0002
#define XP3200S_AUDIO_CONTROL  0x0004
#define XP3200S_AUDIO_CONFIG   0x0008
#define XP3200S_AUDIO_STATUS   0x000a
#define XP3200S_AUDIO_READ_READY  0x04
#define XP3200S_AUDIO_WRITE_READY 0x10
#define XP3200S_AUDIO_STREAM_FLUSH_BYTES 512
#define XP3200S_AUDIO_FIFO_CAPACITY (2 * KiB)
#define XP3200S_AUDIO_DREQ_BYTES 32
#define XP3200S_AUDIO_DREQ_GPIO_BIT 0x08
#define XP3200S_AUDIO_DEFAULT_BYTES_PER_SECOND 16000
#define XP3200S_AUDIO_STREAM_MAX_BYTES (32 * MiB)
#define XP3200S_AUDIO_PLAYER_BUFFER_SIZE 4096
#define XP3200_EXPANSION_READY  0x0202
#define XP3200S_AUDIO_SCI_WRITE 0x02
#define XP3200S_AUDIO_SCI_READ  0x03
#define XP3200S_AUDIO_SCI_MODE  0x00
#define XP3200S_AUDIO_SCI_DECODE_TIME 0x04
#define XP3200S_AUDIO_SCI_AUDATA 0x05
#define XP3200S_AUDIO_SCI_VOL   0x0b
#define XP3200S_AUDIO_SCI_MODE_RESET 0x0004
#define XP3200S_AUDIO_SCI_MODE_SDINEW 0x0800
#define XP3200S_AUDIO_DEFAULT_AUDATA 0xac45
/* The VS1003 decoder and the SD card share the SPI block at 0x003a0000;
 * P3.0 low selects the card and high selects the decoder. */
#define XP3200S_SPI_SPIRD      0x00
#define XP3200S_SPI_SPITD      0x02
#define XP3200S_SPI_CTRL       0x04
#define XP3200S_SPI_WAIT       0x08
#define XP3200S_SPI_STATUS     0x0a
#define XP3200S_SPI_INTCTL     0x0c
#define XP3200S_SPI_STATUS_RDFF 0x0004
#define XP3200S_SPI_STATUS_TDEF 0x0010
#define XP3200S_SPI_STATUS_BSYF 0x0040
#define XP3200S_SD_CS_PORT     0x22
#define XP3200S_SD_BLOCK_SIZE  512
#define XP3200S_SD_ST_IDLE      0
#define XP3200S_SD_ST_ARG       1
#define XP3200S_SD_ST_RESP      2
#define XP3200S_SD_ST_TAIL      3
#define XP3200S_SD_ST_READ_TOKEN 4
#define XP3200S_SD_ST_READ_DATA 5
#define XP3200S_SD_ST_READ_CRC  6
#define XP3200S_SD_ST_WAIT_TOKEN 7
#define XP3200S_SD_ST_WRITE_DATA 8
#define XP3200S_SD_ST_WRITE_CRC 9
#define XP3200S_SD_ST_WRITE_RESP 10
#define XP3200S_NAND_DATA_BASE 0x04000000u
#define XP3200S_NAND_DATA_SIZE 0x00001000u
#define XP3200S_NAND_CE_SELECT 0x100
#define XP3200S_NAND_ECC_READY 0x101
#define XP3200S_NAND_ECC_ENABLE 0x102
#define XP3200S_NAND_MODE      0x103
#define XP3200S_NAND_ECC_BASE  0x104
#define XP3200S_NAND_ECC_SIZE  6
#define XP3200S_NAND_CLE       0x10
#define XP3200S_NAND_ALE       0x20
#define XP3200S_NAND_READY     0x40
/* XP3200 routes CLE/ALE through P1D: bit6 = CLE, bit5 = ALE. */
#define XP3200S_P1D_DATA       0x02d5
#define XP3200S_P1D_CLE        0x40
#define XP3200S_P1D_ALE        0x20
/* K9F1G08U0D: 2 column cycles + 2 row cycles for read/program, 2 row
 * cycles for block erase (16-bit page number, 12-bit column). */
#define XP3200S_NAND_ADDR_CYCLES 4
/* K9F1G08: EC F1 00 15 */
#define XP3200S_NAND_DEFAULT_ID 0x1500f1ecu
#define XP3200S_NAND_PAGE_SIZE 2048
#define XP3200S_NAND_OOB_SIZE  64
#define XP3200S_NAND_RAW_PAGE_SIZE \
    (XP3200S_NAND_PAGE_SIZE + XP3200S_NAND_OOB_SIZE)
#define XP3200S_NAND_PAGES_PER_BLOCK 64
#define XP3200S_NAND_BLOCKS    1024
#define XP3200S_NAND_PAGE_COUNT \
    (XP3200S_NAND_PAGES_PER_BLOCK * XP3200S_NAND_BLOCKS)
#define XP3200S_NAND_STORAGE_SIZE \
    ((size_t)XP3200S_NAND_PAGE_COUNT * XP3200S_NAND_RAW_PAGE_SIZE)
#define XP3200S_NAND_RAW_BLOCK_SIZE \
    (XP3200S_NAND_RAW_PAGE_SIZE * XP3200S_NAND_PAGES_PER_BLOCK)
#define XP3200S_NAND_DIRTY_BYTES DIV_ROUND_UP(XP3200S_NAND_BLOCKS, 8)
#define XP3200S_NAND_FLUSH_DELAY_MS 250
#define XP3200S_NAND_BLOCK_DATA_SIZE \
    (XP3200S_NAND_PAGE_SIZE * XP3200S_NAND_PAGES_PER_BLOCK)
#define XP3200S_NAND_RESERVED_BLOCKS 40
#define XP3200S_NAND_LOGICAL_BLOCKS  984
#define XP3200S_FAT_SECTOR_SIZE      512
#define XP3200S_FAT_MAX_DEPTH        8
#define XP3200S_FAT_CLUSTER_END      0xfff8
#define XP3200S_LCDC_IO_BASE   0x00380000u
#define XP3200S_LCDC_IO_SIZE   0x00010000u
#define XP3200S_IVRAM_BASE     0x003c0000u
#define XP3200S_IVRAM_SIZE     (40 * KiB)
#define XP3200S_KERNEL_HEADER  0x40
#define XP3200S_LCD_WIDTH      160
#define XP3200S_LCD_HEIGHT     240
#define XP3200_LCD_WIDTH       320
#define XP3200_LCD_HEIGHT      240
#define XP3200S_LCD_MAX_BPP    8
#define XP3200S_MMIO_TRACE_LEN 64
#define XP3200S_TBRP_OFFSET    0x812d
#define XP3200S_TTBR_OFFSET    0x8134
#define XP3200S_USB_IRQ_BIT    0x40
#define XP3200S_ADC_DATA_LO    0x0240
#define XP3200S_ADC_DATA_HI    0x0241
#define XP3200S_ADC_TRIGGER    0x0242
#define XP3200S_ADC_CHANNEL    0x0243
#define XP3200S_ADC_ENABLE     0x0244
#define XP3200S_ADC_SAMPLING   0x0245
#define XP3200S_ADC_FLAGS      0x0246
#define XP3200S_ADC_OVERWRITE  0x0247
#define XP3200S_ADC_BUFFER_BASE 0x0248
#define XP3200S_ADC_CHANNELS   8
#define XP3200S_ADC_BATTERY_OK 0x03c0
#define XP3200S_ADC_UPPER_LO   0x0258
#define XP3200S_ADC_UPPER_HI   0x0259
#define XP3200S_ADC_LOWER_LO   0x025a
#define XP3200S_ADC_LOWER_HI   0x025b
#define XP3200S_ADC_INTMASK    0x025c
#define XP3200S_ADC_MODE       0x025f
#define XP3200S_PIR_PORT0_1    0x0260
#define XP3200S_PIR_PORT2_3    0x0261
#define XP3200S_PIR_KEY_PORT0_3 0x0262
#define XP3200S_PIR_SIF1_ADC   0x026a
#define XP3200S_PIR2_16TM0_1   0x0266
#define XP3200S_PIR3_16TM2_3   0x0267
#define XP3200S_PIR4_16TM4_5   0x0268
#define XP3200S_PIR_8TM_SIF0   0x0269
#define XP3200S_PIR_PORT4_5    0x026c
#define XP3200S_PIR_PORT6_7    0x026d
#define XP3200S_EIR0_KEY_PORT0_3 0x0270
#define XP3200S_EIR3_16TM2_3   0x0273
#define XP3200S_EIR4_16TM4_5   0x0274
#define XP3200S_EIR5_8TM0_3    0x0275
#define XP3200S_EIR7_PORT4_7_CTM_ADC 0x0277
#define XP3200S_FIR0_KEY_PORT0_3 0x0280
#define XP3200S_FIR3_16TM2_3   0x0283
#define XP3200S_FIR4_16TM4_5   0x0284
#define XP3200S_FIR5_8TM0_3    0x0285
#define XP3200S_FIR7_PORT4_7_CTM_ADC 0x0287
#define XP3200S_ADC_IRQ_BIT    0x01
#define XP3200S_ADC_VECTOR     64
#define XP3200S_PORT0_VECTOR   12
#define XP3200S_KEY0_IRQ_BIT   0x10
#define XP3200S_KEY1_IRQ_BIT   0x20
#define XP3200S_KEY0_VECTOR    16
#define XP3200S_KEY1_VECTOR    17
#define XP3200S_EIR2_16TM0_1   0x0272
#define XP3200S_FIR2_16TM0_1   0x0282
#define XP3200S_16TM0A_IRQ_BIT 0x08
#define XP3200S_16TM0A_VECTOR  31
#define XP3200S_16TM0B_IRQ_BIT 0x04
#define XP3200S_16TM0B_VECTOR  30
#define XP3200S_16TM1A_IRQ_BIT 0x80
#define XP3200S_16TM1A_VECTOR  35
#define XP3200S_16TM1B_IRQ_BIT 0x40
#define XP3200S_16TM1B_VECTOR  34
#define XP3200S_16TM2B_IRQ_BIT 0x04
#define XP3200S_16TM2B_VECTOR  38
#define XP3200S_16TM3B_IRQ_BIT 0x40
#define XP3200S_16TM3B_VECTOR  42
#define XP3200S_16TM3B_WAKE_MS 10
#define XP3200S_16TM4B_IRQ_BIT 0x04
#define XP3200S_16TM4B_VECTOR  46
#define XP3200S_8TM0_VECTOR    52
#define XP3200S_8TM0_3_MASK    0x0f
#define XP3200S_8TM_CTRL_BASE  0x0160
#define XP3200S_8TM_STRIDE     4
#define XP3200S_8TM_CHANNELS   4
#define XP3200S_8TM_DIRECT_CLOCK 0x0146
#define XP3200S_8TM_CLOCK01    0x014d
#define XP3200S_8TM_CLOCK23    0x014e
#define XP3200S_8TM_CTRL_RUN   0x01
#define XP3200S_8TM_CTRL_PRESET 0x02
#define XP3200S_CTM_RUN_CTRL   0x0151
#define XP3200S_CTM_INT_CTRL   0x0152
#define XP3200S_CTM_DIVIDER    0x0153
#define XP3200S_CTM_SEC        0x0154
#define XP3200S_CTM_MIN        0x0155
#define XP3200S_CTM_HOUR       0x0156
#define XP3200S_CTM_DAY_LO     0x0157
#define XP3200S_CTM_DAY_HI     0x0158
#define XP3200S_CTM_MIN_CMP    0x0159
#define XP3200S_CTM_HOUR_CMP   0x015a
#define XP3200S_CTM_DAY_CMP    0x015b
#define XP3200S_SIF0_STATUS    0x01e2
#define XP3200S_SIF1_STATUS    0x01e7
#define XP3200S_SIF2_STATUS    0x01f2
#define XP3200S_SIF3_STATUS    0x01f7
#define XP3200S_SIF_TDBE       0x02
#define XP3200S_CTM_COUNTER_BASE XP3200S_CTM_SEC
#define XP3200S_CTM_COUNTER_SIZE 5
#define XP3200S_CTM_TCRUN      0x01
#define XP3200S_CTM_TCRST      0x02
#define XP3200S_CTM_TCAF       0x01
#define XP3200S_CTM_TCIF       0x02
#define XP3200S_CTM_SELECT_MASK 0xfc
#define XP3200S_CTM_TICKS_PER_SECOND 32
#define XP3200S_CTM_TICK_MS    31
#define XP3200S_CTM_EV_32HZ    0x01
#define XP3200S_CTM_EV_8HZ     0x02
#define XP3200S_CTM_EV_2HZ     0x04
#define XP3200S_CTM_EV_1HZ     0x08
#define XP3200S_CTM_EV_1MIN    0x10
#define XP3200S_CTM_EV_1HOUR   0x20
#define XP3200S_CTM_EV_1DAY    0x40
#define XP3200S_16TM_CLOCK_CTRL_BASE 0x0147
#define XP3200S_16TM_CHANNELS  6
#define XP3200S_16TM_STRIDE    0x0008
#define XP3200S_16TM_BASE      0x8180
#define XP3200S_16TM_CLOCK_CTRL(n) \
    (XP3200S_16TM_CLOCK_CTRL_BASE + (n))
#define XP3200S_16TM_COMPARE_A(n) \
    (XP3200S_16TM_BASE + (n) * XP3200S_16TM_STRIDE)
#define XP3200S_16TM_COMPARE_B(n) (XP3200S_16TM_COMPARE_A(n) + 2)
#define XP3200S_16TM_COUNTER(n)   (XP3200S_16TM_COMPARE_A(n) + 4)
#define XP3200S_16TM_CTRL(n)      (XP3200S_16TM_COMPARE_A(n) + 6)
#define XP3200S_16TM0_CLOCK_CTRL XP3200S_16TM_CLOCK_CTRL(0)
#define XP3200S_16TM_CTRL_PRUN    0x01
#define XP3200S_16TM_CTRL_PRESET  0x02
#define XP3200S_16TM_CTRL_PTM     0x04
#define XP3200S_16TM_CTRL_CKSL    0x08
#define XP3200S_16TM_CTRL_READ_MASK 0x7d
#define XP3200S_PRESCALER_CLOCK_SELECT 0x0181
#define XP3200S_PRESCALER_OSC1    0x01
#define XP3200S_OSC1_HZ           32768
#define XP3200S_OSC3_PLL_HZ        48000000
#define XP3200S_CLG_POWER_CTRL 0x0180
#define XP3200S_CLG_PRESCALER  0x0181
#define XP3200S_CLG_CLOCK_OPT  0x0190
#define XP3200S_CLG_PROTECT    0x019e
#define XP3200S_PIR_CTM        0x026b
#define XP3200S_CTM_IRQ_BIT    0x02
#define XP3200S_CTM_VECTOR     65
#define XP3200S_PORT4_IRQ_BIT  0x04
#define XP3200S_PORT4_VECTOR   68
#define XP3200S_HSDMA_TRIG01   0x0298
#define XP3200S_HSDMA_TRIG23   0x0299
#define XP3200S_HSDMA_SW_TRIG  0x029a
#define XP3200S_HSDMA_BASE     0x8220
#define XP3200S_HSDMA_CH_SIZE  0x10
#define XP3200S_HSDMA_CHANNELS 4
#define XP3200S_PORT0_FUNC     0x02d0
#define XP3200S_PORT0_DATA     0x02d1
#define XP3200S_PORT0_IOCTRL   0x02d2
#define XP3200S_PORT1_FUNC     0x02d4
#define XP3200S_PORT1_DATA     0x02d5
#define XP3200S_PORT1_IOCTRL   0x02d6
#define XP3200S_PORT2_FUNC     0x02d8
#define XP3200S_PORT2_DATA     0x02d9
#define XP3200S_PORT2_IOCTRL   0x02da
#define XP3200S_BACKLIGHT_GPIO_BIT 0x08
#define XP3200S_PORT3_FUNC     0x02dc
#define XP3200S_PORT3_DATA     0x02dd
#define XP3200S_PORT3_IOCTRL   0x02de
#define XP3200S_KEY_GPIO_PORT  0
#define XP3200S_KEY_P0_MASK    0xfb
#define XP3200S_KEY_STROBE_BIT 0x08
#define XP3200S_KEY_LINES       6
/* Last valid debug-key-rc / host-key rc value: five rows x 14 columns. */
#define XP3200_KEY_RC_LAST      69
#define XP3200_KEY_ROWS          8
#define XP3200_KEY_COLS          7
#define XP3200_KEY_MATRIX_IDS    (XP3200_KEY_ROWS * \
                                   (XP3200_KEY_COLS + 1))
#define XP3200_KEY_ROW_SELECT    0x0f46
#define XP3200S_K5_FUNC_SELECT 0x02c0
#define XP3200S_K5_DATA        0x02c1
#define XP3200S_K6_DATA        0x02c4
#define XP3200S_PORT_INT_SELECT1 0x02c6
#define XP3200S_PORT_INT_SELECT2 0x02c7
#define XP3200S_PORT_INT_POLARITY 0x02c8
#define XP3200S_PORT_INT_EDGE_LEVEL 0x02c9
#define XP3200S_KEY_INPUT_SELECT 0x02ca
#define XP3200S_KEY_INPUT_CONDITION0 0x02cc
#define XP3200S_KEY_INPUT_CONDITION1 0x02cd
#define XP3200S_KEY_INPUT_MASK0 0x02ce
#define XP3200S_KEY_INPUT_MASK1 0x02cf
#define XP3200S_K6_FUNC_SELECT 0x02c3
#define XP3200S_WAKE_K6_BIT    0x10
#define XP3200S_DEFAULT_TOUCH_FPT 6
/*
 * Physical column -> mask bit (measured from the firmware's position code).
 */
static const uint8_t xp3200_kbd_col_bit[14] = {
    10, 9, 11, 13, 12, 0, 1, 2, 3, 4, 5, 8, 7, 6,
};

/*
 * Host key -> keypad matrix (row, column index).
 *
 * The firmware scans 14 columns and reads a 5-bit key code (K5[3:0] +
 * K6.4).  It stores the resulting position (0x02210DE4) as
 *      position = 14 * (row - 1) + col_bit[column] + 1
 * where xp3200_kbd_col_bit[] maps the column index used below to the mask
 * bit the firmware drives.  Row 5 (K6.4 low, K5[3:0] = 0xF) is the fifth
 * matrix row and was only reachable once the comparator got a wake value.
 *
 * The key names for every position are not guessed: the retail firmware
 * keeps the authoritative table at 0x021D20B0 (key index -> position) and
 * 0x021D1E28 (key index -> key code), indexed through the key-name table at
 * 0x021E30BC.  Inverting them gives, for all five rows:
 *
 *   row 1: Right Down 0 5 - Esc Speech Follow Help Tab Space - Left Enter
 *   row 2: PgDn Up 4 F5 - Shift Z X C V B Input M N
 *   row 3: PgUp Del 3 F4 F1 A S D F G H L K J
 *   row 4: . P 1 F3 F2 Q W E R T Y O I U
 *   row 5: 9 8 2 volume Font Catalog - Udisk Xddict Tongue Vlisten 7 6 Mp3
 *
 * ("-" marks an empty matrix slot.)  The device hotkeys have no host keys
 * of their own; they are mapped to F6..F12 and the numeric keypad so the
 * whole keyboard is reachable.
 */
typedef struct XP3200KbdMap {
    int qcode;
    uint8_t row;
    uint8_t bit;
} XP3200KbdMap;

static const XP3200KbdMap xp3200_kbd_map[] = {
    /* row 1: Right Down 0 5 - Esc Speech Follow Help Tab Space - Left Enter */
    { Q_KEY_CODE_RIGHT,      1,  5 },
    { Q_KEY_CODE_DOWN,       1,  6 },
    { Q_KEY_CODE_0,          1,  7 },
    { Q_KEY_CODE_5,          1,  8 },
    { Q_KEY_CODE_ESC,        1, 10 },
    { Q_KEY_CODE_F6,         1, 13 },   /* Speech  (报时) */
    { Q_KEY_CODE_F7,         1, 12 },   /* Follow  (复读) */
    { Q_KEY_CODE_F8,         1, 11 },   /* Help    (帮助) */
    { Q_KEY_CODE_TAB,        1,  1 },
    { Q_KEY_CODE_SPC,        1,  0 },
    { Q_KEY_CODE_LEFT,       1,  4 },
    { Q_KEY_CODE_RET,        1,  3 },
    { Q_KEY_CODE_KP_ENTER,   1,  3 },

    /* row 2: PageDown Up 4 F5 - Shift Z X C V B Input M N */
    { Q_KEY_CODE_PGDN,       2,  5 },
    { Q_KEY_CODE_UP,         2,  6 },
    { Q_KEY_CODE_4,          2,  7 },
    { Q_KEY_CODE_F5,         2,  8 },
    { Q_KEY_CODE_CAPS_LOCK,  2, 10 },   /* Caps Shift */
    { Q_KEY_CODE_Z,          2, 13 },
    { Q_KEY_CODE_X,          2, 12 },
    { Q_KEY_CODE_C,          2, 11 },
    { Q_KEY_CODE_V,          2,  1 },
    { Q_KEY_CODE_B,          2,  0 },
    { Q_KEY_CODE_INSERT,     2,  2 },   /* Input (输入法) */
    { Q_KEY_CODE_M,          2,  4 },
    { Q_KEY_CODE_N,          2,  3 },

    /* row 3: PageUp Delete 3 F4 F1 A S D F G H L K J */
    { Q_KEY_CODE_PGUP,       3,  5 },
    { Q_KEY_CODE_DELETE,     3,  6 },
    { Q_KEY_CODE_BACKSPACE,  3,  6 },
    { Q_KEY_CODE_3,          3,  7 },
    { Q_KEY_CODE_F4,         3,  8 },
    { Q_KEY_CODE_F1,         3,  9 },
    { Q_KEY_CODE_A,          3, 10 },
    { Q_KEY_CODE_S,          3, 13 },
    { Q_KEY_CODE_D,          3, 12 },
    { Q_KEY_CODE_F,          3, 11 },
    { Q_KEY_CODE_G,          3,  1 },
    { Q_KEY_CODE_H,          3,  0 },
    { Q_KEY_CODE_L,          3,  2 },
    { Q_KEY_CODE_K,          3,  4 },
    { Q_KEY_CODE_J,          3,  3 },

    /* row 4: . P 1 F3 F2 Q W E R T Y O I U */
    { Q_KEY_CODE_DOT,        4,  5 },
    { Q_KEY_CODE_P,          4,  6 },
    { Q_KEY_CODE_1,          4,  7 },
    { Q_KEY_CODE_F3,         4,  8 },
    { Q_KEY_CODE_F2,         4,  9 },
    { Q_KEY_CODE_Q,          4, 10 },
    { Q_KEY_CODE_W,          4, 13 },
    { Q_KEY_CODE_E,          4, 12 },
    { Q_KEY_CODE_R,          4, 11 },
    { Q_KEY_CODE_T,          4,  1 },
    { Q_KEY_CODE_Y,          4,  0 },
    { Q_KEY_CODE_O,          4,  2 },
    { Q_KEY_CODE_I,          4,  4 },
    { Q_KEY_CODE_U,          4,  3 },

    /* row 5: 9 8 2 volume Font Catalog - Udisk Xddict Tongue Vlisten 7 6 Mp3 */
    { Q_KEY_CODE_9,          5,  5 },
    { Q_KEY_CODE_8,          5,  6 },
    { Q_KEY_CODE_2,          5,  7 },
    { Q_KEY_CODE_KP_5,       5,  8 },   /* volume  (音量; F12 保留给自检后门) */
    { Q_KEY_CODE_F11,        5,  9 },   /* Font    (字体) */
    { Q_KEY_CODE_F9,         5, 10 },   /* Catalog (目录) */
    { Q_KEY_CODE_F10,        5, 12 },   /* Udisk   (U 盘) */
    { Q_KEY_CODE_KP_1,       5, 11 },   /* Xddict  (现汉朗文) */
    { Q_KEY_CODE_KP_2,       5,  1 },   /* Tongue  (背单词教材) */
    { Q_KEY_CODE_KP_3,       5,  0 },   /* Vlisten (测评视听) */
    { Q_KEY_CODE_7,          5,  2 },
    { Q_KEY_CODE_6,          5,  4 },
    { Q_KEY_CODE_KP_4,       5,  3 },   /* Mp3     (录音 MP3) */
};

/* Hidden factory self-test menu entry (XP3200V1.9.5 image). *//* Hidden factory self-test menu entry (XP3200V1.9.5 image). *//* Hidden factory self-test menu entry (XP3200V1.9.5 image). *//* Hidden factory self-test menu entry (XP3200V1.9.5 image). *//* Hidden factory self-test menu entry (XP3200V1.9.5 image). *//* Hidden factory self-test menu entry (XP3200V1.9.5 image). */
#define XP3200_SELFTEST_MENU_ADDR 0x0217d6ceu
#define XP3200S_LCDC_HNDP      0x0040
#define XP3200S_LCDC_HSIZE     0x0042
#define XP3200S_LCDC_VNDP      0x004a
#define XP3200S_LCDC_VSIZE     0x004c
#define XP3200S_LCDC_MODE0     0x0200
#define XP3200S_LCDC_MODE1     0x0202
#define XP3200S_LCDC_SADDR     0x0210
#define XP3200S_LCDC_LUT_BASE  0x0400
#define XP3200S_LCDC_LUT_END   0x0800
#define XP3200S_LCDC_LUT_ENTRY_SIZE 4
#define XP3200S_LCDC_LUT_GREEN 1
#define XP3200S_LCDC_BPP_MASK  0x07
#define XP3200S_LCDC_LUTPASS   0x10
/* LCDC mode register 1 (0x380202), manual VII-2-42. */
#define XP3200S_LCDC_MODE1_BLANK 0x0100u   /* D8 */
#define XP3200S_LCDC_MODE1_SWINV 0x0200u   /* D9 */
#define XP3200S_16TM0_COMPARE_A XP3200S_16TM_COMPARE_A(0)
#define XP3200S_16TM0_COMPARE_B XP3200S_16TM_COMPARE_B(0)
#define XP3200S_16TM0_COUNTER   XP3200S_16TM_COUNTER(0)
#define XP3200S_16TM0_CTRL      XP3200S_16TM_CTRL(0)

static const uint8_t xp3200s_board_latch_data[] = {
    0x00, 0xff, 0x55, 0xaa,
};

static const uint8_t xp3200s_iram_rtc_cookie[] = {
    0x01, 0x02, 0x03, 0x04,
};

struct XP3200SMachineState {
    MachineState parent_obj;

    uint32_t debug_usb_wakeup_ms;
    uint32_t debug_nmi_wakeup_ms;
    uint32_t debug_port4_wakeup_ms;
    uint32_t debug_port5_wakeup_ms;
    uint32_t debug_key_p0_low_mask;
    uint32_t debug_lcd_dump_ms;
    uint32_t debug_lcd_dump_seq;
    uint32_t debug_call_ms;
    uint32_t debug_call_addr;
    uint32_t audio_force_data_in;
    uint32_t debug_key_rowsel;
    uint32_t trace_io_all_ms;
    uint32_t trace_io_all_len;
    unsigned trace_io_all_count;
    uint8_t trace_dup[0x10000];
    uint32_t debug_key_rc;
    uint8_t kbd_scan_col;
    uint8_t kbd_p0_latch;
    uint8_t host_key_row;
    uint8_t host_key_col;
    uint8_t host_key_stack_row[16];
    uint8_t host_key_stack_col[16];
    unsigned host_key_stack_len;
    bool kbd_row5_present;
    unsigned kbd_mask_trace;
    unsigned kbd_rc_trace;
    unsigned kbd_gate_trace;
    unsigned kbd_row5_trace;
    uint32_t touch_fpt;
    uint32_t touch_k5_low_mask;
    char *debug_lcd_dump_path;
    char *nand_image_path;
    char *sd_image_path;
    uint32_t nand_id;
    uint32_t nand_linear;
    char *audio_stream_path;
    char *audio_player_path;
    char *audio_inject_path;
    uint32_t audio_inject_ms;
    bool usb_connected;
    bool key_matrix;
    bool touch_p0_low;
    bool debug_timer16_factors;
    uint32_t debug_sd_insert_ms;
    bool trace_io;
    bool trace_key_scan;
    bool trace_audio;
    uint32_t debug_key_code;
    uint32_t debug_key_matrix;
    bool strict_board_io;
    bool is_9288;
    void *soc;              /* XP3200SState, set in xp3200s_init */
};

typedef enum XP3200SRegFlags {
    XP3200S_REG_NONE = 0,
    XP3200S_REG_RESET_ONE_TO_CLEAR = 1 << 0,
} XP3200SRegFlags;

typedef enum XP3200STouchSerialPhase {
    XP3200S_TOUCH_COMMAND,
    XP3200S_TOUCH_DUMMY,
    XP3200S_TOUCH_DATA,
    XP3200S_TOUCH_TRAILER,
} XP3200STouchSerialPhase;

typedef enum XP3200SNANDPhase {
    XP3200S_NAND_IDLE,
    XP3200S_NAND_READ_ID,
    XP3200S_NAND_READ_STATUS,
    XP3200S_NAND_READ_SETUP,
    XP3200S_NAND_READ_DATA,
    XP3200S_NAND_ERASE_SETUP,
    XP3200S_NAND_PROGRAM_DATA,
    XP3200S_NAND_COPYBACK_READY,
} XP3200SNANDPhase;

typedef struct XP3200SRegInfo {
    hwaddr offset;
    const char *name;
    const char *group;
    uint8_t mask;
    uint8_t reset;
    XP3200SRegFlags flags;
} XP3200SRegInfo;

typedef struct XP3200SITCPair {
    hwaddr enable_offset;
    hwaddr flag_offset;
    uint8_t mask;
    const char *name;
} XP3200SITCPair;

typedef struct XP3200SMMIOTrace {
    uint64_t seq;
    uint32_t pc;
    hwaddr addr;
    uint64_t value;
    unsigned size;
    bool is_write;
} XP3200SMMIOTrace;

typedef struct XP3200SIVRAMStats {
    uint32_t bytes;
    uint32_t nonzero;
    uint32_t checksum;
    uint32_t first_offset;
    uint32_t last_offset;
} XP3200SIVRAMStats;

typedef struct XP3200SState {
    MemoryRegion io;
    MemoryRegion board_io;
    MemoryRegion touch_io;
    MemoryRegion audio_io;
    MemoryRegion nand_io;
    MemoryRegion lcdc_io;
    MemoryRegion ivram;
    CPUState *cpu;
    QemuConsole *con;
    uint8_t *ivram_ptr;
    uint8_t io_regs[XP3200S_IO_SIZE];
    uint8_t io_seen[XP3200S_IO_SIZE];
    uint8_t board_regs[XP3200S_BOARD_IO_SIZE];
    uint8_t board_seen[XP3200S_BOARD_IO_SIZE];
    uint8_t touch_io_regs[XP3200S_TOUCH_IO_SIZE];
    uint8_t audio_regs[XP3200_AUDIO_IO_SIZE];
    uint8_t audio_seen[XP3200_AUDIO_IO_SIZE];
    uint16_t spi_regs[8];
    uint8_t spi_rx;
    bool spi_rx_full;
    bool sd_present;
    bool sd_cs_prev;
    FILE *sd_file;
    uint64_t sd_size;
    int sd_state;
    uint8_t sd_cmd;
    uint32_t sd_arg;
    uint32_t sd_block;
    int sd_arg_pos;
    int sd_pos;
    int sd_crc_pos;
    uint8_t sd_r1;
    uint8_t sd_next;
    bool sd_resp_delay;
    uint16_t sd_crc;
    uint8_t sd_tail[16];
    int sd_tail_len;
    int sd_tail_pos;
    uint8_t sd_buf[XP3200S_SD_BLOCK_SIZE];
    bool sd_app_cmd;
    bool sd_multi;
    bool sd_inserted;
    QEMUTimer *sd_insert_timer;
    /* Watchdog: the OS writes 0x23 to 16TM0 control (0x8186) every ~10 ms.
     * If that stops for 2 s the real device resets. */
    QEMUTimer *wdt_timer;
    bool wdt_armed;
    uint8_t board_data_latch_reads;
    bool board_data_latch_pending;
    bool usb_connected;
    bool key_matrix;
    uint8_t lcdc_regs[XP3200S_LCDC_IO_SIZE];
    uint8_t lcdc_seen[XP3200S_LCDC_IO_SIZE];
    XP3200SMMIOTrace mmio_trace[XP3200S_MMIO_TRACE_LEN];
    unsigned mmio_trace_pos;
    uint64_t mmio_trace_seq;
    uint32_t itc_pending_mask;
    uint8_t itc_irq_vector;
    uint8_t itc_irq_level;
    uint8_t gpio_input_low[4];
    uint8_t port_int_input_low[4];
    uint8_t k5_input_low;
    uint8_t k6_input_low;
    uint32_t dbg_kport_reads[0x80];
    uint32_t dbg_kport_writes[0x80];
    unsigned audio_trace_count;
    unsigned audio_data_trace_count;
    unsigned kbd_serial_trace_count;
    uint32_t dbg_audio_reads[0x300];
    uint32_t dbg_audio_writes[0x300];
    uint32_t dbg_touch_reads[XP3200S_TOUCH_IO_SIZE];
    uint32_t dbg_touch_writes[XP3200S_TOUCH_IO_SIZE];
    uint8_t key_input_high[2];
    bool key_input_high_valid[2];
    uint8_t port_input_high;
    bool port_input_high_valid;
    bool keyboard_qcode_down[Q_KEY_CODE__MAX];
    uint16_t keyboard_line_down_count[XP3200S_KEY_LINES];
    uint16_t keyboard_matrix_down_count[XP3200_KEY_ROWS]
                                               [XP3200_KEY_COLS];
    uint8_t debug_key_p0_low_mask;
    uint16_t adc_touch_x;
    uint16_t adc_touch_y;
    uint16_t touch_pixel_x;
    uint16_t touch_pixel_y;
    uint16_t touch_serial_data;
    uint8_t touch_serial_command;
    uint8_t touch_serial_bits;
    uint8_t touch_serial_trailer_clocks;
    uint8_t touch_serial_logged_channels;
    XP3200STouchSerialPhase touch_serial_phase;
    XP3200SNANDPhase nand_phase;
    uint8_t nand_command;
    uint8_t nand_address[5];
    uint8_t nand_address_count;
    uint8_t nand_ctrl1;   /* XP3200 0x0402D1 */
    uint8_t nand_ctrl5;   /* P2 function select/config at 0x402D5 */
    uint8_t nand_p2data;  /* P2 data at 0x402D9: bit4=P24=CLE, bit5=P25=ALE */
    uint8_t nand_id_index;
    uint16_t nand_ecc_count;
    uint32_t nand_id;
    uint32_t nand_page;
    uint16_t nand_column;
    uint8_t nand_page_cache[XP3200S_NAND_RAW_PAGE_SIZE];
    uint8_t nand_program_address_cycles;
    bool nand_page_cache_valid;
    bool nand_program_page_valid;
    bool nand_copyback_program;
    uint32_t nand_data_reads;
    uint32_t nand_data_writes;
    uint8_t *nand_storage;
    size_t nand_storage_size;
    bool nand_linear;
    char *nand_image_path;
    FILE *nand_file;
    uint8_t nand_dirty_blocks[XP3200S_NAND_DIRTY_BYTES];
    bool nand_dirty;
    uint64_t nand_command_count;
    uint64_t nand_page_read_count;
    uint64_t nand_program_count;
    uint64_t nand_erase_count;
    FILE *audio_stream;
    char *audio_stream_path;
    char *audio_player_path;
    char *audio_inject_path;
    uint8_t *audio_inject_buf;
    size_t audio_inject_len;
    size_t audio_inject_pos;
    QEMUTimer *audio_inject_timer;
    GPid audio_player_pid;
    int audio_player_fd;
    uint8_t audio_player_buffer[XP3200S_AUDIO_PLAYER_BUFFER_SIZE];
    size_t audio_player_buffer_len;
    uint64_t audio_stream_bytes;
    unsigned audio_flush_bytes;
    uint32_t audio_fifo_level;
    uint32_t audio_bytes_per_second;
    uint32_t audio_mp3_header;
    uint32_t audio_candidate_bytes_per_second;
    unsigned audio_candidate_frames;
    int64_t audio_fifo_updated_ns;
    uint64_t audio_drain_remainder;
    uint64_t audio_decoded_ns;
    uint64_t audio_decode_remainder;
    uint8_t audio_dreq_bytes;
    uint16_t audio_sci_regs[16];
    uint8_t audio_sci_command;
    uint8_t audio_sci_register;
    uint8_t audio_sci_phase;
    uint8_t audio_sci_read_data[2];
    uint8_t audio_sci_read_pos;
    uint64_t hsdma_transfer_count[XP3200S_HSDMA_CHANNELS];
    uint8_t adc_last_channel;
    uint8_t touch_fpt;
    uint8_t touch_k5_low_mask;
    uint16_t lcd_width;
    uint16_t lcd_height;
    uint16_t audio_io_size;
    bool has_touchscreen;
    bool is_9288;
    bool native_boot;
    bool touch_p0_low;
    bool touch_down;
    bool debug_timer16_factors;
    uint32_t debug_sd_insert_ms;
    bool trace_io;
    bool trace_key_scan;
    bool trace_audio;
    uint32_t debug_key_code;
    uint32_t debug_key_matrix;
    bool strict_board_io;
    QemuInputHandlerState *kbd_handler;
    QemuInputHandlerState *ptr_handler;
    QEMUTimer *debug_usb_wakeup_timer;
    QEMUTimer *debug_nmi_wakeup_timer;
    QEMUTimer *debug_port4_wakeup_timer;
    QEMUTimer *debug_port5_wakeup_timer;
    uint32_t debug_call_ms;
    uint32_t debug_call_addr;
    uint32_t audio_force_data_in;
    uint32_t debug_key_rowsel;
    unsigned k5_read_trace_count;
    unsigned row_trace_count;
    unsigned gpio_trace_count;
    uint32_t trace_io_all_ms;
    uint32_t trace_io_all_len;
    unsigned trace_io_all_count;
    uint8_t trace_dup[0x10000];
    uint32_t debug_key_rc;
    uint8_t kbd_scan_col;
    uint8_t kbd_p0_latch;
    uint8_t host_key_row;
    uint8_t host_key_col;
    uint8_t host_key_stack_row[16];
    uint8_t host_key_stack_col[16];
    unsigned host_key_stack_len;
    bool kbd_row5_present;
    unsigned kbd_mask_trace;
    unsigned kbd_rc_trace;
    unsigned kbd_gate_trace;
    unsigned kbd_row5_trace;
    void *machine_backref;
    QEMUTimer *debug_lcd_dump_timer;
    QEMUTimer *debug_lcd_dump_poll_timer;
    uint32_t debug_lcd_dump_seen;
    QEMUTimer *debug_call_timer;
    char *debug_lcd_dump_path;
    QEMUTimer *timer3b_wakeup_timer;
    QEMUTimer *timer16_compare_timer[2][2];
    QEMUTimer *direct_os_irq_timer;
    QEMUTimer *timer2b_compare_timer;
    QEMUTimer *timer4b_compare_timer;
    QEMUTimer *timer8_underflow_timer[XP3200S_8TM_CHANNELS];
    QEMUTimer *clock_timer;
    QEMUTimer *audio_flush_timer;
    QEMUTimer *nand_flush_timer;
    uint8_t ctm_subsecond;
    Notifier exit;
} XP3200SState;

static XP3200SState *xp3200s_active_machine;

static const char *xp3200s_model_name(const XP3200SState *s)
{
    return s->is_9288 ? "XP3200" : "XP3200S";
}

static uint8_t xp3200s_gpio_input_low_mask(XP3200SState *s,
                                            unsigned port_index);
static void xp3200s_key_input_update(XP3200SState *s, const char *reason,
                                      bool allow_edges);
static void xp3200s_debug_call_cb(void *opaque);
static void xp3200s_clock_timer_update(XP3200SState *s);
static void xp3200s_clock_timer_cb(void *opaque);
static void xp3200s_timer16_update(XP3200SState *s);
static void xp3200s_timer0a_compare_cb(void *opaque);
static void xp3200s_timer0b_compare_cb(void *opaque);
static void xp3200s_timer1a_compare_cb(void *opaque);
static void xp3200s_timer1b_compare_cb(void *opaque);
static void xp3200s_timer2b_update(XP3200SState *s);
static void xp3200s_timer2b_compare_cb(void *opaque);
static void xp3200s_timer3b_update(XP3200SState *s);
static void xp3200s_timer4b_update(XP3200SState *s);
static void xp3200s_timer4b_compare_cb(void *opaque);
static void xp3200s_timer8_0_underflow_cb(void *opaque);
static void xp3200s_timer8_1_underflow_cb(void *opaque);
static void xp3200s_timer8_2_underflow_cb(void *opaque);
static void xp3200s_timer8_3_underflow_cb(void *opaque);
static void xp3200s_touch_serial_reset(XP3200SState *s);
static void xp3200s_nand_reset(XP3200SState *s);
static void xp3200s_nand_storage_save(XP3200SState *s);
static void xp3200s_nand_flush_cb(void *opaque);
static void xp3200s_audio_flush_cb(void *opaque);
static void xp3200s_audio_inject_cb(void *opaque);
static void xp3200s_audio_select_update(XP3200SState *s, uint8_t old,
                                         uint8_t value);
static bool xp3200s_audio_dreq_ready(XP3200SState *s);

#define XP3200S_16TM_REG(addr, name, mask) \
    { addr, name, "timer-16bit", mask, 0x00, XP3200S_REG_NONE }

static const XP3200SRegInfo xp3200s_regs[] = {
    { XP3200S_ADC_DATA_LO, "ADD_LO", "adc-touch", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_DATA_HI, "ADD_HI", "adc-touch", 0x03, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_TRIGGER, "ADTRG", "adc-touch", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_CHANNEL, "ADCH", "adc-touch", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_ENABLE, "ADCTL", "adc-touch", 0x7f, 0x10,
      XP3200S_REG_NONE },
    { XP3200S_ADC_SAMPLING, "ADSMP", "adc-touch", 0xf3, 0x03,
      XP3200S_REG_NONE },
    { XP3200S_ADC_FLAGS, "ADF_FLAGS", "adc-touch", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_OVERWRITE, "OWE_FLAGS", "adc-touch", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_UPPER_LO, "ADUPPER_LO", "adc-touch", 0xff, 0xff,
      XP3200S_REG_NONE },
    { XP3200S_ADC_UPPER_HI, "ADUPPER_HI", "adc-touch", 0x03, 0x03,
      XP3200S_REG_NONE },
    { XP3200S_ADC_LOWER_LO, "ADLOWER_LO", "adc-touch", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_LOWER_HI, "ADLOWER_HI", "adc-touch", 0x03, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_ADC_INTMASK, "ADINTMASK", "adc-touch", 0xff, 0xff,
      XP3200S_REG_NONE },
    { XP3200S_ADC_MODE, "ADCADV", "adc-touch", 0x01, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_8TM_DIRECT_CLOCK, "T8_DIRECT_CLK", "timer-8bit",
      0x0f, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CLOCK01, "T8_CLK01", "timer-8bit",
      0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CLOCK23, "T8_CLK23", "timer-8bit",
      0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 0 * XP3200S_8TM_STRIDE, "T8_CTRL0",
      "timer-8bit", 0x07, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 0 * XP3200S_8TM_STRIDE + 1, "T8_RELOAD0",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 0 * XP3200S_8TM_STRIDE + 2, "T8_COUNT0",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 1 * XP3200S_8TM_STRIDE, "T8_CTRL1",
      "timer-8bit", 0x07, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 1 * XP3200S_8TM_STRIDE + 1, "T8_RELOAD1",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 1 * XP3200S_8TM_STRIDE + 2, "T8_COUNT1",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 2 * XP3200S_8TM_STRIDE, "T8_CTRL2",
      "timer-8bit", 0x07, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 2 * XP3200S_8TM_STRIDE + 1, "T8_RELOAD2",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 2 * XP3200S_8TM_STRIDE + 2, "T8_COUNT2",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 3 * XP3200S_8TM_STRIDE, "T8_CTRL3",
      "timer-8bit", 0x07, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 3 * XP3200S_8TM_STRIDE + 1, "T8_RELOAD3",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_8TM_CTRL_BASE + 3 * XP3200S_8TM_STRIDE + 2, "T8_COUNT3",
      "timer-8bit", 0xff, 0x00, XP3200S_REG_NONE },
    { XP3200S_16TM_CLOCK_CTRL(0), "T16_CLK0", "timer-16bit", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_16TM_CLOCK_CTRL(1), "T16_CLK1", "timer-16bit", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_16TM_CLOCK_CTRL(2), "T16_CLK2", "timer-16bit", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_16TM_CLOCK_CTRL(3), "T16_CLK3", "timer-16bit", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_16TM_CLOCK_CTRL(4), "T16_CLK4", "timer-16bit", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_16TM_CLOCK_CTRL(5), "T16_CLK5", "timer-16bit", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_RUN_CTRL, "TCRUN", "clock-timer", 0x03, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_INT_CTRL, "TCINT", "clock-timer", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_DIVIDER, "TCDIV", "clock-timer", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_SEC, "TCSEC", "clock-timer", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_MIN, "TCMIN", "clock-timer", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_HOUR, "TCHOUR", "clock-timer", 0x1f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_DAY_LO, "TCDAY_LO", "clock-timer", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_DAY_HI, "TCDAY_HI", "clock-timer", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_MIN_CMP, "TCALM_MIN", "clock-timer", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_HOUR_CMP, "TCALM_HOUR", "clock-timer", 0x1f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CTM_DAY_CMP, "TCALM_DAY", "clock-timer", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CLG_POWER_CTRL, "CLG_POWER", "clock-generator", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CLG_PRESCALER, "CLG_PRESCALER", "clock-generator", 0x01, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CLG_CLOCK_OPT, "CLG_CLOCK_OPT", "clock-generator", 0x0d, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_CLG_PROTECT, "CLG_PROTECT", "clock-generator", 0xff, 0x00,
      XP3200S_REG_NONE },
    /*
     * Standard serial I/F status.  Transfers complete immediately in this
     * board model, so TDBE remains asserted while no receive/error flags are
     * pending.
     */
    { XP3200S_SIF0_STATUS, "SIF0_STATUS", "serial", 0x3f,
      XP3200S_SIF_TDBE, XP3200S_REG_NONE },
    { XP3200S_SIF1_STATUS, "SIF1_STATUS", "serial", 0x3f,
      XP3200S_SIF_TDBE, XP3200S_REG_NONE },
    { XP3200S_SIF2_STATUS, "SIF2_STATUS", "serial", 0x3f,
      XP3200S_SIF_TDBE, XP3200S_REG_NONE },
    { XP3200S_SIF3_STATUS, "SIF3_STATUS", "serial", 0x3f,
      XP3200S_SIF_TDBE, XP3200S_REG_NONE },
    { XP3200S_PIR_PORT0_1, "PPORT0_1", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_PORT2_3, "PPORT2_3", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_KEY_PORT0_3, "PKEY_PORT0_3", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { 0x263, "PHSDMA01", "itc-priority", 0x77, 0x00, XP3200S_REG_NONE },
    { 0x264, "PHSDMA23", "itc-priority", 0x77, 0x00, XP3200S_REG_NONE },
    { XP3200S_PIR2_16TM0_1, "P16TM0_1", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR3_16TM2_3, "P16TM2_3", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR4_16TM4_5, "P16TM4_5", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_8TM_SIF0, "P8TM_SIF0", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_SIF1_ADC, "PSIF1_ADC", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_CTM, "PCTM", "itc-priority", 0x07, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_PORT4_5, "PPORT4_5", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PIR_PORT6_7, "PPORT6_7", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_EIR0_KEY_PORT0_3, "EIR0_KEY_PORT0_3", "itc-enable", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { 0x271, "EIR1_DMA", "itc-enable", 0x1f, 0x00, XP3200S_REG_NONE },
    { XP3200S_EIR2_16TM0_1, "EIR2_16TM0_1", "itc-enable", 0xcc, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_EIR3_16TM2_3, "EIR3_16TM2_3", "itc-enable", 0xcc, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_EIR4_16TM4_5, "EIR4_16TM4_5", "itc-enable", 0xcc, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_EIR5_8TM0_3, "EIR5_8TM0_3", "itc-enable", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { 0x276, "EIR6_SIF0_1", "itc-enable", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_EIR7_PORT4_7_CTM_ADC, "EIR7_PORT4_7_CTM_ADC",
      "itc-enable", 0x3f, 0x00, XP3200S_REG_NONE },
    { 0x278, "EIR8_8TM4_5", "itc-enable", 0x03, 0x00,
      XP3200S_REG_NONE },
    { 0x279, "EIR9_SIF2_3", "itc-enable", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_FIR0_KEY_PORT0_3, "FIR0_KEY_PORT0_3", "itc-flag", 0x3f, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { 0x281, "FIR1_DMA", "itc-flag", 0x1f, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { XP3200S_FIR2_16TM0_1, "FIR2_16TM0_1", "itc-flag", 0xcc, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { XP3200S_FIR3_16TM2_3, "FIR3_16TM2_3", "itc-flag", 0xcc, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { XP3200S_FIR4_16TM4_5, "FIR4_16TM4_5", "itc-flag", 0xcc, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { XP3200S_FIR5_8TM0_3, "FIR5_8TM0_3", "itc-flag", 0x0f, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { 0x286, "FIR6_SIF0_1", "itc-flag", 0x3f, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { XP3200S_FIR7_PORT4_7_CTM_ADC, "FIR7_PORT4_7_CTM_ADC",
      "itc-flag", 0x3f, 0x00, XP3200S_REG_RESET_ONE_TO_CLEAR },
    { 0x288, "FIR8_8TM4_5", "itc-flag", 0x03, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { 0x289, "FIR9_SIF2_3", "itc-flag", 0x3f, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { 0x2a3, "PIR_USB_SPI", "itc-priority", 0x77, 0x00,
      XP3200S_REG_NONE },
    { 0x2a6, "EIR_LCDC_USB_SPI", "itc-enable", 0xe0, 0x00,
      XP3200S_REG_NONE },
    { 0x2a9, "FIR_LCDC_USB_SPI", "itc-flag", 0xe0, 0x00,
      XP3200S_REG_RESET_ONE_TO_CLEAR },
    { XP3200S_K5_FUNC_SELECT, "CFK5", "key-input-port", 0x1f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_K5_DATA, "K5D", "key-input-port", 0x1f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_K6_FUNC_SELECT, "CFK6", "adc-touch", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_K6_DATA, "K6D", "key-input-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT_INT_SELECT1, "SPT0_3", "port-input-int", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT_INT_SELECT2, "SPT4_7", "port-input-int", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT_INT_POLARITY, "SPPT0_7", "port-input-int", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT_INT_EDGE_LEVEL, "SEPT0_7", "port-input-int", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_KEY_INPUT_SELECT, "SPPK", "key-input-int", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_KEY_INPUT_CONDITION0, "SCPK0", "key-input-int", 0x1f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_KEY_INPUT_CONDITION1, "SCPK1", "key-input-int", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_KEY_INPUT_MASK0, "SMPK0", "key-input-int", 0x1f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_KEY_INPUT_MASK1, "SMPK1", "key-input-int", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT0_FUNC, "CFP0", "gpio-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT0_DATA, "P0D", "gpio-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT0_IOCTRL, "IOC0", "gpio-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT1_FUNC, "CFP1", "gpio-port", 0x7f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT1_DATA, "P1D", "gpio-port", 0x7f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT1_IOCTRL, "IOC1", "gpio-port", 0x7f, 0x00,
      XP3200S_REG_NONE },
    { 0x2d7, "CFSIO", "gpio-port", 0x0f, 0x00, XP3200S_REG_NONE },
    { XP3200S_PORT2_FUNC, "CFP2", "gpio-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT2_DATA, "P2D", "gpio-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT2_IOCTRL, "IOC2", "gpio-port", 0xff, 0x00,
      XP3200S_REG_NONE },
    { 0x2db, "CFSIO2", "gpio-port", 0x0f, 0x00, XP3200S_REG_NONE },
    { XP3200S_PORT3_FUNC, "CFP3", "gpio-port", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT3_DATA, "P3D", "gpio-port", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_PORT3_IOCTRL, "IOC3", "gpio-port", 0x3f, 0x00,
      XP3200S_REG_NONE },
    { 0x2df, "CFEX", "port-function", 0xff, 0x03, XP3200S_REG_NONE },
    { XP3200S_HSDMA_TRIG01, "HSDMA_TRIG01", "hsdma-trigger", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_HSDMA_TRIG23, "HSDMA_TRIG23", "hsdma-trigger", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_HSDMA_SW_TRIG, "HSDMA_SW_TRIG", "hsdma-trigger", 0x0f, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_TBRP_OFFSET, "TBRP", "trap-table", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_TTBR_OFFSET + 0, "TTBR0", "trap-table", 0x00, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_TTBR_OFFSET + 1, "TTBR1", "trap-table", 0xfc, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_TTBR_OFFSET + 2, "TTBR2", "trap-table", 0xff, 0x00,
      XP3200S_REG_NONE },
    { XP3200S_TTBR_OFFSET + 3, "TTBR3", "trap-table", 0x0f, 0x02,
      XP3200S_REG_NONE },
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(0) + 0,
                      "T16_CMPA0_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(0) + 1,
                      "T16_CMPA0_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(0) + 0,
                      "T16_CMPB0_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(0) + 1,
                      "T16_CMPB0_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(0) + 0,
                      "T16_CNT0_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(0) + 1,
                      "T16_CNT0_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_CTRL(0), "T16_CTRL0", 0x7f),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(1) + 0,
                      "T16_CMPA1_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(1) + 1,
                      "T16_CMPA1_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(1) + 0,
                      "T16_CMPB1_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(1) + 1,
                      "T16_CMPB1_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(1) + 0,
                      "T16_CNT1_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(1) + 1,
                      "T16_CNT1_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_CTRL(1), "T16_CTRL1", 0x7f),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(2) + 0,
                      "T16_CMPA2_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(2) + 1,
                      "T16_CMPA2_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(2) + 0,
                      "T16_CMPB2_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(2) + 1,
                      "T16_CMPB2_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(2) + 0,
                      "T16_CNT2_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(2) + 1,
                      "T16_CNT2_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_CTRL(2), "T16_CTRL2", 0x7f),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(3) + 0,
                      "T16_CMPA3_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(3) + 1,
                      "T16_CMPA3_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(3) + 0,
                      "T16_CMPB3_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(3) + 1,
                      "T16_CMPB3_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(3) + 0,
                      "T16_CNT3_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(3) + 1,
                      "T16_CNT3_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_CTRL(3), "T16_CTRL3", 0x7f),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(4) + 0,
                      "T16_CMPA4_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(4) + 1,
                      "T16_CMPA4_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(4) + 0,
                      "T16_CMPB4_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(4) + 1,
                      "T16_CMPB4_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(4) + 0,
                      "T16_CNT4_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(4) + 1,
                      "T16_CNT4_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_CTRL(4), "T16_CTRL4", 0x7f),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(5) + 0,
                      "T16_CMPA5_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_A(5) + 1,
                      "T16_CMPA5_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(5) + 0,
                      "T16_CMPB5_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COMPARE_B(5) + 1,
                      "T16_CMPB5_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(5) + 0,
                      "T16_CNT5_LO", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_COUNTER(5) + 1,
                      "T16_CNT5_HI", 0xff),
    XP3200S_16TM_REG(XP3200S_16TM_CTRL(5), "T16_CTRL5", 0x7f),
};

static const XP3200SITCPair xp3200s_itc_pairs[] = {
    { XP3200S_EIR0_KEY_PORT0_3, XP3200S_FIR0_KEY_PORT0_3, 0x3f,
      "key-port0-3" },
    { 0x271, 0x281, 0x1f, "dma" },
    { XP3200S_EIR2_16TM0_1, XP3200S_FIR2_16TM0_1, 0xcc, "16tm0-1" },
    { XP3200S_EIR3_16TM2_3, XP3200S_FIR3_16TM2_3, 0xcc, "16tm2-3" },
    { XP3200S_EIR4_16TM4_5, XP3200S_FIR4_16TM4_5, 0xcc, "16tm4-5" },
    { 0x275, 0x285, 0x0f, "8tm0-3" },
    { 0x276, 0x286, 0x3f, "sif0-1" },
    { 0x277, 0x287, 0x3f, "port4-7-ctm-adc" },
    { 0x278, 0x288, 0x03, "8tm4-5" },
    { 0x279, 0x289, 0x3f, "sif2-3" },
    { 0x2a6, 0x2a9, 0xe0, "lcdc-usb-spi" },
};

static const XP3200SRegInfo *xp3200s_find_reg(hwaddr offset)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(xp3200s_regs); i++) {
        if (xp3200s_regs[i].offset == offset) {
            return &xp3200s_regs[i];
        }
    }

    return NULL;
}

static bool xp3200s_is_hsdma_reg(hwaddr offset)
{
    return offset >= XP3200S_HSDMA_BASE &&
           offset < XP3200S_HSDMA_BASE +
                    XP3200S_HSDMA_CHANNELS * XP3200S_HSDMA_CH_SIZE;
}

static bool xp3200s_is_adc_reg(hwaddr offset)
{
    return offset >= XP3200S_ADC_DATA_LO && offset <= XP3200S_ADC_MODE;
}

static const char *xp3200s_adc_reg_name(hwaddr offset)
{
    if (offset >= XP3200S_ADC_BUFFER_BASE &&
        offset < XP3200S_ADC_BUFFER_BASE + XP3200S_ADC_CHANNELS * 2) {
        return (offset & 1) ? "ADBUF_HI" : "ADBUF_LO";
    }

    return "unknown";
}

static const char *xp3200s_hsdma_reg_name(hwaddr offset)
{
    switch (offset & 0xf) {
    case 0x0:
    case 0x1:
        return "HSDMA_COUNT";
    case 0x2:
    case 0x3:
        return "HSDMA_CTRL";
    case 0x4:
    case 0x5:
        return "HSDMA_SRC_LO";
    case 0x6:
    case 0x7:
        return "HSDMA_SRC_HI";
    case 0x8:
    case 0x9:
        return "HSDMA_DST_LO";
    case 0xa:
    case 0xb:
        return "HSDMA_DST_HI";
    case 0xc:
        return "HSDMA_ENABLE";
    case 0xe:
        return "HSDMA_TRIGGER_FLAG";
    default:
        return "HSDMA_RESERVED";
    }
}

static const char *xp3200s_io_group_name(hwaddr offset)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);

    if (reg != NULL) {
        return reg->group;
    }

    if (xp3200s_is_hsdma_reg(offset)) {
        return "hsdma";
    }
    if (xp3200s_is_adc_reg(offset)) {
        return "adc-touch";
    }
    if (offset >= 0x260 && offset <= 0x2af) {
        return "itc";
    }
    if (offset >= 0x2c0 && offset <= 0x2ff) {
        return "gpio-key";
    }
    if (offset >= 0x8120 && offset <= 0x813f) {
        return "trap-table";
    }

    switch ((offset >> 8) & 0xff) {
    case 0x00:
        return "system";
    case 0x01:
        return "timer";
    case 0x03:
        return "adc-touch";
    case 0x04:
    case 0x05:
        return "lcdc";
    case 0x06:
    case 0x07:
        return "nand";
    default:
        return "unknown";
    }
}

static const char *xp3200s_io_reg_name(hwaddr offset)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);

    if (reg != NULL) {
        return reg->name;
    }
    if (xp3200s_is_hsdma_reg(offset)) {
        return xp3200s_hsdma_reg_name(offset);
    }
    if (xp3200s_is_adc_reg(offset)) {
        return xp3200s_adc_reg_name(offset);
    }
    return "unknown";
}

static const char *xp3200s_lcdc_reg_name(hwaddr offset)
{
    switch (offset) {
    case XP3200S_LCDC_HNDP:
        return "HNDP";
    case XP3200S_LCDC_HSIZE:
        return "HSIZE";
    case XP3200S_LCDC_VNDP:
        return "VNDP";
    case XP3200S_LCDC_VSIZE:
        return "VSIZE";
    case XP3200S_LCDC_MODE0:
        return "MODE0";
    case XP3200S_LCDC_MODE1:
        return "MODE1";
    case XP3200S_LCDC_SADDR:
        return "SADDR0";
    case XP3200S_LCDC_SADDR + 1:
        return "SADDR1";
    default:
        if (offset >= XP3200S_LCDC_LUT_BASE &&
            offset < XP3200S_LCDC_LUT_END) {
            return "LUT";
        }
        return "unknown";
    }
}

static uint32_t xp3200s_current_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs == NULL) {
        return 0xffffffffu;
    }

    return S1C33_CPU(cs)->env.pc;
}

static bool xp3200s_range_touches(hwaddr offset, unsigned size,
                                   hwaddr base, unsigned base_size)
{
    return offset < base + base_size && offset + size > base;
}

static uint32_t xp3200s_ttbr_from_regs(XP3200SState *s)
{
    return ((uint32_t)s->io_regs[XP3200S_TTBR_OFFSET + 0]) |
           ((uint32_t)s->io_regs[XP3200S_TTBR_OFFSET + 1] << 8) |
           ((uint32_t)s->io_regs[XP3200S_TTBR_OFFSET + 2] << 16) |
           ((uint32_t)s->io_regs[XP3200S_TTBR_OFFSET + 3] << 24);
}

static void xp3200s_set_ttbr(XP3200SState *s, uint32_t value)
{
    uint32_t ttbr = value & S1C33_TTBR_MASK;

    s->io_regs[XP3200S_TTBR_OFFSET + 0] = ttbr & 0xff;
    s->io_regs[XP3200S_TTBR_OFFSET + 1] = (ttbr >> 8) & 0xff;
    s->io_regs[XP3200S_TTBR_OFFSET + 2] = (ttbr >> 16) & 0xff;
    s->io_regs[XP3200S_TTBR_OFFSET + 3] = (ttbr >> 24) & 0xff;
    if (s->cpu != NULL) {
        S1C33_CPU(s->cpu)->env.ttbr = ttbr;
    }
}

static uint8_t xp3200s_irq_level_or_one(uint8_t level)
{
    return level != 0 ? level : 1;
}

static bool xp3200s_select_irq(XP3200SState *s, uint8_t *vector,
                                uint8_t *level, const char **name)
{
    static const char * const port0_3_names[] = {
        "port0", "port1", "port2", "port3",
    };
    static const char * const port4_7_names[] = {
        "port4", "port5", "port6", "port7",
    };
    uint8_t key_port0_3_active =
        s->io_regs[XP3200S_EIR0_KEY_PORT0_3] &
        s->io_regs[XP3200S_FIR0_KEY_PORT0_3] & 0x3f;
    uint8_t timer16_0_1_active =
        s->io_regs[XP3200S_EIR2_16TM0_1] &
        s->io_regs[XP3200S_FIR2_16TM0_1] & 0xcc;
    uint8_t timer2b_active =
        s->io_regs[XP3200S_EIR3_16TM2_3] &
        s->io_regs[XP3200S_FIR3_16TM2_3] &
        XP3200S_16TM2B_IRQ_BIT;
    uint8_t timer3b_active =
        s->io_regs[XP3200S_EIR3_16TM2_3] &
        s->io_regs[XP3200S_FIR3_16TM2_3] &
        XP3200S_16TM3B_IRQ_BIT;
    uint8_t timer4b_active =
        s->io_regs[XP3200S_EIR4_16TM4_5] &
        s->io_regs[XP3200S_FIR4_16TM4_5] &
        XP3200S_16TM4B_IRQ_BIT;
    uint8_t timer8_active =
        s->io_regs[XP3200S_EIR5_8TM0_3] &
        s->io_regs[XP3200S_FIR5_8TM0_3] &
        XP3200S_8TM0_3_MASK;
    uint8_t adc_active =
        s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC] &
        s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] &
        XP3200S_ADC_IRQ_BIT;
    uint8_t ctm_active =
        s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC] &
        s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] &
        XP3200S_CTM_IRQ_BIT;
    uint8_t port4_active =
        s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC] &
        s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] &
        0x3c;
    uint8_t active = s->io_regs[0x2a6] & s->io_regs[0x2a9] & 0xe0;
    unsigned port;
    unsigned timer8;

    for (port = 0; port <= 3; port++) {
        if ((key_port0_3_active & (1u << port)) == 0) {
            continue;
        }
        *vector = XP3200S_PORT0_VECTOR + port;
        if (port == 0) {
            *level = xp3200s_irq_level_or_one(
                s->io_regs[XP3200S_PIR_PORT0_1] & 0x07);
        } else if (port == 1) {
            *level = xp3200s_irq_level_or_one(
                (s->io_regs[XP3200S_PIR_PORT0_1] >> 4) & 0x07);
        } else if (port == 2) {
            *level = xp3200s_irq_level_or_one(
                s->io_regs[XP3200S_PIR_PORT2_3] & 0x07);
        } else {
            *level = xp3200s_irq_level_or_one(
                (s->io_regs[XP3200S_PIR_PORT2_3] >> 4) & 0x07);
        }
        *name = port0_3_names[port];
        return true;
    }
    if ((key_port0_3_active & XP3200S_KEY0_IRQ_BIT) != 0) {
        *vector = XP3200S_KEY0_VECTOR;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR_KEY_PORT0_3] & 0x07);
        *name = "key-input0";
        return true;
    }
    if ((key_port0_3_active & XP3200S_KEY1_IRQ_BIT) != 0) {
        *vector = XP3200S_KEY1_VECTOR;
        *level = xp3200s_irq_level_or_one(
            (s->io_regs[XP3200S_PIR_KEY_PORT0_3] >> 4) & 0x07);
        *name = "key-input1";
        return true;
    }
    if ((timer16_0_1_active & XP3200S_16TM0B_IRQ_BIT) != 0) {
        *vector = XP3200S_16TM0B_VECTOR;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR2_16TM0_1] & 0x07);
        *name = "16tm0-b";
        return true;
    }
    if ((timer16_0_1_active & XP3200S_16TM0A_IRQ_BIT) != 0) {
        *vector = XP3200S_16TM0A_VECTOR;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR2_16TM0_1] & 0x07);
        *name = "16tm0-a";
        return true;
    }
    if ((timer16_0_1_active & XP3200S_16TM1B_IRQ_BIT) != 0) {
        *vector = XP3200S_16TM1B_VECTOR;
        *level = xp3200s_irq_level_or_one(
            (s->io_regs[XP3200S_PIR2_16TM0_1] >> 4) & 0x07);
        *name = "16tm1-b";
        return true;
    }
    if ((timer16_0_1_active & XP3200S_16TM1A_IRQ_BIT) != 0) {
        *vector = XP3200S_16TM1A_VECTOR;
        *level = xp3200s_irq_level_or_one(
            (s->io_regs[XP3200S_PIR2_16TM0_1] >> 4) & 0x07);
        *name = "16tm1-a";
        return true;
    }
    if (timer3b_active != 0 &&
        (timer2b_active == 0 ||
         ((s->io_regs[XP3200S_PIR3_16TM2_3] >> 4) & 0x07) >
         (s->io_regs[XP3200S_PIR3_16TM2_3] & 0x07))) {
        *vector = XP3200S_16TM3B_VECTOR;
        *level = xp3200s_irq_level_or_one(
            (s->io_regs[XP3200S_PIR3_16TM2_3] >> 4) & 0x07);
        *name = "16tm3-b";
        return true;
    }
    if (timer2b_active != 0) {
        *vector = XP3200S_16TM2B_VECTOR;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR3_16TM2_3] & 0x07);
        *name = "16tm2-b";
        return true;
    }
    if (timer3b_active != 0) {
        *vector = XP3200S_16TM3B_VECTOR;
        *level = xp3200s_irq_level_or_one(
            (s->io_regs[XP3200S_PIR3_16TM2_3] >> 4) & 0x07);
        *name = "16tm3-b";
        return true;
    }
    if (timer4b_active != 0) {
        *vector = XP3200S_16TM4B_VECTOR;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR4_16TM4_5] & 0x07);
        *name = "16tm4-b";
        return true;
    }
    for (timer8 = 0; timer8 < XP3200S_8TM_CHANNELS; timer8++) {
        if ((timer8_active & (1u << timer8)) == 0) {
            continue;
        }
        *vector = XP3200S_8TM0_VECTOR + timer8;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR_8TM_SIF0] & 0x07);
        *name = timer8 == 0 ? "8tm0" :
                timer8 == 1 ? "8tm1" :
                timer8 == 2 ? "8tm2" : "8tm3";
        return true;
    }
    if (adc_active != 0) {
        *vector = XP3200S_ADC_VECTOR;
        *level = xp3200s_irq_level_or_one(
            (s->io_regs[XP3200S_PIR_SIF1_ADC] >> 4) & 0x07);
        *name = "adc";
        return true;
    }
    if (ctm_active != 0) {
        *vector = XP3200S_CTM_VECTOR;
        *level = xp3200s_irq_level_or_one(
            s->io_regs[XP3200S_PIR_CTM] & 0x07);
        *name = "clock-timer";
        return true;
    }
    for (port = 4; port <= 7; port++) {
        if ((port4_active & (1u << (port - 2))) == 0) {
            continue;
        }
        *vector = XP3200S_PORT4_VECTOR + port - 4;
        if (port == 4) {
            *level = xp3200s_irq_level_or_one(
                s->io_regs[XP3200S_PIR_PORT4_5] & 0x07);
        } else if (port == 5) {
            *level = xp3200s_irq_level_or_one(
                (s->io_regs[XP3200S_PIR_PORT4_5] >> 4) & 0x07);
        } else if (port == 6) {
            *level = xp3200s_irq_level_or_one(
                s->io_regs[XP3200S_PIR_PORT6_7] & 0x07);
        } else {
            *level = xp3200s_irq_level_or_one(
                (s->io_regs[XP3200S_PIR_PORT6_7] >> 4) & 0x07);
        }
        *name = port4_7_names[port - 4];
        return true;
    }
    if (active & 0x40) {
        *vector = 113;
        *level = s->io_regs[0x2a3] & 0x07;
        *name = "usb";
        return true;
    }
    if (active & 0x80) {
        *vector = 114;
        *level = (s->io_regs[0x2a3] >> 4) & 0x07;
        *name = "spi";
        return true;
    }
    if (active & 0x20) {
        *vector = 112;
        *level = 1;
        *name = "lcdc";
        return true;
    }

    return false;
}

static void xp3200s_update_irq(XP3200SState *s)
{
    uint32_t pending = 0;
    uint8_t vector = 0;
    uint8_t level = 0;
    const char *source = NULL;
    bool have_irq;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(xp3200s_itc_pairs); i++) {
        const XP3200SITCPair *pair = &xp3200s_itc_pairs[i];
        uint8_t active = s->io_regs[pair->enable_offset] &
                         s->io_regs[pair->flag_offset] & pair->mask;

        if (active != 0) {
            pending |= 1u << i;
        }
    }

    have_irq = xp3200s_select_irq(s, &vector, &level, &source);
    if (pending == s->itc_pending_mask &&
        vector == s->itc_irq_vector &&
        level == s->itc_irq_level) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-itc: pending 0x%08x -> 0x%08x\n",
                  s->itc_pending_mask, pending);
    for (i = 0; i < ARRAY_SIZE(xp3200s_itc_pairs); i++) {
        uint32_t bit = 1u << i;

        if ((pending ^ s->itc_pending_mask) & bit) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-itc: %s %s\n",
                          xp3200s_itc_pairs[i].name,
                          (pending & bit) ? "pending" : "clear");
        }
    }
    if (have_irq) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-itc: deliver source=%s vector=%u level=%u\n",
                      source, vector, level);
    } else if (pending != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-itc: pending source has no implemented "
                      "vector yet\n");
    }

    s->itc_pending_mask = pending;
    s->itc_irq_vector = have_irq ? vector : 0;
    s->itc_irq_level = have_irq ? level : 0;
    if (s->cpu != NULL) {
        s1c33_cpu_set_irq(s->cpu, have_irq, vector, level);
    }
}

static void xp3200s_raise_usb_factor(XP3200SState *s, const char *reason)
{
    s->io_regs[0x2a9] |= XP3200S_USB_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-itc: USB factor set by %s "
                  "eir=0x%02x fir=0x%02x pir=0x%02x\n",
                  reason, s->io_regs[0x2a6], s->io_regs[0x2a9],
                  s->io_regs[0x2a3]);
    xp3200s_update_irq(s);
}

static void xp3200s_raise_adc_factor(XP3200SState *s, const char *reason,
                                      unsigned channel)
{
    uint8_t priority = (s->io_regs[XP3200S_PIR_SIF1_ADC] >> 4) & 0x07;

    s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] |= XP3200S_ADC_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-adc: factor set by %s ch=%u "
                  "eir=0x%02x fir=0x%02x pir=0x%02x level=%u\n",
                  reason, channel,
                  s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC],
                  s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC],
                  s->io_regs[XP3200S_PIR_SIF1_ADC],
                  xp3200s_irq_level_or_one(priority));
    xp3200s_update_irq(s);
}

static void xp3200s_clock_timer_reset_counters(XP3200SState *s)
{
    memset(&s->io_regs[XP3200S_CTM_COUNTER_BASE], 0,
           XP3200S_CTM_COUNTER_SIZE);
    s->io_regs[XP3200S_CTM_DIVIDER] = 0;
    s->ctm_subsecond = 0;
}

static bool xp3200s_is_leap_year(unsigned year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static void xp3200s_clock_timer_init_rtc(XP3200SState *s)
{
    struct tm now;
    unsigned year;
    uint32_t days = 0;

    qemu_get_timedate(&now, 0);
    year = now.tm_year + 1900;
    if (year < 1901) {
        year = 1901;
        now.tm_yday = 0;
    } else if (year > 2049) {
        year = 2049;
        now.tm_yday = 364;
    }
    for (unsigned y = 1901; y < year; y++) {
        days += xp3200s_is_leap_year(y) ? 366 : 365;
    }
    days += now.tm_yday;

    s->io_regs[XP3200S_CTM_SEC] = now.tm_sec;
    s->io_regs[XP3200S_CTM_MIN] = now.tm_min;
    s->io_regs[XP3200S_CTM_HOUR] = now.tm_hour;
    s->io_regs[XP3200S_CTM_DAY_LO] = days & 0xff;
    s->io_regs[XP3200S_CTM_DAY_HI] = days >> 8;
    info_report("%s battery RTC initialized: "
                "%04u-%02u-%02u %02u:%02u:%02u day=%u",
                xp3200s_model_name(s),
                year, now.tm_mon + 1, now.tm_mday, now.tm_hour,
                now.tm_min, now.tm_sec, days);
}

static uint8_t xp3200s_clock_timer_increment(XP3200SState *s)
{
    uint8_t events = XP3200S_CTM_EV_32HZ;
    uint16_t day;

    s->io_regs[XP3200S_CTM_DIVIDER]++;
    s->ctm_subsecond++;
    if ((s->ctm_subsecond & 0x03) == 0) {
        events |= XP3200S_CTM_EV_8HZ;
    }
    if ((s->ctm_subsecond & 0x0f) == 0) {
        events |= XP3200S_CTM_EV_2HZ;
    }
    if (s->ctm_subsecond < XP3200S_CTM_TICKS_PER_SECOND) {
        return events;
    }
    s->ctm_subsecond = 0;
    events |= XP3200S_CTM_EV_1HZ;

    s->io_regs[XP3200S_CTM_SEC]++;
    if (s->io_regs[XP3200S_CTM_SEC] < 60) {
        return events;
    }
    s->io_regs[XP3200S_CTM_SEC] = 0;
    events |= XP3200S_CTM_EV_1MIN;

    s->io_regs[XP3200S_CTM_MIN]++;
    if (s->io_regs[XP3200S_CTM_MIN] < 60) {
        return events;
    }
    s->io_regs[XP3200S_CTM_MIN] = 0;
    events |= XP3200S_CTM_EV_1HOUR;

    s->io_regs[XP3200S_CTM_HOUR]++;
    if (s->io_regs[XP3200S_CTM_HOUR] < 24) {
        return events;
    }
    s->io_regs[XP3200S_CTM_HOUR] = 0;
    events |= XP3200S_CTM_EV_1DAY;

    day = ((uint16_t)s->io_regs[XP3200S_CTM_DAY_HI] << 8) |
          s->io_regs[XP3200S_CTM_DAY_LO];
    day++;
    s->io_regs[XP3200S_CTM_DAY_LO] = day & 0xff;
    s->io_regs[XP3200S_CTM_DAY_HI] = day >> 8;
    return events;
}

static uint8_t xp3200s_clock_timer_periodic_event(XP3200SState *s)
{
    switch ((s->io_regs[XP3200S_CTM_INT_CTRL] >> 5) & 0x07) {
    case 0:
        return XP3200S_CTM_EV_32HZ;
    case 1:
        return XP3200S_CTM_EV_8HZ;
    case 2:
        return XP3200S_CTM_EV_2HZ;
    case 3:
        return XP3200S_CTM_EV_1HZ;
    case 4:
        return XP3200S_CTM_EV_1MIN;
    case 5:
        return XP3200S_CTM_EV_1HOUR;
    case 6:
        return XP3200S_CTM_EV_1DAY;
    default:
        return 0;
    }
}

static bool xp3200s_clock_timer_alarm_match(XP3200SState *s)
{
    uint8_t tcase = (s->io_regs[XP3200S_CTM_INT_CTRL] >> 2) & 0x07;
    uint16_t day = ((uint16_t)s->io_regs[XP3200S_CTM_DAY_HI] << 8) |
                   s->io_regs[XP3200S_CTM_DAY_LO];

    if (tcase == 0) {
        return false;
    }
    if ((tcase & 0x01) &&
        ((s->io_regs[XP3200S_CTM_MIN_CMP] ^ s->io_regs[XP3200S_CTM_MIN]) &
         0x3f) != 0) {
        return false;
    }
    if ((tcase & 0x02) &&
        ((s->io_regs[XP3200S_CTM_HOUR_CMP] ^ s->io_regs[XP3200S_CTM_HOUR]) &
         0x1f) != 0) {
        return false;
    }
    if ((tcase & 0x04) &&
        ((s->io_regs[XP3200S_CTM_DAY_CMP] ^ day) & 0x1f) != 0) {
        return false;
    }
    return true;
}

static void xp3200s_raise_clock_timer_factor(XP3200SState *s,
                                              const char *reason,
                                              uint8_t tcint_flag)
{
    uint8_t priority = s->io_regs[XP3200S_PIR_CTM] & 0x07;

    s->io_regs[XP3200S_CTM_INT_CTRL] |= tcint_flag;
    s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] |= XP3200S_CTM_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-ctm: factor set by %s "
                  "tcint=0x%02x eir=0x%02x fir=0x%02x pir=0x%02x "
                  "level=%u sec=%u min=%u hour=%u day=0x%02x%02x\n",
                  reason, s->io_regs[XP3200S_CTM_INT_CTRL],
                  s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC],
                  s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC],
                  s->io_regs[XP3200S_PIR_CTM],
                  xp3200s_irq_level_or_one(priority),
                  s->io_regs[XP3200S_CTM_SEC],
                  s->io_regs[XP3200S_CTM_MIN],
                  s->io_regs[XP3200S_CTM_HOUR],
                  s->io_regs[XP3200S_CTM_DAY_HI],
                  s->io_regs[XP3200S_CTM_DAY_LO]);
    xp3200s_update_irq(s);
}

static bool xp3200s_clock_timer_armed(XP3200SState *s)
{
    return (s->io_regs[XP3200S_CTM_RUN_CTRL] & XP3200S_CTM_TCRUN) != 0;
}

static void xp3200s_clock_timer_update(XP3200SState *s)
{
    if (!xp3200s_clock_timer_armed(s)) {
        if (s->clock_timer != NULL) {
            timer_del(s->clock_timer);
        }
        return;
    }

    if (s->clock_timer == NULL) {
        s->clock_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_clock_timer_cb, s);
    }
    if (!timer_pending(s->clock_timer)) {
        timer_mod(s->clock_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  XP3200S_CTM_TICK_MS);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-ctm: tick scheduled after %u ms "
                      "tcint=0x%02x eir=0x%02x fir=0x%02x\n",
                      XP3200S_CTM_TICK_MS,
                      s->io_regs[XP3200S_CTM_INT_CTRL],
                      s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC],
                      s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC]);
    }
}

static void xp3200s_clock_timer_cb(void *opaque)
{
    XP3200SState *s = opaque;
    uint8_t events;
    uint8_t periodic_event;

    if ((s->io_regs[XP3200S_CTM_RUN_CTRL] & XP3200S_CTM_TCRUN) == 0) {
        xp3200s_clock_timer_update(s);
        return;
    }

    events = xp3200s_clock_timer_increment(s);
    periodic_event = xp3200s_clock_timer_periodic_event(s);
    if (periodic_event != 0 && (events & periodic_event) != 0 &&
        (s->io_regs[XP3200S_CTM_INT_CTRL] & XP3200S_CTM_TCIF) == 0 &&
        (s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] &
         XP3200S_CTM_IRQ_BIT) == 0) {
        xp3200s_raise_clock_timer_factor(s, "periodic-tick",
                                          XP3200S_CTM_TCIF);
    }
    if ((events & (XP3200S_CTM_EV_1MIN | XP3200S_CTM_EV_1HOUR |
                   XP3200S_CTM_EV_1DAY)) != 0 &&
        xp3200s_clock_timer_alarm_match(s) &&
        (s->io_regs[XP3200S_CTM_INT_CTRL] & XP3200S_CTM_TCAF) == 0 &&
        (s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] &
         XP3200S_CTM_IRQ_BIT) == 0) {
        xp3200s_raise_clock_timer_factor(s, "alarm",
                                          XP3200S_CTM_TCAF);
    }
    xp3200s_clock_timer_update(s);
}

static void xp3200s_raise_port4_factor(XP3200SState *s, const char *reason)
{
    uint8_t priority = s->io_regs[XP3200S_PIR_PORT4_5] & 0x07;

    s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] |= XP3200S_PORT4_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-itc: port4 factor set by %s "
                  "eir=0x%02x fir=0x%02x pir=0x%02x level=%u\n",
                  reason, s->io_regs[XP3200S_EIR7_PORT4_7_CTM_ADC],
                  s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC],
                  s->io_regs[XP3200S_PIR_PORT4_5],
                  xp3200s_irq_level_or_one(priority));
    xp3200s_update_irq(s);
}

static void xp3200s_sd_insert_cb(void *opaque)
{
    XP3200SState *s = opaque;

    s->sd_inserted = true;
    xp3200s_raise_port4_factor(s, "sd-insert");
    info_report("%s SD: card inserted", xp3200s_model_name(s));
}

static uint8_t xp3200s_port_input_select(XP3200SState *s, unsigned fpt)
{
    uint8_t reg = s->io_regs[fpt < 4 ? XP3200S_PORT_INT_SELECT1 :
                                      XP3200S_PORT_INT_SELECT2];

    return (reg >> ((fpt & 3) * 2)) & 0x03;
}

static bool xp3200s_external_port_high(XP3200SState *s, unsigned port,
                                        unsigned bit)
{
    uint8_t low = xp3200s_gpio_input_low_mask(s, port) |
                  s->port_int_input_low[port];

    return (low & (1u << bit)) == 0;
}

static bool xp3200s_k5_high(XP3200SState *s, unsigned bit)
{
    return (s->k5_input_low & (1u << bit)) == 0;
}

static bool xp3200s_k6_high(XP3200SState *s, unsigned bit)
{
    return (s->k6_input_low & (1u << bit)) == 0;
}

static bool xp3200s_port_input_signal_high(XP3200SState *s, unsigned fpt)
{
    switch (xp3200s_port_input_select(s, fpt)) {
    case 0:
        return xp3200s_k6_high(s, fpt);
    case 1:
        if (fpt <= 4) {
            return xp3200s_k5_high(s, fpt);
        }
        return xp3200s_external_port_high(s, 3, fpt - 4);
    case 2:
        return xp3200s_external_port_high(s, 0, fpt);
    case 3:
        return xp3200s_external_port_high(s, 2, fpt);
    default:
        g_assert_not_reached();
    }
}

static void xp3200s_port_input_update(XP3200SState *s, const char *reason,
                                       bool allow_edges, bool force_level)
{
    uint8_t new_high = s->port_input_high;
    bool changed = false;
    unsigned fpt;

    for (fpt = 4; fpt <= 7; fpt++) {
        bool high = xp3200s_port_input_signal_high(s, fpt);
        bool old_high = (s->port_input_high & (1u << fpt)) != 0;
        bool have_old = s->port_input_high_valid;
        bool edge = have_old && old_high != high;
        bool polarity_high =
            (s->io_regs[XP3200S_PORT_INT_POLARITY] & (1u << fpt)) != 0;
        bool edge_mode =
            (s->io_regs[XP3200S_PORT_INT_EDGE_LEVEL] & (1u << fpt)) != 0;
        bool selected = polarity_high ? high : !high;
        bool trigger = edge_mode ? (allow_edges && edge && selected) :
                                   (selected && (force_level || edge));

        if (high) {
            new_high |= 1u << fpt;
        } else {
            new_high &= ~(1u << fpt);
        }

        if (!trigger) {
            continue;
        }

        if ((s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] &
             (1u << (fpt - 2))) == 0) {
            changed = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-port: FPT%u factor set by %s "
                          "spt=%u sept=%u sppt=%u input=%s\n",
                          fpt, reason, xp3200s_port_input_select(s, fpt),
                          edge_mode ? 1 : 0, polarity_high ? 1 : 0,
                          high ? "high" : "low");
        }
        s->io_regs[XP3200S_FIR7_PORT4_7_CTM_ADC] |= 1u << (fpt - 2);
    }

    s->port_input_high = new_high;
    s->port_input_high_valid = true;
    if (changed) {
        xp3200s_update_irq(s);
    }
}

static bool xp3200s_keyboard_any_down(XP3200SState *s)
{
    unsigned row;
    unsigned col;

    if (s->is_9288) {
        for (row = 0; row < XP3200_KEY_ROWS; row++) {
            for (col = 0; col < XP3200_KEY_COLS; col++) {
                if (s->keyboard_matrix_down_count[row][col] != 0) {
                    return true;
                }
            }
        }
        return false;
    }

    for (row = 0; row < XP3200S_KEY_LINES; row++) {
        if (s->keyboard_line_down_count[row] != 0) {
            return true;
        }
    }
    return false;
}

static void xp3200s_update_port_wake_line(XP3200SState *s,
                                           const char *reason)
{
    uint8_t old_k5 = s->k5_input_low;
    uint8_t old_k6 = s->k6_input_low;
    uint8_t old_p0_port_int = s->port_int_input_low[0];
    uint8_t touch_bit = 1u << s->touch_fpt;
    uint8_t k5_managed_bits = s->touch_k5_low_mask & 0x1f;
    uint8_t managed_bits = XP3200S_WAKE_K6_BIT | touch_bit;
    uint8_t new_low = 0;
    bool keyboard_active = xp3200s_keyboard_any_down(s);
    bool touch_active = s->touch_down;
    uint8_t new_k5_low = touch_active ? k5_managed_bits : 0;

    if (keyboard_active) {
        new_low |= XP3200S_WAKE_K6_BIT;
    }
    if (touch_active) {
        new_low |= touch_bit;
    }

    s->k5_input_low = (s->k5_input_low & ~k5_managed_bits) | new_k5_low;
    s->k6_input_low = (s->k6_input_low & ~managed_bits) | new_low;
    s->port_int_input_low[0] =
        (s->port_int_input_low[0] & ~managed_bits) | new_low;

    if (old_k5 == s->k5_input_low &&
        old_k6 == s->k6_input_low &&
        old_p0_port_int == s->port_int_input_low[0]) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-port: wake line %s by %s "
                  "touch-fpt=%u k5_low=0x%02x k6_low=0x%02x "
                  "p0_int_low=0x%02x\n",
                  (keyboard_active || touch_active) ? "low" : "high",
                  reason, s->touch_fpt, s->k5_input_low,
                  s->k6_input_low, s->port_int_input_low[0]);
    /*
     * Feed the hardware key-input comparator on every change, including while
     * a key is held.  The XP3200 firmware arms FK0 on K5[3:0] (compare 0x0F,
     * mask 0x0F, EIR0 = 0x39) and only scans the key port from that interrupt,
     * so gating this on "no key down" kept the interrupt from ever firing.
     */
    xp3200s_key_input_update(s, reason, true);
    xp3200s_port_input_update(s, reason, true, true);
}

/*
 * Host key -> XP3200 keypad line, per the firmware's own table (swept with
 * -M xp3200,debug-key-code=0x00..0x0F):
 *
 *   K5.0 (code 0x0E) -> Right     (firmware's own "Keyboard test" screen)
 *   K5.1 (code 0x0D) -> PageDown  (same source)
 *   K5.2 (code 0x0B) -> PageUp    (same source)
 *   K5.3 (code 0x07) -> unnamed / no action
 *
 * The remaining keys (left/down/up arrows, Enter, Esc, F1-F5 and the hotkeys)
 * are NOT reachable from K5[3:0]; they come from the keyboard controller over
 * the bit-banged serial port, which is still to be emulated.
 */
static unsigned xp3200s_keyboard_line_for_qcode(int qcode)
{
    switch (qcode) {
    case Q_KEY_CODE_RET:
    case Q_KEY_CODE_KP_ENTER:
    case Q_KEY_CODE_SPC:
        return 5;
    case Q_KEY_CODE_ESC:
    case Q_KEY_CODE_BACKSPACE:
        return 4;
    case Q_KEY_CODE_PGUP:
    case Q_KEY_CODE_UP:
        return 2;               /* K5.2 = PageUp   (per Keyboard test) */
    case Q_KEY_CODE_PGDN:
    case Q_KEY_CODE_DOWN:
        return 1;               /* K5.1 = PageDown (per Keyboard test) */
    case Q_KEY_CODE_RIGHT:
        return 0;               /* K5.0 = Right    (per Keyboard test) */
    default:
        return XP3200S_KEY_LINES;
    }
}

static const char *xp3200s_keyboard_line_name(unsigned line)
{
    static const char *const names[XP3200S_KEY_LINES] = {
        "K5.0", "K5.1", "K5.2", "K5.3", "P0.4", "P0.5",
    };

    if (line >= XP3200S_KEY_LINES) {
        return "invalid";
    }
    return names[line];
}

/*
 * V1.5 kernel table at 0x021994dc.  Each non-zero matrix position is
 * translated by the firmware to a Linux input key code before it enters the
 * GUI event queue.  Keeping the same table here lets QEMU invert that wiring
 * for normal host keyboard events.
 */
static const uint8_t xp3200_matrix_to_linux[XP3200_KEY_MATRIX_IDS] = {
    0x00, 0x0b, 0x10, 0x1e, 0x2c, 0x18, 0x3f, 0x01,
    0x00, 0x02, 0x11, 0x1f, 0x2d, 0x19, 0x3b, 0x1c,
    0x00, 0x03, 0x12, 0x20, 0x2e, 0x26, 0x40, 0x68,
    0x00, 0x04, 0x13, 0x21, 0x2f, 0x09, 0x78, 0x6d,
    0x00, 0x05, 0x14, 0x22, 0x30, 0x0a, 0x57, 0x00,
    0x00, 0x06, 0x15, 0x23, 0x31, 0x00, 0x6f, 0x00,
    0x00, 0x07, 0x16, 0x24, 0x32, 0x00, 0x39, 0x00,
    0x00, 0x08, 0x17, 0x25, 0x67, 0x6c, 0x69, 0x6a,
};

static unsigned xp3200_keyboard_scan_code_for_qcode(int qcode)
{
    uint16_t linux_code;
    unsigned matrix_id;

    /*
     * Three 9288-only panel keys occupy positions that have no equivalent in
     * QEMU's generic key-code table.  V1.5 uses 0x78 for its input-method key;
     * Shift and matrix ID 47 are consumed as raw modifier/function positions.
     * F12 is a VNC-safe transport alias for ID 47; QEMU Help is retained as a
     * monitor-side alias.  The physical 9288 Help key is the F11 entry
     * (matrix ID 38) in the table above.  Firmware treats Esc (ID 7) as Exit.
     */
    switch (qcode) {
    case Q_KEY_CODE_MENU:
        return 30;
    case Q_KEY_CODE_SHIFT:
        return 39;
    case Q_KEY_CODE_F12:
    case Q_KEY_CODE_HELP:
        return 47;
    default:
        break;
    }

    if (qcode < 0 || qcode >= qemu_input_map_qcode_to_linux_len) {
        return XP3200_KEY_MATRIX_IDS;
    }

    /*
     * The firmware returns a one-based column in each eight-byte row, leaving
     * matrix IDs 0, 8, 16 ... unused.  Invert its board table so letters,
     * digits, function keys, page keys and all four arrows retain their host
     * meanings.
     */
    linux_code = qemu_input_map_qcode_to_linux[qcode];
    if (linux_code == 0 || linux_code > UINT8_MAX) {
        return XP3200_KEY_MATRIX_IDS;
    }
    for (matrix_id = 1; matrix_id < ARRAY_SIZE(xp3200_matrix_to_linux);
         matrix_id++) {
        if (xp3200_matrix_to_linux[matrix_id] == linux_code) {
            return matrix_id;
        }
    }

    return XP3200_KEY_MATRIX_IDS;
}

static void xp3200_keyboard_refresh_matrix(XP3200SState *s)
{
    uint8_t row_select = s->touch_io_regs[XP3200_KEY_ROW_SELECT];
    uint8_t k5_low = 0;
    uint8_t p0_low = 0;
    unsigned row;
    unsigned col;

    for (row = 0; row < XP3200_KEY_ROWS; row++) {
        bool row_driven = (row_select & (1u << row)) == 0;

        for (col = 0; col < XP3200_KEY_COLS; col++) {
            if (s->keyboard_matrix_down_count[row][col] == 0) {
                continue;
            }
            if (col < 4) {
                /*
                 * K5[3:0] are the scanned columns: only a driven row can pull
                 * them low, so they are gated by the row select.
                 */
                if (row_driven) {
                    k5_low |= 1u << col;
                }
            } else {
                /*
                 * P0[6:4] are wired straight to the keypad, not through the
                 * scan rows.  The firmware proves it: after the FK interrupt it
                 * parks the row select at 0xFF (no row driven) and still reads
                 * P0 to identify the key, so do not gate these by row_select.
                 */
                p0_low |= 1u << col;
            }
        }
    }

    s->k5_input_low = (s->k5_input_low & ~0x0f) | k5_low;
    s->gpio_input_low[XP3200S_KEY_GPIO_PORT] =
        (s->gpio_input_low[XP3200S_KEY_GPIO_PORT] & ~0x70) | p0_low;
}

static void xp3200s_keyboard_refresh_lines(XP3200SState *s)
{
    uint8_t k5_low = 0;
    uint8_t p0_low = 0;
    unsigned i;

    if (s->key_matrix) {
        xp3200_keyboard_refresh_matrix(s);
        /*
         * Do NOT drive the legacy K6 "wake" line here.  That line is a BBK
         * kernel wake-up hack; on the XP3200 it makes the K6 key comparator
         * (key1, EIR0 bit5) fire on every key press, and vector 17 points at
         * NOR 0x00C43A90, which immediately re-runs the boot/reset path.
         * The real board only ever raises FK0 from the K5 key lines.
         */
        xp3200s_key_input_update(s, "keyboard", true);
        return;
    }

    for (i = 0; i < 4; i++) {
        if (s->keyboard_line_down_count[i] != 0) {
            k5_low |= 1u << i;
        }
    }
    for (i = 4; i < XP3200S_KEY_LINES; i++) {
        if (s->keyboard_line_down_count[i] != 0) {
            p0_low |= 1u << i;
        }
    }

    if (s->debug_key_code <= 0x1f) {
        /*
         * Present an arbitrary 5-bit keypad code: K5[3:0] carry the low nibble
         * (active low) and K6.4 carries bit 4, exactly as the keyboard
         * controller does.
         */
        if (s->keyboard_line_down_count[0] != 0) {
            k5_low = ~s->debug_key_code & 0x0f;
        } else {
            k5_low = 0;
        }
        p0_low = 0;
    }
    if (s->trace_key_scan) {
        fprintf(stderr, "REFR k5_low=0x%02x p0_low=0x%02x lines=%u,%u,%u,%u,%u,"
            "%u\n", k5_low, p0_low,
            s->keyboard_line_down_count[0], s->keyboard_line_down_count[1],
            s->keyboard_line_down_count[2], s->keyboard_line_down_count[3],
            s->keyboard_line_down_count[4], s->keyboard_line_down_count[5]);
    }
    s->k5_input_low = (s->k5_input_low & ~0x0f) | k5_low;
    s->gpio_input_low[XP3200S_KEY_GPIO_PORT] =
        (s->gpio_input_low[XP3200S_KEY_GPIO_PORT] & ~0x30) | p0_low;
    if (s->debug_key_code <= 0x1f) {
        /* K6.4 carries bit 4 of the keypad code (low while no key is down). */
        if (k5_low == 0) {
            s->k6_input_low &= ~0x10;
        } else {
            s->k6_input_low = (s->k6_input_low & ~0x10) |
                              ((~s->debug_key_code & 0x10) ? 0x10 : 0);
        }
    }
    /*
     * No legacy K6 "wake" line here either: on the XP3200 it raises FK1
     * (vector 17 -> NOR 0x00C43A90) and the machine reboots on every key.
     * Sample the hardware key comparator directly instead.
     */
    xp3200s_key_input_update(s, "keyboard", true);
}

static void xp3200s_host_key_push(XP3200SState *s, uint8_t row,
                                     uint8_t col)
{
    unsigned i;

    for (i = 0; i < s->host_key_stack_len; i++) {
        if (s->host_key_stack_row[i] == row &&
            s->host_key_stack_col[i] == col) {
            s->host_key_row = row;
            s->host_key_col = col;
            return;
        }
    }
    if (s->host_key_stack_len < 16) {
        i = s->host_key_stack_len++;
        s->host_key_stack_row[i] = row;
        s->host_key_stack_col[i] = col;
    }
    s->host_key_row = row;
    s->host_key_col = col;
}

static void xp3200s_host_key_pop(XP3200SState *s, uint8_t row, uint8_t col)
{
    unsigned i;

    for (i = 0; i < s->host_key_stack_len; i++) {
        if (s->host_key_stack_row[i] == row &&
            s->host_key_stack_col[i] == col) {
            memmove(&s->host_key_stack_row[i], &s->host_key_stack_row[i + 1],
                    (s->host_key_stack_len - i - 1));
            memmove(&s->host_key_stack_col[i], &s->host_key_stack_col[i + 1],
                    (s->host_key_stack_len - i - 1));
            s->host_key_stack_len--;
            break;
        }
    }
    if (s->host_key_stack_len > 0) {
        s->host_key_row = s->host_key_stack_row[s->host_key_stack_len - 1];
        s->host_key_col = s->host_key_stack_col[s->host_key_stack_len - 1];
    } else {
        s->host_key_row = 0;
        s->host_key_col = 0;
    }
}

static void xp3200s_keyboard_event(DeviceState *dev, QemuConsole *src,
                                    InputEvent *evt)
{
    XP3200SState *s = xp3200s_active_machine;
    InputKeyEvent *key;
    int qcode;
    unsigned line;
    unsigned scan_code;
    unsigned row;
    unsigned col;

    (void)dev;
    (void)src;

    if (s == NULL || evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }

    key = evt->u.key.data;
    qcode = qemu_input_key_value_to_qcode(key->key);
    if (qcode < 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }

    if (s->key_matrix) {
        scan_code = xp3200_keyboard_scan_code_for_qcode(qcode);
        if (scan_code >= XP3200_KEY_MATRIX_IDS) {
            return;
        }
        row = scan_code >> 3;
        col = (scan_code & 7) - 1;

        if (key->down) {
            bool key_changed;

            if (s->keyboard_qcode_down[qcode]) {
                return;
            }
            key_changed = s->keyboard_matrix_down_count[row][col] == 0;
            s->keyboard_qcode_down[qcode] = true;
            s->keyboard_matrix_down_count[row][col]++;
            xp3200s_keyboard_refresh_lines(s);
            fprintf(stderr, "KEYEV down qcode=%d scan=%u row=%u col=%u "
                    "k5=0x%02x p0=0x%02x wake=%s\n",
                    qcode, scan_code, row, col, s->k5_input_low,
                    s->gpio_input_low[XP3200S_KEY_GPIO_PORT],
                    key_changed ? "port-input" : "none");
        } else {
            if (!s->keyboard_qcode_down[qcode]) {
                return;
            }
            s->keyboard_qcode_down[qcode] = false;
            if (s->keyboard_matrix_down_count[row][col] != 0) {
                s->keyboard_matrix_down_count[row][col]--;
            }
            xp3200s_keyboard_refresh_lines(s);
            fprintf(stderr, "KEYEVU up qcode=%d scan=%u row=%u col=%u "
                    "k5=0x%02x p0=0x%02x\n",
                    qcode, scan_code, row, col, s->k5_input_low,
                    s->gpio_input_low[XP3200S_KEY_GPIO_PORT]);
        }
        return;
    }

    /*
     * Diagnostic code injection: the keypad controller reports a 5-bit code
     * as (K5D & 0x0F) | (K6D & 0x10).  Presenting an arbitrary code lets us
     * sweep the firmware's key table key by key.
     */
    {
        size_t mi;
        bool mapped = false;

        for (mi = 0; mi < ARRAY_SIZE(xp3200_kbd_map); mi++) {
            if (xp3200_kbd_map[mi].qcode != qcode) {
                continue;
            }
            mapped = true;
            if (key->down) {
                xp3200s_host_key_push(s, xp3200_kbd_map[mi].row,
                                      xp3200_kbd_map[mi].bit);
                if (s->trace_key_scan) {
                    fprintf(stderr, "KEYMAP qcode=%d -> row=%u bit=%u\n",
                            qcode, s->host_key_row, s->host_key_col);
                }
            } else {
                xp3200s_host_key_pop(s, xp3200_kbd_map[mi].row,
                                     xp3200_kbd_map[mi].bit);
            }
            break;
        }
        if (mapped) {
            /*
             * Re-evaluate the key comparator right now.  Without this the
             * FK0 edge is only produced by the legacy K5 lines (arrows,
             * Space, Enter, Esc), which still go through
             * xp3200s_keyboard_refresh_lines(); every other matrix key then
             * had to wait for the firmware's slow polling tick (~1 s) before
             * the first scan saw it.  The comparator read itself goes through
             * the same row/column gate, so it sees the row code and raises
             * FK0 immediately.
             */
            xp3200s_key_input_update(s, "keyboard", true);
            xp3200s_port_input_update(s, "keyboard", true, true);
        }
    }
    if (qcode == Q_KEY_CODE_F12 && key->down) {
        /*
         * Emulator-side backdoor: F12 jumps into the firmware's hidden factory
         * self-test menu (XP3200V1.9.5, SDRAM 0x0217D6CE).  The retail image
         * contains the module but nothing references it -- verified: no 32-bit
         * pointer, no call/jump target, no menu entry, no launcher file.
         * Only the key-down edge may jump: doing it on key-up too re-entered
         * the menu from inside itself and corrupted the stack.
         */
        if (s->debug_call_timer == NULL) {
            s->debug_call_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                               xp3200s_debug_call_cb, s);
        }
        s->debug_call_addr = XP3200_SELFTEST_MENU_ADDR;
        timer_mod(s->debug_call_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
        fprintf(stderr, "KEYF12 -> selftest menu %08x\n",
                XP3200_SELFTEST_MENU_ADDR);
        return;
    }
    if (s->machine_backref != NULL) {
        /* Pick up live qom-set changes for the diagnostic code sweep. */
        s->debug_key_code =
            ((XP3200SMachineState *)s->machine_backref)->debug_key_code;
        s->debug_key_matrix =
            ((XP3200SMachineState *)s->machine_backref)->debug_key_matrix;
    }
    if (s->debug_key_code <= 0x1f) {
        /*
         * Diagnostic: pretend line 0 was pressed/released so the normal line
         * state machine (keyboard_any_down, wake line, comparator, scan) runs
         * exactly as for a real key; xp3200s_keyboard_refresh_lines() replaces
         * the line bits with the requested 5-bit code.
         */
        if (key->down) {
            if (s->keyboard_line_down_count[0] == 0) {
                s->keyboard_line_down_count[0] = 1;
            }
        } else {
            s->keyboard_line_down_count[0] = 0;
        }
        fprintf(stderr, "KEYCODE %s code=0x%02x\n",
                key->down ? "down" : "up", s->debug_key_code);
        xp3200s_keyboard_refresh_lines(s);
        return;
    }

    line = xp3200s_keyboard_line_for_qcode(qcode);
    if (line >= XP3200S_KEY_LINES) {
        return;
    }

    if (key->down) {
        bool line_changed;

        if (s->keyboard_qcode_down[qcode]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-input: key repeat qcode=%d "
                          "mapped=%s-low wake=none\n",
                          qcode, xp3200s_keyboard_line_name(line));
            return;
        }

        line_changed = s->keyboard_line_down_count[line] == 0;
        s->keyboard_qcode_down[qcode] = true;
        s->keyboard_line_down_count[line]++;
        xp3200s_keyboard_refresh_lines(s);
        fprintf(stderr, "KEYEVL down qcode=%d mapped=%s k5=0x%02x p0=0x%02x "
                "wake=%s\n", qcode, xp3200s_keyboard_line_name(line),
                s->k5_input_low, s->gpio_input_low[XP3200S_KEY_GPIO_PORT],
                line_changed ? "port-input" : "none");
    } else {
        const char *line_state;

        if (!s->keyboard_qcode_down[qcode]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-input: key up qcode=%d "
                          "mapped=%s-ignored\n",
                          qcode, xp3200s_keyboard_line_name(line));
            return;
        }

        s->keyboard_qcode_down[qcode] = false;
        if (s->keyboard_line_down_count[line] != 0) {
            s->keyboard_line_down_count[line]--;
        }
        xp3200s_keyboard_refresh_lines(s);
        line_state = s->keyboard_line_down_count[line] ? "low" : "high";
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-input: key up qcode=%d "
                      "mapped=%s-%s k5=0x%02x p0=0x%02x\n",
                      qcode, xp3200s_keyboard_line_name(line), line_state,
                      s->k5_input_low,
                      s->gpio_input_low[XP3200S_KEY_GPIO_PORT]);
    }
}

static const QemuInputHandler xp3200s_keyboard_handler = {
    .name = "xp3200s-keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = xp3200s_keyboard_event,
};

static uint16_t xp3200s_touch_pixel_to_adc(int pixel, unsigned max_pixel)
{
    if (pixel < 0) {
        pixel = 0;
    } else if ((unsigned)pixel > max_pixel) {
        pixel = max_pixel;
    }

    if (max_pixel == 0) {
        return 0;
    }
    return ((uint32_t)pixel * 1023u + max_pixel / 2u) / max_pixel;
}

static void xp3200s_pointer_event(DeviceState *dev, QemuConsole *src,
                                   InputEvent *evt)
{
    XP3200SState *s = xp3200s_active_machine;
    InputMoveEvent *move;
    InputBtnEvent *btn;

    (void)dev;
    (void)src;

    if (s == NULL) {
        return;
    }

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            int x = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                          INPUT_EVENT_ABS_MAX, 0,
                                          s->lcd_width - 1);

            s->adc_touch_x =
                xp3200s_touch_pixel_to_adc(x, s->lcd_width - 1);
            s->touch_pixel_x = x;
        } else if (move->axis == INPUT_AXIS_Y) {
            int y = qemu_input_scale_axis(move->value, INPUT_EVENT_ABS_MIN,
                                          INPUT_EVENT_ABS_MAX, 0,
                                          s->lcd_height - 1);

            s->adc_touch_y =
                xp3200s_touch_pixel_to_adc(
                    s->lcd_height - 1 - y,
                    s->lcd_height - 1);
            s->touch_pixel_y = y;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button != INPUT_BUTTON_LEFT &&
            btn->button != INPUT_BUTTON_TOUCH) {
            return;
        }
        if (s->touch_down == btn->down) {
            return;
        }
        s->touch_down = btn->down;
        s->touch_serial_logged_channels = 0;
        xp3200s_touch_serial_reset(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-touch: pen %s adc-x=%u adc-y=%u fpt=%u\n",
                      s->touch_down ? "down" : "up",
                      s->adc_touch_x, s->adc_touch_y, s->touch_fpt);
        xp3200s_update_port_wake_line(s, "touch-pen");
        break;
    default:
        break;
    }
}

static const QemuInputHandler xp3200s_pointer_handler = {
    .name = "xp3200s-touch",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = xp3200s_pointer_event,
};

static void xp3200s_debug_usb_wakeup_cb(void *opaque)
{
    xp3200s_raise_usb_factor(opaque, "debug-usb-wakeup-ms");
}

static void xp3200s_debug_port4_wakeup_cb(void *opaque)
{
    xp3200s_raise_port4_factor(opaque, "debug-port4-wakeup-ms");
}

static void xp3200s_debug_port5_wakeup_cb(void *opaque)
{
    XP3200SState *s = opaque;

    s->port_int_input_low[0] |= 0x20;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-port: P05 diagnostic wake line low "
                  "by debug-port5-wakeup-ms p0_int_low=0x%02x\n",
                  s->port_int_input_low[0]);
    xp3200s_port_input_update(s, "debug-port5-wakeup-ms", true, true);
}

static void xp3200s_debug_nmi_wakeup_cb(void *opaque)
{
    XP3200SState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR, "xp3200s-itc: NMI raised by debug timer\n");
    if (s->cpu != NULL) {
        s1c33_cpu_raise_nmi(s->cpu);
    }
}

static uint16_t xp3200s_timer16_lduw(XP3200SState *s, hwaddr offset)
{
    return (uint16_t)s->io_regs[offset] |
           ((uint16_t)s->io_regs[offset + 1] << 8);
}

static void xp3200s_timer16_stw(XP3200SState *s, hwaddr offset,
                                 uint16_t value)
{
    s->io_regs[offset] = value & 0xff;
    s->io_regs[offset + 1] = value >> 8;
}

static bool xp3200s_timer16_channel_running(XP3200SState *s,
                                             unsigned channel)
{
    uint8_t clock_ctrl;
    uint8_t ctrl = s->io_regs[XP3200S_16TM_CTRL(channel)];

    if (channel > 1) {
        return false;
    }
    clock_ctrl = s->io_regs[XP3200S_16TM_CLOCK_CTRL(channel)];
    if ((ctrl & XP3200S_16TM_CTRL_PRUN) == 0) {
        return false;
    }
    return (clock_ctrl & 0x08) != 0 &&
           (ctrl & XP3200S_16TM_CTRL_CKSL) == 0;
}

static void xp3200s_timer16_compare_match(XP3200SState *s,
                                           unsigned channel,
                                           const char *which,
                                           uint8_t irq_bit,
                                           uint16_t count)
{
    uint8_t eir = s->io_regs[XP3200S_EIR2_16TM0_1];
    uint8_t fir = s->io_regs[XP3200S_FIR2_16TM0_1];
    uint8_t idma_request = s->io_regs[0x290] & irq_bit;
    uint8_t idma_enable = s->io_regs[0x294] & irq_bit;

    if (s->trace_key_scan || s->trace_io || s->debug_timer16_factors) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-timer: 16tm%u-%s compare match "
                      "count=0x%04x cmpa=0x%04x cmpb=0x%04x "
                      "eir=0x%02x fir=0x%02x idma-req=0x%02x "
                      "idma-en=0x%02x debug-factors=%u\n",
                      channel, which, count,
                      xp3200s_timer16_lduw(s,
                                            XP3200S_16TM_COMPARE_A(channel)),
                      xp3200s_timer16_lduw(s,
                                            XP3200S_16TM_COMPARE_B(channel)),
                      eir, fir, idma_request, idma_enable,
                      s->debug_timer16_factors ? 1 : 0);
    }

    s->io_regs[XP3200S_FIR2_16TM0_1] |= irq_bit;
    xp3200s_update_irq(s);
}

static uint64_t xp3200s_timer16_compare_period_ns(XP3200SState *s,
                                                   unsigned channel,
                                                   bool compare_a)
{
    static const uint16_t divisors[8] = {
        1, 2, 4, 16, 64, 256, 1024, 4096,
    };
    uint16_t count;
    uint16_t compare;
    uint32_t ticks;
    uint64_t cycles;
    uint64_t source_hz = XP3200S_OSC3_PLL_HZ;
    uint8_t clock_ctrl =
        s->io_regs[XP3200S_16TM_CLOCK_CTRL(channel)];

    count = xp3200s_timer16_lduw(s, XP3200S_16TM_COUNTER(channel));
    compare = xp3200s_timer16_lduw(
        s, compare_a ? XP3200S_16TM_COMPARE_A(channel) :
                       XP3200S_16TM_COMPARE_B(channel));
    ticks = compare > count ? compare - count :
                              0x10000u + compare - count;
    cycles = (uint64_t)ticks * divisors[clock_ctrl & 0x07];
    if ((s->io_regs[XP3200S_PRESCALER_CLOCK_SELECT] &
         XP3200S_PRESCALER_OSC1) != 0) {
        source_hz = XP3200S_OSC1_HZ;
    }
    return MAX(DIV_ROUND_UP(cycles * NANOSECONDS_PER_SECOND, source_hz),
               1);
}

static void xp3200s_timer16_schedule_channel(XP3200SState *s,
                                               unsigned channel)
{
    static QEMUTimerCB * const callbacks[2][2] = {
        { xp3200s_timer0a_compare_cb, xp3200s_timer0b_compare_cb },
        { xp3200s_timer1a_compare_cb, xp3200s_timer1b_compare_cb },
    };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    unsigned which;

    if (!xp3200s_timer16_channel_running(s, channel)) {
        for (which = 0; which < 2; which++) {
            if (s->timer16_compare_timer[channel][which] != NULL) {
                timer_del(s->timer16_compare_timer[channel][which]);
            }
        }
        return;
    }
    for (which = 0; which < 2; which++) {
        if (s->timer16_compare_timer[channel][which] == NULL) {
            s->timer16_compare_timer[channel][which] =
                timer_new_ns(QEMU_CLOCK_REALTIME,
                             callbacks[channel][which], s);
        }
        timer_mod_ns(s->timer16_compare_timer[channel][which],
                     now + xp3200s_timer16_compare_period_ns(
                               s, channel, which == 0));
    }
}

static void xp3200s_timer16_compare_cb(XP3200SState *s,
                                        unsigned channel, bool compare_a)
{
    uint16_t compare = xp3200s_timer16_lduw(
        s, compare_a ? XP3200S_16TM_COMPARE_A(channel) :
                       XP3200S_16TM_COMPARE_B(channel));
    uint8_t irq_bit = channel == 0 ?
                      (compare_a ? XP3200S_16TM0A_IRQ_BIT :
                                   XP3200S_16TM0B_IRQ_BIT) :
                      (compare_a ? XP3200S_16TM1A_IRQ_BIT :
                                   XP3200S_16TM1B_IRQ_BIT);

    if (!xp3200s_timer16_channel_running(s, channel)) {
        xp3200s_timer16_update(s);
        return;
    }
    xp3200s_timer16_stw(s, XP3200S_16TM_COUNTER(channel),
                         compare_a ? compare : 0);
    xp3200s_timer16_compare_match(s, channel,
                                   compare_a ? "a" : "b",
                                   irq_bit, compare);
    if (!compare_a) {
        xp3200s_timer16_schedule_channel(s, channel);
    }
}

static void xp3200s_timer0a_compare_cb(void *opaque)
{
    xp3200s_timer16_compare_cb(opaque, 0, true);
}

static void xp3200s_timer0b_compare_cb(void *opaque)
{
    xp3200s_timer16_compare_cb(opaque, 0, false);
}

static void xp3200s_timer1a_compare_cb(void *opaque)
{
    xp3200s_timer16_compare_cb(opaque, 1, true);
}

static void xp3200s_timer1b_compare_cb(void *opaque)
{
    xp3200s_timer16_compare_cb(opaque, 1, false);
}

static void xp3200s_timer16_update(XP3200SState *s)
{
    unsigned channel;

    for (channel = 0; channel < 2; channel++) {
        xp3200s_timer16_schedule_channel(s, channel);
    }
}

static bool xp3200s_timer_compare_b_armed(XP3200SState *s,
                                           unsigned channel)
{
    uint8_t clock_ctrl =
        s->io_regs[XP3200S_16TM_CLOCK_CTRL(channel)];
    uint8_t timer_ctrl = s->io_regs[XP3200S_16TM_CTRL(channel)];

    return (clock_ctrl & 0x08) != 0 &&
           (timer_ctrl & XP3200S_16TM_CTRL_PRUN) != 0 &&
           (timer_ctrl & XP3200S_16TM_CTRL_CKSL) == 0 &&
           xp3200s_timer16_lduw(
               s, XP3200S_16TM_COMPARE_B(channel)) != 0;
}

static uint64_t xp3200s_timer_compare_b_period_ms(XP3200SState *s,
                                                   unsigned channel)
{
    static const uint16_t divisors[8] = {
        1, 2, 4, 16, 64, 256, 1024, 4096,
    };
    uint16_t compare =
        xp3200s_timer16_lduw(
            s, XP3200S_16TM_COMPARE_B(channel));
    uint8_t clock_ctrl =
        s->io_regs[XP3200S_16TM_CLOCK_CTRL(channel)];
    uint64_t cycles = (uint64_t)compare * divisors[clock_ctrl & 0x07];
    uint64_t source_hz = XP3200S_OSC3_PLL_HZ;

    if ((s->io_regs[XP3200S_PRESCALER_CLOCK_SELECT] &
         XP3200S_PRESCALER_OSC1) != 0) {
        source_hz = XP3200S_OSC1_HZ;
    }
    return MAX(DIV_ROUND_UP(cycles * 1000, source_hz), 1);
}

static void xp3200s_timer2b_compare_cb(void *opaque)
{
    XP3200SState *s = opaque;

    if (!xp3200s_timer_compare_b_armed(s, 2)) {
        xp3200s_timer2b_update(s);
        return;
    }

    /* A comparison-B match raises F16TU2 and resets the counter to zero. */
    xp3200s_timer16_stw(s, XP3200S_16TM_COUNTER(2), 0);
    s->io_regs[XP3200S_FIR3_16TM2_3] |= XP3200S_16TM2B_IRQ_BIT;
    if (s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-timer: 16tm2-b factor set "
                      "cmpb=0x%04x eir=0x%02x fir=0x%02x "
                      "pir=0x%02x period=%" PRIu64 "ms\n",
                      xp3200s_timer16_lduw(
                          s, XP3200S_16TM_COMPARE_B(2)),
                      s->io_regs[XP3200S_EIR3_16TM2_3],
                      s->io_regs[XP3200S_FIR3_16TM2_3],
                      s->io_regs[XP3200S_PIR3_16TM2_3],
                      xp3200s_timer_compare_b_period_ms(s, 2));
    }
    xp3200s_update_irq(s);
    xp3200s_timer2b_update(s);
}

static void xp3200s_timer2b_update(XP3200SState *s)
{
    if (!xp3200s_timer_compare_b_armed(s, 2)) {
        if (s->timer2b_compare_timer != NULL) {
            timer_del(s->timer2b_compare_timer);
        }
        return;
    }
    if (s->timer2b_compare_timer == NULL) {
        s->timer2b_compare_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         xp3200s_timer2b_compare_cb, s);
    }
    if (!timer_pending(s->timer2b_compare_timer)) {
        timer_mod(s->timer2b_compare_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  xp3200s_timer_compare_b_period_ms(s, 2));
    }
}

static void xp3200s_timer4b_compare_cb(void *opaque)
{
    XP3200SState *s = opaque;

    if (!xp3200s_timer_compare_b_armed(s, 4)) {
        xp3200s_timer4b_update(s);
        return;
    }

    xp3200s_timer16_stw(s, XP3200S_16TM_COUNTER(4), 0);
    s->io_regs[XP3200S_FIR4_16TM4_5] |= XP3200S_16TM4B_IRQ_BIT;
    if (s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-timer: 16tm4-b factor set "
                      "cmpb=0x%04x eir=0x%02x fir=0x%02x "
                      "pir=0x%02x period=%" PRIu64 "ms\n",
                      xp3200s_timer16_lduw(
                          s, XP3200S_16TM_COMPARE_B(4)),
                      s->io_regs[XP3200S_EIR4_16TM4_5],
                      s->io_regs[XP3200S_FIR4_16TM4_5],
                      s->io_regs[XP3200S_PIR4_16TM4_5],
                      xp3200s_timer_compare_b_period_ms(s, 4));
    }
    xp3200s_update_irq(s);
    xp3200s_timer4b_update(s);
}

static void xp3200s_timer4b_update(XP3200SState *s)
{
    if (!xp3200s_timer_compare_b_armed(s, 4)) {
        if (s->timer4b_compare_timer != NULL) {
            timer_del(s->timer4b_compare_timer);
        }
        return;
    }
    if (s->timer4b_compare_timer == NULL) {
        s->timer4b_compare_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         xp3200s_timer4b_compare_cb, s);
    }
    if (!timer_pending(s->timer4b_compare_timer)) {
        timer_mod(s->timer4b_compare_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  xp3200s_timer_compare_b_period_ms(s, 4));
    }
}

static void xp3200s_timer3b_wakeup_cb(void *opaque)
{
    XP3200SState *s = opaque;
    uint8_t priority = (s->io_regs[XP3200S_PIR3_16TM2_3] >> 4) & 0x07;

    if ((s->io_regs[XP3200S_16TM_CTRL(3)] &
         XP3200S_16TM_CTRL_PRUN) == 0 ||
        (s->io_regs[XP3200S_EIR3_16TM2_3] &
         XP3200S_16TM3B_IRQ_BIT) == 0) {
        xp3200s_timer3b_update(s);
        return;
    }
    s->io_regs[XP3200S_FIR3_16TM2_3] |= XP3200S_16TM3B_IRQ_BIT;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-timer: 16tm3-b factor set "
                  "eir=0x%02x fir=0x%02x pir=0x%02x level=%u\n",
                  s->io_regs[XP3200S_EIR3_16TM2_3],
                  s->io_regs[XP3200S_FIR3_16TM2_3],
                  s->io_regs[XP3200S_PIR3_16TM2_3],
                  xp3200s_irq_level_or_one(priority));
    xp3200s_update_irq(s);
}

static void xp3200s_timer3b_update(XP3200SState *s)
{
    bool enabled =
        (s->io_regs[XP3200S_EIR3_16TM2_3] & XP3200S_16TM3B_IRQ_BIT) != 0;
    bool running =
        (s->io_regs[XP3200S_16TM_CTRL(3)] &
         XP3200S_16TM_CTRL_PRUN) != 0;
    bool pending =
        (s->io_regs[XP3200S_FIR3_16TM2_3] & XP3200S_16TM3B_IRQ_BIT) != 0;

    if (!enabled || !running) {
        if (s->timer3b_wakeup_timer != NULL) {
            timer_del(s->timer3b_wakeup_timer);
        }
        return;
    }
    if (pending) {
        return;
    }
    if (s->timer3b_wakeup_timer == NULL) {
        s->timer3b_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_timer3b_wakeup_cb, s);
    }
    if (!timer_pending(s->timer3b_wakeup_timer)) {
        timer_mod(s->timer3b_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  XP3200S_16TM3B_WAKE_MS);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-timer: 16tm3-b wake scheduled after %u ms\n",
                      XP3200S_16TM3B_WAKE_MS);
    }
}

static bool xp3200s_timer8_channel_running(XP3200SState *s,
                                             unsigned channel)
{
    hwaddr ctrl = XP3200S_8TM_CTRL_BASE +
                  channel * XP3200S_8TM_STRIDE;
    hwaddr clock_reg = channel < 2 ? XP3200S_8TM_CLOCK01 :
                                     XP3200S_8TM_CLOCK23;
    unsigned shift = (channel & 1) * 4;

    return (s->io_regs[ctrl] & XP3200S_8TM_CTRL_RUN) != 0 &&
           (s->io_regs[clock_reg] & (0x08u << shift)) != 0;
}

static uint8_t xp3200s_timer8_running_mask(XP3200SState *s)
{
    uint8_t running = 0;
    unsigned channel;

    for (channel = 0; channel < XP3200S_8TM_CHANNELS; channel++) {
        if (xp3200s_timer8_channel_running(s, channel)) {
            running |= 1u << channel;
        }
    }
    return running;
}

static uint64_t xp3200s_timer8_period_ns(XP3200SState *s,
                                          unsigned channel)
{
    static const uint16_t divisors[XP3200S_8TM_CHANNELS][8] = {
        { 2, 4, 8, 16, 32, 64, 128, 256 },
        { 32, 64, 128, 256, 512, 1024, 2048, 4096 },
        { 2, 4, 8, 16, 32, 64, 2048, 4096 },
        { 2, 4, 8, 16, 32, 64, 128, 256 },
    };
    hwaddr clock_reg = channel < 2 ? XP3200S_8TM_CLOCK01 :
                                     XP3200S_8TM_CLOCK23;
    unsigned shift = (channel & 1) * 4;
    unsigned select = (s->io_regs[clock_reg] >> shift) & 7;
    hwaddr count_reg = XP3200S_8TM_CTRL_BASE +
                       channel * XP3200S_8TM_STRIDE + 2;
    uint64_t ticks = (uint64_t)s->io_regs[count_reg] + 1;
    uint64_t divisor = divisors[channel][select];
    uint64_t source_hz = XP3200S_OSC3_PLL_HZ;

    if ((s->io_regs[XP3200S_8TM_DIRECT_CLOCK] &
         (1u << channel)) != 0) {
        divisor = 1;
    }
    if ((s->io_regs[XP3200S_PRESCALER_CLOCK_SELECT] &
         XP3200S_PRESCALER_OSC1) != 0) {
        source_hz = XP3200S_OSC1_HZ;
    }
    return MAX(DIV_ROUND_UP(ticks * divisor * NANOSECONDS_PER_SECOND,
                            source_hz), 1);
}

static void xp3200s_timer8_update(XP3200SState *s)
{
    static QEMUTimerCB * const callbacks[XP3200S_8TM_CHANNELS] = {
        xp3200s_timer8_0_underflow_cb,
        xp3200s_timer8_1_underflow_cb,
        xp3200s_timer8_2_underflow_cb,
        xp3200s_timer8_3_underflow_cb,
    };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    unsigned channel;

    for (channel = 0; channel < XP3200S_8TM_CHANNELS; channel++) {
        QEMUTimer **timer = &s->timer8_underflow_timer[channel];

        if (!xp3200s_timer8_channel_running(s, channel)) {
            if (*timer != NULL) {
                timer_del(*timer);
            }
            continue;
        }
        if (*timer == NULL) {
            *timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                  callbacks[channel], s);
        }
        timer_mod_ns(*timer, now + xp3200s_timer8_period_ns(s, channel));
    }
    if (s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-timer: 8tm0-3 schedule running=0x%02x "
                      "fir=0x%02x\n",
                      xp3200s_timer8_running_mask(s),
                      s->io_regs[XP3200S_FIR5_8TM0_3]);
    }
}

static void xp3200s_timer8_underflow(XP3200SState *s, unsigned channel)
{
    hwaddr reload_reg = XP3200S_8TM_CTRL_BASE +
                        channel * XP3200S_8TM_STRIDE + 1;
    hwaddr count_reg = reload_reg + 1;
    uint8_t underflow = 1u << channel;
    bool osc3_stopped = channel == 1 && s->cpu != NULL &&
                        S1C33_CPU(s->cpu)->env.in_sleep &&
                        (s->io_regs[XP3200S_PRESCALER_CLOCK_SELECT] &
                         XP3200S_PRESCALER_OSC1) == 0;

    if (!xp3200s_timer8_channel_running(s, channel)) {
        if (s->timer8_underflow_timer[channel] != NULL) {
            timer_del(s->timer8_underflow_timer[channel]);
        }
        return;
    }

    /*
     * Timer 1 is the OSC3 stabilization delay *after* an NMI, port, or
     * clock-timer interrupt releases SLEEP.  OSC3 is stopped while sleeping,
     * so its underflow cannot be the event that releases SLEEP.  Poll slowly
     * until an actual wake occurs, then continue normal timer accounting.
     */
    if (osc3_stopped) {
        timer_mod_ns(s->timer8_underflow_timer[channel],
                     qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                     10 * SCALE_MS);
        return;
    }

    s->io_regs[count_reg] = s->io_regs[reload_reg];
    s->io_regs[XP3200S_FIR5_8TM0_3] |= underflow;
    if (s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-timer: 8tm%u underflow factor set "
                      "reload=0x%02x eir=0x%02x fir=0x%02x\n",
                      channel, s->io_regs[reload_reg],
                      s->io_regs[XP3200S_EIR5_8TM0_3],
                      s->io_regs[XP3200S_FIR5_8TM0_3]);
    }
    xp3200s_update_irq(s);
    timer_mod_ns(s->timer8_underflow_timer[channel],
                 qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                 xp3200s_timer8_period_ns(s, channel));
}

static void xp3200s_timer8_0_underflow_cb(void *opaque)
{
    xp3200s_timer8_underflow(opaque, 0);
}

static void xp3200s_timer8_1_underflow_cb(void *opaque)
{
    xp3200s_timer8_underflow(opaque, 1);
}

static void xp3200s_timer8_2_underflow_cb(void *opaque)
{
    xp3200s_timer8_underflow(opaque, 2);
}

static void xp3200s_timer8_3_underflow_cb(void *opaque)
{
    xp3200s_timer8_underflow(opaque, 3);
}

static uint16_t xp3200s_touch_channel_value(XP3200SState *s,
                                             unsigned channel)
{
    if (channel == 0) {
        return XP3200S_ADC_BATTERY_OK;
    }

    if (!s->touch_down) {
        return 0x03ff;
    }

    switch (channel) {
    case 1:
        return s->adc_touch_x & 0x03ff;
    case 2:
        return s->adc_touch_y & 0x03ff;
    case 3:
        return 0x0100;
    default:
        return 0x0200;
    }
}

static bool xp3200s_adc_buffer_info(hwaddr offset, unsigned *channel)
{
    if (offset < XP3200S_ADC_BUFFER_BASE ||
        offset >= XP3200S_ADC_BUFFER_BASE + XP3200S_ADC_CHANNELS * 2) {
        return false;
    }

    *channel = (offset - XP3200S_ADC_BUFFER_BASE) / 2;
    return true;
}

static void xp3200s_adc_store_result(XP3200SState *s, unsigned channel,
                                      uint16_t value)
{
    hwaddr buffer = XP3200S_ADC_BUFFER_BASE + channel * 2;

    value &= 0x03ff;
    s->io_regs[XP3200S_ADC_DATA_LO] = value & 0xff;
    s->io_regs[XP3200S_ADC_DATA_HI] = (value >> 8) & 0x03;
    s->io_regs[buffer] = value & 0xff;
    s->io_regs[buffer + 1] = (value >> 8) & 0x03;
    s->io_regs[XP3200S_ADC_FLAGS] |= 1u << channel;
    s->adc_last_channel = channel;
}

static bool xp3200s_adc_completion_irq_enabled(XP3200SState *s,
                                                unsigned channel)
{
    if ((s->io_regs[XP3200S_ADC_MODE] & 1) == 0) {
        return true;
    }
    if ((s->io_regs[XP3200S_ADC_ENABLE] & 0x10) == 0) {
        return false;
    }
    return (s->io_regs[XP3200S_ADC_INTMASK] & (1u << channel)) != 0;
}

static void xp3200s_adc_complete(XP3200SState *s, const char *reason)
{
    unsigned start = s->io_regs[XP3200S_ADC_CHANNEL] & 0x07;
    unsigned end = (s->io_regs[XP3200S_ADC_CHANNEL] >> 3) & 0x07;
    unsigned channel = start;
    unsigned count;
    unsigned irq_channel = start;
    bool set_irq = false;

    for (count = 0; count < XP3200S_ADC_CHANNELS; count++) {
        uint16_t value = xp3200s_touch_channel_value(s, channel);

        xp3200s_adc_store_result(s, channel, value);
        s->io_regs[XP3200S_ADC_TRIGGER] =
            (s->io_regs[XP3200S_ADC_TRIGGER] & 0x38) | channel;
        if (xp3200s_adc_completion_irq_enabled(s, channel)) {
            set_irq = true;
            irq_channel = channel;
        }
        if (channel == end) {
            break;
        }
        channel = (channel + 1) & 0x07;
    }

    s->io_regs[XP3200S_ADC_ENABLE] |= 0x08;
    if ((s->io_regs[XP3200S_ADC_TRIGGER] & 0x20) == 0) {
        s->io_regs[XP3200S_ADC_ENABLE] &= ~0x02;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-adc: complete %s ch=%u..%u last=%u "
                  "add=0x%03x flags=0x%02x touch=%s x=%u y=%u\n",
                  reason, start, end, s->adc_last_channel,
                  ((uint16_t)s->io_regs[XP3200S_ADC_DATA_HI] << 8) |
                  s->io_regs[XP3200S_ADC_DATA_LO],
                  s->io_regs[XP3200S_ADC_FLAGS],
                  s->touch_down ? "down" : "up",
                  s->adc_touch_x, s->adc_touch_y);
    if (set_irq) {
        xp3200s_raise_adc_factor(s, reason, irq_channel);
    }
}

static uint8_t xp3200s_adc_read_byte(XP3200SState *s, hwaddr offset)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);
    uint8_t value = s->io_regs[offset];
    unsigned channel;

    switch (offset) {
    case XP3200S_ADC_DATA_LO:
    case XP3200S_ADC_DATA_HI:
        s->io_regs[XP3200S_ADC_ENABLE] &= ~0x09;
        return offset == XP3200S_ADC_DATA_HI ? value & 0x03 : value;
    case XP3200S_ADC_FLAGS:
    case XP3200S_ADC_OVERWRITE:
        return value;
    default:
        break;
    }

    if (xp3200s_adc_buffer_info(offset, &channel)) {
        s->io_regs[XP3200S_ADC_FLAGS] &= ~(1u << channel);
        s->io_regs[XP3200S_ADC_OVERWRITE] &= ~(1u << channel);
        if (s->io_regs[XP3200S_ADC_OVERWRITE] == 0) {
            s->io_regs[XP3200S_ADC_ENABLE] &= ~0x01;
        }
        return (offset & 1) ? value & 0x03 : value;
    }

    return reg != NULL ? value & reg->mask : value;
}

static bool xp3200s_adc_write_byte(XP3200SState *s, hwaddr offset,
                                    uint8_t value)
{
    const XP3200SRegInfo *reg;

    if (!xp3200s_is_adc_reg(offset)) {
        return false;
    }

    switch (offset) {
    case XP3200S_ADC_DATA_LO:
    case XP3200S_ADC_DATA_HI:
        return true;
    case XP3200S_ADC_TRIGGER:
        s->io_regs[offset] =
            (s->io_regs[offset] & 0x07) | (value & 0x38);
        return true;
    case XP3200S_ADC_CHANNEL:
        s->io_regs[offset] = value & 0x3f;
        return true;
    case XP3200S_ADC_ENABLE: {
        uint8_t flags = s->io_regs[offset] & 0x09;

        if ((value & 0x01) == 0) {
            flags &= ~0x01;
        }
        s->io_regs[offset] = flags | (value & 0x76);
        if ((s->io_regs[offset] & 0x04) == 0) {
            s->io_regs[offset] &= ~0x02;
        }
        if ((value & 0x02) != 0 && (s->io_regs[offset] & 0x04) != 0) {
            if ((s->io_regs[XP3200S_ADC_TRIGGER] & 0x18) == 0) {
                xp3200s_adc_complete(s, "software-trigger");
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-adc: ADST ignored, trigger mode "
                              "ADTRG=0x%02x is not software\n",
                              s->io_regs[XP3200S_ADC_TRIGGER]);
            }
        }
        return true;
    }
    case XP3200S_ADC_SAMPLING:
        s->io_regs[offset] = value & 0xf3;
        return true;
    case XP3200S_ADC_FLAGS:
        return true;
    case XP3200S_ADC_OVERWRITE:
        s->io_regs[offset] &= value;
        if (s->io_regs[offset] == 0) {
            s->io_regs[XP3200S_ADC_ENABLE] &= ~0x01;
        }
        return true;
    default:
        if (offset >= XP3200S_ADC_BUFFER_BASE &&
            offset < XP3200S_ADC_BUFFER_BASE + XP3200S_ADC_CHANNELS * 2) {
            return true;
        }
        reg = xp3200s_find_reg(offset);
        s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
        return true;
    }
}

static bool xp3200s_is_clock_timer_reg(hwaddr offset)
{
    return offset >= XP3200S_CTM_RUN_CTRL &&
           offset <= XP3200S_CTM_DAY_CMP;
}

static uint8_t xp3200s_clock_timer_read_byte(XP3200SState *s,
                                              hwaddr offset)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);
    uint8_t value = s->io_regs[offset];

    switch (offset) {
    case XP3200S_CTM_RUN_CTRL:
        return value & XP3200S_CTM_TCRUN;
    case XP3200S_CTM_INT_CTRL:
    case XP3200S_CTM_DIVIDER:
        return value;
    default:
        return reg != NULL ? value & reg->mask : value;
    }
}

static bool xp3200s_clock_timer_write_byte(XP3200SState *s, hwaddr offset,
                                            uint8_t value)
{
    const XP3200SRegInfo *reg;

    if (!xp3200s_is_clock_timer_reg(offset)) {
        return false;
    }

    switch (offset) {
    case XP3200S_CTM_RUN_CTRL:
        if ((value & XP3200S_CTM_TCRST) != 0 &&
            (value & XP3200S_CTM_TCRUN) == 0 &&
            (s->io_regs[offset] & XP3200S_CTM_TCRUN) == 0) {
            xp3200s_clock_timer_reset_counters(s);
        }
        s->io_regs[offset] = value & XP3200S_CTM_TCRUN;
        return true;
    case XP3200S_CTM_INT_CTRL: {
        uint8_t flags = s->io_regs[offset] &
                        (XP3200S_CTM_TCIF | XP3200S_CTM_TCAF);

        flags &= ~(value & (XP3200S_CTM_TCIF | XP3200S_CTM_TCAF));
        s->io_regs[offset] = (value & XP3200S_CTM_SELECT_MASK) | flags;
        return true;
    }
    case XP3200S_CTM_DIVIDER:
    case XP3200S_CTM_SEC:
        return true;
    default:
        reg = xp3200s_find_reg(offset);
        s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
        return true;
    }
}

static uint16_t xp3200s_io_lduw_raw(XP3200SState *s, hwaddr offset)
{
    return (uint16_t)s->io_regs[offset] |
           ((uint16_t)s->io_regs[offset + 1] << 8);
}

static void xp3200s_io_stw_raw(XP3200SState *s, hwaddr offset,
                                uint16_t value)
{
    s->io_regs[offset] = value & 0xff;
    s->io_regs[offset + 1] = value >> 8;
}

static bool xp3200s_gpio_data_info(hwaddr offset, hwaddr *ctrl_offset,
                                    uint8_t *mask, unsigned *port_index)
{
    switch (offset) {
    case XP3200S_PORT0_DATA:
        *ctrl_offset = XP3200S_PORT0_IOCTRL;
        *mask = 0xff;
        *port_index = 0;
        return true;
    case XP3200S_PORT1_DATA:
        *ctrl_offset = XP3200S_PORT1_IOCTRL;
        *mask = 0x7f;
        *port_index = 1;
        return true;
    case XP3200S_PORT2_DATA:
        *ctrl_offset = XP3200S_PORT2_IOCTRL;
        *mask = 0xff;
        *port_index = 2;
        return true;
    case XP3200S_PORT3_DATA:
        *ctrl_offset = XP3200S_PORT3_IOCTRL;
        *mask = 0x3f;
        *port_index = 3;
        return true;
    default:
        return false;
    }
}

static bool xp3200s_backlight_on(XP3200SState *s)
{
    /*
     * P2.3 is driven by the audio code (VS1003 chip select / control), not
     * by a backlight enable: the volume screen writes P2=0x78 and the
     * panel must stay lit.  Keep the panel always visible.
     */
    (void)s;
    return true;
}

static uint8_t xp3200s_touch_gpio_low_mask(XP3200SState *s)
{
    if (!s->touch_p0_low || !s->touch_down || s->touch_fpt > 7) {
        return 0;
    }

    return 1u << s->touch_fpt;
}

static uint8_t xp3200s_gpio_input_low_mask(XP3200SState *s,
                                            unsigned port_index)
{
    uint8_t input_low = s->gpio_input_low[port_index];
    uint8_t touch_low = 0;

    if (port_index == XP3200S_KEY_GPIO_PORT) {
        touch_low = xp3200s_touch_gpio_low_mask(s) & XP3200S_KEY_P0_MASK;
        input_low |= s->debug_key_p0_low_mask & XP3200S_KEY_P0_MASK;
    }

    /*
     * The firmware scan loop writes P1D bit 3 low before reading P0D. Until
     * the real matrix is mapped, expose host keys only during that strobe.
     */
    if (!s->is_9288 && port_index == XP3200S_KEY_GPIO_PORT &&
        (s->io_regs[XP3200S_PORT1_DATA] & XP3200S_KEY_STROBE_BIT)) {
        input_low = 0;
    }

    if (port_index == XP3200S_KEY_GPIO_PORT) {
        input_low |= touch_low;
        if (!xp3200s_audio_dreq_ready(s)) {
            input_low |= XP3200S_AUDIO_DREQ_GPIO_BIT;
        }
    }

    /*
     * The media driver also samples the decoder ready line on P2.4
     * (0x00402D9 bit 4) before it pushes another buffer to the VS1003.
     */
    if (port_index == 2 && !xp3200s_audio_dreq_ready(s)) {
        input_low |= 0x10;
    }

    return input_low;
}

static uint8_t xp3200s_gpio_data_read(XP3200SState *s, hwaddr offset,
                                        hwaddr ctrl_offset, uint8_t mask,
                                        unsigned port_index)
{
    uint8_t latch = s->io_regs[offset] & mask;
    uint8_t output = s->io_regs[ctrl_offset] & mask;
    uint8_t input = mask & ~xp3200s_gpio_input_low_mask(s, port_index);

    return (latch & output) | (input & ~output);
}

static uint8_t xp3200s_k_input_data_read_impl(XP3200SState *s, hwaddr offset)
{
    switch (offset) {
    case XP3200S_K5_DATA:
        /*
         * Every K5 read is a fresh presentation.  The row-5 flag is consumed
         * by the K6 read that the firmware's key-code reader issues right
         * after K5 (see 0x0216E3D0), so leaving a stale "row 5 selected"
         * state across reads makes blocked columns look like a row-5 key.
         * That pinned the firmware position to column 1 for every row-5 key
         * instead of the column actually being scanned.
         */
        s->kbd_row5_present = false;
        {
            uint32_t rc;

            if (s->machine_backref != NULL) {
                s->debug_key_rc =
                    ((XP3200SMachineState *)s->machine_backref)->debug_key_rc;
            }
            rc = s->debug_key_rc;
            if (s->trace_key_scan && rc <= XP3200_KEY_RC_LAST &&
                s->cpu != NULL) {
                uint32_t caller = 0;

                cpu_physical_memory_read(S1C33_CPU(s->cpu)->env.sp, &caller,
                                         sizeof(caller));
                s->k5_read_trace_count++;
                fprintf(stderr, "K5RD n=%u caller=%08x colmask=%04x\n",
                        s->k5_read_trace_count, caller,
                        s->touch_io_regs[XP3200_KEY_ROW_SELECT] |
                        (s->touch_io_regs[XP3200_KEY_ROW_SELECT + 1] << 8));
            }
            if (s->trace_key_scan && rc <= XP3200_KEY_RC_LAST &&
                s->kbd_rc_trace < 400) {
                s->kbd_rc_trace++;
                fprintf(stderr, "K5RC rc=%u hostrow=%u\n",
                        (unsigned)rc, s->host_key_row);
            }

            /*
             * Host keys keep using the legacy row-only presentation: the
             * main menu path only consumes that, and gating it by column
             * made every key invisible.  The full matrix model is used for
             * explicit calibration (debug-key-rc) only, valid range
             * 0..XP3200_KEY_RC_LAST (all five rows, 14 columns).
             */
            if (rc > XP3200_KEY_RC_LAST && s->host_key_row != 0 &&
                offset == XP3200S_K5_DATA) {
                rc = (uint32_t)(s->host_key_row - 1) * 14 +
                     s->host_key_col;
            }
            if (rc <= XP3200_KEY_RC_LAST && s->trace_key_scan &&
                s->kbd_gate_trace < 2000) {
                s->kbd_gate_trace++;
                fprintf(stderr, "K5GATE rc=%u row=%u\n", (unsigned)rc,
                        rc / 14);
            }
            if (rc <= XP3200_KEY_RC_LAST) {
            /*
             * Rebuild the firmware's 14-bit column mask from the ports it
             * distributes it over (see 0x0216E324):
             *   mask[3:0]   -> P0[3:0]
             *   mask[4]     -> P0[5]
             *   mask[9:5]   -> 0x300F46[4:0]
             *   mask[10]    -> 0x300F46[7]
             *   mask[13:11] -> 0x300020[2:0]
             * A column is selected when its mask bit is 0.
             */
            uint16_t mask = 0;
            unsigned want_row = rc / 14;
            /* rc = (row-1)*14 + (column-1), physical columns. */
            unsigned want_col = rc % 14;
            unsigned want_bit = xp3200_kbd_col_bit[want_col];

            want_col = want_bit;

            mask |= s->kbd_p0_latch & 0x0f;
            mask |= ((s->kbd_p0_latch >> 5) & 1) << 4;
            mask |= (s->touch_io_regs[XP3200_KEY_ROW_SELECT] & 0x1f) << 5;
            mask |= ((s->touch_io_regs[XP3200_KEY_ROW_SELECT] >> 7) & 1)
                    << 10;
            mask |= (s->touch_io_regs[XP3200S_TOUCH_SERIAL] & 0x07) << 11;

            /*
             * Only gate when the firmware is really scanning a single
             * column (the full-scan path drives a one-hot mask).  The
             * quick row-only path leaves several columns selected, so it
             * must see the key unconditionally (it cannot tell columns
             * apart anyway).
             */
            {
                unsigned cleared = 0, b;

                for (b = 0; b < 14; b++) {
                    if (((mask >> b) & 1) == 0) {
                        cleared++;
                    }
                }
                if (s->trace_key_scan && s->kbd_mask_trace < 2000) {
                    s->kbd_mask_trace++;
                    fprintf(stderr,
                            "K5MASK n=%u mask=%04x cleared=%u bit=%u "
                            "hit=%u\n", s->kbd_mask_trace, mask, cleared,
                            want_col, (mask >> want_col) & 1);
                }
                /*
                 * A real column scan drives at most a couple of lines low at
                 * a time; the row-only quick path leaves most of the 14 lines
                 * low (mask 0x001e / 0x0000).  Only gate during a scan.
                 */
                if (cleared <= 4 && ((mask >> want_col) & 1)) {
                    if (s->trace_key_scan && cleared <= 2 &&
                        s->kbd_mask_trace < 2000) {
                        s->kbd_mask_trace++;
                        fprintf(stderr, "K5BLK mask=%04x bit=%u\n",
                                mask, want_col);
                    }
                    return 0x0f;
                }
                if (s->trace_key_scan && cleared <= 2 &&
                    s->kbd_mask_trace < 2000) {
                    s->kbd_mask_trace++;
                    fprintf(stderr, "K5PASS mask=%04x bit=%u cleared=%u\n",
                            mask, want_col, cleared);
                }
            }
            s->kbd_row5_present = false;
            switch (want_row) {
            case 0: return 0x0e;
            case 1: return 0x0d;
            case 2: return 0x0b;
            case 3: return 0x07;
            case 4:
                /*
                 * Fifth row: probe which K6 line carries it.  The code reader
                 * only takes K6.4 (bit4 of the key code); driving K6.4 low
                 * gives code 0x0F, which the firmware does not accept, so try
                 * K6[0] as well (see docs).
                 */
                s->kbd_row5_present = true;
                return 0x0f;
            default: return 0x0f;
            }
            }
        }
        if (s->debug_key_rowsel <= 0xffff) {
            uint16_t colmask =
                s->touch_io_regs[XP3200_KEY_ROW_SELECT] |
                (s->touch_io_regs[XP3200_KEY_ROW_SELECT + 1] << 8);

            if (colmask != (uint16_t)s->debug_key_rowsel) {
                return 0x0f;
            }
            return (~s->k5_input_low) &
                   ~s->io_regs[XP3200S_K5_FUNC_SELECT] & 0x0f;
        }
        if (s->debug_key_matrix <= 0x3f) {
            /*
             * Matrix read: the firmware selects one row on 0x300F46 (active
             * low) and reads K5[3:0] for that row's columns.
             */
            unsigned want_row = (s->debug_key_matrix >> 3) & 0x07;
            unsigned want_col = s->debug_key_matrix & 0x07;
            uint8_t rowsel = s->touch_io_regs[XP3200_KEY_ROW_SELECT];

            if ((rowsel & (1u << want_row)) == 0 && want_col < 4) {
                return (~(1u << want_col)) & 0x0f;
            }
            return 0x0f;
        }
        return (~s->k5_input_low) &
               ~s->io_regs[XP3200S_K5_FUNC_SELECT] & 0x1f;
    case XP3200S_K6_DATA:
        if (s->trace_key_scan && s->kbd_row5_present &&
            s->kbd_row5_trace < 2000) {
            s->kbd_row5_trace++;
            fprintf(stderr, "K6RD row5=%d k6low=%02x\n",
                    s->kbd_row5_present, s->k6_input_low);
        }
        if (s->kbd_row5_present) {
            return ((~s->k6_input_low) &
                    ~s->io_regs[XP3200S_K6_FUNC_SELECT] & 0xff) & ~0x10;
        }
        /*
         * K6.4 doubles as the keyboard wake line in this model, but the key
         * code reader also uses K6.4 as bit 4 of the code.  With a key held
         * the wake line pulls it low, which made every K5=0x0F scan (blocked
         * column) look like a row-5 key press (the game help hotkey).  Keep
         * bit 4 high unless a real row-5 key was just detected.
         */
        return ((~s->k6_input_low) | XP3200S_WAKE_K6_BIT) &
               ~s->io_regs[XP3200S_K6_FUNC_SELECT] & 0xff;
    default:
        g_assert_not_reached();
    }
}

static uint8_t xp3200s_k_input_data_read(XP3200SState *s, hwaddr offset)
{
    uint8_t value = xp3200s_k_input_data_read_impl(s, offset);
    if (s->trace_key_scan && offset == XP3200S_K5_DATA &&
        s->debug_key_rc <= XP3200_KEY_RC_LAST) {
        fprintf(stderr, "K5RET rc=%u ret=%02x row5=%d\n",
                (unsigned)s->debug_key_rc, value, s->kbd_row5_present);
    }
    return value;
}

static uint8_t xp3200s_key_input_gpio_value(XP3200SState *s, unsigned port)
{
    switch (port) {
    case 0:
        return xp3200s_gpio_data_read(s, XP3200S_PORT0_DATA,
                                       XP3200S_PORT0_IOCTRL, 0xff, 0);
    case 2:
        return xp3200s_gpio_data_read(s, XP3200S_PORT2_DATA,
                                       XP3200S_PORT2_IOCTRL, 0xff, 2);
    default:
        g_assert_not_reached();
    }
}

static uint8_t xp3200s_key_input_selected_value(XP3200SState *s,
                                                 unsigned key,
                                                 const char **source)
{
    uint8_t select = (s->io_regs[XP3200S_KEY_INPUT_SELECT] >>
                      (key * 2)) & 0x03;

    if (key == 0) {
        switch (select) {
        case 0: {
            uint8_t v;

            *source = "K5[4:0]";
            v = xp3200s_k_input_data_read(s, XP3200S_K5_DATA) & 0x1f;
            /*
             * The fifth matrix row presents K5[3:0] = 0xF with K6.4 low, so
             * the key-code read is 0x0F.  The firmware's FK0 comparator only
             * watches K5[3:0] against 0x0F, which is also its "no key" value:
             * a row-5 key would therefore never raise the interrupt that
             * makes the firmware scan.  Present one harmless low line to the
             * comparator instead; the key-code reader does not use this path
             * and still sees 0x0F (fifth row).
             */
            if (v == 0x0f &&
                ((s->debug_key_rc >= 56 &&
                  s->debug_key_rc <= XP3200_KEY_RC_LAST) ||
                 s->host_key_row == 5)) {
                v = 0x0e;
            }
            return v;
        }
        case 1:
            *source = "K6[4:0]";
            return xp3200s_k_input_data_read(s, XP3200S_K6_DATA) & 0x1f;
        case 2:
            *source = "P0[4:0]";
            return xp3200s_key_input_gpio_value(s, 0) & 0x1f;
        case 3:
            *source = "P2[4:0]";
            return xp3200s_key_input_gpio_value(s, 2) & 0x1f;
        default:
            g_assert_not_reached();
        }
    }

    switch (select) {
    case 0:
        *source = "K6[3:0]";
        return xp3200s_k_input_data_read(s, XP3200S_K6_DATA) & 0x0f;
    case 1:
        *source = "K6[7:4]";
        /*
         * K6.4 is the fifth keypad row, not a wake input: driving it low must
         * not raise FK1 (vector 17 -> NOR 0x00C43A90 reboots the machine).
         * Keep that bit out of the comparator.
         */
        return ((xp3200s_k_input_data_read(s, XP3200S_K6_DATA) >> 4) & 0x0f)
               & ~0x01;
    case 2:
        *source = "P0[7:4]";
        return (xp3200s_key_input_gpio_value(s, 0) >> 4) & 0x0f;
    case 3:
        *source = "P2[7:4]";
        return (xp3200s_key_input_gpio_value(s, 2) >> 4) & 0x0f;
    default:
        g_assert_not_reached();
    }
}

static uint8_t xp3200s_key_input_condition(XP3200SState *s, unsigned key)
{
    return key == 0 ? s->io_regs[XP3200S_KEY_INPUT_CONDITION0] & 0x1f :
                      s->io_regs[XP3200S_KEY_INPUT_CONDITION1] & 0x0f;
}

static uint8_t xp3200s_key_input_mask(XP3200SState *s, unsigned key)
{
    return key == 0 ? s->io_regs[XP3200S_KEY_INPUT_MASK0] & 0x1f :
                      s->io_regs[XP3200S_KEY_INPUT_MASK1] & 0x0f;
}

static void xp3200s_key_input_update(XP3200SState *s, const char *reason,
                                      bool allow_edges)
{
    bool changed = false;
    unsigned key;

    for (key = 0; key < 2; key++) {
        const char *source = NULL;
        uint8_t current =
            xp3200s_key_input_selected_value(s, key, &source);
        uint8_t previous = s->key_input_high[key];
        uint8_t compare = xp3200s_key_input_condition(s, key);
        uint8_t mask = xp3200s_key_input_mask(s, key);
        uint8_t was_matched = ~(previous ^ compare) & mask;
        uint8_t now_unmatched = (current ^ compare) & mask;
        uint8_t trigger = was_matched & now_unmatched;
        uint8_t irq_bit = key == 0 ? XP3200S_KEY0_IRQ_BIT :
                                     XP3200S_KEY1_IRQ_BIT;

        if (!s->key_input_high_valid[key]) {
            s->key_input_high[key] = current;
            s->key_input_high_valid[key] = true;
            fprintf(stderr, "KINIT key=%u src=%s val=0x%02x cmp=0x%02x "
                    "mask=0x%02x eir0=0x%02x fir0=0x%02x\n",
                    key, source ? source : "?", current, compare, mask,
                    s->io_regs[XP3200S_EIR0_KEY_PORT0_3],
                    s->io_regs[XP3200S_FIR0_KEY_PORT0_3]);
            continue;
        }

        if (allow_edges && trigger != 0) {
            if ((s->io_regs[XP3200S_FIR0_KEY_PORT0_3] & irq_bit) == 0) {
                changed = true;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-keyinput: FK%u factor set by %s "
                              "source=%s old=0x%02x new=0x%02x "
                              "compare=0x%02x mask=0x%02x edge=0x%02x "
                              "eir=0x%02x fir=0x%02x pir=0x%02x "
                              "level=%u\n",
                              key, reason, source, previous, current,
                              compare, mask, trigger,
                              s->io_regs[XP3200S_EIR0_KEY_PORT0_3],
                              s->io_regs[XP3200S_FIR0_KEY_PORT0_3] | irq_bit,
                              s->io_regs[XP3200S_PIR_KEY_PORT0_3],
                              xp3200s_irq_level_or_one(
                                  key == 0 ?
                                  s->io_regs[XP3200S_PIR_KEY_PORT0_3] & 0x07 :
                                  (s->io_regs[XP3200S_PIR_KEY_PORT0_3] >> 4) &
                                  0x07));
            }
            s->io_regs[XP3200S_FIR0_KEY_PORT0_3] |= irq_bit;
        } else if (s->trace_key_scan && previous != current) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-keyinput: FK%u line change by %s "
                          "source=%s old=0x%02x new=0x%02x "
                          "compare=0x%02x mask=0x%02x allow_edges=%u\n",
                          key, reason, source, previous, current, compare,
                          mask, allow_edges ? 1 : 0);
        }

        s->key_input_high[key] = current;
    }

    if (changed) {
        xp3200s_update_irq(s);
    }
}

static const char *xp3200s_key_scan_reg_name(hwaddr offset)
{
    switch (offset) {
    case XP3200S_PORT0_DATA:
        return "P0D";
    case XP3200S_PORT0_IOCTRL:
        return "IOC0";
    case XP3200S_PORT1_DATA:
        return "P1D";
    case XP3200S_PORT1_IOCTRL:
        return "IOC1";
    case XP3200S_K5_DATA:
        return "K5D";
    case XP3200S_K6_DATA:
        return "K6D";
    default:
        return NULL;
    }
}

static void xp3200s_key_scan_trace(XP3200SState *s, bool is_write,
                                    hwaddr offset, uint8_t value)
{
    const char *reg = xp3200s_key_scan_reg_name(offset);
    uint8_t host_low;
    uint8_t debug_low;
    uint8_t touch_low;
    uint8_t raw_low;
    uint8_t gated_low;
    uint8_t p1_latch;

    if (!s->trace_key_scan || reg == NULL) {
        return;
    }

    host_low = s->gpio_input_low[XP3200S_KEY_GPIO_PORT] &
               XP3200S_KEY_P0_MASK;
    debug_low = s->debug_key_p0_low_mask & XP3200S_KEY_P0_MASK;
    touch_low = xp3200s_touch_gpio_low_mask(s) & XP3200S_KEY_P0_MASK;
    raw_low = host_low | debug_low | touch_low;
    gated_low = xp3200s_gpio_input_low_mask(s, XP3200S_KEY_GPIO_PORT) &
                XP3200S_KEY_P0_MASK;
    p1_latch = s->io_regs[XP3200S_PORT1_DATA] & 0x7f;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-keyscan: %s pc=0x%08x reg=%s value=0x%02x "
                  "p1-latch=0x%02x p1-ioc=0x%02x strobe=%s "
                  "p0-latch=0x%02x p0-ioc=0x%02x host-low=0x%02x "
                  "debug-low=0x%02x touch-low=0x%02x raw-low=0x%02x "
                  "gated-low=0x%02x k5-low=0x%02x cfk5=0x%02x "
                  "k6-low=0x%02x cfk6=0x%02x touch=%s\n",
                  is_write ? "write" : "read", xp3200s_current_pc(),
                  reg, value, p1_latch,
                  s->io_regs[XP3200S_PORT1_IOCTRL] & 0x7f,
                  (p1_latch & XP3200S_KEY_STROBE_BIT) ? "off" : "on",
                  s->io_regs[XP3200S_PORT0_DATA],
                  s->io_regs[XP3200S_PORT0_IOCTRL],
                  host_low, debug_low, touch_low, raw_low, gated_low,
                  s->k5_input_low,
                  s->io_regs[XP3200S_K5_FUNC_SELECT] & 0x1f,
                  s->k6_input_low,
                  s->io_regs[XP3200S_K6_FUNC_SELECT],
                  s->touch_down ? "down" : "up");
}

static hwaddr xp3200s_hsdma_channel_base(unsigned channel)
{
    return XP3200S_HSDMA_BASE + channel * XP3200S_HSDMA_CH_SIZE;
}

static uint32_t xp3200s_hsdma_address(uint16_t low, uint16_t high)
{
    return ((uint32_t)(high & 0x0fff) << 16) | low;
}

static uint32_t xp3200s_hsdma_next_address(uint32_t addr, unsigned control,
                                            unsigned unit_size)
{
    switch (control) {
    case 3: /* Increment without initialization. */
    case 2: /* Increment with initialization. */
        return addr + unit_size;
    case 1:
        return addr - unit_size;
    default:
        return addr;
    }
}

static void xp3200s_ivram_stats_add(XP3200SIVRAMStats *stats,
                                     uint32_t addr, const uint8_t *buf,
                                     unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t phys = addr + i;
        uint32_t offset;
        uint8_t value;

        if (phys < XP3200S_IVRAM_BASE ||
            phys >= XP3200S_IVRAM_BASE + XP3200S_IVRAM_SIZE) {
            continue;
        }

        offset = phys - XP3200S_IVRAM_BASE;
        value = buf[i];
        if (stats->bytes == 0) {
            stats->first_offset = offset;
        }
        stats->last_offset = offset;
        stats->bytes++;
        stats->checksum += value;
        if (value != 0) {
            stats->nonzero++;
        }
    }
}

static void xp3200s_hsdma_log_ivram_stats(unsigned channel, const char *side,
                                           const XP3200SIVRAMStats *stats)
{
    if (stats->bytes == 0) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-hsdma: ch%u ivram-%s bytes=%u nonzero=%u "
                  "checksum=0x%08x range=0x%04x..0x%04x\n",
                  channel, side, stats->bytes, stats->nonzero,
                  stats->checksum, stats->first_offset, stats->last_offset);
}

static void xp3200s_hsdma_finish_channel(XP3200SState *s, unsigned channel,
                                          hwaddr base, uint16_t ctrl)
{
    s->io_regs[base + 0x0c] &= ~1u;
    s->io_regs[base + 0x0e] &= ~1u;
    xp3200s_io_stw_raw(s, base + 0x00, 0);
    xp3200s_io_stw_raw(s, base + 0x02, ctrl & 0xff00);
    s->io_regs[0x281] |= 1u << channel;
    xp3200s_update_irq(s);
}

static void xp3200s_hsdma_store_address(XP3200SState *s, hwaddr offset,
                                         uint16_t old_high, uint32_t addr)
{
    xp3200s_io_stw_raw(s, offset, addr & 0xffff);
    xp3200s_io_stw_raw(s, offset + 2,
                        (old_high & 0xf000) | ((addr >> 16) & 0x0fff));
}

static void xp3200s_hsdma_run_channel(XP3200SState *s, unsigned channel)
{
    hwaddr base = xp3200s_hsdma_channel_base(channel);
    uint16_t count_reg = xp3200s_io_lduw_raw(s, base + 0x00);
    uint16_t ctrl = xp3200s_io_lduw_raw(s, base + 0x02);
    uint16_t src_low = xp3200s_io_lduw_raw(s, base + 0x04);
    uint16_t src_high = xp3200s_io_lduw_raw(s, base + 0x06);
    uint16_t dst_low = xp3200s_io_lduw_raw(s, base + 0x08);
    uint16_t dst_high = xp3200s_io_lduw_raw(s, base + 0x0a);
    bool dual = (ctrl & 0x8000) != 0;
    unsigned mode = (dst_high >> 14) & 0x3;
    unsigned unit_size = (src_high & 0x4000) ? 2 : 1;
    unsigned src_control = (src_high >> 12) & 0x3;
    unsigned dst_control = (dst_high >> 12) & 0x3;
    uint32_t src = xp3200s_hsdma_address(src_low, src_high);
    uint32_t dst = xp3200s_hsdma_address(dst_low, dst_high);
    uint32_t initial_src = src;
    uint32_t initial_dst = dst;
    uint32_t remaining;
    uint32_t units;
    uint8_t buf[2];
    XP3200SIVRAMStats src_ivram = { 0 };
    XP3200SIVRAMStats dst_ivram = { 0 };
    bool log_transfer;
    uint32_t i;

    s->io_regs[base + 0x0e] &= ~1u;
    if (mode == 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-hsdma: ch%u reserved mode %u; "
                      "trigger cleared\n",
                      channel, mode);
        return;
    }

    if (mode == 2) {
        remaining = ((uint32_t)(ctrl & 0xff) << 8) |
                    (count_reg >> 8);
        units = count_reg & 0xff;
        if (units == 0) {
            units = 256;
        }
    } else {
        remaining = ((uint32_t)(ctrl & 0xff) << 16) | count_reg;
        units = mode == 0 ? MIN(remaining, 1u) : remaining;
    }
    if (remaining == 0) {
        xp3200s_hsdma_finish_channel(s, channel, base, ctrl);
        return;
    }

    s->hsdma_transfer_count[channel]++;
    log_transfer = s->trace_io || s->hsdma_transfer_count[channel] <= 16 ||
                   (s->hsdma_transfer_count[channel] & 0xfff) == 0;
    if (log_transfer) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-hsdma: ch%u transfer=%" PRIu64 " %s "
                      "mode=%u units=%u remaining=%u unit=%u "
                      "src=0x%08x dst=0x%08x sctl=%u dctl=%u\n",
                      channel, s->hsdma_transfer_count[channel],
                      dual ? "dual" : "single", mode, units, remaining,
                      unit_size, src, dst, src_control, dst_control);
    }
    for (i = 0; i < units; i++) {
        if (dual) {
            cpu_physical_memory_read(src, buf, unit_size);
            if (log_transfer) {
                xp3200s_ivram_stats_add(&src_ivram, src, buf, unit_size);
                xp3200s_ivram_stats_add(&dst_ivram, dst, buf, unit_size);
            }
            cpu_physical_memory_write(dst, buf, unit_size);
            dst = xp3200s_hsdma_next_address(
                dst, dst_control, unit_size);
        } else if ((ctrl & 0x4000) != 0) {
            /* External I/O input is modeled as an idle, pulled-up bus. */
            memset(buf, 0xff, unit_size);
            cpu_physical_memory_write(src, buf, unit_size);
            if (log_transfer) {
                xp3200s_ivram_stats_add(&dst_ivram, src, buf, unit_size);
            }
        } else {
            /* Memory-to-external-I/O transfers consume and discard data. */
            cpu_physical_memory_read(src, buf, unit_size);
            if (log_transfer) {
                xp3200s_ivram_stats_add(&src_ivram, src, buf, unit_size);
            }
        }
        src = xp3200s_hsdma_next_address(src, src_control, unit_size);
    }

    if (mode == 2 && src_control == 2) {
        src = initial_src;
    }
    if (mode == 2 && dst_control == 2) {
        dst = initial_dst;
    }
    xp3200s_hsdma_store_address(s, base + 0x04, src_high, src);
    if (dual) {
        xp3200s_hsdma_store_address(s, base + 0x08, dst_high, dst);
    }
    if (log_transfer) {
        xp3200s_hsdma_log_ivram_stats(channel, "src", &src_ivram);
        xp3200s_hsdma_log_ivram_stats(channel, "dst", &dst_ivram);
    }

    if (mode == 2) {
        remaining--;
        count_reg = (remaining & 0xff) << 8 |
                    (count_reg & 0xff);
        ctrl = (ctrl & 0xff00) | ((remaining >> 8) & 0xff);
    } else {
        remaining -= units;
        count_reg = remaining & 0xffff;
        ctrl = (ctrl & 0xff00) | ((remaining >> 16) & 0xff);
    }
    xp3200s_io_stw_raw(s, base + 0x00, count_reg);
    xp3200s_io_stw_raw(s, base + 0x02, ctrl);
    if (remaining == 0) {
        xp3200s_hsdma_finish_channel(s, channel, base, ctrl);
    }
}

static bool xp3200s_hsdma_uses_software_trigger(XP3200SState *s,
                                                 unsigned channel)
{
    uint8_t setup = channel < 2 ? s->io_regs[XP3200S_HSDMA_TRIG01] :
                                  s->io_regs[XP3200S_HSDMA_TRIG23];
    unsigned shift = (channel & 1) ? 4 : 0;

    return ((setup >> shift) & 0xf) == 0;
}

static void xp3200s_hsdma_software_trigger(XP3200SState *s, uint8_t value)
{
    unsigned channel;

    for (channel = 0; channel < XP3200S_HSDMA_CHANNELS; channel++) {
        hwaddr base = xp3200s_hsdma_channel_base(channel);

        if ((value & (1u << channel)) == 0) {
            continue;
        }
        if (!xp3200s_hsdma_uses_software_trigger(s, channel)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-hsdma: ch%u software trigger ignored; "
                          "trigger setup is 0x%02x/0x%02x\n",
                          channel, s->io_regs[XP3200S_HSDMA_TRIG01],
                          s->io_regs[XP3200S_HSDMA_TRIG23]);
            continue;
        }

        s->io_regs[base + 0x0e] |= 1;
        if (s->io_regs[base + 0x0c] & 1) {
            xp3200s_hsdma_run_channel(s, channel);
        }
    }
}

static uint8_t xp3200s_hsdma_read_byte(XP3200SState *s, hwaddr offset)
{
    if (offset == XP3200S_HSDMA_SW_TRIG) {
        return 0;
    }
    if (!xp3200s_is_hsdma_reg(offset)) {
        return s->io_regs[offset];
    }

    switch (offset & 0xf) {
    case 0x0c:
    case 0x0e:
        return s->io_regs[offset] & 1;
    default:
        return s->io_regs[offset];
    }
}

static bool xp3200s_hsdma_write_byte(XP3200SState *s, hwaddr offset,
                                      uint8_t value)
{
    if (offset == XP3200S_HSDMA_SW_TRIG) {
        xp3200s_hsdma_software_trigger(s, value & 0x0f);
        s->io_regs[offset] = 0;
        return true;
    }
    if (!xp3200s_is_hsdma_reg(offset)) {
        return false;
    }

    switch (offset & 0xf) {
    case 0x0c:
        s->io_regs[offset] = value & 1;
        break;
    case 0x0e:
        if (value & 1) {
            s->io_regs[offset] &= ~1u;
        }
        break;
    default:
        s->io_regs[offset] = value;
        break;
    }
    return true;
}

static bool xp3200s_timer16_clock_ctrl_channel(hwaddr offset,
                                                unsigned *channel)
{
    if (offset < XP3200S_16TM_CLOCK_CTRL_BASE ||
        offset >= XP3200S_16TM_CLOCK_CTRL_BASE + XP3200S_16TM_CHANNELS) {
        return false;
    }
    *channel = offset - XP3200S_16TM_CLOCK_CTRL_BASE;
    return true;
}

static bool xp3200s_timer16_reg_channel(hwaddr offset, unsigned *channel)
{
    if (offset < XP3200S_16TM_BASE ||
        offset >= XP3200S_16TM_BASE +
                  XP3200S_16TM_CHANNELS * XP3200S_16TM_STRIDE) {
        return false;
    }
    *channel = (offset - XP3200S_16TM_BASE) / XP3200S_16TM_STRIDE;
    return true;
}

static bool xp3200s_timer16_read_byte(XP3200SState *s, hwaddr offset,
                                       uint8_t *value)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);
    unsigned channel;
    hwaddr rel;

    if (xp3200s_timer16_clock_ctrl_channel(offset, &channel)) {
        *value = reg != NULL ? s->io_regs[offset] & reg->mask :
                               s->io_regs[offset];
        return true;
    }
    if (!xp3200s_timer16_reg_channel(offset, &channel)) {
        return false;
    }

    rel = (offset - XP3200S_16TM_BASE) % XP3200S_16TM_STRIDE;
    if (rel == 6) {
        *value = s->io_regs[offset] & XP3200S_16TM_CTRL_READ_MASK;
    } else {
        *value = reg != NULL ? s->io_regs[offset] & reg->mask :
                               s->io_regs[offset];
    }
    return true;
}

static void xp3200s_wdt_expire(void *opaque)
{
    XP3200SState *s = opaque;

    if (!s->wdt_armed) {
        return;
    }
    s->wdt_armed = false;
    error_report("xp3200: watchdog timeout - resetting");
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static bool xp3200s_timer16_write_byte(XP3200SState *s, hwaddr offset,
                                        uint8_t value)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);
    unsigned channel;
    hwaddr rel;
    uint8_t mask;

    if (xp3200s_timer16_clock_ctrl_channel(offset, &channel)) {
        s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
        if (channel <= 1) {
            xp3200s_timer16_update(s);
        } else if (channel == 2) {
            xp3200s_timer2b_update(s);
        } else if (channel == 4) {
            xp3200s_timer4b_update(s);
        }
        return true;
    }
    if (!xp3200s_timer16_reg_channel(offset, &channel)) {
        return false;
    }

    rel = (offset - XP3200S_16TM_BASE) % XP3200S_16TM_STRIDE;
    if (rel == 4 || rel == 5) {
        return true;
    }
    if (rel == 6) {
        if (offset == XP3200S_16TM_CTRL(0) && value == 0x23) {
            /* OS watchdog feed: restart the 2 s timer. */
            s->wdt_armed = true;
            timer_mod(s->wdt_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 2000);
        }
        mask = reg != NULL ? reg->mask : 0x7f;
        if ((value & XP3200S_16TM_CTRL_PRESET) != 0) {
            xp3200s_timer16_stw(s, XP3200S_16TM_COUNTER(channel), 0);
            if (s->trace_io || s->trace_key_scan) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-timer: 16tm%u preset counter=0 "
                              "ctrl-write=0x%02x\n", channel, value);
            }
        }
        s->io_regs[offset] =
            (value & mask) & ~XP3200S_16TM_CTRL_PRESET;
        if (channel <= 1) {
            xp3200s_timer16_update(s);
        } else if (channel == 2) {
            xp3200s_timer2b_update(s);
        } else if (channel == 4) {
            xp3200s_timer4b_update(s);
        }
        return true;
    }

    s->io_regs[offset] = reg != NULL ? value & reg->mask : value;
    if (channel <= 1) {
        xp3200s_timer16_update(s);
    } else if (channel == 2) {
        xp3200s_timer2b_update(s);
    } else if (channel == 4) {
        xp3200s_timer4b_update(s);
    }
    return true;
}

static bool xp3200s_timer8_write_byte(XP3200SState *s, hwaddr offset,
                                       uint8_t value)
{
    unsigned channel;
    unsigned rel;
    hwaddr base;

    if (offset < XP3200S_8TM_CTRL_BASE ||
        offset >= XP3200S_8TM_CTRL_BASE +
                  XP3200S_8TM_CHANNELS * XP3200S_8TM_STRIDE) {
        return false;
    }
    channel = (offset - XP3200S_8TM_CTRL_BASE) /
              XP3200S_8TM_STRIDE;
    rel = (offset - XP3200S_8TM_CTRL_BASE) %
          XP3200S_8TM_STRIDE;
    base = XP3200S_8TM_CTRL_BASE +
           channel * XP3200S_8TM_STRIDE;

    switch (rel) {
    case 0:
        if ((value & XP3200S_8TM_CTRL_PRESET) != 0) {
            s->io_regs[base + 2] = s->io_regs[base + 1];
        }
        s->io_regs[base] = (value & 0x07) &
                           ~XP3200S_8TM_CTRL_PRESET;
        break;
    case 1:
        s->io_regs[offset] = value;
        break;
    case 2:
        /* The down-counter is read-only. */
        return true;
    default:
        return false;
    }
    xp3200s_timer8_update(s);
    return true;
}

static uint8_t xp3200s_io_read_byte(XP3200SState *s, hwaddr offset)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);
    uint8_t value = s->io_regs[offset];
    hwaddr ctrl_offset;
    uint8_t gpio_mask;
    unsigned gpio_port;

    if (xp3200s_is_adc_reg(offset)) {
        return xp3200s_adc_read_byte(s, offset);
    }
    if (offset == XP3200S_HSDMA_SW_TRIG || xp3200s_is_hsdma_reg(offset)) {
        return xp3200s_hsdma_read_byte(s, offset);
    }
    if (xp3200s_timer16_read_byte(s, offset, &value)) {
        return value;
    }
    if (xp3200s_is_clock_timer_reg(offset)) {
        return xp3200s_clock_timer_read_byte(s, offset);
    }
    if (offset == XP3200S_K5_DATA || offset == XP3200S_K6_DATA) {
        value = xp3200s_k_input_data_read(s, offset);
        xp3200s_key_scan_trace(s, false, offset, value);
        return value;
    }
    if (xp3200s_gpio_data_info(offset, &ctrl_offset, &gpio_mask,
                                &gpio_port)) {
        value = xp3200s_gpio_data_read(s, offset, ctrl_offset, gpio_mask,
                                        gpio_port);
        xp3200s_key_scan_trace(s, false, offset, value);
        return value;
    }
    return reg != NULL ? value & reg->mask : value;
}

static void xp3200s_io_write_byte(XP3200SState *s, hwaddr offset,
                                   uint8_t value)
{
    const XP3200SRegInfo *reg = xp3200s_find_reg(offset);

    if (offset == XP3200S_SIF0_STATUS ||
        offset == XP3200S_SIF1_STATUS ||
        offset == XP3200S_SIF2_STATUS ||
        offset == XP3200S_SIF3_STATUS) {
        /*
         * Error flags are write-zero-to-clear; TDBE and TEND are read-only.
         * With the instantaneous backend there is never a full TX buffer.
         */
        s->io_regs[offset] = XP3200S_SIF_TDBE;
        return;
    }
    if (xp3200s_adc_write_byte(s, offset, value)) {
        return;
    }
    if (xp3200s_hsdma_write_byte(s, offset, value)) {
        return;
    }
    if (xp3200s_timer8_write_byte(s, offset, value)) {
        return;
    }
    if (xp3200s_timer16_write_byte(s, offset, value)) {
        return;
    }
    if (xp3200s_clock_timer_write_byte(s, offset, value)) {
        return;
    }
    if (offset == XP3200S_K5_DATA || offset == XP3200S_K6_DATA) {
        xp3200s_key_scan_trace(s, true, offset,
                                xp3200s_k_input_data_read(s, offset));
        return;
    }
    if (reg == NULL) {
        s->io_regs[offset] = value;
        xp3200s_key_scan_trace(s, true, offset, s->io_regs[offset]);
        return;
    }

    if (reg->flags & XP3200S_REG_RESET_ONE_TO_CLEAR) {
        s->io_regs[offset] &= ~(value & reg->mask);
    } else {
        s->io_regs[offset] = value & reg->mask;
    }
    xp3200s_key_scan_trace(s, true, offset, s->io_regs[offset]);
}

static uint64_t xp3200s_io_load_masked(XP3200SState *s, hwaddr offset,
                                        unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value |= (uint64_t)xp3200s_io_read_byte(s, offset + i) << (i * 8);
    }

    return value;
}

static void xp3200s_io_store_masked(XP3200SState *s, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    bool backlight_was_on = xp3200s_backlight_on(s);
    unsigned i;

    for (i = 0; i < size; i++) {
        xp3200s_io_write_byte(s, offset + i, value >> (i * 8));
    }

    if ((xp3200s_range_touches(offset, size, XP3200S_PORT2_DATA, 1) ||
         xp3200s_range_touches(offset, size, XP3200S_PORT2_IOCTRL, 1)) &&
        backlight_was_on != xp3200s_backlight_on(s)) {
        if (s->trace_io) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xp3200s-lcd: backlight %s "
                          "p2d=0x%02x ioc2=0x%02x\n",
                          xp3200s_backlight_on(s) ? "on" : "off",
                          s->io_regs[XP3200S_PORT2_DATA],
                          s->io_regs[XP3200S_PORT2_IOCTRL]);
        }
        graphic_hw_update(s->con);
    }

    if (xp3200s_range_touches(offset, size, XP3200S_TTBR_OFFSET, 4)) {
        xp3200s_set_ttbr(s, xp3200s_ttbr_from_regs(s));
    }

    if (xp3200s_range_touches(offset, size,
                               XP3200S_PORT_INT_SELECT2, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_PORT_INT_POLARITY, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_PORT_INT_EDGE_LEVEL, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_EIR7_PORT4_7_CTM_ADC, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_FIR7_PORT4_7_CTM_ADC, 1)) {
        xp3200s_port_input_update(s, "port-input-config", false, true);
    }
    if (xp3200s_range_touches(offset, size, XP3200S_PORT1_DATA, 1)) {
        xp3200s_port_input_update(s, "gpio-strobe", true, true);
    }
    if (xp3200s_range_touches(offset, size,
                               XP3200S_KEY_INPUT_SELECT, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_KEY_INPUT_CONDITION0, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_KEY_INPUT_CONDITION1, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_KEY_INPUT_MASK0, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_KEY_INPUT_MASK1, 1)) {
        xp3200s_key_input_update(s, "key-input-config", false);
    }
    if (xp3200s_range_touches(offset, size, XP3200S_K5_FUNC_SELECT, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_K6_FUNC_SELECT, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_PORT0_DATA, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_PORT0_IOCTRL, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_PORT2_DATA, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_PORT2_IOCTRL, 1)) {
        xp3200s_key_input_update(s, "key-input-source", true);
    }

    if (offset <= 0x2a9 && offset + size > 0x260) {
        xp3200s_update_irq(s);
    }
    if (xp3200s_range_touches(offset, size, XP3200S_PIR3_16TM2_3, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_EIR3_16TM2_3, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_FIR3_16TM2_3, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_16TM_CTRL(3), 1)) {
        xp3200s_timer2b_update(s);
        xp3200s_timer3b_update(s);
    }
    if (xp3200s_range_touches(offset, size, XP3200S_PIR4_16TM4_5, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_EIR4_16TM4_5, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_FIR4_16TM4_5, 1)) {
        xp3200s_timer4b_update(s);
    }
    if (xp3200s_range_touches(offset, size, XP3200S_PIR_8TM_SIF0, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_EIR5_8TM0_3, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_FIR5_8TM0_3, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_8TM_DIRECT_CLOCK, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_8TM_CLOCK01, 2) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_PRESCALER_CLOCK_SELECT, 1) ||
        xp3200s_range_touches(offset, size, XP3200S_8TM_CTRL_BASE,
                               XP3200S_8TM_CHANNELS *
                               XP3200S_8TM_STRIDE)) {
        xp3200s_timer8_update(s);
    }
    if (xp3200s_range_touches(offset, size,
                               XP3200S_PRESCALER_CLOCK_SELECT, 1)) {
        xp3200s_timer16_update(s);
        xp3200s_timer2b_update(s);
        xp3200s_timer4b_update(s);
    }
    if (xp3200s_range_touches(offset, size, XP3200S_CTM_RUN_CTRL,
                               XP3200S_CTM_DAY_CMP -
                               XP3200S_CTM_RUN_CTRL + 1) ||
        xp3200s_range_touches(offset, size, XP3200S_PIR_CTM, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_EIR7_PORT4_7_CTM_ADC, 1) ||
        xp3200s_range_touches(offset, size,
                               XP3200S_FIR7_PORT4_7_CTM_ADC, 1)) {
        xp3200s_clock_timer_update(s);
    }
}

static void xp3200s_io_reset(XP3200SState *s)
{
    unsigned i;

    memset(s->io_regs, 0, sizeof(s->io_regs));
    memset(s->gpio_input_low, 0, sizeof(s->gpio_input_low));
    memset(s->port_int_input_low, 0, sizeof(s->port_int_input_low));
    s->k5_input_low = 0;
    s->k6_input_low = 0;
    memset(s->key_input_high, 0, sizeof(s->key_input_high));
    memset(s->key_input_high_valid, 0, sizeof(s->key_input_high_valid));
    s->port_input_high = 0xf0;
    s->port_input_high_valid = false;
    memset(s->keyboard_qcode_down, 0, sizeof(s->keyboard_qcode_down));
    memset(s->keyboard_line_down_count, 0,
           sizeof(s->keyboard_line_down_count));
    memset(s->keyboard_matrix_down_count, 0,
           sizeof(s->keyboard_matrix_down_count));
    s->adc_touch_x = 512;
    s->adc_touch_y = 512;
    s->touch_pixel_x = s->lcd_width / 2;
    s->touch_pixel_y = s->lcd_height / 2;
    s->adc_last_channel = 0;
    s->touch_down = false;
    s->ctm_subsecond = 0;
    for (i = 0; i < 2; i++) {
        unsigned which;

        for (which = 0; which < 2; which++) {
            if (s->timer16_compare_timer[i][which] != NULL) {
                timer_del(s->timer16_compare_timer[i][which]);
            }
        }
    }
    for (i = 0; i < XP3200S_8TM_CHANNELS; i++) {
        if (s->timer8_underflow_timer[i] != NULL) {
            timer_del(s->timer8_underflow_timer[i]);
        }
    }
    if (s->timer2b_compare_timer != NULL) {
        timer_del(s->timer2b_compare_timer);
    }
    if (s->timer4b_compare_timer != NULL) {
        timer_del(s->timer4b_compare_timer);
    }
    if (s->clock_timer != NULL) {
        timer_del(s->clock_timer);
    }
    for (i = 0; i < ARRAY_SIZE(xp3200s_regs); i++) {
        const XP3200SRegInfo *reg = &xp3200s_regs[i];

        s->io_regs[reg->offset] = reg->reset & reg->mask;
    }
    xp3200s_clock_timer_init_rtc(s);
}

static void xp3200s_board_io_reset(XP3200SState *s)
{
    memset(s->board_regs, 0, sizeof(s->board_regs));
    memset(s->board_seen, 0, sizeof(s->board_seen));
    s->board_data_latch_reads = 0;
    s->board_data_latch_pending = false;
    if (s->usb_connected) {
        s->board_regs[XP3200S_BOARD_USB_STATUS] =
            XP3200S_BOARD_USB_CONNECTED_BIT;
    }

    /*
     * The boot self-test reads 0x01000084/85 as a big-endian 16-bit status
     * word and expects 0x0004 after its command latch sequence.
     */
    s->board_regs[XP3200S_BOARD_STATUS_H] = 0x00;
    s->board_regs[XP3200S_BOARD_STATUS_L] = 0x04;
}

static void xp3200s_record_mmio(XP3200SState *s, bool is_write,
                                 hwaddr offset, uint64_t value,
                                 unsigned size)
{
    XP3200SMMIOTrace *trace;
    uint8_t seen_mask = is_write ? 2 : 1;
    uint32_t pc = xp3200s_current_pc();
    bool first;

    trace = &s->mmio_trace[s->mmio_trace_pos];
    trace->seq = ++s->mmio_trace_seq;
    trace->pc = pc;
    trace->addr = XP3200S_IO_BASE + offset;
    trace->value = value;
    trace->size = size;
    trace->is_write = is_write;
    s->mmio_trace_pos = (s->mmio_trace_pos + 1) % XP3200S_MMIO_TRACE_LEN;

    first = (s->io_seen[offset] & seen_mask) == 0;
    if (first) {
        s->io_seen[offset] |= seen_mask;
    }
    if (first || s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-mmio: %s %s pc=0x%08x "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                      " reg=%s group=%s\n",
                      first ? "first" : "trace",
                      is_write ? "write" : "read", pc,
                      XP3200S_IO_BASE + offset, size, size * 2, value,
                      xp3200s_io_reg_name(offset),
                      xp3200s_io_group_name(offset));
    }
}

static void xp3200s_dump_mmio_trace(XP3200SState *s, const char *reason)
{
    unsigned count = MIN(s->mmio_trace_seq, (uint64_t)XP3200S_MMIO_TRACE_LEN);
    unsigned start;
    unsigned i;

    if (count == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-mmio: no accesses recorded before %s\n",
                      reason);
        return;
    }

    start = s->mmio_trace_seq > XP3200S_MMIO_TRACE_LEN ?
            s->mmio_trace_pos : 0;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-mmio: last %u accesses before %s\n",
                  count, reason);
    for (i = 0; i < count; i++) {
        unsigned idx = (start + i) % XP3200S_MMIO_TRACE_LEN;
        XP3200SMMIOTrace *trace = &s->mmio_trace[idx];

        qemu_log_mask(LOG_GUEST_ERROR,
                      "  #%06" PRIu64 " pc=0x%08x %c%u "
                      "addr=0x%08" HWADDR_PRIx " value=0x%0*" PRIx64
                      " reg=%s group=%s\n",
                      trace->seq, trace->pc,
                      trace->is_write ? 'W' : 'R', trace->size,
                      trace->addr, trace->size * 2, trace->value,
                      xp3200s_io_reg_name(trace->addr - XP3200S_IO_BASE),
                      xp3200s_io_group_name(trace->addr - XP3200S_IO_BASE));
    }
}

static void xp3200s_exit_notify(Notifier *notifier, void *data)
{
    XP3200SState *s = container_of(notifier, XP3200SState, exit);
    unsigned i;

    (void)data;
    if (s->debug_usb_wakeup_timer != NULL) {
        timer_del(s->debug_usb_wakeup_timer);
    }
    if (s->debug_nmi_wakeup_timer != NULL) {
        timer_del(s->debug_nmi_wakeup_timer);
    }
    if (s->debug_port4_wakeup_timer != NULL) {
        timer_del(s->debug_port4_wakeup_timer);
    }
    if (s->debug_port5_wakeup_timer != NULL) {
        timer_del(s->debug_port5_wakeup_timer);
    }
    if (s->debug_lcd_dump_timer != NULL) {
        timer_del(s->debug_lcd_dump_timer);
    }
    if (s->timer3b_wakeup_timer != NULL) {
        timer_del(s->timer3b_wakeup_timer);
    }
    for (i = 0; i < 2; i++) {
        unsigned which;

        for (which = 0; which < 2; which++) {
            if (s->timer16_compare_timer[i][which] != NULL) {
                timer_del(s->timer16_compare_timer[i][which]);
            }
        }
    }
    if (s->timer2b_compare_timer != NULL) {
        timer_del(s->timer2b_compare_timer);
    }
    if (s->timer4b_compare_timer != NULL) {
        timer_del(s->timer4b_compare_timer);
    }
    for (i = 0; i < XP3200S_8TM_CHANNELS; i++) {
        if (s->timer8_underflow_timer[i] != NULL) {
            timer_del(s->timer8_underflow_timer[i]);
        }
    }
    if (s->clock_timer != NULL) {
        timer_del(s->clock_timer);
    }
    if (s->audio_flush_timer != NULL) {
        timer_del(s->audio_flush_timer);
    }
    if (s->nand_flush_timer != NULL) {
        timer_del(s->nand_flush_timer);
    }
    if (s->kbd_handler != NULL) {
        qemu_input_handler_unregister(s->kbd_handler);
        s->kbd_handler = NULL;
    }
    if (s->ptr_handler != NULL) {
        qemu_input_handler_unregister(s->ptr_handler);
        s->ptr_handler = NULL;
    }
    if (xp3200s_active_machine == s) {
        xp3200s_active_machine = NULL;
    }
    if (s->audio_stream != NULL) {
        fflush(s->audio_stream);
        fclose(s->audio_stream);
        s->audio_stream = NULL;
    }
    xp3200s_audio_flush_cb(s);
    if (s->audio_player_fd >= 0) {
        close(s->audio_player_fd);
        s->audio_player_fd = -1;
    }
    if (s->audio_player_pid != 0) {
        g_spawn_close_pid(s->audio_player_pid);
        s->audio_player_pid = 0;
    }
    g_free(s->audio_stream_path);
    s->audio_stream_path = NULL;
    g_free(s->audio_player_path);
    s->audio_player_path = NULL;
    xp3200s_nand_storage_save(s);
    if (s->nand_file != NULL) {
        fclose(s->nand_file);
        s->nand_file = NULL;
    }
    g_free(s->nand_storage);
    s->nand_storage = NULL;
    g_free(s->nand_image_path);
    s->nand_image_path = NULL;
    if (s->sd_file != NULL) {
        fclose(s->sd_file);
        s->sd_file = NULL;
    }
    g_free(s->debug_lcd_dump_path);
    s->debug_lcd_dump_path = NULL;
    xp3200s_dump_mmio_trace(s, "qemu exit");
}

static uint64_t xp3200s_nand_io_read(void *opaque, hwaddr offset, unsigned size);
static void xp3200s_nand_io_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size);

static void xp3200s_trace_io_all(XP3200SState *s, const char *space,
                                 hwaddr offset, uint64_t value, unsigned size);

static uint64_t xp3200s_io_read_impl(void *opaque, hwaddr offset,
                                     unsigned size);

static uint64_t xp3200s_io_read(void *opaque, hwaddr offset, unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t v = xp3200s_io_read_impl(opaque, offset, size);

    xp3200s_trace_io_all(s, "ior", offset, v, size);
    return v;
}

static uint64_t xp3200s_io_read_impl(void *opaque, hwaddr offset,
                                     unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t value;

    if (offset + size > XP3200S_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-mmio: read outside io window "
                      "addr=0x%08" HWADDR_PRIx " size=%u\n",
                      XP3200S_IO_BASE + offset, size);
        return 0;
    }

    value = xp3200s_io_load_masked(s, offset, size);
    if (offset == XP3200S_PORT2_DATA) {
        /* P26 = NAND RY/BY, ready=1 (manual III-14 sample). */
        value |= 0x40;
    }
    if (offset >= 0x0280 && offset < 0x0300) {
        s->dbg_kport_reads[offset - 0x0280]++;
    }
    if (s->trace_key_scan &&
        (offset == XP3200S_K5_DATA || offset == XP3200S_PORT0_DATA)) {
        fprintf(stderr, "SCANRD off=%03x val=%02x pc=%08x k5=0x%02x p0=0x%02x\n",
                (unsigned)offset, (unsigned)value, xp3200s_current_pc(),
                s->k5_input_low, s->gpio_input_low[XP3200S_KEY_GPIO_PORT]);
    }
    xp3200s_record_mmio(s, false, offset, value, size);
    return value;
}

static void xp3200s_trace_io_all(XP3200SState *s, const char *space,
                                 hwaddr offset, uint64_t value, unsigned size)
{
    int64_t now;

    if (s->trace_io_all_ms == 0 || s->trace_io_all_count >= 20000) {
        return;
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (now < (int64_t)s->trace_io_all_ms ||
        now > (int64_t)(s->trace_io_all_ms + s->trace_io_all_len)) {
        return;
    }
    if (offset < ARRAY_SIZE(s->trace_dup)) {
        if (s->trace_dup[offset] >= 8) {
            return;
        }
        s->trace_dup[offset]++;
    }
    s->trace_io_all_count++;
    fprintf(stderr, "IOALL %s off=%05x val=%04x size=%u pc=%08x t=%" PRId64
            "\n", space, (unsigned)offset, (unsigned)value, size,
            xp3200s_current_pc(), now);
}

static void xp3200s_io_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    XP3200SState *s_trace = opaque;

    if (offset == XP3200S_PORT0_DATA) {
        s_trace->kbd_p0_latch = value & 0xff;
    }

    xp3200s_trace_io_all(s_trace, "io", offset, value, size);
    if (s_trace->trace_key_scan &&
        (offset == XP3200S_PORT0_DATA || offset == XP3200S_PORT1_DATA ||
         offset == XP3200S_PORT2_DATA || offset == XP3200S_PORT3_DATA) &&
        s_trace->gpio_trace_count < 400) {
        uint32_t caller = 0;

        if (s_trace->cpu != NULL) {
            cpu_physical_memory_read(S1C33_CPU(s_trace->cpu)->env.sp, &caller,
                                     sizeof(caller));
        }
        s_trace->gpio_trace_count++;
        fprintf(stderr, "GPIOW n=%u off=%04x val=%02x pc=%08x caller=%08x\n",
                s_trace->gpio_trace_count, (unsigned)offset,
                (unsigned)value, xp3200s_current_pc(), caller);
    }
    XP3200SState *s = opaque;

    if (offset + size > XP3200S_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-mmio: write outside io window "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                      "\n",
                      XP3200S_IO_BASE + offset, size, value);
        return;
    }

    xp3200s_io_store_masked(s, offset, value, size);
    if (offset >= 0x0280 && offset < 0x0300) {
        s->dbg_kport_writes[offset - 0x0280]++;
    }
    if (s->trace_key_scan &&
        ((offset >= 0x02ca && offset <= 0x02cf) ||
         offset == 0x0270 || offset == 0x0280 || offset == 0x02c0)) {
        fprintf(stderr, "KEYREG wr off=%03x val=%02x pc=%08x\n",
                (unsigned)offset, (unsigned)value, xp3200s_current_pc());
    }
    xp3200s_record_mmio(s, true, offset, value, size);
}

static const MemoryRegionOps xp3200s_io_ops = {
    .read = xp3200s_io_read,
    .write = xp3200s_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void xp3200s_touch_serial_set_miso(XP3200SState *s, bool high)
{
    if (high) {
        s->touch_io_regs[XP3200S_TOUCH_SERIAL] |= XP3200S_TOUCH_MISO;
    } else {
        s->touch_io_regs[XP3200S_TOUCH_SERIAL] &= ~XP3200S_TOUCH_MISO;
    }
}

static void xp3200s_touch_serial_reset(XP3200SState *s)
{
    s->touch_serial_phase = XP3200S_TOUCH_COMMAND;
    s->touch_serial_command = 0;
    s->touch_serial_data = 0;
    s->touch_serial_bits = 0;
    s->touch_serial_trailer_clocks = 0;
    s->touch_io_regs[XP3200S_TOUCH_SERIAL] &=
        ~(XP3200S_TOUCH_CLK | XP3200S_TOUCH_MISO);
}

static uint16_t xp3200s_touch_serial_sample(XP3200SState *s,
                                             uint8_t command)
{
    switch (command & 0xf0) {
    case 0x90:
        return MIN(s->adc_touch_y << 2, 0x0fff);
    case 0xd0:
        return MIN(s->adc_touch_x << 2, 0x0fff);
    default:
        return 0;
    }
}

static void xp3200s_touch_serial_clock_rise(XP3200SState *s)
{
    uint8_t channel;

    switch (s->touch_serial_phase) {
    case XP3200S_TOUCH_COMMAND:
        s->touch_serial_command = (s->touch_serial_command << 1) |
            ((s->io_regs[XP3200S_PORT1_DATA] & XP3200S_TOUCH_MOSI) != 0);
        if (++s->touch_serial_bits == 8) {
            s->touch_serial_data = xp3200s_touch_serial_sample(
                s, s->touch_serial_command);
            s->touch_serial_phase = XP3200S_TOUCH_DUMMY;
            s->touch_serial_bits = 0;
            xp3200s_touch_serial_set_miso(s, false);
            channel = (s->touch_serial_command & 0xf0) == 0x90 ? 1 :
                      (s->touch_serial_command & 0xf0) == 0xd0 ? 2 : 0;
            if (s->trace_io ||
                (channel != 0 &&
                 (s->touch_serial_logged_channels & channel) == 0)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-touch-adc: command=0x%02x "
                              "sample=0x%03x pen=%u pixel=%u,%u\n",
                              s->touch_serial_command, s->touch_serial_data,
                              s->touch_down, s->touch_pixel_x,
                              s->touch_pixel_y);
            }
            s->touch_serial_logged_channels |= channel;
        }
        break;
    case XP3200S_TOUCH_DUMMY:
        s->touch_serial_phase = XP3200S_TOUCH_DATA;
        s->touch_serial_bits = 0;
        xp3200s_touch_serial_set_miso(s, false);
        break;
    case XP3200S_TOUCH_DATA:
        xp3200s_touch_serial_set_miso(
            s, (s->touch_serial_data &
                (1u << (11 - s->touch_serial_bits))) != 0);
        if (++s->touch_serial_bits == 12) {
            s->touch_serial_phase = XP3200S_TOUCH_TRAILER;
            s->touch_serial_trailer_clocks = 0;
        }
        break;
    case XP3200S_TOUCH_TRAILER:
        xp3200s_touch_serial_set_miso(s, false);
        if (++s->touch_serial_trailer_clocks == 3) {
            xp3200s_touch_serial_reset(s);
        }
        break;
    }
}

static uint64_t xp3200s_touch_io_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (offset + size > XP3200S_TOUCH_IO_SIZE) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->touch_io_regs[offset + i] << (i * 8);
    }
    if (offset < ARRAY_SIZE(s->dbg_touch_reads)) {
        s->dbg_touch_reads[offset]++;
    }
    if (s->trace_key_scan && (offset == 0x020 || offset == 0x021) &&
        s->kbd_serial_trace_count < 600) {
        s->kbd_serial_trace_count++;
        fprintf(stderr, "SER r off=%03x val=%02x pc=%08x\n",
                (unsigned)offset, (unsigned)value, xp3200s_current_pc());
    }
    if (offset == XP3200S_SD_CS_PORT && !s->sd_inserted) {
        /* Card-detect input reads high while no card is present. */
        value |= 0x04;
    }
    if (offset == 0xf46) {
        /*
         * NAND R/B# is sampled through this port: both the NOR loader
         * (0x00C466A2) and the OS driver (0x021745A0) poll bit5 and wait for
         * it to go high.  Bit5 therefore always reads back ready.
         */
        value |= 0x20;
        if (s->trace_io) {
            fprintf(stderr, "NANDRB rd size=%u val=%02x pc=%08x\n",
                    size, (unsigned)value, xp3200s_current_pc());
        }
    }
    return value;
}

/* S1C33L05 NAND interface: two 256-byte parity areas per 512-byte cycle. */
static void xp3200s_nand_ecc_byte(XP3200SState *s, uint8_t value)
{
    static const uint8_t column_masks[] = {
        0x55, 0xaa, 0x33, 0xcc, 0x0f, 0xf0,
    };
    unsigned count, index, area, base, bit;
    uint16_t line;
    uint8_t column;
    bool parity;

    if (!(s->touch_io_regs[XP3200S_NAND_ECC_ENABLE] & 1) ||
        s->nand_ecc_count == 512) {
        return;
    }

    count = ++s->nand_ecc_count;
    index = count - 1; /* ECC line parity addresses bytes 0..255. */
    area = index / 256;
    base = XP3200S_NAND_ECC_BASE + area * 3;
    column = s->touch_io_regs[base];
    line = s->touch_io_regs[base + 1] |
           (uint16_t)s->touch_io_regs[base + 2] << 8;
    parity = ctpop8(value) & 1;
    for (bit = 0; bit < ARRAY_SIZE(column_masks); bit++) {
        column ^= (ctpop8(value & column_masks[bit]) & 1) << (bit + 2);
    }
    for (bit = 0; bit < 8; bit++) {
        line ^= (uint16_t)parity << (bit * 2 + ((index >> bit) & 1));
    }
    s->touch_io_regs[base] = column;
    s->touch_io_regs[base + 1] = line;
    s->touch_io_regs[base + 2] = line >> 8;
    if (count == 512) {
        s->touch_io_regs[XP3200S_NAND_ECC_READY] = 1;
    }
}

static void xp3200s_touch_io_write_byte(XP3200SState *s, hwaddr offset,
                                         uint8_t value)
{
    uint8_t old;

    if (s->trace_key_scan && s->row_trace_count < 400 &&
        (value == 0xfe || value == 0xfd || value == 0xfb || value == 0xf7 ||
         value == 0xef || value == 0xdf || value == 0xbf || value == 0x7f)) {
        uint32_t caller = 0;

        if (s->cpu != NULL) {
            cpu_physical_memory_read(S1C33_CPU(s->cpu)->env.sp, &caller,
                                     sizeof(caller));
        }
        s->row_trace_count++;
        fprintf(stderr, "ROW n=%u off=%04x val=%02x pc=%08x caller=%08x\n",
                s->row_trace_count, (unsigned)offset, (unsigned)value,
                xp3200s_current_pc(), caller);
    }

    if (offset == XP3200S_NAND_ECC_READY) {
        if (value & 1) {
            memset(&s->touch_io_regs[XP3200S_NAND_ECC_BASE], 0xff,
                   XP3200S_NAND_ECC_SIZE);
            s->nand_ecc_count = 0;
            s->touch_io_regs[offset] = 0;
        }
        return;
    }
    if (offset == XP3200_KEY_ROW_SELECT) {
        s->touch_io_regs[offset] = value;
        /*
         * The firmware scans the keypad by driving one row low here and then
         * reading K5[3:0] / P0[6:4].  Store first, then recompute what the
         * matrix presents for the row that was just selected.  Only when the
         * matrix model is active: the key-line model drives K5 directly and
         * must not be overwritten.
         */
        if (s->key_matrix) {
            xp3200_keyboard_refresh_matrix(s);
        }
        return;
    }
    if (offset != XP3200S_TOUCH_SERIAL) {
        s->touch_io_regs[offset] = value;
        return;
    }

    old = s->touch_io_regs[offset];
    s->touch_io_regs[offset] =
        (value & ~XP3200S_TOUCH_MISO) | (old & XP3200S_TOUCH_MISO);
    xp3200s_audio_select_update(s, old, s->touch_io_regs[offset]);
    if ((old & XP3200S_TOUCH_CLK) == 0 &&
        (value & XP3200S_TOUCH_CLK) != 0) {
        xp3200s_touch_serial_clock_rise(s);
    }
}

static void xp3200s_touch_io_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    XP3200SState *s = opaque;
    unsigned i;

    if (offset + size > XP3200S_TOUCH_IO_SIZE) {
        return;
    }
    if (offset < ARRAY_SIZE(s->dbg_touch_writes)) {
        s->dbg_touch_writes[offset]++;
    }
    xp3200s_trace_io_all(s, "tio", offset, value, size);
    if (s->trace_key_scan) {
        if (offset >= 0x020 && offset <= 0x023) {
            fprintf(stderr, "P5WR off=%03x val=%02x pc=%08x\n",
                    (unsigned)offset, (unsigned)value,
                    xp3200s_current_pc());
        }
        if (offset == XP3200_KEY_ROW_SELECT) {
            fprintf(stderr, "ROWWR val=%02x pc=%08x k5=0x%02x p0=0x%02x\n",
                    (unsigned)value, xp3200s_current_pc(), s->k5_input_low,
                    s->gpio_input_low[XP3200S_KEY_GPIO_PORT]);
        }
    }
    for (i = 0; i < size; i++) {
        xp3200s_touch_io_write_byte(s, offset + i, value >> (i * 8));
    }
}

static const MemoryRegionOps xp3200s_touch_io_ops = {
    .read = xp3200s_touch_io_read,
    .write = xp3200s_touch_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static bool xp3200s_audio_stream_selected(XP3200SState *s)
{
    uint8_t select = s->touch_io_regs[XP3200S_TOUCH_SERIAL];
    uint8_t p3 = s->touch_io_regs[XP3200S_SD_CS_PORT];

    /*
     * The board latch uses bit 0 for the compressed-data chip select and
     * bit 1 for the decoder control port. Both selects are active low.
     * The XP3200 media driver instead toggles P3 directly: bit 0 set and
     * bit 5 clear marks the decoder data port (bit 5 set is the SCI port).
     */
    if ((p3 & 0x01) != 0 && (p3 & 0x20) == 0) {
        return true;
    }
    return (select & 0x01) == 0 && (select & 0x02) != 0;
}

static bool xp3200s_audio_control_selected_value(uint8_t select)
{
    return (select & 0x01) != 0 && (select & 0x02) == 0;
}

static bool xp3200s_audio_control_selected(XP3200SState *s)
{
    return xp3200s_audio_control_selected_value(
        s->touch_io_regs[XP3200S_TOUCH_SERIAL]);
}

static void xp3200s_audio_sci_transaction_reset(XP3200SState *s)
{
    s->audio_sci_command = 0;
    s->audio_sci_register = 0;
    s->audio_sci_phase = 0;
    s->audio_sci_read_pos = 0;
}

static void xp3200s_audio_select_update(XP3200SState *s, uint8_t old,
                                         uint8_t value)
{
    bool old_control = xp3200s_audio_control_selected_value(old);
    bool new_control = xp3200s_audio_control_selected_value(value);

    if (old_control != new_control) {
        xp3200s_audio_sci_transaction_reset(s);
    }
}

static void xp3200s_audio_decoder_reset(XP3200SState *s)
{
    s->audio_fifo_level = 0;
    s->audio_bytes_per_second = XP3200S_AUDIO_DEFAULT_BYTES_PER_SECOND;
    s->audio_mp3_header = 0;
    s->audio_candidate_bytes_per_second = 0;
    s->audio_candidate_frames = 0;
    s->audio_fifo_updated_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    s->audio_drain_remainder = 0;
    s->audio_decoded_ns = 0;
    s->audio_decode_remainder = 0;
    s->audio_dreq_bytes = 0;
}

static void xp3200s_audio_fifo_update(XP3200SState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t elapsed = now - s->audio_fifo_updated_ns;
    uint64_t scaled;
    uint64_t drained;
    uint64_t consumed;
    uint64_t decoded_scaled;

    s->audio_fifo_updated_ns = now;
    if (elapsed <= 0 || s->audio_fifo_level == 0) {
        if (s->audio_fifo_level == 0) {
            s->audio_drain_remainder = 0;
        }
        return;
    }

    scaled = (uint64_t)elapsed * s->audio_bytes_per_second +
             s->audio_drain_remainder;
    drained = scaled / NANOSECONDS_PER_SECOND;
    s->audio_drain_remainder = scaled % NANOSECONDS_PER_SECOND;
    consumed = MIN(drained, s->audio_fifo_level);
    decoded_scaled = consumed * NANOSECONDS_PER_SECOND +
                     s->audio_decode_remainder;
    s->audio_decoded_ns += decoded_scaled / s->audio_bytes_per_second;
    s->audio_decode_remainder =
        decoded_scaled % s->audio_bytes_per_second;
    if (consumed == s->audio_fifo_level) {
        s->audio_fifo_level = 0;
        s->audio_drain_remainder = 0;
    } else {
        s->audio_fifo_level -= consumed;
    }
}

static bool xp3200s_audio_dreq_ready(XP3200SState *s)
{
    xp3200s_audio_fifo_update(s);
    if (s->audio_dreq_bytes != 0) {
        return true;
    }
    if (s->audio_fifo_level + XP3200S_AUDIO_DREQ_BYTES >
        XP3200S_AUDIO_FIFO_CAPACITY) {
        return false;
    }
    s->audio_dreq_bytes = XP3200S_AUDIO_DREQ_BYTES;
    return true;
}

static unsigned xp3200s_audio_mp3_rate(uint32_t header)
{
    static const unsigned mpeg1_layer3_kbps[16] = {
        0, 32, 40, 48, 56, 64, 80, 96,
        112, 128, 160, 192, 224, 256, 320, 0,
    };
    static const unsigned mpeg2_layer3_kbps[16] = {
        0, 8, 16, 24, 32, 40, 48, 56,
        64, 80, 96, 112, 128, 144, 160, 0,
    };
    unsigned version = extract32(header, 19, 2);
    unsigned layer = extract32(header, 17, 2);
    unsigned bitrate_index = extract32(header, 12, 4);
    unsigned sample_rate_index = extract32(header, 10, 2);
    unsigned kbps;

    if ((header & 0xffe00000) != 0xffe00000 ||
        version == 1 || layer != 1 ||
        bitrate_index == 0 || bitrate_index == 15 ||
        sample_rate_index == 3) {
        return 0;
    }

    kbps = version == 3 ? mpeg1_layer3_kbps[bitrate_index] :
                          mpeg2_layer3_kbps[bitrate_index];
    return kbps * 1000 / 8;
}

static void xp3200s_audio_detect_rate(XP3200SState *s, uint8_t value)
{
    unsigned bytes_per_second;

    s->audio_mp3_header = (s->audio_mp3_header << 8) | value;
    bytes_per_second = xp3200s_audio_mp3_rate(s->audio_mp3_header);
    if (bytes_per_second == 0) {
        return;
    }
    if (bytes_per_second != s->audio_candidate_bytes_per_second) {
        s->audio_candidate_bytes_per_second = bytes_per_second;
        s->audio_candidate_frames = 1;
        return;
    }
    if (s->audio_candidate_frames < 2) {
        s->audio_candidate_frames++;
    }
    if (s->audio_candidate_frames < 2 ||
        bytes_per_second == s->audio_bytes_per_second) {
        return;
    }

    xp3200s_audio_fifo_update(s);
    s->audio_bytes_per_second = bytes_per_second;
    s->audio_decode_remainder = 0;
    info_report("%s audio MPEG rate: %u kbps", xp3200s_model_name(s),
                bytes_per_second * 8 / 1000);
}

static void xp3200s_audio_flush_cb(void *opaque)
{
    XP3200SState *s = opaque;
    ssize_t written;

    if (s->audio_stream != NULL && s->audio_flush_bytes != 0) {
        fflush(s->audio_stream);
        s->audio_flush_bytes = 0;
    }
    if (s->audio_player_fd >= 0 && s->audio_player_buffer_len != 0) {
        written = qemu_write_full(s->audio_player_fd,
                                  s->audio_player_buffer,
                                  s->audio_player_buffer_len);
        if (written != s->audio_player_buffer_len) {
            warn_report("%s audio player stopped accepting data: %s",
                        xp3200s_model_name(s), strerror(errno));
            close(s->audio_player_fd);
            s->audio_player_fd = -1;
        }
        s->audio_player_buffer_len = 0;
    }
}

static bool xp3200s_audio_stream_rotate(XP3200SState *s)
{
    if (s->audio_stream_bytes < XP3200S_AUDIO_STREAM_MAX_BYTES) {
        return true;
    }

    fflush(s->audio_stream);
    fclose(s->audio_stream);
    s->audio_stream = fopen(s->audio_stream_path, "wb");
    s->audio_stream_bytes = 0;
    s->audio_flush_bytes = 0;
    if (s->audio_stream == NULL) {
        warn_report("could not rotate %s audio stream '%s': %s",
                    xp3200s_model_name(s), s->audio_stream_path,
                    strerror(errno));
        return false;
    }
    info_report("%s audio stream rotated at %u MiB",
                xp3200s_model_name(s),
                (unsigned)(XP3200S_AUDIO_STREAM_MAX_BYTES / MiB));
    return true;
}

static void xp3200s_audio_stream_write(XP3200SState *s, uint8_t value)
{
    bool queued = false;

    if (s->audio_stream != NULL && xp3200s_audio_stream_rotate(s)) {
        if (fputc(value, s->audio_stream) == EOF) {
            warn_report("%s audio stream write failed: %s",
                        xp3200s_model_name(s), strerror(errno));
            fclose(s->audio_stream);
            s->audio_stream = NULL;
        } else {
            s->audio_stream_bytes++;
            s->audio_flush_bytes++;
            queued = true;
        }
    }

    if (s->audio_player_fd >= 0) {
        s->audio_player_buffer[s->audio_player_buffer_len++] = value;
        queued = true;
    }

    if (!queued || s->audio_flush_timer == NULL) {
        return;
    }
    if (s->audio_flush_bytes >= XP3200S_AUDIO_STREAM_FLUSH_BYTES ||
        s->audio_player_buffer_len == sizeof(s->audio_player_buffer)) {
        xp3200s_audio_flush_cb(s);
        timer_del(s->audio_flush_timer);
    } else {
        timer_mod(s->audio_flush_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 50);
    }
    if (s->trace_io || s->audio_stream_bytes == 1 ||
        (s->audio_stream_bytes != 0 &&
         (s->audio_stream_bytes & 0xffff) == 0)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-audio: compressed stream byte=%" PRIu64
                      " value=0x%02x select=0x%02x\n",
                      s->audio_stream_bytes, value,
                      s->touch_io_regs[XP3200S_TOUCH_SERIAL]);
    }
}

/*
 * Diagnostic: push an MP3 file into the decoder data path from the model side,
 * bypassing the firmware (used to validate the VS1003B plumbing when the
 * emulated U-disk has no music files).
 */
static void xp3200s_audio_inject_cb(void *opaque)
{
    XP3200SState *s = opaque;
    size_t chunk = 256;
    size_t i;

    if (s->audio_inject_buf == NULL ||
        s->audio_inject_pos >= s->audio_inject_len) {
        return;
    }
    if (s->audio_inject_pos + chunk > s->audio_inject_len) {
        chunk = s->audio_inject_len - s->audio_inject_pos;
    }
    for (i = 0; i < chunk; i++) {
        xp3200s_audio_stream_write(s,
            s->audio_inject_buf[s->audio_inject_pos + i]);
    }
    s->audio_inject_pos += chunk;
    if (s->audio_inject_pos >= s->audio_inject_len) {
        info_report("%s audio inject finished (%zu bytes)",
                    xp3200s_model_name(s), s->audio_inject_len);
        return;
    }
    timer_mod(s->audio_inject_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
}

static void xp3200s_audio_data_write(XP3200SState *s, uint8_t value)
{
    if (!xp3200s_audio_stream_selected(s)) {
        return;
    }

    xp3200s_audio_fifo_update(s);
    if (s->audio_fifo_level >= XP3200S_AUDIO_FIFO_CAPACITY) {
        return;
    }
    s->audio_fifo_level++;
    if (s->audio_dreq_bytes != 0) {
        s->audio_dreq_bytes--;
    }
    xp3200s_audio_detect_rate(s, value);
    xp3200s_audio_stream_write(s, value);
}

static uint16_t xp3200s_audio_sci_read_register(XP3200SState *s,
                                                 uint8_t reg)
{
    xp3200s_audio_fifo_update(s);
    switch (reg) {
    case XP3200S_AUDIO_SCI_DECODE_TIME:
        return MIN(s->audio_decoded_ns / NANOSECONDS_PER_SECOND, UINT16_MAX);
    case XP3200S_AUDIO_SCI_AUDATA:
        return s->audio_sci_regs[reg] != 0 ?
               s->audio_sci_regs[reg] : XP3200S_AUDIO_DEFAULT_AUDATA;
    default:
        return s->audio_sci_regs[reg & 0x0f];
    }
}

static void xp3200s_audio_sci_write_register(XP3200SState *s,
                                              uint8_t reg,
                                              uint16_t value)
{
    reg &= 0x0f;
    switch (reg) {
    case XP3200S_AUDIO_SCI_MODE:
        if ((value & XP3200S_AUDIO_SCI_MODE_RESET) != 0) {
            xp3200s_audio_decoder_reset(s);
            value &= ~XP3200S_AUDIO_SCI_MODE_RESET;
        }
        break;
    case XP3200S_AUDIO_SCI_DECODE_TIME:
        s->audio_decoded_ns = (uint64_t)value * NANOSECONDS_PER_SECOND;
        s->audio_decode_remainder = 0;
        break;
    default:
        break;
    }
    s->audio_sci_regs[reg] = value;
    if (s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-audio: SCI write reg=0x%02x value=0x%04x\n",
                      reg, value);
    }
}

static void xp3200s_audio_sci_write(XP3200SState *s, uint8_t value)
{
    uint16_t reg_value;

    if (!xp3200s_audio_control_selected(s)) {
        return;
    }
    switch (s->audio_sci_phase) {
    case 0:
        if (value != XP3200S_AUDIO_SCI_WRITE &&
            value != XP3200S_AUDIO_SCI_READ) {
            return;
        }
        s->audio_sci_command = value;
        s->audio_sci_phase = 1;
        break;
    case 1:
        s->audio_sci_register = value & 0x0f;
        if (s->audio_sci_command == XP3200S_AUDIO_SCI_READ) {
            reg_value = xp3200s_audio_sci_read_register(
                s, s->audio_sci_register);
            s->audio_sci_read_data[0] = reg_value >> 8;
            s->audio_sci_read_data[1] = reg_value;
            s->audio_sci_read_pos = 0;
            s->audio_sci_phase = 4;
            if (s->trace_io) {
                qemu_log_mask(
                    LOG_GUEST_ERROR,
                    "xp3200s-audio: SCI read reg=0x%02x value=0x%04x\n",
                    s->audio_sci_register, reg_value);
            }
        } else {
            s->audio_sci_phase = 2;
        }
        break;
    case 2:
        s->audio_sci_read_data[0] = value;
        s->audio_sci_phase = 3;
        break;
    case 3:
        reg_value = ((uint16_t)s->audio_sci_read_data[0] << 8) | value;
        xp3200s_audio_sci_write_register(
            s, s->audio_sci_register, reg_value);
        s->audio_sci_phase = 4;
        break;
    default:
        /* SCI reads use DATA_OUT dummy writes to clock DATA_IN bytes. */
        break;
    }
}

static uint8_t xp3200s_audio_sci_data_read(XP3200SState *s)
{
    if (!xp3200s_audio_control_selected(s) ||
        s->audio_sci_command != XP3200S_AUDIO_SCI_READ ||
        s->audio_sci_read_pos >= ARRAY_SIZE(s->audio_sci_read_data)) {
        return 0xff;
    }
    return s->audio_sci_read_data[s->audio_sci_read_pos++];
}

static void xp3200s_audio_record(XP3200SState *s, bool is_write,
                                  hwaddr offset, uint64_t value,
                                  unsigned size)
{
    uint8_t seen_mask = is_write ? 2 : 1;
    bool first = (s->audio_seen[offset] & seen_mask) == 0;

    if (first) {
        s->audio_seen[offset] |= seen_mask;
    }
    if (first || s->trace_io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-audio: %s %s pc=0x%08x "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                      "\n",
                      first ? "first" : "trace",
                      is_write ? "write" : "read", xp3200s_current_pc(),
                      XP3200S_AUDIO_IO_BASE + offset, size, size * 2, value);
    }
}

/* ------------------------------------------------------------------ */
/* SD card on the shared SPI block (S1C33L05 MMC, SPI mode)           */
/* ------------------------------------------------------------------ */

static const uint8_t xp3200s_sd_cid[16] = {
    0x03, 0x53, 0x53, 0x44, 0x36, 0x34, 0x4d, 0x00,
    0x00, 0x00, 0x00, 0x12, 0x34, 0x10, 0x06, 0xf5,
};

static uint8_t xp3200s_sd_crc7(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    int i, b;

    for (i = 0; i < len; i++) {
        for (b = 7; b >= 0; b--) {
            int bit = ((data[i] >> b) & 1) ^ ((crc >> 6) & 1);

            crc = (crc << 1) & 0x7f;
            if (bit) {
                crc ^= 0x09;
            }
        }
    }
    return crc;
}

static uint16_t xp3200s_sd_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0;
    int i, b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) :
                                   (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void xp3200s_sd_build_csd(XP3200SState *s, uint8_t *csd)
{
    /* The XP3200 SD driver reverses the 16 received CSD bytes and then
     * extracts C_SIZE/C_SIZE_MULT as little-endian fields, so build the
     * mirrored "B" image first and reverse it at the end. */
    uint8_t b[16] = {
        0xe3, 0x00, 0x00, 0xfc, 0xff, 0x80, 0x03, 0x01,
        0x78, 0x00, 0xf9, 0x05, 0x32, 0x00, 0x26, 0x00,
    };
    uint64_t blocks = s->sd_size / XP3200S_SD_BLOCK_SIZE;
    uint32_t mult = 7;                 /* factor = 4 << 7 = 512 sectors */
    uint32_t factor = 4u << mult;
    uint32_t c_size = (uint32_t)((blocks + factor - 1) / factor);
    int i;

    if (c_size > 0) {
        c_size--;
    }
    if (c_size > 4095) {
        c_size = 4095;
    }
    b[5] = 0x80 | ((mult >> 0) & 1);
    b[6] = (mult >> 1) & 3;
    b[7] = (uint8_t)(((c_size & 3) << 6) | 0x01);
    b[8] = (uint8_t)((c_size >> 2) & 0xff);
    b[9] = (uint8_t)((c_size >> 10) & 3);
    for (i = 0; i < 16; i++) {
        csd[i] = b[15 - i];
    }
    csd[15] = (xp3200s_sd_crc7(csd, 15) << 1) | 1;
}

static void xp3200s_sd_reset_transfer(XP3200SState *s)
{
    s->sd_state = XP3200S_SD_ST_IDLE;
    s->sd_arg_pos = 0;
    s->sd_arg = 0;
    s->sd_pos = 0;
    s->sd_crc_pos = 0;
    s->sd_tail_len = 0;
    s->sd_tail_pos = 0;
    s->sd_app_cmd = false;
    s->sd_multi = false;
    s->sd_resp_delay = false;
}

static bool xp3200s_sd_read_block(XP3200SState *s, uint32_t block)
{
    if (!s->sd_present || s->sd_file == NULL) {
        return false;
    }
    if ((uint64_t)block * XP3200S_SD_BLOCK_SIZE + XP3200S_SD_BLOCK_SIZE >
        s->sd_size) {
        memset(s->sd_buf, 0xff, sizeof(s->sd_buf));
        return true;
    }
    if (fseeko(s->sd_file, (off_t)block * XP3200S_SD_BLOCK_SIZE,
               SEEK_SET) != 0) {
        return false;
    }
    if (fread(s->sd_buf, 1, sizeof(s->sd_buf), s->sd_file) !=
        sizeof(s->sd_buf)) {
        return false;
    }
    s->sd_crc = xp3200s_sd_crc16(s->sd_buf, sizeof(s->sd_buf));
    return true;
}

static bool xp3200s_sd_write_block(XP3200SState *s, uint32_t block)
{
    if (!s->sd_present || s->sd_file == NULL) {
        return false;
    }
    if ((uint64_t)block * XP3200S_SD_BLOCK_SIZE + XP3200S_SD_BLOCK_SIZE >
        s->sd_size) {
        return false;
    }
    if (fseeko(s->sd_file, (off_t)block * XP3200S_SD_BLOCK_SIZE,
               SEEK_SET) != 0) {
        return false;
    }
    if (fwrite(s->sd_buf, 1, sizeof(s->sd_buf), s->sd_file) !=
        sizeof(s->sd_buf)) {
        return false;
    }
    fflush(s->sd_file);
    return true;
}

static void xp3200s_sd_command(XP3200SState *s)
{
    uint32_t arg = s->sd_arg;

    s->sd_tail_len = 0;
    s->sd_tail_pos = 0;
    s->sd_r1 = 0x00;
    s->sd_next = XP3200S_SD_ST_IDLE;
    s->sd_multi = false;
    s->sd_resp_delay = true;

    if (s->sd_app_cmd) {
        s->sd_app_cmd = false;
        s->sd_r1 = (s->sd_cmd == 41) ? 0x00 : 0x04;
        s->sd_state = XP3200S_SD_ST_RESP;
        return;
    }

    switch (s->sd_cmd) {
    case 0:  /* GO_IDLE_STATE */
        s->sd_r1 = 0x01;
        break;
    case 1:  /* SEND_OP_COND (MMC) */
        s->sd_r1 = 0x00;
        break;
    case 8:  /* SEND_IF_COND */
        s->sd_r1 = 0x01;
        s->sd_tail[0] = 0x00;
        s->sd_tail[1] = 0x00;
        s->sd_tail[2] = 0x01;
        s->sd_tail[3] = (uint8_t)arg;
        s->sd_tail_len = 4;
        s->sd_next = XP3200S_SD_ST_TAIL;
        break;
    case 9:  /* SEND_CSD */
        s->sd_r1 = 0x00;
        s->sd_tail[0] = 0xfe;
        xp3200s_sd_build_csd(s, s->sd_tail + 1);
        s->sd_tail_len = 17;
        s->sd_next = XP3200S_SD_ST_TAIL;
        break;
    case 10: /* SEND_CID */
        s->sd_r1 = 0x00;
        s->sd_tail[0] = 0xfe;
        memcpy(s->sd_tail + 1, xp3200s_sd_cid, 16);
        s->sd_tail_len = 17;
        s->sd_next = XP3200S_SD_ST_TAIL;
        break;
    case 12: /* STOP_TRANSMISSION */
        s->sd_r1 = 0x00;
        break;
    case 13: /* SEND_STATUS */
        s->sd_r1 = 0x00;
        s->sd_tail[0] = 0x00;
        s->sd_tail_len = 1;
        s->sd_next = XP3200S_SD_ST_TAIL;
        break;
    case 16: /* SET_BLOCKLEN */
        s->sd_r1 = 0x00;
        break;
    case 17: /* READ_SINGLE_BLOCK */
    case 18: /* READ_MULTIPLE_BLOCK */
        /* SDSC cards take a byte address; the firmware multiplies the
         * sector number by the 512-byte block length before sending. */
        arg /= XP3200S_SD_BLOCK_SIZE;
        if (!xp3200s_sd_read_block(s, arg)) {
            s->sd_r1 = 0x04;
            break;
        }
        s->sd_block = arg;
        s->sd_multi = (s->sd_cmd == 18);
        s->sd_r1 = 0x00;
        s->sd_next = XP3200S_SD_ST_READ_TOKEN;
        break;
    case 24: /* WRITE_BLOCK */
    case 25: /* WRITE_MULTIPLE_BLOCK */
        arg /= XP3200S_SD_BLOCK_SIZE;
        s->sd_block = arg;
        s->sd_multi = (s->sd_cmd == 25);
        s->sd_r1 = 0x00;
        s->sd_next = XP3200S_SD_ST_WAIT_TOKEN;
        break;
    case 55: /* APP_CMD */
        s->sd_app_cmd = true;
        s->sd_r1 = 0x01;
        break;
    case 58: /* READ_OCR */
        s->sd_r1 = 0x00;
        s->sd_tail[0] = 0x00;
        s->sd_tail[1] = 0xff;
        s->sd_tail[2] = 0x80;
        s->sd_tail[3] = 0x00;
        s->sd_tail_len = 4;
        s->sd_next = XP3200S_SD_ST_TAIL;
        break;
    case 59: /* CRC_ON_OFF */
        s->sd_r1 = 0x00;
        break;
    default:
        s->sd_r1 = 0x04;
        break;
    }
    s->sd_state = XP3200S_SD_ST_RESP;
}

static uint8_t xp3200s_sd_xfer(XP3200SState *s, uint8_t tx)
{
    uint8_t value;

    switch (s->sd_state) {
    case XP3200S_SD_ST_IDLE:
        if ((tx & 0xc0) == 0x40) {
            s->sd_cmd = tx & 0x3f;
            s->sd_arg = 0;
            s->sd_arg_pos = 0;
            s->sd_state = XP3200S_SD_ST_ARG;
        }
        return 0xff;
    case XP3200S_SD_ST_ARG:
        if (s->sd_arg_pos < 4) {
            s->sd_arg = (s->sd_arg << 8) | tx;
        }
        /* The 5th byte is the CRC; the R1 response starts on the next
         * byte, i.e. the dummy clock the host sends to read it. */
        if (++s->sd_arg_pos >= 5) {
            xp3200s_sd_command(s);
        }
        return 0xff;
    case XP3200S_SD_ST_RESP:
        if (s->sd_resp_delay) {
            s->sd_resp_delay = false;
            return 0xff;
        }
        value = s->sd_r1;
        s->sd_state = s->sd_next;
        if (s->sd_state == XP3200S_SD_ST_TAIL) {
            s->sd_tail_pos = 0;
        }
        return value;
    case XP3200S_SD_ST_TAIL:
        if (s->sd_tail_pos < s->sd_tail_len) {
            return s->sd_tail[s->sd_tail_pos++];
        }
        s->sd_state = XP3200S_SD_ST_IDLE;
        return 0xff;
    case XP3200S_SD_ST_READ_TOKEN:
        s->sd_state = XP3200S_SD_ST_READ_DATA;
        s->sd_pos = 0;
        return 0xfe;
    case XP3200S_SD_ST_READ_DATA:
        value = s->sd_buf[s->sd_pos++];
        if (s->sd_pos >= XP3200S_SD_BLOCK_SIZE) {
            s->sd_state = XP3200S_SD_ST_READ_CRC;
            s->sd_crc_pos = 0;
        }
        return value;
    case XP3200S_SD_ST_READ_CRC:
        value = (s->sd_crc_pos == 0) ? (s->sd_crc >> 8) : (uint8_t)s->sd_crc;
        s->sd_crc_pos++;
        if (s->sd_crc_pos >= 2) {
            if (s->sd_multi) {
                s->sd_block++;
                if (!xp3200s_sd_read_block(s, s->sd_block)) {
                    s->sd_state = XP3200S_SD_ST_IDLE;
                    return value;
                }
                s->sd_state = XP3200S_SD_ST_READ_TOKEN;
            } else {
                s->sd_state = XP3200S_SD_ST_IDLE;
            }
        }
        return value;
    case XP3200S_SD_ST_WAIT_TOKEN:
        if (tx == 0xfe) {
            s->sd_state = XP3200S_SD_ST_WRITE_DATA;
            s->sd_pos = 0;
        } else if (tx == 0xfd) {
            s->sd_state = XP3200S_SD_ST_IDLE;
        }
        return 0xff;
    case XP3200S_SD_ST_WRITE_DATA:
        s->sd_buf[s->sd_pos++] = tx;
        if (s->sd_pos >= XP3200S_SD_BLOCK_SIZE) {
            s->sd_state = XP3200S_SD_ST_WRITE_CRC;
            s->sd_crc_pos = 0;
        }
        return 0xff;
    case XP3200S_SD_ST_WRITE_CRC:
        if (++s->sd_crc_pos >= 2) {
            xp3200s_sd_write_block(s, s->sd_block);
            s->sd_state = XP3200S_SD_ST_WRITE_RESP;
        }
        return 0xff;
    case XP3200S_SD_ST_WRITE_RESP:
        if (s->sd_multi) {
            s->sd_block++;
            s->sd_state = XP3200S_SD_ST_WAIT_TOKEN;
        } else {
            s->sd_state = XP3200S_SD_ST_IDLE;
        }
        return 0x05;
    default:
        s->sd_state = XP3200S_SD_ST_IDLE;
        return 0xff;
    }
}

static bool xp3200s_spi_selected(XP3200SState *s)
{
    uint32_t pc = xp3200s_current_pc();

    (void)s;
    /* The SD driver lives in 0x02192xxx-0x02194xxx; the VS1003 SCI path
     * uses the same registers from 0x0216Fxxx.  Distinguish by caller. */
    return (pc >= 0x02190000u && pc < 0x02198000u) ||
           (pc >= 0x00c00000u && pc < 0x00c80000u);
}

static uint64_t xp3200s_spi_read(XP3200SState *s, hwaddr offset)
{
    uint16_t value = 0;

    if (!s->sd_cs_prev) {
        xp3200s_sd_reset_transfer(s);
        s->sd_cs_prev = true;
    }
    switch (offset & ~1u) {
    case XP3200S_SPI_SPIRD:
        value = s->spi_rx;
        s->spi_rx_full = false;
        break;
    case XP3200S_SPI_CTRL:
        value = s->spi_regs[2];
        break;
    case XP3200S_SPI_WAIT:
        value = s->spi_regs[4];
        break;
    case XP3200S_SPI_STATUS:
        value = XP3200S_SPI_STATUS_TDEF;
        if (s->spi_rx_full) {
            value |= XP3200S_SPI_STATUS_RDFF;
        }
        break;
    case XP3200S_SPI_INTCTL:
        value = s->spi_regs[6];
        break;
    default:
        break;
    }
    return value;
}

static void xp3200s_spi_write(XP3200SState *s, hwaddr offset, uint64_t value)
{
    if (!s->sd_cs_prev) {
        xp3200s_sd_reset_transfer(s);
        s->sd_cs_prev = true;
    }
    switch (offset & ~1u) {
    case XP3200S_SPI_SPITD:
        s->spi_rx = xp3200s_sd_xfer(s, value & 0xff);
        s->spi_rx_full = true;
        break;
    case XP3200S_SPI_CTRL:
        s->spi_regs[2] = value;
        break;
    case 0x06:
        s->spi_regs[3] = value;
        break;
    case XP3200S_SPI_WAIT:
        s->spi_regs[4] = value;
        break;
    case XP3200S_SPI_INTCTL:
        s->spi_regs[6] = value;
        break;
    default:
        break;
    }
}

static void xp3200s_sd_init(XP3200SState *s, const char *path)
{
    s->sd_file = NULL;
    s->sd_present = false;
    s->sd_size = 0;
    s->sd_cs_prev = false;
    xp3200s_sd_reset_transfer(s);
    if (path == NULL || path[0] == 0) {
        return;
    }
    s->sd_file = fopen(path, "r+b");
    if (s->sd_file == NULL) {
        s->sd_file = fopen(path, "rb");
    }
    if (s->sd_file == NULL) {
        info_report("%s SD: cannot open %s", xp3200s_model_name(s), path);
        return;
    }
    if (fseeko(s->sd_file, 0, SEEK_END) == 0) {
        off_t len = ftello(s->sd_file);

        if (len > 0) {
            s->sd_size = len;
            s->sd_present = true;
        }
    }
    info_report("%s SD: image %s, %" PRIu64 " MiB%s",
                xp3200s_model_name(s), path, s->sd_size >> 20,
                s->sd_present ? "" : " (unusable)");
}

static uint64_t xp3200s_audio_io_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (offset + size > s->audio_io_size) {
        return 0;
    }
    if (offset < XP3200S_AUDIO_IO_SIZE && xp3200s_spi_selected(s)) {
        return xp3200s_spi_read(s, offset);
    }
    s->sd_cs_prev = false;
    if (offset < ARRAY_SIZE(s->dbg_audio_reads)) {
        s->dbg_audio_reads[offset]++;
    }
    if (s->trace_audio && s->audio_trace_count < 400) {
        s->audio_trace_count++;
        fprintf(stderr, "AUDIO r off=%03x size=%u pc=%08x\n",
                (unsigned)offset, size, xp3200s_current_pc());
    }
    if (offset <= XP3200S_AUDIO_STATUS &&
        offset + size > XP3200S_AUDIO_STATUS) {
        /* Parallel bus ready is separate from the decoder DREQ on P0.3. */
        s->audio_regs[XP3200S_AUDIO_STATUS] |=
            XP3200S_AUDIO_READ_READY | XP3200S_AUDIO_WRITE_READY;
    }
    if (offset <= XP3200S_AUDIO_DATA_IN &&
        offset + size > XP3200S_AUDIO_DATA_IN) {
        if (s->audio_force_data_in <= 0xffff) {
            s->audio_regs[XP3200S_AUDIO_DATA_IN] =
                s->audio_force_data_in & 0xff;
            s->audio_regs[XP3200S_AUDIO_DATA_IN + 1] =
                (s->audio_force_data_in >> 8) & 0xff;
        } else {
            s->audio_regs[XP3200S_AUDIO_DATA_IN] =
                xp3200s_audio_sci_data_read(s);
        }
    }
    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->audio_regs[offset + i] << (i * 8);
    }
    xp3200s_audio_record(s, false, offset, value, size);
    return value;
}

static void xp3200s_audio_io_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    XP3200SState *s = opaque;
    unsigned i;

    if (offset + size > s->audio_io_size) {
        return;
    }
    if (offset < XP3200S_AUDIO_IO_SIZE && xp3200s_spi_selected(s)) {
        xp3200s_spi_write(s, offset, value);
        return;
    }
    s->sd_cs_prev = false;
    if (offset < ARRAY_SIZE(s->dbg_audio_writes)) {
        s->dbg_audio_writes[offset]++;
    }
    xp3200s_trace_io_all(s, "aud", offset, value, size);
    if (s->trace_audio && s->audio_trace_count < 400) {
        s->audio_trace_count++;
        fprintf(stderr, "AUDIO w off=%03x size=%u val=%02x pc=%08x\n",
                (unsigned)offset, size, (unsigned)value,
                xp3200s_current_pc());
    }
    if (s->trace_audio && offset == XP3200S_AUDIO_DATA_OUT &&
        s->audio_data_trace_count < 3000) {
        s->audio_data_trace_count++;
        uint32_t caller = 0;

        if (s->cpu != NULL) {
            cpu_physical_memory_read(S1C33_CPU(s->cpu)->env.sp, &caller,
                                     sizeof(caller));
        }
        fprintf(stderr, "AUDDATA n=%u val=%04x size=%u pc=%08x ret=%08x "
                "ctrl=%04x\n",
                s->audio_data_trace_count, (unsigned)(value & 0xffff),
                size, xp3200s_current_pc(), caller,
                (unsigned)(s->audio_regs[XP3200S_AUDIO_DATA_OUT - 2] |
                           (s->audio_regs[XP3200S_AUDIO_DATA_OUT - 1] << 8)));
    }
    for (i = 0; i < size; i++) {
        hwaddr byte_offset = offset + i;
        uint8_t byte = value >> (i * 8);

        if (byte_offset != XP3200S_AUDIO_STATUS) {
            s->audio_regs[byte_offset] = byte;
        }
    }
    if (s->is_9288 && offset == XP3200_EXPANSION_READY &&
        size >= 2 && (value & 0xffff) == 4) {
        /*
         * The 9288 programs the external board controller at 0x003a0200,
         * then writes command 4 here and polls until the controller reports
         * ready as value 8.  This window is absent from the 9288S firmware.
         */
        s->audio_regs[XP3200_EXPANSION_READY] = 8;
        s->audio_regs[XP3200_EXPANSION_READY + 1] = 0;
    }
    if (offset == XP3200S_AUDIO_DATA_OUT) {
        if (xp3200s_audio_stream_selected(s)) {
            xp3200s_audio_data_write(s, value & 0xff);
        } else {
            xp3200s_audio_sci_write(s, value & 0xff);
        }
    }
    xp3200s_audio_record(s, true, offset, value, size);
}

static const MemoryRegionOps xp3200s_audio_io_ops = {
    .read = xp3200s_audio_io_read,
    .write = xp3200s_audio_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void xp3200s_audio_init(XP3200SState *s, const char *path,
                                const char *player_path,
                                const char *inject_path,
                                uint32_t inject_ms)
{
    g_autoptr(GError) gerr = NULL;
    const char *argv[] = {
        player_path,
        "-nodisp",
        "-autoexit",
        "-loglevel", "warning",
        "-f", "mp3",
        "-i", "pipe:0",
        NULL,
    };

    memset(s->audio_regs, 0, sizeof(s->audio_regs));
    memset(s->audio_seen, 0, sizeof(s->audio_seen));
    s->audio_regs[XP3200S_AUDIO_STATUS] =
        XP3200S_AUDIO_READ_READY | XP3200S_AUDIO_WRITE_READY;
    memset(s->audio_sci_regs, 0, sizeof(s->audio_sci_regs));
    s->audio_sci_regs[XP3200S_AUDIO_SCI_MODE] =
        XP3200S_AUDIO_SCI_MODE_SDINEW;
    s->audio_sci_regs[XP3200S_AUDIO_SCI_AUDATA] =
        XP3200S_AUDIO_DEFAULT_AUDATA;
    s->audio_sci_regs[XP3200S_AUDIO_SCI_VOL] = 0xffff;
    xp3200s_audio_sci_transaction_reset(s);
    s->audio_stream_path =
        g_strdup(path != NULL && path[0] != 0 ? path : NULL);
    s->audio_player_path =
        g_strdup(player_path != NULL && player_path[0] != 0 ?
                 player_path : NULL);
    s->audio_player_fd = -1;
    s->audio_stream_bytes = 0;
    s->audio_flush_bytes = 0;
    s->audio_player_buffer_len = 0;
    xp3200s_audio_decoder_reset(s);

    if (s->audio_stream_path == NULL && s->audio_player_path == NULL) {
        info_report("%s audio stream output disabled",
                    xp3200s_model_name(s));
        return;
    }
    s->audio_inject_path =
        g_strdup(inject_path != NULL && inject_path[0] != 0 ? inject_path
                                                            : NULL);
    if (s->audio_inject_path != NULL) {
        g_autoptr(GError) ierr = NULL;

        if (!g_file_get_contents(s->audio_inject_path,
                                 (gchar **)&s->audio_inject_buf,
                                 &s->audio_inject_len, &ierr)) {
            warn_report("%s audio inject: %s", xp3200s_model_name(s),
                        ierr->message);
            s->audio_inject_buf = NULL;
            s->audio_inject_len = 0;
        } else {
            s->audio_inject_pos = 0;
            s->audio_inject_timer =
                timer_new_ms(QEMU_CLOCK_VIRTUAL,
                             xp3200s_audio_inject_cb, s);
            timer_mod(s->audio_inject_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      (inject_ms != 0 ? inject_ms : 65000));
            info_report("%s audio inject: %s (%zu bytes)",
                        xp3200s_model_name(s), s->audio_inject_path,
                        s->audio_inject_len);
        }
    }
    if (s->audio_stream_path != NULL) {
        s->audio_stream = fopen(s->audio_stream_path, "wb");
        if (s->audio_stream == NULL) {
            warn_report("could not open %s audio stream '%s': %s",
                        xp3200s_model_name(s), s->audio_stream_path,
                        strerror(errno));
        } else {
            info_report("%s audio stream: %s", xp3200s_model_name(s),
                        s->audio_stream_path);
        }
    }
    if (s->audio_player_path != NULL &&
        !g_spawn_async_with_pipes(
            NULL, (char **)argv, NULL,
            G_SPAWN_CLOEXEC_PIPES | G_SPAWN_DO_NOT_REAP_CHILD |
            G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, &s->audio_player_pid, &s->audio_player_fd,
            NULL, NULL, &gerr)) {
        warn_report("could not start %s audio player '%s': %s",
                    xp3200s_model_name(s), s->audio_player_path,
                    gerr->message);
        s->audio_player_pid = 0;
        s->audio_player_fd = -1;
    } else if (s->audio_player_path != NULL) {
        info_report("%s audio player: %s", xp3200s_model_name(s),
                    s->audio_player_path);
    }
    if (s->audio_stream != NULL || s->audio_player_fd >= 0) {
        s->audio_flush_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_audio_flush_cb, s);
    }
}

static void xp3200s_nand_storage_init(XP3200SState *s, const char *path)
{
    GError *gerr = NULL;
    char *contents = NULL;
    gsize length = 0;
    bool exists;

    s->nand_storage_size = XP3200S_NAND_STORAGE_SIZE;
    s->nand_storage = g_malloc(s->nand_storage_size);
    memset(s->nand_storage, 0xff, s->nand_storage_size);
    memset(s->nand_dirty_blocks, 0, sizeof(s->nand_dirty_blocks));
    s->nand_image_path = g_strdup(path != NULL && path[0] != 0 ? path : NULL);

    if (s->nand_image_path == NULL) {
        info_report("%s NAND: volatile blank 256 MiB device",
                    xp3200s_model_name(s));
        return;
    }
    exists = g_file_test(s->nand_image_path, G_FILE_TEST_EXISTS);
    if (!exists) {
        info_report("%s NAND: new blank image %s (%zu raw bytes)",
                    xp3200s_model_name(s), s->nand_image_path,
                    s->nand_storage_size);
    } else if (!g_file_get_contents(s->nand_image_path, &contents, &length,
                                    &gerr)) {
        error_report("could not load %s NAND image '%s': %s",
                     xp3200s_model_name(s), s->nand_image_path,
                     gerr->message);
        g_error_free(gerr);
        exit(1);
    }
    if (exists && length != s->nand_storage_size) {
        error_report("%s NAND image '%s' has %zu bytes; expected %zu",
                     xp3200s_model_name(s), s->nand_image_path, (size_t)length,
                     s->nand_storage_size);
        g_free(contents);
        exit(1);
    }

    if (exists) {
        memcpy(s->nand_storage, contents, length);
        g_free(contents);
        info_report("%s NAND: loaded %s (%zu raw bytes)",
                    xp3200s_model_name(s), s->nand_image_path,
                    s->nand_storage_size);
    }

    s->nand_file = fopen(s->nand_image_path, exists ? "r+b" : "w+b");
    if (s->nand_file == NULL) {
        error_report("could not open %s NAND image '%s' for persistence: %s",
                     xp3200s_model_name(s), s->nand_image_path,
                     strerror(errno));
        exit(1);
    }
    if (!exists &&
        fwrite(s->nand_storage, 1, s->nand_storage_size, s->nand_file) !=
        s->nand_storage_size) {
        error_report("could not initialize %s NAND image '%s': %s",
                     xp3200s_model_name(s), s->nand_image_path,
                     strerror(errno));
        exit(1);
    }
    fflush(s->nand_file);
    if (!exists) {
        qemu_fdatasync(fileno(s->nand_file));
    }
    s->nand_flush_timer =
        timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_nand_flush_cb, s);
}

static void xp3200s_nand_storage_save(XP3200SState *s)
{
    unsigned block;
    unsigned flushed = 0;
    bool failed = false;

    if (!s->nand_dirty || s->nand_file == NULL) {
        return;
    }

    for (block = 0; block < XP3200S_NAND_BLOCKS; block++) {
        unsigned byte = block / 8;
        uint8_t bit = 1u << (block % 8);
        size_t offset;

        if ((s->nand_dirty_blocks[byte] & bit) == 0) {
            continue;
        }
        offset = (size_t)block * XP3200S_NAND_RAW_BLOCK_SIZE;
        if (fseek(s->nand_file, offset, SEEK_SET) != 0 ||
            fwrite(s->nand_storage + offset, 1,
                   XP3200S_NAND_RAW_BLOCK_SIZE, s->nand_file) !=
            XP3200S_NAND_RAW_BLOCK_SIZE) {
            error_report("could not incrementally save %s NAND block %u "
                         "to '%s': %s",
                         xp3200s_model_name(s), block,
                         s->nand_image_path, strerror(errno));
            failed = true;
            break;
        }
        s->nand_dirty_blocks[byte] &= ~bit;
        flushed++;
    }

    if (flushed != 0) {
        fflush(s->nand_file);
        if (qemu_fdatasync(fileno(s->nand_file)) != 0) {
            error_report("could not synchronize %s NAND image '%s': %s",
                         xp3200s_model_name(s), s->nand_image_path,
                         strerror(errno));
            failed = true;
        }
    }
    if (!failed) {
        s->nand_dirty = false;
    }
    if (flushed != 0 && (s->trace_io || failed)) {
        info_report("%s NAND: incrementally saved %u raw block%s to %s",
                    xp3200s_model_name(s), flushed,
                    flushed == 1 ? "" : "s", s->nand_image_path);
    }
}

static void xp3200s_nand_flush_cb(void *opaque)
{
    xp3200s_nand_storage_save(opaque);
}

static void xp3200s_nand_mark_dirty(XP3200SState *s, unsigned block)
{
    if (block >= XP3200S_NAND_BLOCKS) {
        return;
    }
    s->nand_dirty_blocks[block / 8] |= 1u << (block % 8);
    s->nand_dirty = true;
    if (s->nand_flush_timer != NULL) {
        timer_mod(s->nand_flush_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  XP3200S_NAND_FLUSH_DELAY_MS);
    }
}

typedef struct XP3200SFatBoot {
    XP3200SState *machine;
    uint16_t *block_map;
    uint64_t fat_offset;
    uint64_t root_offset;
    uint64_t data_offset;
    uint32_t root_entries;
    uint32_t cluster_size;
    uint32_t cluster_count;
} XP3200SFatBoot;

static unsigned xp3200s_nand_tag_parity(unsigned value)
{
    unsigned parity = 0;

    while (value != 0) {
        parity ^= value & 1;
        value >>= 1;
    }
    return parity;
}

static bool xp3200s_nand_block_tag(XP3200SState *s,
                                    unsigned physical_block,
                                    uint16_t *result)
{
    uint16_t tags[8];
    unsigned tag_count = 0;
    unsigned best_count = 0;
    unsigned best = 0;
    unsigned page;
    unsigned sector;
    unsigned i;
    unsigned j;

    for (page = 0; page < 2; page++) {
        size_t offset = (size_t)physical_block *
                        XP3200S_NAND_RAW_BLOCK_SIZE +
                        page * XP3200S_NAND_RAW_PAGE_SIZE +
                        XP3200S_NAND_PAGE_SIZE;
        const uint8_t *oob = s->nand_storage + offset;

        for (sector = 0; sector < 4; sector++) {
            const uint8_t *slot = oob + sector * 16;
            uint16_t first = lduw_le_p(slot + 6);
            uint16_t second = lduw_le_p(slot + 11);

            if (slot[1] == 0 && first != UINT16_MAX && first == second) {
                tags[tag_count++] = first;
            }
        }
    }

    for (i = 0; i < tag_count; i++) {
        unsigned count = 0;

        for (j = 0; j < tag_count; j++) {
            count += tags[j] == tags[i];
        }
        if (count > best_count) {
            best = i;
            best_count = count;
        }
    }
    if (best_count == 0) {
        return false;
    }

    *result = tags[best];
    return true;
}

static uint16_t *xp3200s_nand_build_block_map(XP3200SState *s)
{
    uint16_t *map = g_new(uint16_t, XP3200S_NAND_LOGICAL_BLOCKS);
    unsigned physical;
    unsigned logical;

    for (logical = 0; logical < XP3200S_NAND_LOGICAL_BLOCKS; logical++) {
        map[logical] = UINT16_MAX;
    }

    for (physical = XP3200S_NAND_RESERVED_BLOCKS;
         physical < XP3200S_NAND_BLOCKS; physical++) {
        uint16_t tag;

        if (!xp3200s_nand_block_tag(s, physical, &tag)) {
            continue;
        }
        logical = tag >> 1;
        if (logical >= XP3200S_NAND_LOGICAL_BLOCKS ||
            (tag & 1) != xp3200s_nand_tag_parity(logical)) {
            continue;
        }

        /*
         * The FTL appends replacement blocks. Scanning in physical order
         * therefore selects the newest copy when a logical block is repeated.
         */
        map[logical] = physical;
    }
    return map;
}

static bool xp3200s_nand_read_logical(XP3200SState *s,
                                       const uint16_t *map,
                                       uint64_t offset,
                                       void *buffer,
                                       size_t length)
{
    uint8_t *out = buffer;
    uint64_t flat_size = (uint64_t)XP3200S_NAND_LOGICAL_BLOCKS *
                         XP3200S_NAND_BLOCK_DATA_SIZE;

    if (offset > flat_size || length > flat_size - offset) {
        return false;
    }

    while (length != 0) {
        unsigned logical = offset / XP3200S_NAND_BLOCK_DATA_SIZE;
        size_t block_offset = offset % XP3200S_NAND_BLOCK_DATA_SIZE;
        unsigned page = block_offset / XP3200S_NAND_PAGE_SIZE;
        size_t page_offset = block_offset % XP3200S_NAND_PAGE_SIZE;
        size_t chunk = MIN(length,
                           (size_t)XP3200S_NAND_PAGE_SIZE - page_offset);
        uint16_t physical = map[logical];
        size_t raw_offset;

        if (physical == UINT16_MAX) {
            return false;
        }
        raw_offset = (size_t)physical * XP3200S_NAND_RAW_BLOCK_SIZE +
                     page * XP3200S_NAND_RAW_PAGE_SIZE + page_offset;
        memcpy(out, s->nand_storage + raw_offset, chunk);
        out += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

static bool xp3200s_fat_boot_init(XP3200SFatBoot *fat,
                                   XP3200SState *s,
                                   uint16_t *map)
{
    uint8_t mbr[XP3200S_FAT_SECTOR_SIZE];
    uint8_t boot[XP3200S_FAT_SECTOR_SIZE];
    uint32_t partition_lba;
    uint32_t total_sectors;
    uint32_t reserved_sectors;
    uint32_t fat_sectors;
    uint32_t root_sectors;
    uint32_t data_sectors;
    uint8_t sectors_per_cluster;
    uint8_t number_of_fats;

    if (!xp3200s_nand_read_logical(s, map, 0, mbr, sizeof(mbr)) ||
        lduw_le_p(mbr + 510) != 0xaa55) {
        return false;
    }
    partition_lba = ldl_le_p(mbr + 446 + 8);
    if (!xp3200s_nand_read_logical(
            s, map, (uint64_t)partition_lba * XP3200S_FAT_SECTOR_SIZE,
            boot, sizeof(boot)) ||
        lduw_le_p(boot + 510) != 0xaa55 ||
        lduw_le_p(boot + 11) != XP3200S_FAT_SECTOR_SIZE) {
        return false;
    }

    sectors_per_cluster = boot[13];
    reserved_sectors = lduw_le_p(boot + 14);
    number_of_fats = boot[16];
    fat->root_entries = lduw_le_p(boot + 17);
    total_sectors = lduw_le_p(boot + 19);
    if (total_sectors == 0) {
        total_sectors = ldl_le_p(boot + 32);
    }
    fat_sectors = lduw_le_p(boot + 22);
    if (sectors_per_cluster == 0 || reserved_sectors == 0 ||
        number_of_fats == 0 || fat->root_entries == 0 ||
        total_sectors == 0 || fat_sectors == 0) {
        return false;
    }

    root_sectors =
        (fat->root_entries * 32 + XP3200S_FAT_SECTOR_SIZE - 1) /
        XP3200S_FAT_SECTOR_SIZE;
    if (total_sectors <= reserved_sectors +
                         number_of_fats * fat_sectors + root_sectors) {
        return false;
    }
    data_sectors = total_sectors - reserved_sectors -
                   number_of_fats * fat_sectors - root_sectors;

    fat->machine = s;
    fat->block_map = map;
    fat->fat_offset =
        ((uint64_t)partition_lba + reserved_sectors) *
        XP3200S_FAT_SECTOR_SIZE;
    fat->root_offset =
        fat->fat_offset +
        (uint64_t)number_of_fats * fat_sectors * XP3200S_FAT_SECTOR_SIZE;
    fat->data_offset =
        fat->root_offset + (uint64_t)root_sectors * XP3200S_FAT_SECTOR_SIZE;
    fat->cluster_size =
        sectors_per_cluster * XP3200S_FAT_SECTOR_SIZE;
    fat->cluster_count = data_sectors / sectors_per_cluster;
    return fat->cluster_count != 0;
}

static uint16_t xp3200s_fat_next_cluster(XP3200SFatBoot *fat,
                                          uint16_t cluster)
{
    uint8_t entry[2];

    if (!xp3200s_nand_read_logical(
            fat->machine, fat->block_map,
            fat->fat_offset + (uint64_t)cluster * sizeof(entry),
            entry, sizeof(entry))) {
        return UINT16_MAX;
    }
    return lduw_le_p(entry);
}

static bool xp3200s_fat_kernel_name(const uint8_t *entry)
{
    static const char short_name[11] = "KERNEL  BIN";
    unsigned i;

    for (i = 0; i < sizeof(short_name); i++) {
        if (g_ascii_toupper(entry[i]) != short_name[i]) {
            return false;
        }
    }
    return true;
}

static bool xp3200s_fat_find_kernel(XP3200SFatBoot *fat,
                                     bool root,
                                     uint16_t first_cluster,
                                     unsigned depth,
                                     uint16_t *kernel_cluster,
                                     uint32_t *kernel_size)
{
    uint16_t cluster = first_cluster;
    uint32_t chain_length = 0;

    if (depth > XP3200S_FAT_MAX_DEPTH) {
        return false;
    }

    do {
        uint64_t offset;
        uint32_t entries;
        uint32_t i;

        if (root) {
            offset = fat->root_offset;
            entries = fat->root_entries;
        } else {
            if (cluster < 2 ||
                cluster >= fat->cluster_count + 2 ||
                chain_length++ >= fat->cluster_count) {
                return false;
            }
            offset = fat->data_offset +
                     (uint64_t)(cluster - 2) * fat->cluster_size;
            entries = fat->cluster_size / 32;
        }

        for (i = 0; i < entries; i++) {
            uint8_t entry[32];
            uint8_t attributes;
            uint16_t entry_cluster;

            if (!xp3200s_nand_read_logical(
                    fat->machine, fat->block_map, offset + i * sizeof(entry),
                    entry, sizeof(entry))) {
                return false;
            }
            if (entry[0] == 0) {
                return false;
            }
            if (entry[0] == 0xe5) {
                continue;
            }

            attributes = entry[11];
            if (attributes == 0x0f || (attributes & 0x08) != 0) {
                continue;
            }
            entry_cluster = lduw_le_p(entry + 26);

            if ((attributes & 0x10) == 0 &&
                xp3200s_fat_kernel_name(entry)) {
                *kernel_cluster = entry_cluster;
                *kernel_size = ldl_le_p(entry + 28);
                return *kernel_cluster >= 2 && *kernel_size != 0;
            }
            if ((attributes & 0x10) != 0 && entry[0] != '.' &&
                xp3200s_fat_find_kernel(
                    fat, false, entry_cluster, depth + 1,
                    kernel_cluster, kernel_size)) {
                return true;
            }
        }

        if (root) {
            break;
        }
        cluster = xp3200s_fat_next_cluster(fat, cluster);
    } while (cluster < XP3200S_FAT_CLUSTER_END);

    return false;
}

static uint8_t *xp3200s_fat_read_file(XP3200SFatBoot *fat,
                                       uint16_t cluster,
                                       uint32_t size)
{
    uint8_t *data = g_malloc(size);
    uint8_t *out = data;
    uint32_t remaining = size;
    uint32_t chain_length = 0;

    while (remaining != 0) {
        size_t chunk;
        uint64_t offset;

        if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
            chain_length++ >= fat->cluster_count) {
            g_free(data);
            return NULL;
        }
        chunk = MIN(remaining, fat->cluster_size);
        offset = fat->data_offset +
                 (uint64_t)(cluster - 2) * fat->cluster_size;
        if (!xp3200s_nand_read_logical(
                fat->machine, fat->block_map, offset, out, chunk)) {
            g_free(data);
            return NULL;
        }
        out += chunk;
        remaining -= chunk;
        if (remaining != 0) {
            cluster = xp3200s_fat_next_cluster(fat, cluster);
            if (cluster >= XP3200S_FAT_CLUSTER_END) {
                g_free(data);
                return NULL;
            }
        }
    }
    return data;
}

static uint8_t *xp3200s_nand_extract_kernel(XP3200SState *s,
                                             gsize *length,
                                             uint64_t ram_size)
{
    g_autofree uint16_t *map = NULL;
    XP3200SFatBoot fat = { 0 };
    uint16_t cluster;
    uint32_t size;
    uint8_t *data;

    if (s->nand_image_path == NULL) {
        return NULL;
    }
    map = xp3200s_nand_build_block_map(s);
    if (!xp3200s_fat_boot_init(&fat, s, map) ||
        !xp3200s_fat_find_kernel(
            &fat, true, 0, 0, &cluster, &size)) {
        return NULL;
    }
    if (size < XP3200S_KERNEL_HEADER ||
        size - XP3200S_KERNEL_HEADER > ram_size) {
        return NULL;
    }

    data = xp3200s_fat_read_file(&fat, cluster, size);
    if (data == NULL) {
        return NULL;
    }
    *length = size;
    info_report("%s boot: loaded kernel.bin from NAND FAT16 "
                "(cluster=%u size=%u)", xp3200s_model_name(s),
                cluster, size);
    return data;
}

static void xp3200s_touch_io_reset(XP3200SState *s)
{
    memset(s->touch_io_regs, 0, sizeof(s->touch_io_regs));
    s->touch_io_regs[XP3200_KEY_ROW_SELECT] = 0xff;
    s->touch_io_regs[XP3200S_NAND_CE_SELECT] = 0x80;
    s->touch_io_regs[XP3200S_NAND_ECC_READY] = 0;
    memset(&s->touch_io_regs[XP3200S_NAND_ECC_BASE], 0xff,
           XP3200S_NAND_ECC_SIZE);
    s->touch_serial_logged_channels = 0;
    xp3200s_touch_serial_reset(s);
    xp3200s_nand_reset(s);
}

static void xp3200s_nand_reset(XP3200SState *s)
{
    s->nand_phase = XP3200S_NAND_IDLE;
    s->nand_command = 0xff;
    s->nand_address_count = 0;
    s->nand_ctrl1 = 0;
    s->nand_ctrl5 = 0;
    s->nand_id_index = 0;
    s->nand_ecc_count = 0;
    s->nand_page = 0;
    s->nand_column = 0;
    memset(s->nand_page_cache, 0xff, sizeof(s->nand_page_cache));
    s->nand_program_address_cycles = 0;
    s->nand_page_cache_valid = false;
    s->nand_program_page_valid = false;
    s->nand_copyback_program = false;
    s->nand_data_reads = 0;
    s->nand_data_writes = 0;
}

/*
 * K9F1G08U0D address cycles (datasheet p.8, Figure "Address Cycle"):
 *   1st  A0..A7                       column low
 *   2nd  A8..A11 (+ A12..A15 low)     column high, upper nibble must be 0
 *   3rd  A12..A19                     row low
 *   4th  A20..A27                     row high
 * Page read / page program therefore use FOUR address cycles and block erase
 * uses the two row cycles only.  There is no 0xFF sentinel byte: the boot
 * program never sends a fifth cycle, so decoding one only picked up stale
 * register content (and, before the CLE/ALE fix, the first data byte).
 */
static void xp3200s_nand_decode_page_address(XP3200SState *s)
{
    s->nand_column = (s->nand_address[0] |
                      ((uint16_t)(s->nand_address[1] & 0x0f) << 8)) & 0x0fff;
    s->nand_page = (uint32_t)s->nand_address[2] |
                   ((uint32_t)s->nand_address[3] << 8);
    if (s->nand_linear) {
        s->nand_page &= 0x1fff;
    }
}

static bool xp3200s_nand_log_sample(XP3200SState *s, uint64_t count)
{
    return s->trace_io || count <= 16 || (count & 0xfff) == 0;
}

static void xp3200s_nand_load_page_cache(XP3200SState *s)
{
    if (s->nand_page < XP3200S_NAND_PAGE_COUNT) {
        size_t offset = (size_t)s->nand_page *
                        XP3200S_NAND_RAW_PAGE_SIZE;

        memcpy(s->nand_page_cache, s->nand_storage + offset,
               sizeof(s->nand_page_cache));
    } else {
        memset(s->nand_page_cache, 0xff, sizeof(s->nand_page_cache));
    }
    s->nand_page_cache_valid = true;
}

static void xp3200s_nand_commit_page_cache(XP3200SState *s)
{
    size_t offset;
    unsigned i;
    bool changed = false;

    if (!s->nand_page_cache_valid || !s->nand_program_page_valid ||
        s->nand_page >= XP3200S_NAND_PAGE_COUNT) {
        return;
    }

    offset = (size_t)s->nand_page * XP3200S_NAND_RAW_PAGE_SIZE;
    for (i = 0; i < XP3200S_NAND_RAW_PAGE_SIZE; i++) {
        uint8_t programmed = s->nand_storage[offset + i] &
                             s->nand_page_cache[i];

        if (programmed != s->nand_storage[offset + i]) {
            s->nand_storage[offset + i] = programmed;
            changed = true;
        }
    }
    if (changed) {
        xp3200s_nand_mark_dirty(
            s, s->nand_page / XP3200S_NAND_PAGES_PER_BLOCK);
        if (s->trace_io || s->nand_program_count <= 64) {
            fprintf(stderr, "NANDPRG page=%u bytes=%u oob=%02x,%02x,%02x,%02x,"
                    "%02x,%02x,%02x,%02x main=%02x,%02x,%02x,%02x\n",
                    s->nand_page, s->nand_data_writes,
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 0],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 1],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 2],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 3],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 4],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 5],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 6],
                    s->nand_page_cache[XP3200S_NAND_PAGE_SIZE + 7],
                    s->nand_page_cache[0], s->nand_page_cache[1],
                    s->nand_page_cache[2], s->nand_page_cache[3]);
        }
    }
}

static void xp3200s_nand_erase_block(XP3200SState *s)
{
    uint32_t page;
    uint32_t first_page;
    size_t offset;
    size_t length;

    if (s->nand_address_count < 2) {
        return;
    }

    page = s->nand_address[0] |
           ((uint32_t)s->nand_address[1] << 8);
    if (s->nand_address_count >= 3) {
        page |= (uint32_t)s->nand_address[2] << 16;
    }
    first_page = page & ~(XP3200S_NAND_PAGES_PER_BLOCK - 1);
    if (first_page >= XP3200S_NAND_PAGE_COUNT) {
        return;
    }

    offset = (size_t)first_page * XP3200S_NAND_RAW_PAGE_SIZE;
    length = XP3200S_NAND_PAGES_PER_BLOCK * XP3200S_NAND_RAW_PAGE_SIZE;
    memset(s->nand_storage + offset, 0xff, length);
    xp3200s_nand_mark_dirty(
        s, first_page / XP3200S_NAND_PAGES_PER_BLOCK);
    s->nand_erase_count++;
    if (s->trace_io || s->nand_erase_count <= 64) {
        fprintf(stderr, "NANDERS block=%u page=%u\n",
                first_page / XP3200S_NAND_PAGES_PER_BLOCK, first_page);
    }
    if (xp3200s_nand_log_sample(s, s->nand_erase_count)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-nand: erase=%" PRIu64 " block=%u "
                      "first-page=%u\n",
                      s->nand_erase_count,
                      first_page / XP3200S_NAND_PAGES_PER_BLOCK,
                      first_page);
    }
}

static void xp3200s_nand_command(XP3200SState *s, uint8_t command)
{
    s->nand_command = command;
    s->nand_command_count++;
    if (command == 0x60 || command == 0xd0 || command == 0x80 ||
        command == 0x10 || command == 0x70) {
        fprintf(stderr, "NCMD %02x phase=%d addrc=%d page=%u col=%u "
                "a=%02x,%02x,%02x,%02x,%02x\n",
                command, s->nand_phase, s->nand_address_count,
                s->nand_page, s->nand_column,
                s->nand_address[0], s->nand_address[1], s->nand_address[2],
                s->nand_address[3], s->nand_address[4]);
    }
    if (s->trace_io || s->nand_command_count <= 24 ||
        command == 0xff || command == 0x90) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-nand: command=%" PRIu64 " value=0x%02x "
                      "phase=%u addresses=%u\n",
                      s->nand_command_count, command, s->nand_phase,
                      s->nand_address_count);
    }

    switch (command) {
    case 0xff:
        xp3200s_nand_reset(s);
        break;
    case 0x90:
        s->nand_phase = XP3200S_NAND_READ_ID;
        s->nand_address_count = 0;
        s->nand_id_index = 0;
        break;
    case 0x70:
        s->nand_phase = XP3200S_NAND_READ_STATUS;
        break;
    case 0x05:
        /* Random Data Output setup: next two address cycles set the column. */
        s->nand_phase = XP3200S_NAND_READ_SETUP;
        s->nand_address_count = 0;
        s->nand_data_reads = 0;
        break;
    case 0xe0:
        if (s->nand_phase == XP3200S_NAND_READ_SETUP &&
            s->nand_address_count >= 2) {
            s->nand_column = s->nand_address[0] |
                             ((uint16_t)s->nand_address[1] << 8);
            s->nand_phase = XP3200S_NAND_READ_DATA;
            s->nand_data_reads = 0;
            if (s->trace_io) {
                fprintf(stderr, "NANDCOL cmd=e0 pc=%08x page=%u col=%u\n",
                        xp3200s_current_pc(), s->nand_page, s->nand_column);
            }
        }
        break;
    case 0x00:
        s->nand_phase = XP3200S_NAND_READ_SETUP;
        s->nand_address_count = 0;
        s->nand_data_reads = 0;
        break;
    case 0x30:
        if (s->nand_phase == XP3200S_NAND_READ_SETUP &&
            s->nand_address_count >= XP3200S_NAND_ADDR_CYCLES) {
            xp3200s_nand_decode_page_address(s);
            s->nand_phase = XP3200S_NAND_READ_DATA;
            s->nand_page_read_count++;
            if (s->trace_io || s->nand_page_read_count <= 48) {
                fprintf(stderr, "NANDRD n=%" PRIu64 " pc=%08x page=%u "
                        "col=%u a=%02x,%02x,%02x,%02x\n",
                        s->nand_page_read_count, xp3200s_current_pc(),
                        s->nand_page, s->nand_column,
                        s->nand_address[0], s->nand_address[1],
                        s->nand_address[2], s->nand_address[3]);
            }
            if (xp3200s_nand_log_sample(s, s->nand_page_read_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-nand: read=%" PRIu64 " page=%u "
                              "column=%u\n",
                              s->nand_page_read_count, s->nand_page,
                              s->nand_column);
            }
        } else if (s->nand_phase == XP3200S_NAND_READ_SETUP &&
                   s->nand_address_count >= 2) {
            /* Column-only 0x30 keeps the current page (random output). */
            s->nand_column = (s->nand_address[0] |
                              ((uint16_t)(s->nand_address[1] & 0x0f) << 8)) &
                             0x0fff;
            s->nand_phase = XP3200S_NAND_READ_DATA;
            s->nand_data_reads = 0;
            if (s->trace_io) {
                fprintf(stderr, "NANDCOL cmd=30 pc=%08x page=%u col=%u\n",
                        xp3200s_current_pc(), s->nand_page, s->nand_column);
            }
        }
        break;
    case 0x35:
        if (s->nand_phase == XP3200S_NAND_READ_SETUP &&
            s->nand_address_count >= XP3200S_NAND_ADDR_CYCLES) {
            xp3200s_nand_decode_page_address(s);
            xp3200s_nand_load_page_cache(s);
            s->nand_phase = XP3200S_NAND_COPYBACK_READY;
            s->nand_page_read_count++;
            if (xp3200s_nand_log_sample(s, s->nand_page_read_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-nand: copyback-read=%" PRIu64
                              " page=%u\n",
                              s->nand_page_read_count, s->nand_page);
            }
        }
        break;
    case 0x60:
        s->nand_phase = XP3200S_NAND_ERASE_SETUP;
        s->nand_address_count = 0;
        break;
    case 0xd0:
        if (s->nand_phase == XP3200S_NAND_ERASE_SETUP) {
            xp3200s_nand_erase_block(s);
            s->nand_phase = XP3200S_NAND_READ_STATUS;
        }
        break;
    case 0x80:
        s->nand_phase = XP3200S_NAND_PROGRAM_DATA;
        s->nand_address_count = 0;
        s->nand_data_writes = 0;
        memset(s->nand_page_cache, 0xff, sizeof(s->nand_page_cache));
        s->nand_page_cache_valid = true;
        s->nand_program_page_valid = false;
        s->nand_program_address_cycles = XP3200S_NAND_ADDR_CYCLES;
        s->nand_copyback_program = false;
        break;
    case 0x85:
        if (s->nand_phase == XP3200S_NAND_COPYBACK_READY &&
            s->nand_page_cache_valid) {
            s->nand_phase = XP3200S_NAND_PROGRAM_DATA;
            s->nand_address_count = 0;
            s->nand_data_writes = 0;
            s->nand_program_page_valid = false;
            s->nand_program_address_cycles = XP3200S_NAND_ADDR_CYCLES;
            s->nand_copyback_program = true;
        } else if (s->nand_phase == XP3200S_NAND_PROGRAM_DATA &&
                   s->nand_page_cache_valid &&
                   s->nand_program_page_valid) {
            /* Random data input changes the column in the page register. */
            s->nand_address_count = 0;
            s->nand_program_address_cycles = 2;
        }
        break;
    case 0x10:
        if (s->nand_phase == XP3200S_NAND_PROGRAM_DATA) {
            xp3200s_nand_commit_page_cache(s);
            s->nand_program_count++;
            if (xp3200s_nand_log_sample(s, s->nand_program_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xp3200s-nand: program=%" PRIu64 " page=%u "
                              "bytes=%u copyback=%u\n",
                              s->nand_program_count, s->nand_page,
                              s->nand_data_writes,
                              s->nand_copyback_program ? 1 : 0);
            }
        }
        s->nand_phase = XP3200S_NAND_READ_STATUS;
        s->nand_page_cache_valid = false;
        s->nand_program_page_valid = false;
        s->nand_program_address_cycles = 0;
        s->nand_copyback_program = false;
        break;
    default:
        break;
    }
}

static void xp3200s_nand_address(XP3200SState *s, uint8_t value)
{
    if (s->nand_address_count < ARRAY_SIZE(s->nand_address)) {
        s->nand_address[s->nand_address_count++] = value;
    }
    if (s->nand_phase == XP3200S_NAND_READ_ID) {
        s->nand_id_index = 0;
    } else if (s->nand_phase == XP3200S_NAND_PROGRAM_DATA) {
        if (s->nand_program_address_cycles == XP3200S_NAND_ADDR_CYCLES &&
            s->nand_address_count == XP3200S_NAND_ADDR_CYCLES) {
            xp3200s_nand_decode_page_address(s);
            s->nand_program_page_valid = true;
        } else if (s->nand_program_address_cycles == 2 &&
                   s->nand_address_count == 2) {
            s->nand_column = (s->nand_address[0] |
                              ((uint16_t)(s->nand_address[1] & 0x0f) << 8)) &
                             0x0fff;
        }
    }
}

static uint8_t xp3200s_nand_data_read_byte(XP3200SState *s)
{
    switch (s->nand_phase) {
    case XP3200S_NAND_READ_ID:
        if (s->nand_id_index < 4) {
            /* ID bytes are output LSB first: EC F1 00 15 for K9F1G08. */
            return (s->nand_id >> (8 * s->nand_id_index++)) & 0xff;
        }
        return 0xff;
    case XP3200S_NAND_READ_STATUS:
        /* bit6 = ready, bit7 = write protect (0 = not protected). */
        return 0x40;
    case XP3200S_NAND_READ_DATA: {
        uint8_t value = 0xff;

        if (s->nand_page < XP3200S_NAND_PAGE_COUNT &&
            s->nand_column < XP3200S_NAND_RAW_PAGE_SIZE) {
            size_t offset = (size_t)s->nand_page *
                            XP3200S_NAND_RAW_PAGE_SIZE + s->nand_column;

            value = s->nand_storage[offset];
        }
        xp3200s_nand_ecc_byte(s, value);
        if (s->trace_io) {
            fprintf(stderr, "NANDD pc=%08x page=%u col=%u val=%02x\n",
                    xp3200s_current_pc(), s->nand_page, s->nand_column, value);
        }
        s->nand_data_reads++;
        s->nand_column++;
        return value;
    }
    default:
        return 0xff;
    }
}

static void xp3200s_nand_data_write_byte(XP3200SState *s, uint8_t value)
{
    if (s->nand_phase != XP3200S_NAND_PROGRAM_DATA ||
        !s->nand_page_cache_valid || !s->nand_program_page_valid ||
        s->nand_address_count < s->nand_program_address_cycles) {
        return;
    }
    if (s->nand_page >= XP3200S_NAND_PAGE_COUNT ||
        s->nand_column >= XP3200S_NAND_RAW_PAGE_SIZE) {
        s->nand_column++;
        return;
    }

    s->nand_page_cache[s->nand_column] = value;
    xp3200s_nand_ecc_byte(s, value);
    s->nand_data_writes++;
    s->nand_column++;
}

/*
 * NAND command/address/data multiplexing.
 *
 * The XP3200 board drives CLE/ALE from the P1 data register at 0x00402D5
 * (labelled P1D in the IO trace): bit6 = CLE, bit5 = ALE.  The boot code
 * always writes that register before every bus transfer:
 *
 *   0x5C = CLE + latched low pins   -> command
 *   0x3C = ALE + latched low pins   -> address
 *   0x1C = neither                  -> data
 *
 * The BBK9288 style mapping (P2 data at 0x00402D9, bit4 = CLE, bit5 = ALE)
 * is only used when the guest never touches 0x00402D5, so BBK's BOOT0 keeps
 * working.  Falling back to P2 whenever P1D has neither bit set was wrong:
 * P2 is parked at 0x60 on this board, so every program data byte was latched
 * as an address byte and page programs never wrote anything.
 */
static void xp3200s_nand_cle_ale(XP3200SState *s, bool *cle, bool *ale)
{
    uint8_t p1d = s->io_regs[XP3200S_P1D_DATA];
    uint8_t p2 = s->io_regs[XP3200S_PORT2_DATA];

    if (p1d != 0) {
        *cle = (p1d & 0x40) != 0;
        *ale = (p1d & 0x20) != 0;
    } else {
        *cle = (p2 & XP3200S_NAND_CLE) != 0;
        *ale = (p2 & XP3200S_NAND_ALE) != 0;
    }
}

static uint64_t xp3200s_nand_io_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (offset != 0) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        value |= (uint64_t)xp3200s_nand_data_read_byte(s) << (i * 8);
    }
    if (s->trace_io) {
        fprintf(stderr, "NANDR pc=%08x val=%02x cmd=%02x phase=%d idx=%d "
                "page=%u col=%u a=%02x,%02x,%02x,%02x,%02x\n",
                xp3200s_current_pc(), (unsigned)value, s->nand_command,
                s->nand_phase, s->nand_id_index, s->nand_page, s->nand_column,
                s->nand_address[0], s->nand_address[1], s->nand_address[2],
                s->nand_address[3], s->nand_address[4]);
    }
    return value;
}

static void xp3200s_nand_io_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    XP3200SState *s = opaque;
    uint8_t p2 = s->io_regs[XP3200S_PORT2_DATA];
    uint8_t c5 = s->io_regs[XP3200S_P1D_DATA];
    unsigned i;
    bool cle;
    bool ale;

    xp3200s_nand_cle_ale(s, &cle, &ale);

    if (s->trace_io) {
        fprintf(stderr, "NANDW pc=%08x off=%03x val=%02x p2=%02x c5=%02x "
                "cle=%d ale=%d\n",
                xp3200s_current_pc(), (unsigned)offset, (unsigned)value,
                p2, c5, cle, ale);
    }

    if (offset != 0) {
        return;
    }
    for (i = 0; i < size; i++) {
        uint8_t byte = value >> (i * 8);

        if (cle) {
            xp3200s_nand_command(s, byte);
        } else if (ale) {
            xp3200s_nand_address(s, byte);
        } else {
            xp3200s_nand_data_write_byte(s, byte);
        }
    }
}

static const MemoryRegionOps xp3200s_nand_io_ops = {
    .read = xp3200s_nand_io_read,
    .write = xp3200s_nand_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void xp3200s_board_record(XP3200SState *s, bool is_write,
                                  hwaddr offset, uint64_t value,
                                  unsigned size)
{
    uint8_t seen_mask = is_write ? 2 : 1;
    bool first = (s->board_seen[offset] & seen_mask) == 0;

    if (!first && !s->trace_io) {
        return;
    }

    if (first) {
        s->board_seen[offset] |= seen_mask;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-board: %s %s pc=0x%08x "
                  "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                  "\n",
                  first ? "first" : "trace",
                  is_write ? "write" : "read", xp3200s_current_pc(),
                  XP3200S_BOARD_IO_BASE + offset, size, size * 2, value);
}

static bool xp3200s_board_is_known_reg(hwaddr offset)
{
    switch (offset) {
    case XP3200S_BOARD_USB_STATUS:
    case XP3200S_BOARD_CTRL:
    case XP3200S_BOARD_DATA:
    case XP3200S_BOARD_STATUS_H:
    case XP3200S_BOARD_STATUS_L:
        return true;
    default:
        return false;
    }
}

static uint8_t xp3200s_board_read_byte(XP3200SState *s, hwaddr offset)
{
    if (s->strict_board_io && !xp3200s_board_is_known_reg(offset)) {
        return 0;
    }
    if (offset == XP3200S_BOARD_DATA && s->board_data_latch_pending) {
        uint8_t phase = s->board_data_latch_reads++;
        uint8_t value =
            xp3200s_board_latch_data[MIN(phase,
                                      ARRAY_SIZE(xp3200s_board_latch_data) -
                                      1)];

        s->board_regs[XP3200S_BOARD_DATA] = value;
        if (s->board_data_latch_reads >=
            ARRAY_SIZE(xp3200s_board_latch_data)) {
            s->board_data_latch_pending = false;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-board: command latch data phase %u "
                      "pc=0x%08x value=0x%02x\n",
                      phase, xp3200s_current_pc(), value);
        return value;
    }

    return s->board_regs[offset];
}

static void xp3200s_board_write_byte(XP3200SState *s, hwaddr offset,
                                      uint8_t value)
{
    uint8_t old = s->board_regs[offset];

    if (xp3200s_current_pc() >= 0x00c446d0u &&
        xp3200s_current_pc() <= 0x00c44780u) {
        fprintf(stderr, "NANDINIT BDW off=%04x val=%02x pc=%08x\n",
                (unsigned)offset, value, xp3200s_current_pc());
    }

    if (!s->strict_board_io || xp3200s_board_is_known_reg(offset)) {
        s->board_regs[offset] = value;
    }
    if (offset == XP3200S_BOARD_CTRL &&
        (old & 0x01) == 0 && (value & 0x01) != 0) {
        s->board_regs[XP3200S_BOARD_DATA] = 0;
        s->board_data_latch_reads = 0;
        s->board_data_latch_pending = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-board: command latch complete pc=0x%08x "
                      "ctrl=0x%02x data=0x00\n",
                      xp3200s_current_pc(), value);
    }
    if (offset == XP3200S_BOARD_CTRL &&
        (old & 0x01) != 0 && (value & 0x01) == 0) {
        s->board_regs[XP3200S_BOARD_STATUS_H] = 0;
        s->board_regs[XP3200S_BOARD_STATUS_L] = 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-board: command latch status clear "
                      "pc=0x%08x ctrl=0x%02x status=0x0000\n",
                      xp3200s_current_pc(), value);
    }
}

static uint64_t xp3200s_board_io_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t value = 0;
    unsigned i;

    if (offset + size > XP3200S_BOARD_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-board: read outside board io "
                      "addr=0x%08" HWADDR_PRIx " size=%u\n",
                      XP3200S_BOARD_IO_BASE + offset, size);
        return 0;
    }

    for (i = 0; i < size; i++) {
        value |= (uint64_t)xp3200s_board_read_byte(s, offset + i) <<
                 (i * 8);
    }
    xp3200s_board_record(s, false, offset, value, size);
    return value;
}

static void xp3200s_board_io_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    XP3200SState *s = opaque;
    unsigned i;

    if (offset + size > XP3200S_BOARD_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-board: write outside board io "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                      "\n",
                      XP3200S_BOARD_IO_BASE + offset, size, value);
        return;
    }

    for (i = 0; i < size; i++) {
        xp3200s_board_write_byte(s, offset + i, value >> (i * 8));
    }
    xp3200s_board_record(s, true, offset, value, size);
}

static const MemoryRegionOps xp3200s_board_io_ops = {
    .read = xp3200s_board_io_read,
    .write = xp3200s_board_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t xp3200s_lcdc_io_load(XP3200SState *s, hwaddr offset,
                                      unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value |= (uint64_t)s->lcdc_regs[offset + i] << (i * 8);
    }

    return value;
}

static void xp3200s_lcdc_io_store(XP3200SState *s, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        s->lcdc_regs[offset + i] = value >> (i * 8);
    }
}

static void xp3200s_lcdc_record(XP3200SState *s, bool is_write,
                                 hwaddr offset, uint64_t value,
                                 unsigned size)
{
    uint8_t seen_mask = is_write ? 2 : 1;
    bool first = (s->lcdc_seen[offset] & seen_mask) == 0;

    if (!first && !s->trace_io) {
        return;
    }

    if (first) {
        s->lcdc_seen[offset] |= seen_mask;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-lcdc: %s %s pc=0x%08x "
                  "addr=0x%08" HWADDR_PRIx " size=%u value=0x%0*" PRIx64
                  " reg=%s\n",
                  first ? "first" : "trace",
                  is_write ? "write" : "read", xp3200s_current_pc(),
                  XP3200S_LCDC_IO_BASE + offset, size, size * 2, value,
                  xp3200s_lcdc_reg_name(offset));
}

static uint64_t xp3200s_lcdc_io_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    XP3200SState *s = opaque;
    uint64_t value;

    if (offset + size > XP3200S_LCDC_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-lcdc: read outside lcdc window "
                      "addr=0x%08" HWADDR_PRIx " size=%u\n",
                      XP3200S_LCDC_IO_BASE + offset, size);
        return 0;
    }

    value = xp3200s_lcdc_io_load(s, offset, size);
    xp3200s_lcdc_record(s, false, offset, value, size);
    return value;
}

static void xp3200s_lcdc_io_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    XP3200SState *s = opaque;

    if (offset + size > XP3200S_LCDC_IO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-lcdc: write outside lcdc window "
                      "addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64
                      "\n",
                      XP3200S_LCDC_IO_BASE + offset, size, value);
        return;
    }

    xp3200s_lcdc_io_store(s, offset, value, size);
    xp3200s_lcdc_record(s, true, offset, value, size);
}

static const MemoryRegionOps xp3200s_lcdc_io_ops = {
    .read = xp3200s_lcdc_io_read,
    .write = xp3200s_lcdc_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static unsigned xp3200s_lcd_bpp(XP3200SState *s)
{
    static const uint8_t bpp_by_mode[] = { 1, 2, 4, 8 };
    uint8_t mode = s->lcdc_regs[XP3200S_LCDC_MODE0] &
                   XP3200S_LCDC_BPP_MASK;

    if (mode < ARRAY_SIZE(bpp_by_mode)) {
        return bpp_by_mode[mode];
    }

    if (!(s->lcdc_seen[XP3200S_LCDC_MODE0] & 4)) {
        s->lcdc_seen[XP3200S_LCDC_MODE0] |= 4;
        qemu_log_mask(LOG_UNIMP,
                      "xp3200s-lcd: unsupported BPP mode %u; "
                      "falling back to 2bpp\n", mode);
    }
    return 2;
}

static unsigned xp3200s_lcd_stride(XP3200SState *s, unsigned bpp)
{
    return s->lcd_width * bpp / 8;
}

static uint8_t xp3200s_lcd_expand_six_bit(uint8_t value)
{
    value >>= 2;
    return (value << 2) | (value >> 4);
}

static uint8_t xp3200s_lcd_gray_value(XP3200SState *s, uint8_t pixel,
                                       unsigned bpp)
{
    uint8_t mode0 = s->lcdc_regs[XP3200S_LCDC_MODE0];
    unsigned lut_offset;

    if (mode0 & XP3200S_LCDC_LUTPASS) {
        if (bpp <= 4) {
            return pixel * 255 / ((1u << bpp) - 1);
        }
        return xp3200s_lcd_expand_six_bit(pixel);
    }

    lut_offset = XP3200S_LCDC_LUT_BASE +
                 pixel * XP3200S_LCDC_LUT_ENTRY_SIZE +
                 XP3200S_LCDC_LUT_GREEN;
    return xp3200s_lcd_expand_six_bit(s->lcdc_regs[lut_offset]);
}

static uint8_t xp3200s_lcd_unlit_value(uint8_t level)
{
    return 0x20 + ((uint16_t)level * (0x98 - 0x20) + 127) / 255;
}

static uint16_t xp3200s_lcd_mode1(XP3200SState *s)
{
    return s->lcdc_regs[XP3200S_LCDC_MODE1] |
           (uint16_t)(s->lcdc_regs[XP3200S_LCDC_MODE1 + 1] << 8);
}

/*
 * Panel polarity.
 *
 * The mono STN panel is reflective: an undriven pixel looks paper white and a
 * fully driven pixel looks black.  The LUT / LUTPASS level is the drive
 * strength, so the model has to invert it to get what the panel shows.
 * SWINV (LCDC mode register 1, 0x380202 D9, manual VII-2-42) is the software
 * "negative display" bit and flips the LUT output back, i.e. it restores the
 * level order.  BLANK (D8) drives every output to the undriven state.
 */
static uint8_t xp3200s_lcd_panel_gray(XP3200SState *s, uint8_t level)
{
    uint16_t mode1 = xp3200s_lcd_mode1(s);

    if (mode1 & XP3200S_LCDC_MODE1_BLANK) {
        level = 0;
    }
    return (mode1 & XP3200S_LCDC_MODE1_SWINV) ? level : (uint8_t)(0xff - level);
}

static uint8_t xp3200s_lcd_vram_pixel(XP3200SState *s, unsigned x,
                                       unsigned y, unsigned bpp)
{
    unsigned stride = xp3200s_lcd_stride(s, bpp);
    const uint8_t *src = s->ivram_ptr + y * stride;
    unsigned bit_offset = (x * bpp) & 7;
    unsigned shift = 8 - bpp - bit_offset;
    uint8_t mask = (1u << bpp) - 1;

    return (src[x * bpp / 8] >> shift) & mask;
}

static uint32_t xp3200s_lcd_pixel(DisplaySurface *surface, uint8_t level)
{
    switch (surface_bits_per_pixel(surface)) {
    case 8:
        return rgb_to_pixel8(level, level, level);
    case 15:
        return rgb_to_pixel15(level, level, level);
    case 16:
        return rgb_to_pixel16(level, level, level);
    case 24:
        return rgb_to_pixel24(level, level, level);
    case 32:
        return rgb_to_pixel32(level, level, level);
    default:
        return 0;
    }
}

static void xp3200s_lcd_store_pixel(DisplaySurface *surface, int x, int y,
                                     uint32_t color)
{
    int bpp = (surface_bits_per_pixel(surface) + 7) / 8;
    uint8_t *dst = surface_data(surface) + y * surface_stride(surface) +
                   x * bpp;

    switch (bpp) {
    case 1:
        dst[0] = color;
        break;
    case 2:
        *(uint16_t *)dst = color;
        break;
    case 3:
        dst[0] = color;
        dst[1] = color >> 8;
        dst[2] = color >> 16;
        break;
    case 4:
        *(uint32_t *)dst = color;
        break;
    }
}

static void xp3200s_lcd_update(void *opaque)
{
    XP3200SState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t colors[1 << XP3200S_LCD_MAX_BPP];
    unsigned bpp;
    unsigned color_count;
    bool backlight_on;
    unsigned x;
    unsigned y;

    if (surface_bits_per_pixel(surface) == 0 || s->ivram_ptr == NULL) {
        return;
    }

    bpp = xp3200s_lcd_bpp(s);
    color_count = 1u << bpp;
    backlight_on = xp3200s_backlight_on(s);
    for (x = 0; x < color_count; x++) {
        uint8_t level = xp3200s_lcd_panel_gray(
            s, xp3200s_lcd_gray_value(s, x, bpp));

        if (!backlight_on) {
            level = xp3200s_lcd_unlit_value(level);
        }
        colors[x] = xp3200s_lcd_pixel(surface, level);
    }

    for (y = 0; y < s->lcd_height; y++) {
        for (x = 0; x < s->lcd_width; x++) {
            uint8_t pixel = xp3200s_lcd_vram_pixel(s, x, y, bpp);

            xp3200s_lcd_store_pixel(surface, x, y, colors[pixel]);
        }
    }

    dpy_gfx_update(s->con, 0, 0, s->lcd_width, s->lcd_height);
}

static void xp3200s_lcd_invalidate(void *opaque)
{
    XP3200SState *s = opaque;

    dpy_gfx_update(s->con, 0, 0, s->lcd_width, s->lcd_height);
}

static const GraphicHwOps xp3200s_lcd_ops = {
    .invalidate = xp3200s_lcd_invalidate,
    .gfx_update = xp3200s_lcd_update,
};

static bool xp3200s_lcd_dump_pgm(XP3200SState *s, const char *path)
{
    g_autofree char *header = NULL;
    GByteArray *out;
    GError *gerr = NULL;
    uint32_t counts[1 << XP3200S_LCD_MAX_BPP] = { 0 };
    unsigned bpp;
    unsigned color_count;
    unsigned used = 0;
    bool ok;
    unsigned x;
    unsigned y;

    if (s->ivram_ptr == NULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-lcd: cannot dump; IVRAM is not mapped\n");
        return false;
    }
    if (path == NULL || path[0] == 0) {
        path = "xp3200s-lcd.pgm";
    }

    bpp = xp3200s_lcd_bpp(s);
    color_count = 1u << bpp;
    {
        unsigned i;

        fprintf(stderr, "LCDSTATE mode0=%02x mode1=%04x bpp=%u backlight=%u "
                "blank=%u swinv=%u lut=",
                s->lcdc_regs[XP3200S_LCDC_MODE0],
                xp3200s_lcd_mode1(s), bpp,
                xp3200s_backlight_on(s) ? 1u : 0u,
                (xp3200s_lcd_mode1(s) & XP3200S_LCDC_MODE1_BLANK) ? 1u : 0u,
                (xp3200s_lcd_mode1(s) & XP3200S_LCDC_MODE1_SWINV) ? 1u : 0u);
        for (i = 0; i < 16; i++) {
            fprintf(stderr, "%02x,",
                    s->lcdc_regs[XP3200S_LCDC_LUT_BASE +
                                 i * XP3200S_LCDC_LUT_ENTRY_SIZE +
                                 XP3200S_LCDC_LUT_GREEN]);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "KSTATE eir0=0x%02x fir0=0x%02x sel=0x%02x cond=0x%02x "
                "mask=0x%02x k5low=0x%02x k6low=0x%02x p0low=0x%02x\n",
                s->io_regs[XP3200S_EIR0_KEY_PORT0_3],
                s->io_regs[XP3200S_FIR0_KEY_PORT0_3],
                s->io_regs[XP3200S_KEY_INPUT_SELECT],
                s->io_regs[XP3200S_KEY_INPUT_CONDITION0],
                s->io_regs[XP3200S_KEY_INPUT_MASK0],
                s->k5_input_low, s->k6_input_low,
                s->gpio_input_low[XP3200S_KEY_GPIO_PORT]);
    }
    {
        unsigned i;

        fprintf(stderr, "READHIST io(0x280-0x2ff):");
        for (i = 0; i < ARRAY_SIZE(s->dbg_kport_reads); i++) {
            if (s->dbg_kport_reads[i] != 0) {
                fprintf(stderr, " %03x=%u", 0x280 + i,
                        s->dbg_kport_reads[i]);
            }
        }
        fprintf(stderr, "\nAUDIOHIST rd:");
        for (i = 0; i < ARRAY_SIZE(s->dbg_audio_reads); i++) {
            if (s->dbg_audio_reads[i] != 0) {
                fprintf(stderr, " %03x=%u", i, s->dbg_audio_reads[i]);
            }
        }
        fprintf(stderr, "\nAUDIOHIST wr:");
        for (i = 0; i < ARRAY_SIZE(s->dbg_audio_writes); i++) {
            if (s->dbg_audio_writes[i] != 0) {
                fprintf(stderr, " %03x=%u", i, s->dbg_audio_writes[i]);
            }
        }
        fprintf(stderr, "\nWRITEHIST io(0x280-0x2ff):");
        for (i = 0; i < ARRAY_SIZE(s->dbg_kport_writes); i++) {
            if (s->dbg_kport_writes[i] != 0) {
                fprintf(stderr, " %03x=%u", 0x280 + i,
                        s->dbg_kport_writes[i]);
            }
        }
        fprintf(stderr, "\nREADHIST touchio:");
        for (i = 0; i < ARRAY_SIZE(s->dbg_touch_reads); i++) {
            if (s->dbg_touch_reads[i] != 0) {
                fprintf(stderr, " %03x=%u", i, s->dbg_touch_reads[i]);
            }
        }
        fprintf(stderr, "\nWRITEHIST touchio:");
        for (i = 0; i < ARRAY_SIZE(s->dbg_touch_writes); i++) {
            if (s->dbg_touch_writes[i] != 0) {
                fprintf(stderr, " %03x=%u", i, s->dbg_touch_writes[i]);
            }
        }
        fprintf(stderr, "\n");
    }
    header = g_strdup_printf("P5\n%u %u\n255\n",
                             s->lcd_width, s->lcd_height);
    out = g_byte_array_sized_new(strlen(header) +
                                 s->lcd_width * s->lcd_height);
    g_byte_array_append(out, (const uint8_t *)header, strlen(header));

    for (y = 0; y < s->lcd_height; y++) {
        for (x = 0; x < s->lcd_width; x++) {
            uint8_t pixel = xp3200s_lcd_vram_pixel(s, x, y, bpp);
            uint8_t gray = xp3200s_lcd_panel_gray(
                s, xp3200s_lcd_gray_value(s, pixel, bpp));

            counts[pixel]++;
            g_byte_array_append(out, &gray, 1);
        }
    }

    ok = g_file_set_contents(path, (const char *)out->data, out->len, &gerr);
    g_byte_array_unref(out);

    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xp3200s-lcd: dump failed path=%s error=%s\n",
                      path, gerr != NULL ? gerr->message : "unknown");
        g_clear_error(&gerr);
        return false;
    }

    for (x = 0; x < color_count; x++) {
        used += counts[x] != 0;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xp3200s-lcd: dumped %ux%u %ubpp framebuffer to %s "
                  "gray-levels-used=%u/%u\n",
                  s->lcd_width, s->lcd_height, bpp, path,
                  used, color_count);
    return true;
}

/*
 * Diagnostic backdoor: after debug-call-ms, push the current PC as a return
 * address and jump to debug-call-addr.  Used to enter the firmware's hidden
 * factory self-test module without patching the flash image.
 */
static void xp3200s_debug_call_cb(void *opaque)
{
    XP3200SState *s = opaque;
    CPUS1C33State *env;
    uint32_t ret;
    uint32_t sp;

    if (s->cpu == NULL || s->debug_call_addr == 0) {
        return;
    }
    env = &S1C33_CPU(s->cpu)->env;
    ret = env->pc;
    sp = env->sp - 4;
    cpu_physical_memory_write(sp, &ret, sizeof(ret));
    env->sp = sp;
    env->pc = s->debug_call_addr;
    cpu_exit(s->cpu);
    fprintf(stderr, "DEBUGCALL addr=%08x ret=%08x sp=%08x\n",
            s->debug_call_addr, ret, sp);
    fflush(stderr);
}

static void xp3200s_debug_lcd_dump_cb(void *opaque)
{
    XP3200SState *s = opaque;

    xp3200s_lcd_dump_pgm(s, s->debug_lcd_dump_path);
}

/*
 * On-demand LCD dump: QMP writes a new sequence number to
 * "debug-lcd-dump-seq" and this poll timer writes <path>.<seq>.pgm.
 * Unlike debug-lcd-dump-ms this can be used repeatedly in one run, which is
 * what calibrating every matrix position needs.
 */
static void xp3200s_debug_lcd_dump_poll_cb(void *opaque)
{
    XP3200SState *s = opaque;
    uint32_t seq = 0;

    if (s->machine_backref != NULL) {
        seq = ((XP3200SMachineState *)s->machine_backref)->debug_lcd_dump_seq;
    }
    if (seq != 0 && seq != s->debug_lcd_dump_seen) {
        g_autofree char *path = NULL;

        s->debug_lcd_dump_seen = seq;
        path = g_strdup_printf("%s.%u.pgm", s->debug_lcd_dump_path, seq);
        if (xp3200s_lcd_dump_pgm(s, path)) {
            fprintf(stderr, "LCDDUMP seq=%u path=%s\n", seq, path);
        }
    }
    timer_mod(s->debug_lcd_dump_poll_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
}

static void xp3200s_lcdc_io_reset(XP3200SState *s)
{
    static const uint8_t default_green_lut[4] = {
        0x00, 0x54, 0xa8, 0xfc,
    };
    uint16_t hsize = s->lcd_width / 8 - 1;
    uint16_t vsize = s->lcd_height - 1;
    unsigned i;

    memset(s->lcdc_regs, 0, sizeof(s->lcdc_regs));
    memset(s->lcdc_seen, 0, sizeof(s->lcdc_seen));

    if (s->native_boot) {
        return;
    }

    /*
     * The board loader starts the KNL image directly without running the
     * original boot ROM. Seed the panel geometry, stock 2bpp mode, and the
     * standard four-level grayscale LUT while guest writes can still override
     * every LCDC register and switch to 4bpp/16-gray mode.
     */
    s->lcdc_regs[XP3200S_LCDC_MODE0] = 1;
    s->lcdc_regs[XP3200S_LCDC_HSIZE] = hsize & 0xff;
    s->lcdc_regs[XP3200S_LCDC_HSIZE + 1] = hsize >> 8;
    s->lcdc_regs[XP3200S_LCDC_VSIZE] = vsize & 0xff;
    s->lcdc_regs[XP3200S_LCDC_VSIZE + 1] = vsize >> 8;
    for (i = 0; i < ARRAY_SIZE(default_green_lut); i++) {
        s->lcdc_regs[XP3200S_LCDC_LUT_BASE +
                     i * XP3200S_LCDC_LUT_ENTRY_SIZE +
                     XP3200S_LCDC_LUT_GREEN] = default_green_lut[i];
    }
}

static uint32_t xp3200s_first_vector(const uint8_t *payload,
                                      uint32_t payload_size)
{
    uint32_t scan_size = MIN(payload_size, 256 * 4);
    uint32_t end = XP3200S_SDRAM_BASE + payload_size;
    uint32_t off;

    for (off = 0; off + 4 <= scan_size; off += 4) {
        uint32_t value = ldl_le_p(payload + off);

        if (value >= XP3200S_SDRAM_BASE && value < end &&
            (value & 1) == 0) {
            return value;
        }
    }

    return XP3200S_SDRAM_BASE;
}

static void xp3200s_load_kernel_data(XP3200SState *s, S1C33CPU *cpu,
                                      const uint8_t *data,
                                      gsize len,
                                      const char *source,
                                      uint64_t ram_size)
{
    uint32_t body_size;
    uint32_t pc;

    if (memcmp(data, "KNL ", 4) == 0) {
        /* BBK KNL path, kept for comparison. */
        if (len < XP3200S_KERNEL_HEADER) {
            error_report("kernel from %s is too short", source);
            exit(1);
        }
    } else {
        /* XP3200: raw OS image, load at 0x02100000 and jump to entry. */
        if (len > ram_size) {
            error_report("raw OS image 0x%zx is larger than RAM 0x%" PRIx64,
                         (size_t)len, ram_size);
            exit(1);
        }
        rom_add_blob_fixed("xp3200.os", data, len, 0x02100000u);
        cpu_set_pc(CPU(cpu), 0x02171c9au);
        info_report("%s raw OS: %zu bytes at 0x02100000, pc=0x02171c9a",
                    xp3200s_model_name(s), (size_t)len);
        return;
    }

    body_size = ldl_le_p(data + 0x14);
    if (body_size > len - XP3200S_KERNEL_HEADER) {
        error_report("kernel from %s has invalid body size 0x%x", source,
                     body_size);
        exit(1);
    }
    if (body_size > ram_size) {
        error_report("kernel body 0x%x is larger than RAM size 0x%" PRIx64,
                     body_size, ram_size);
        exit(1);
    }

    rom_add_blob_fixed("xp3200s.kernel",
                       data + XP3200S_KERNEL_HEADER,
                       body_size, XP3200S_SDRAM_BASE);
    pc = xp3200s_first_vector(data + XP3200S_KERNEL_HEADER,
                               body_size);
    cpu_set_pc(CPU(cpu), pc);

    info_report("%s KNL %.16s: body=0x%x load=0x%08x pc=0x%08x",
                xp3200s_model_name(s), data + 4, body_size,
                XP3200S_SDRAM_BASE, pc);
}

static void xp3200s_load_kernel_file(XP3200SState *s, S1C33CPU *cpu,
                                      const char *filename,
                                      uint64_t ram_size)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;
    GError *gerr = NULL;

    if (!g_file_get_contents(filename, &data, &len, &gerr)) {
        error_report("could not load kernel '%s': %s", filename, gerr->message);
        g_error_free(gerr);
        exit(1);
    }
    xp3200s_load_kernel_data(s, cpu, (const uint8_t *)data, len,
                              filename, ram_size);
}

static void xp3200s_load_boot_rom(XP3200SState *s, S1C33CPU *cpu,
                                   const char *filename)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;
    GError *gerr = NULL;

    if (!g_file_get_contents(filename, &data, &len, &gerr)) {
        error_report("could not load boot ROM '%s': %s", filename,
                     gerr->message);
        g_error_free(gerr);
        exit(1);
    }
    if (len == 0) {
        error_report("boot image '%s' is empty", filename);
        exit(1);
    }
    if (len <= XP3200S_IRAM_SIZE) {
        rom_add_blob_fixed("xp3200.boot-rom", data, len, 0);
        cpu_set_pc(CPU(cpu), 0);
        info_report("%s boot: executing IRAM ROM '%s' (%zu bytes) at 0",
                    xp3200s_model_name(s), filename, (size_t)len);
        return;
    }
    if (len <= 0x100000u) {
        rom_add_blob_fixed("xp3200.nor", data, len, 0x00c00000u);
        cpu_set_pc(CPU(cpu), 0x00c43ac8u);
        info_report("%s boot: NOR '%s' (%zu bytes) at 0x00c00000, pc=0x00c43ac8",
                    xp3200s_model_name(s), filename, (size_t)len);
        return;
    }
    error_report("boot image '%s' too large (%zu bytes)", filename, (size_t)len);
    exit(1);
}

static void xp3200s_direct_irq_cb(void *opaque)
{
    XP3200SState *s = opaque;

    s1c33_cpu_set_irq(s->cpu, true, XP3200S_16TM0B_VECTOR, 1);
    s1c33_cpu_set_irq(s->cpu, false, XP3200S_16TM0B_VECTOR, 0);
    timer_mod(s->direct_os_irq_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10 * 1000 * 1000);
}

static void xp3200s_init(MachineState *machine)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(machine);
    XP3200SState *s = g_new0(XP3200SState, 1);
    S1C33CPU *cpu;
    g_autofree uint8_t *nand_kernel = NULL;
    gsize nand_kernel_len = 0;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *iram = g_new(MemoryRegion, 1);
    const char *kernel = machine->kernel_filename;
    const char *boot_rom = machine->firmware;

    if (kernel != NULL && boot_rom != NULL) {
        error_report("-kernel and -bios select different BBK boot paths; "
                     "specify only one");
        exit(1);
    }
    /* XP3200: NOR boot does not require a NAND image. */

    s->is_9288 = bms->is_9288;
    s->nand_id = bms->nand_id;
    s->nand_linear = bms->nand_linear != 0;
    s->native_boot = boot_rom != NULL;
    s->lcd_width = bms->is_9288 ? XP3200_LCD_WIDTH : XP3200S_LCD_WIDTH;
    s->lcd_height = bms->is_9288 ? XP3200_LCD_HEIGHT : XP3200S_LCD_HEIGHT;
    s->audio_io_size =
        bms->is_9288 ? XP3200_AUDIO_IO_SIZE : XP3200S_AUDIO_IO_SIZE;
    s->has_touchscreen = !bms->is_9288;
    s->trace_io = bms->trace_io;
    s->trace_key_scan = bms->trace_key_scan;
    s->trace_audio = bms->trace_audio;
    bms->soc = s;
    s->debug_key_code = bms->debug_key_code;
    s->debug_key_matrix = bms->debug_key_matrix;
    s->strict_board_io = bms->strict_board_io;
    s->usb_connected = bms->usb_connected;
    s->key_matrix = bms->key_matrix;
    s->debug_key_p0_low_mask =
        bms->debug_key_p0_low_mask & XP3200S_KEY_P0_MASK;
    s->touch_fpt = bms->touch_fpt;
    s->touch_k5_low_mask = bms->touch_k5_low_mask & 0x1f;
    s->touch_p0_low = bms->touch_p0_low;
    s->debug_timer16_factors = bms->debug_timer16_factors;
    if (s->touch_fpt < 4 || s->touch_fpt > 7) {
        warn_report("invalid xp3200s touch-fpt=%u; using %u",
                    s->touch_fpt, XP3200S_DEFAULT_TOUCH_FPT);
        s->touch_fpt = XP3200S_DEFAULT_TOUCH_FPT;
    }
    s->machine_backref = bms;
    s->audio_force_data_in = bms->audio_force_data_in;
    s->debug_key_rowsel = bms->debug_key_rowsel;
    s->trace_io_all_ms = bms->trace_io_all_ms;
    s->trace_io_all_len = bms->trace_io_all_len;
    s->debug_key_rc = bms->debug_key_rc;
    s->debug_call_ms = bms->debug_call_ms;
    s->debug_call_addr = bms->debug_call_addr;
    s->debug_lcd_dump_path =
        g_strdup(bms->debug_lcd_dump_path != NULL &&
                 bms->debug_lcd_dump_path[0] != 0 ?
                 bms->debug_lcd_dump_path : "xp3200s-lcd.pgm");
    xp3200s_nand_storage_init(s, bms->nand_image_path);
    xp3200s_sd_init(s, bms->sd_image_path);
    s->wdt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, xp3200s_wdt_expire, s);
    s->sd_inserted = true;
    if (bms->debug_sd_insert_ms != 0) {
        s->sd_inserted = false;
        s->sd_insert_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          xp3200s_sd_insert_cb, s);
        timer_mod(s->sd_insert_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_sd_insert_ms);
        info_report("%s SD: insertion scheduled after %u ms",
                    xp3200s_model_name(s), bms->debug_sd_insert_ms);
    }
    xp3200s_audio_init(s, bms->audio_stream_path,
                        bms->audio_player_path,
                        bms->audio_inject_path, bms->audio_inject_ms);

    memory_region_init_ram(iram, NULL, "xp3200s.iram", XP3200S_IRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, 0, iram);
    {
        MemoryRegion *iram_hi = g_new(MemoryRegion, 1);
        memory_region_init_ram(iram_hi, NULL, "xp3200s.iram-hi",
                               0x10000, &error_fatal);
        memory_region_add_subregion(sysmem, 0x00010000, iram_hi);
    }
    {
        /* Bootloader checks this IRAM byte at 0x6E4: nonzero = normal boot. */
        uint8_t boot_flag = 1;
        rom_add_blob_fixed("xp3200.boot-flag", &boot_flag, 1, 0x6e4);
    }
    {
        /* NAND boot-mode flag checked at SDRAM 0x02000F54. */
        uint8_t nand_mode = 1;
        rom_add_blob_fixed("xp3200.nand-mode", &nand_mode, 1, 0x02000f54);
    }
    rom_add_blob_fixed("xp3200s.rtc-cookie", xp3200s_iram_rtc_cookie,
                       sizeof(xp3200s_iram_rtc_cookie),
                       XP3200S_IRAM_RTC_COOKIE_OFFSET);
    memory_region_init_io(&s->touch_io, NULL, &xp3200s_touch_io_ops, s,
                           "xp3200s.touch-io", XP3200S_TOUCH_IO_SIZE);
    xp3200s_touch_io_reset(s);
    memory_region_add_subregion(sysmem, XP3200S_TOUCH_IO_BASE, &s->touch_io);
    memory_region_init_io(&s->audio_io, NULL, &xp3200s_audio_io_ops, s,
                          "xp3200s.audio-decoder", s->audio_io_size);
    memory_region_add_subregion(sysmem, XP3200S_AUDIO_IO_BASE, &s->audio_io);
    memory_region_init_io(&s->nand_io, NULL, &xp3200s_nand_io_ops, s,
                          "xp3200s.nand-data", XP3200S_NAND_DATA_SIZE);
    memory_region_add_subregion(sysmem, XP3200S_NAND_DATA_BASE, &s->nand_io);
    memory_region_init_ram(&s->ivram, NULL, "xp3200s.ivram",
                           XP3200S_IVRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, XP3200S_IVRAM_BASE, &s->ivram);
    s->ivram_ptr = memory_region_get_ram_ptr(&s->ivram);
    s->con = graphic_console_init(NULL, 0, &xp3200s_lcd_ops, s);
    qemu_console_resize(s->con, s->lcd_width, s->lcd_height);
    memory_region_init_io(&s->board_io, NULL, &xp3200s_board_io_ops, s,
                          "xp3200s.board-io", XP3200S_BOARD_IO_SIZE);
    xp3200s_board_io_reset(s);
    memory_region_add_subregion(sysmem, XP3200S_BOARD_IO_BASE, &s->board_io);
    memory_region_add_subregion(sysmem, XP3200S_SDRAM_BASE, machine->ram);
    {
        MemoryRegion *nor = g_new(MemoryRegion, 1);
        memory_region_init_ram(nor, NULL, "xp3200.nor", 0x100000u,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0x00c00000u, nor);
    }
    memory_region_init_io(&s->lcdc_io, NULL, &xp3200s_lcdc_io_ops, s,
                          "xp3200s.lcdc-io", XP3200S_LCDC_IO_SIZE);
    xp3200s_lcdc_io_reset(s);
    memory_region_add_subregion(sysmem, XP3200S_LCDC_IO_BASE, &s->lcdc_io);
    memory_region_init_io(&s->io, NULL, &xp3200s_io_ops, s,
                          "xp3200s.s1c33l05-io", XP3200S_IO_SIZE);
    xp3200s_io_reset(s);
    memory_region_add_subregion(sysmem, XP3200S_IO_BASE, &s->io);
    s->exit.notify = xp3200s_exit_notify;
    qemu_add_exit_notifier(&s->exit);
    xp3200s_active_machine = s;
    s->kbd_handler =
        qemu_input_handler_register(NULL, &xp3200s_keyboard_handler);
    qemu_input_handler_activate(s->kbd_handler);
    if (s->has_touchscreen) {
        s->ptr_handler =
            qemu_input_handler_register(NULL, &xp3200s_pointer_handler);
        qemu_input_handler_activate(s->ptr_handler);
    }

    cpu = S1C33_CPU(cpu_create(machine->cpu_type));
    s->cpu = CPU(cpu);
    cpu->env.sp = XP3200S_BOOT_STACK_TOP;
    xp3200s_set_ttbr(s, XP3200S_SDRAM_BASE);

    if (boot_rom) {
        xp3200s_load_boot_rom(s, cpu, boot_rom);
    } else if (kernel) {
        xp3200s_load_kernel_file(s, cpu, kernel, machine->ram_size);
    } else {
        nand_kernel = xp3200s_nand_extract_kernel(
            s, &nand_kernel_len, machine->ram_size);
        if (nand_kernel == NULL) {
            error_report("BBK 9288-family NAND does not contain a readable "
                         "FAT16 kernel.bin");
            exit(1);
        }
        xp3200s_load_kernel_data(s, cpu, nand_kernel, nand_kernel_len,
                                  "NAND FAT16", machine->ram_size);
    }
    if (kernel) {
        /* Direct-OS mode: deliver a periodic timer IRQ to leave the idle halt. */
        s->direct_os_irq_timer =
            timer_new_ns(QEMU_CLOCK_VIRTUAL, xp3200s_direct_irq_cb, s);
        timer_mod(s->direct_os_irq_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000);
    }

    if (bms->debug_usb_wakeup_ms != 0) {
        s->debug_usb_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_debug_usb_wakeup_cb, s);
        timer_mod(s->debug_usb_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_usb_wakeup_ms);
        info_report("%s debug USB wakeup scheduled after %u ms",
                    xp3200s_model_name(s), bms->debug_usb_wakeup_ms);
    }
    if (bms->debug_nmi_wakeup_ms != 0) {
        s->debug_nmi_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_debug_nmi_wakeup_cb, s);
        timer_mod(s->debug_nmi_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_nmi_wakeup_ms);
        info_report("%s debug NMI wakeup scheduled after %u ms",
                    xp3200s_model_name(s), bms->debug_nmi_wakeup_ms);
    }
    if (bms->debug_port4_wakeup_ms != 0) {
        s->debug_port4_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         xp3200s_debug_port4_wakeup_cb, s);
        timer_mod(s->debug_port4_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_port4_wakeup_ms);
        info_report("%s debug port4 wakeup scheduled after %u ms",
                    xp3200s_model_name(s), bms->debug_port4_wakeup_ms);
    }
    if (bms->debug_port5_wakeup_ms != 0) {
        s->debug_port5_wakeup_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         xp3200s_debug_port5_wakeup_cb, s);
        timer_mod(s->debug_port5_wakeup_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_port5_wakeup_ms);
        info_report("%s debug port5 wakeup scheduled after %u ms",
                    xp3200s_model_name(s), bms->debug_port5_wakeup_ms);
    }
    if (bms->debug_call_ms != 0 && bms->debug_call_addr != 0) {
        s->debug_call_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                          xp3200s_debug_call_cb, s);
        timer_mod(s->debug_call_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  bms->debug_call_ms);
        info_report("%s debug call scheduled after %u ms to 0x%08x",
                    xp3200s_model_name(s), bms->debug_call_ms,
                    bms->debug_call_addr);
    }
    if (bms->debug_lcd_dump_ms != 0) {
        s->debug_lcd_dump_timer =
            timer_new_ms(QEMU_CLOCK_REALTIME,
                         xp3200s_debug_lcd_dump_cb, s);
        timer_mod(s->debug_lcd_dump_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  bms->debug_lcd_dump_ms);
        info_report("%s debug LCD dump scheduled after %u ms to %s",
                    xp3200s_model_name(s), bms->debug_lcd_dump_ms,
                    s->debug_lcd_dump_path);
    }
    /* On-demand dump support (QMP debug-lcd-dump-seq). */
    s->debug_lcd_dump_seen = bms->debug_lcd_dump_seq;
    s->debug_lcd_dump_poll_timer =
        timer_new_ms(QEMU_CLOCK_REALTIME, xp3200s_debug_lcd_dump_poll_cb, s);
    timer_mod(s->debug_lcd_dump_poll_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 200);
}

static bool xp3200s_get_trace_io(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->trace_io;
}

static void xp3200s_set_trace_io(Object *obj, bool value, Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    bms->trace_io = value;
    if (bms->soc) {
        ((XP3200SState *)bms->soc)->trace_io = value;
    }
}

static bool xp3200s_get_trace_key_scan(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->trace_key_scan;
}

static bool xp3200s_get_trace_audio(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->trace_audio;
}

static void xp3200s_set_trace_audio(Object *obj, bool value, Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    bms->trace_audio = value;
    if (bms->soc) {
        ((XP3200SState *)bms->soc)->trace_audio = value;
    }
}

static void xp3200s_set_trace_key_scan(Object *obj, bool value, Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    bms->trace_key_scan = value;
    if (bms->soc) {
        ((XP3200SState *)bms->soc)->trace_key_scan = value;
    }
}

static char *xp3200s_get_debug_lcd_dump_path(Object *obj, Error **errp)
{
    const char *path = XP3200S_MACHINE(obj)->debug_lcd_dump_path;

    return g_strdup(path != NULL ? path : "");
}

static void xp3200s_set_debug_lcd_dump_path(Object *obj, const char *value,
                                             Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->debug_lcd_dump_path);
    bms->debug_lcd_dump_path =
        g_strdup(value != NULL && value[0] != 0 ? value : "xp3200s-lcd.pgm");
}

static char *xp3200s_get_nand_image_path(Object *obj, Error **errp)
{
    const char *path = XP3200S_MACHINE(obj)->nand_image_path;

    return g_strdup(path != NULL ? path : "");
}

static void xp3200s_set_nand_image_path(Object *obj, const char *value,
                                         Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->nand_image_path);
    bms->nand_image_path =
        g_strdup(value != NULL && value[0] != 0 ? value : NULL);
}

static char *xp3200s_get_sd_image_path(Object *obj, Error **errp)
{
    const char *path = XP3200S_MACHINE(obj)->sd_image_path;

    return g_strdup(path != NULL ? path : "");
}

static void xp3200s_set_sd_image_path(Object *obj, const char *value,
                                      Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->sd_image_path);
    bms->sd_image_path =
        g_strdup(value != NULL && value[0] != 0 ? value : NULL);
}

static char *xp3200s_get_audio_stream_path(Object *obj, Error **errp)
{
    const char *path = XP3200S_MACHINE(obj)->audio_stream_path;

    return g_strdup(path != NULL ? path : "");
}

static void xp3200s_set_audio_stream_path(Object *obj, const char *value,
                                           Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->audio_stream_path);
    bms->audio_stream_path =
        g_strdup(value != NULL && value[0] != 0 ? value : NULL);
}

static char *xp3200s_get_audio_inject_path(Object *obj, Error **errp)
{
    const char *path = XP3200S_MACHINE(obj)->audio_inject_path;

    return g_strdup(path != NULL ? path : "");
}

static void xp3200s_set_audio_inject_path(Object *obj, const char *value,
                                          Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->audio_inject_path);
    bms->audio_inject_path =
        g_strdup(value != NULL && value[0] != 0 ? value : NULL);
}

static char *xp3200s_get_audio_player_path(Object *obj, Error **errp)
{
    const char *path = XP3200S_MACHINE(obj)->audio_player_path;

    return g_strdup(path != NULL ? path : "");
}

static void xp3200s_set_audio_player_path(Object *obj, const char *value,
                                           Error **errp)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->audio_player_path);
    bms->audio_player_path =
        g_strdup(value != NULL && value[0] != 0 ? value : NULL);
}

static bool xp3200s_get_strict_board_io(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->strict_board_io;
}

static void xp3200s_set_strict_board_io(Object *obj, bool value,
                                         Error **errp)
{
    XP3200S_MACHINE(obj)->strict_board_io = value;
}

static bool xp3200s_get_touch_p0_low(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->touch_p0_low;
}

static bool xp3200s_get_usb_connected(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->usb_connected;
}

static bool xp3200s_get_key_matrix(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->key_matrix;
}

static void xp3200s_set_key_matrix(Object *obj, bool value, Error **errp)
{
    XP3200S_MACHINE(obj)->key_matrix = value;
}

static void xp3200s_set_usb_connected(Object *obj, bool value, Error **errp)
{
    XP3200S_MACHINE(obj)->usb_connected = value;
}

static void xp3200s_set_touch_p0_low(Object *obj, bool value, Error **errp)
{
    XP3200S_MACHINE(obj)->touch_p0_low = value;
}

static bool xp3200s_get_debug_timer16_factors(Object *obj, Error **errp)
{
    return XP3200S_MACHINE(obj)->debug_timer16_factors;
}

static void xp3200s_set_debug_timer16_factors(Object *obj, bool value,
                                               Error **errp)
{
    XP3200S_MACHINE(obj)->debug_timer16_factors = value;
}

static void xp3200s_machine_instance_init(Object *obj)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    bms->debug_usb_wakeup_ms = 0;
    bms->debug_nmi_wakeup_ms = 0;
    bms->debug_port4_wakeup_ms = 0;
    bms->debug_port5_wakeup_ms = 0;
    bms->debug_key_p0_low_mask = 0;
    bms->debug_lcd_dump_ms = 0;
    bms->debug_call_ms = 0;
    bms->audio_force_data_in = 0x10000;
    bms->debug_key_rowsel = 0x10000;
    bms->trace_io_all_ms = 0;
    bms->trace_io_all_len = 2000;
    bms->audio_inject_ms = 65000;
    bms->debug_key_rc = 0xffff;
    bms->debug_call_addr = 0;
    bms->touch_fpt = XP3200S_DEFAULT_TOUCH_FPT;
    bms->touch_k5_low_mask = 0;
    bms->debug_lcd_dump_path = g_strdup("xp3200s-lcd.pgm");
    bms->nand_image_path = NULL;
    bms->sd_image_path = NULL;
    bms->nand_id = XP3200S_NAND_DEFAULT_ID;
    bms->audio_stream_path = NULL;
    bms->audio_player_path = NULL;
    bms->usb_connected = true;
    bms->key_matrix = false;
    bms->touch_p0_low = true;
    bms->debug_timer16_factors = false;
    bms->debug_sd_insert_ms = 0;
    bms->trace_io = false;
    bms->trace_key_scan = false;
    bms->trace_audio = false;
    bms->debug_key_code = 0xff;
    bms->debug_key_matrix = 0xff;
    bms->strict_board_io = false;
    bms->is_9288 = false;
    object_property_add_bool(obj, "key-matrix",
                             xp3200s_get_key_matrix,
                             xp3200s_set_key_matrix);
    object_property_set_description(obj, "key-matrix",
                                    "Model the keypad as the 8x7 scan matrix "
                                    "instead of the four K5 key lines; "
                                    "default is off (the XP3200 keypad is "
                                    "wired to K5[3:0] + P0.4/P0.5)");
    object_property_add_bool(obj, "usb-connected",
                             xp3200s_get_usb_connected,
                             xp3200s_set_usb_connected);
    object_property_set_description(obj, "usb-connected",
                                    "Expose USB VBUS on board status "
                                    "register 0x01000022 bit 7");
    object_property_add_uint32_ptr(obj, "debug-usb-wakeup-ms",
                                   &bms->debug_usb_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-usb-wakeup-ms",
                                    "Set the USB interrupt factor after this "
                                    "many host milliseconds; 0 disables it");
    object_property_add_uint32_ptr(obj, "debug-sd-insert-ms",
                                   &bms->debug_sd_insert_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-sd-insert-ms",
                                    "Start with no SD card and insert it "
                                    "after this many host milliseconds");
    object_property_add_uint32_ptr(obj, "debug-nmi-wakeup-ms",
                                   &bms->debug_nmi_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-nmi-wakeup-ms",
                                    "Raise NMI after this many host "
                                    "milliseconds; 0 disables it");
    object_property_add_uint32_ptr(obj, "debug-port4-wakeup-ms",
                                   &bms->debug_port4_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-port4-wakeup-ms",
                                    "Set the port input 4 interrupt factor "
                                    "after this many host milliseconds; "
                                    "0 disables it");
    object_property_add_uint32_ptr(obj, "debug-port5-wakeup-ms",
                                   &bms->debug_port5_wakeup_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-port5-wakeup-ms",
                                    "Drive the diagnostic P05/FPT5 input low "
                                    "after this many host milliseconds; "
                                    "0 disables it");
    object_property_add_uint32_ptr(obj, "debug-key-p0-low-mask",
                                   &bms->debug_key_p0_low_mask,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-key-p0-low-mask",
                                    "Force selected P0 key-matrix input bits "
                                    "low from reset for scan tracing");
    object_property_add_uint32_ptr(obj, "touch-fpt",
                                   &bms->touch_fpt,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "touch-fpt",
                                     "Route QEMU touch pen-down to FPT4-FPT7; "
                                     "default is 6");
    object_property_add_uint32_ptr(obj, "touch-k5-low-mask",
                                   &bms->touch_k5_low_mask,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "touch-k5-low-mask",
                                    "Also drive selected K5 input bits low "
                                    "while QEMU touch is down; 0 disables it");
    object_property_add_bool(obj, "touch-p0-low",
                             xp3200s_get_touch_p0_low,
                             xp3200s_set_touch_p0_low);
    object_property_set_description(obj, "touch-p0-low",
                                    "Also expose QEMU touch pen-down as the "
                                    "selected P0/FPT bit low in GPIO reads; "
                                    "default is on");
    object_property_add_bool(obj, "debug-timer16-factors",
                             xp3200s_get_debug_timer16_factors,
                             xp3200s_set_debug_timer16_factors);
    object_property_set_description(obj, "debug-timer16-factors",
                                    "Diagnostic-only: log modeled 16-bit "
                                    "timer compare matches; default is off");
    object_property_add_uint32_ptr(obj, "debug-key-rc",
                                   &bms->debug_key_rc,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-key-rc",
                                    "Diagnostic-only: hold matrix key "
                                    "row*14+col (row 1-5, col 1-14); the "
                                    "column is presented on the K5D read "
                                    "whose auto-scan counter matches. "
                                    "0xffff disables it");
    object_property_add_uint32_ptr(obj, "trace-io-all-ms",
                                   &bms->trace_io_all_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "trace-io-all-ms",
                                    "Diagnostic-only: from this virtual ms, "
                                    "log every IO write for trace-io-all-len "
                                    "ms (0 disables)");
    object_property_add_uint32_ptr(obj, "trace-io-all-len",
                                   &bms->trace_io_all_len,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "trace-io-all-len",
                                    "Diagnostic-only: window length in ms for "
                                    "trace-io-all-ms (default 2000)");
    object_property_add_uint32_ptr(obj, "debug-key-rowsel",
                                   &bms->debug_key_rowsel,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-key-rowsel",
                                    "Diagnostic-only: only present the keypad "
                                    "key while 0x300F46 reads this value "
                                    "(0x100 disables)");
    object_property_add_uint32_ptr(obj, "audio-force-data-in",
                                   &bms->audio_force_data_in,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "audio-force-data-in",
                                    "Diagnostic-only: force 0x3A0000 (decoder "
                                    "data read) to this value; 0x10000 disables");
    object_property_add_uint32_ptr(obj, "debug-call-ms",
                                   &bms->debug_call_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-call-ms",
                                    "Diagnostic-only: after this many ms, "
                                    "push the current PC and jump to "
                                    "debug-call-addr; 0 disables it");
    object_property_add_uint32_ptr(obj, "debug-call-addr",
                                   &bms->debug_call_addr,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-call-addr",
                                    "Diagnostic-only: guest address to jump "
                                    "to (see debug-call-ms)");
    object_property_add_uint32_ptr(obj, "debug-lcd-dump-ms",
                                   &bms->debug_lcd_dump_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-lcd-dump-ms",
                                    "Dump the LCD framebuffer as PGM "
                                    "after this many host milliseconds; "
                                    "0 disables it");
    object_property_add_uint32_ptr(obj, "debug-lcd-dump-seq",
                                   &bms->debug_lcd_dump_seq,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-lcd-dump-seq",
                                    "Diagnostic-only: write a new non-zero "
                                    "value to dump the LCD to "
                                    "<debug-lcd-dump-path>.<value>.pgm");
    object_property_add_str(obj, "debug-lcd-dump-path",
                            xp3200s_get_debug_lcd_dump_path,
                            xp3200s_set_debug_lcd_dump_path);
    object_property_set_description(obj, "debug-lcd-dump-path",
                                    "Path for debug-lcd-dump-ms output PGM");
    object_property_add_str(obj, "nand-image",
                            xp3200s_get_nand_image_path,
                            xp3200s_set_nand_image_path);
    object_property_set_description(obj, "nand-image",
                                    "Persistent raw 256 MiB NAND image, "
                                    "including 64-byte OOB per 2 KiB page");
    object_property_add_str(obj, "sd-image",
                            xp3200s_get_sd_image_path,
                            xp3200s_set_sd_image_path);
    object_property_set_description(obj, "sd-image",
                                    "SD card image exposed on the S1C33L05 "
                                    "MMC (SPI) interface as drive A:; the "
                                    "image holds a FAT16 file system");
    object_property_add_uint32_ptr(obj, "nand-linear", &bms->nand_linear,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-linear",
                                    "Map logical NAND pages linearly (mod 8192)");
    object_property_add_uint32_ptr(obj, "nand-id", &bms->nand_id,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-id",
                                    "Four NAND ID bytes, most significant "
                                    "byte first (default 0xecda1095)");
    object_property_add_str(obj, "audio-stream",
                            xp3200s_get_audio_stream_path,
                            xp3200s_set_audio_stream_path);
    object_property_set_description(obj, "audio-stream",
                                    "Write the compressed audio stream "
                                    "received by the board decoder to this "
                                    "path");
    object_property_add_str(obj, "audio-inject",
                            xp3200s_get_audio_inject_path,
                            xp3200s_set_audio_inject_path);
    object_property_set_description(obj, "audio-inject",
                                    "Diagnostic-only: feed this MP3 file "
                                    "into the decoder data path from the "
                                    "model side");
    object_property_add_uint32_ptr(obj, "audio-inject-ms",
                                   &bms->audio_inject_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "audio-inject-ms",
                                    "Diagnostic-only: start audio-inject "
                                    "after this many ms (default 65000)");
    object_property_add_str(obj, "audio-player",
                            xp3200s_get_audio_player_path,
                            xp3200s_set_audio_player_path);
    object_property_set_description(obj, "audio-player",
                                    "Launch this ffplay-compatible program "
                                    "and stream decoded-board audio to it");
    object_property_add_bool(obj, "trace-io",
                             xp3200s_get_trace_io,
                             xp3200s_set_trace_io);
    object_property_set_description(obj, "trace-io",
                                    "Log every BBK board, S1C33 IO, and LCDC "
                                    "IO access; default logs first accesses");
    object_property_add_uint32_ptr(obj, "debug-key-matrix",
                                   &bms->debug_key_matrix,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-key-matrix",
                                    "Diagnostic-only: hold matrix key "
                                    "row*8+col on the keypad (8 rows driven "
                                    "via 0x300F46, 4 columns on K5); "
                                    "0xff disables it");
    object_property_add_uint32_ptr(obj, "debug-key-code",
                                   &bms->debug_key_code,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-key-code",
                                    "Diagnostic-only: present this 5-bit "
                                    "keypad code (K5[3:0] | K6.4) on every "
                                    "host key event instead of the mapped "
                                    "key; 0xff disables it");
    object_property_add_bool(obj, "trace-audio",
                             xp3200s_get_trace_audio,
                             xp3200s_set_trace_audio);
    object_property_set_description(obj, "trace-audio",
                                    "Log every decoder port (0x003a0000) "
                                    "access; default off");
    object_property_add_bool(obj, "trace-key-scan",
                             xp3200s_get_trace_key_scan,
                             xp3200s_set_trace_key_scan);
    object_property_set_description(obj, "trace-key-scan",
                                    "Log compact P0/P1 keyboard scan state");
    object_property_add_bool(obj, "strict-board-io",
                             xp3200s_get_strict_board_io,
                             xp3200s_set_strict_board_io);
    object_property_set_description(obj, "strict-board-io",
                                    "Treat unknown board IO registers as "
                                    "non-loopback zeros for hardware-detect "
                                    "experiments");
}

static void xp3200s_machine_instance_finalize(Object *obj)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    g_free(bms->debug_lcd_dump_path);
    bms->debug_lcd_dump_path = NULL;
    g_free(bms->nand_image_path);
    bms->nand_image_path = NULL;
    g_free(bms->audio_stream_path);
    bms->audio_stream_path = NULL;
    g_free(bms->audio_player_path);
    bms->audio_player_path = NULL;
}

static void xp3200s_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "XP3200S (Epson S1C33L05, dev)";
    mc->init = xp3200s_init;
    mc->default_cpu_type = TYPE_C33L05_CPU;
    mc->default_ram_size = XP3200S_DEFAULT_RAM;
    mc->default_ram_id = "xp3200s.sdram";
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static void xp3200_machine_instance_init(Object *obj)
{
    XP3200SMachineState *bms = XP3200S_MACHINE(obj);

    bms->is_9288 = true;
    g_free(bms->debug_lcd_dump_path);
    bms->debug_lcd_dump_path = g_strdup("xp3200-lcd.pgm");
}

static void xp3200_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "XP3200 (Epson S1C33L05, 320x240, 8MiB SDRAM)";
    mc->default_ram_id = "xp3200.sdram";
}

static const TypeInfo xp3200s_machine_type_info = {
    .name = TYPE_XP3200S_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(XP3200SMachineState),
    .instance_init = xp3200s_machine_instance_init,
    .instance_finalize = xp3200s_machine_instance_finalize,
    .class_init = xp3200s_machine_class_init,
};

static const TypeInfo xp3200_machine_type_info = {
    .name = TYPE_XP3200_MACHINE,
    .parent = TYPE_XP3200S_MACHINE,
    .instance_init = xp3200_machine_instance_init,
    .class_init = xp3200_machine_class_init,
};

static void xp3200s_machine_register_types(void)
{
    type_register_static(&xp3200s_machine_type_info);
    type_register_static(&xp3200_machine_type_info);
}

type_init(xp3200s_machine_register_types)




