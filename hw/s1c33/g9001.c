/*
 * SII G9001 (SR-G9001) research machine - Epson S1C33E08.
 *
 * Derived from the BBK 9288S machine (hw/s1c33/bbk9288s.c) of this fork.
 * The CPU core, the QEMU machine plumbing, the LCD rendering helpers, the
 * NAND backend layout and the debug-PGM export all follow 9288S; everything
 * board specific is new because the E08 memory map and peripherals differ.
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
#include "system/system.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/pixel_ops.h"

#define TYPE_G9001_MACHINE   MACHINE_TYPE_NAME("g9001")
OBJECT_DECLARE_SIMPLE_TYPE(G9001MachineState, G9001_MACHINE)

/*
 * ---------------------------------------------------------------------------
 * E08 memory map (Technical Manual I.6, p107)
 * ---------------------------------------------------------------------------
 *   Area 0   0x00000000-0x0001FFFF  A0RAM 8 KiB (mirrored over the 128 KiB)
 *   Area 1   0x00020000-0x0005FFFF  specific ROM area (reserved)
 *   Area 2   0x00060000-0x0007FFFF  reserved for debugging
 *   Area 3   0x00080000-0x000FFFFF  internal RAM (IVRAM 12 KiB + DST 2 KiB)
 *   Area 4   0x00100000-0x001FFFFF  external, 1 MiB   <- card #CE4 default
 *   Area 5   0x00200000-0x002FFFFF  external, 1 MiB
 *   Area 6   0x00300000-0x003FFFFF  IP and peripherals (internal I/O)
 *   Area 7   0x00400000-0x005FFFFF  external, 2 MiB   <- card #CE7 (SDRAMC)
 *   ...
 *   Area 19  0x10000000-0x1FFFFFFF  external, 256 MiB <- SDRAM 16 MiB
 *
 * The G9001 firmware uses SDRAM on area 19 (0x10000000) and the NAND on the
 * card interface, which is a *memory window*: #CE/#RE/#WE come from the bus,
 * while CLE/ALE/#WP/RY-BY are plain GPIO pins driven by software (manual
 * V.4.4.1, figure V.4.4.1.1).
 */
#define G9001_A0RAM_BASE        0x00000000u
#define G9001_A0RAM_SIZE        (8 * KiB)
#define G9001_AREA1_BASE        0x00020000u
#define G9001_AREA1_SIZE        (256 * KiB)
#define G9001_IVRAM_BASE        0x00080000u
#define G9001_IVRAM_SIZE        (12 * KiB)
#define G9001_DSTRAM_BASE       0x00083000u
#define G9001_DSTRAM_SIZE       (2 * KiB)
#define G9001_CARD4_BASE        0x00100000u
#define G9001_CARD4_SIZE        (1 * MiB)
#define G9001_CARD14_BASE       0x03000000u
#define G9001_CARD14_SIZE       (16 * MiB)
#define G9001_AREA7_BASE        0x00400000u
#define G9001_AREA7_SIZE        (2 * MiB)
#define G9001_IO_BASE           0x00300000u
#define G9001_IO_SIZE           0x00004000u
#define G9001_SDRAM_BASE        0x10000000u
#define G9001_DEFAULT_RAM       (16 * MiB)
#define G9001_BOOT_STACK_TOP    0x00003f80u

/* S1C33E08 internal I/O register addresses (Area 6). */
#define G9001_IO_MISC_BOOT      0x000018   /* pMISC_BOOT */
#define G9001_IO_INT_PK01L      0x000262   /* key input priority */
#define G9001_IO_INT_PLCDC      0x000269   /* LCDC priority */
#define G9001_IO_INT_EK01_EP03  0x000270
#define G9001_IO_INT_E16T01     0x000272
#define G9001_IO_INT_ELCDC      0x000278
#define G9001_IO_INT_FK01_FP03  0x000280
#define G9001_IO_INT_F16T01     0x000282
#define G9001_IO_INT_FLCDC      0x000288
#define G9001_IO_CARDSETUP      0x000300   /* pCARDSETUP */
#define G9001_IO_CARDFUNCSEL    0x000302   /* pCARDFUNCSEL05 */
#define G9001_IO_ECC_TRIGSEL    0x000310
#define G9001_IO_ECC_RSTRDY     0x000311
#define G9001_IO_ECC_ENA        0x000312
#define G9001_IO_ECC_MD         0x000313
#define G9001_IO_ECC0_BASE      0x000314   /* ECC0CP..ECC1LPH, 8 bytes */
#define G9001_IO_SDRAMC_INI     0x001600
#define G9001_SDRAMC_INI_READY  0x18       /* init done + ready */
#define G9001_IO_SDRAMC_CTL     0x001604
#define G9001_IO_SDRAMC_REF     0x001608
#define G9001_IO_SDRAMC_APP     0x001610
#define G9001_IO_SRAMC_BCLK     0x001500
#define G9001_IO_SRAMC_SWAIT    0x001504
#define G9001_IO_SRAMC_SIZE     0x001508
#define G9001_IO_SRAMC_BSL      0x00150c
#define G9001_IO_SRAMC_ALS      0x001510
#define G9001_IO_SPI_RXD        0x001700
#define G9001_IO_SPI_TXD        0x001704
#define G9001_IO_SPI_CTL1       0x001708
#define G9001_IO_SPI_CTL2       0x00170c
#define G9001_IO_SPI_WAIT       0x001710
#define G9001_IO_SPI_STAT       0x001714
#define G9001_IO_SPI_INT        0x001718
#define G9001_IO_SPI_RXMK       0x00171c
#define G9001_IO_LCDC_INT       0x001a00
#define G9001_IO_LCDC_PS        0x001a04
#define G9001_IO_LCDC_HD        0x001a10
#define G9001_IO_LCDC_VD        0x001a14
#define G9001_IO_LCDC_MR        0x001a18
#define G9001_IO_LCDC_HDPS      0x001a20
#define G9001_IO_LCDC_VDPS      0x001a24
#define G9001_IO_LCDC_L         0x001a28
#define G9001_IO_LCDC_F         0x001a2c
#define G9001_IO_LCDC_FO        0x001a30
#define G9001_IO_LCDC_TSO       0x001a40
#define G9001_IO_LCDC_DMD       0x001a60
#define G9001_IO_LCDC_IRAM      0x001a64
#define G9001_IO_LCDC_MADD      0x001a70
#define G9001_IO_LCDC_MLADD     0x001a74
#define G9001_IO_CMU_GATEDCLK0  0x001b00
#define G9001_IO_CMU_GATEDCLK1  0x001b04
#define G9001_IO_CMU_CLKCNTL    0x001b08
#define G9001_IO_CMU_OPT        0x001b14
#define G9001_IO_CMU_PROTECT    0x001b24
#define G9001_IO_T16_0CA        0x000780   /* pT16_CA0 */
#define G9001_IO_T16_0CB        0x000782   /* pT16_CB0 */
#define G9001_IO_T16_0TC        0x000784   /* pT16_TC0 */
#define G9001_IO_T16_0CTL       0x000786   /* pT16_CTL0 */
#define G9001_IO_WDT            0x001830

/* GPIO: PnD = 0x300380 + 2n, PnDIR = PnD + 1, Pxy function = 0x3003A0 + ... */
#define G9001_IO_PORT_BASE      0x000380
#define G9001_IO_PORT_FUNC_BASE 0x0003a0
#define G9001_IO_KEYINTSEL      0x0003d0
#define G9001_IO_KEYCMP0        0x0003d2
#define G9001_IO_KEYCMP1        0x0003d3
#define G9001_IO_KEYMASK0       0x0003d4
#define G9001_IO_KEYMASK1       0x0003d5
#define G9001_KEY_PORTS         10
#define G9001_KEY_ROWS          8
#define G9001_KEY_COLS          8

/* LCDC: 640x480 4bpp mono TFT, 324 B/line, VRAM in SDRAM (see G9001 LCD doc). */
#define G9001_LCD_WIDTH         640
#define G9001_LCD_HEIGHT        480
#define G9001_LCD_BPP           4
#define G9001_LCD_STRIDE        324
#define G9001_LCD_MAX_BPP       8
#define G9001_LCDC_PSAVE_MASK   0x03
#define G9001_LCDC_DMD_TFTSEL   0x80000000u
#define G9001_LCDC_DMD_LUTPASS  0x00000010u
#define G9001_LCDC_DMD_BPP_MASK 0x00000007u

/*
 * NAND: 1 GiB, 2048+64 byte pages, 64 pages/block.
 *
 * The geometry is proven by the boot loader itself: it reads records with a
 * 4-cycle address (1 column byte + 3 row bytes, r15=1/r14=3) and record m
 * lands at byte offset m * 2112.  A 4096+128 page would put it at m * 4224.
 * The dump is 8184 blocks of 135168 bytes = 8192 blocks (1 GiB) minus the
 * eight bad blocks that were skipped when it was taken.
 */
#define G9001_NAND_PAGE_DATA    2048
#define G9001_NAND_PAGE_OOB     64
#define G9001_NAND_RAW_PAGE     (G9001_NAND_PAGE_DATA + G9001_NAND_PAGE_OOB)
#define G9001_NAND_PAGES_BLOCK  64
#define G9001_NAND_BLOCK_SIZE   (G9001_NAND_RAW_PAGE * G9001_NAND_PAGES_BLOCK)
#define G9001_NAND_BLOCKS       8184
#define G9001_NAND_TOTAL        ((uint64_t)G9001_NAND_BLOCK_SIZE * \
                                 G9001_NAND_BLOCKS)
#define G9001_NAND_SMALL_PAGE   512
#define G9001_NAND_SMALL_OOB    16
#define G9001_NAND_DEFAULT_ID   0xecda1095u

#define G9001_TRACE_RING        64
#define G9001_MMIO_TRACE_LEN    32

typedef struct G9001TraceEntry {
    uint16_t offset;
    uint8_t size;
    bool write;
    uint32_t pc;
    uint32_t value;
} G9001TraceEntry;

typedef enum G9001NandPhase {
    G9001_NAND_IDLE,
    G9001_NAND_READ,
    G9001_NAND_PROGRAM,
    G9001_NAND_ERASE,
    G9001_NAND_STATUS,
    G9001_NAND_ID,
} G9001NandPhase;

typedef struct G9001Nand {
    int fd;
    uint64_t size;
    uint32_t blocks;
    bool small_page;
    bool ready_low;
    bool write_protected;

    uint8_t page[G9001_NAND_RAW_PAGE];
    int64_t cached_page;            /* -1 when invalid */
    bool page_dirty;

    uint8_t id[5];
    unsigned id_len;
    unsigned id_ptr;

    uint8_t cmd;
    G9001NandPhase phase;
    uint8_t addr[5];
    unsigned addr_count;
    uint32_t col;
    uint32_t row;                   /* page number inside the chip */
    uint32_t data_ptr;
    uint32_t data_end;
    unsigned ecc_sector;
    bool program_loaded;
} G9001Nand;

typedef struct G9001MachineState {
    MachineState parent;

    char *nand_image_path;
    uint32_t nand_id;
    bool nand_save;
    bool nand_small_page;
    char *debug_lcd_dump_path;
    uint32_t debug_lcd_dump_ms;
    uint32_t debug_pc_sample_ms;
    bool trace_io;
    bool trace_nand;
    bool trace_irq;
    uint32_t t16_hz;
    uint32_t nand_cle_pin;          /* port * 8 + bit, 0xffffffff = not wired */
    uint32_t nand_ale_pin;
    uint32_t nand_ryby_pin;
    uint32_t nand_ecc_offset;
} G9001MachineState;

typedef struct G9001T16 {
    bool running;
    uint16_t counter;
    uint16_t ca;
    uint16_t cb;
    uint8_t ctl;
    uint64_t frac;                  /* fractional count accumulator */
    uint64_t last_ns;
} G9001T16;

typedef struct G9001State {
    uint8_t io[G9001_IO_SIZE];
    uint8_t lcdc_seen[G9001_IO_SIZE];
    uint8_t port_prev[G9001_KEY_PORTS];
    bool lcd_dirty;
    bool trace_io;
    bool trace_nand;
    bool trace_irq;

    /* LCD */
    QemuConsole *con;
    uint32_t lcd_present_seq;
    int64_t lcd_last_dump;

    /* NAND */
    G9001Nand nand;
    uint8_t nand_ecc[8];            /* last ECC bytes of the current page */
    uint32_t nand_ecc_offset;

    /* interrupt controller */
    uint8_t irq_pending_mask[16];
    uint8_t irq_level[16];
    uint8_t irq_source[16];
    bool irq_active;
    uint8_t irq_vector;
    uint8_t irq_prio;

    /* 16-bit timer channel 0 (system tick source) */
    G9001T16 t16;
    QEMUTimer *t16_timer;
    uint32_t t16_hz;

    /* keyboard */
    uint8_t key_state[G9001_KEY_COLS][G9001_KEY_ROWS];
    uint8_t key_col_drive;

    /* tracing */
    G9001TraceEntry trace[G9001_TRACE_RING];
    unsigned trace_head;
    unsigned trace_count;
    bool trace_nand_window_seen;

    /* machine */
    MachineState *machine;
    S1C33CPU *cpu;
    MemoryRegion a0ram;
    MemoryRegion area1;
    MemoryRegion ivram;
    MemoryRegion dstram;
    MemoryRegion io_region;
    MemoryRegion card4;
    MemoryRegion card14;
    MemoryRegion sdram_alias;
    uint8_t *a0ram_ptr;
    uint8_t *ivram_ptr;
    uint8_t *sdram_ptr;

    QEMUTimer *lcd_dump_timer;
    QEMUTimer *pc_sample_timer;
    QemuInputHandlerState *kbd_handler;
} G9001State;

static G9001State *g9001_active = NULL;

static uint8_t g9001_port_data(G9001State *s, unsigned port);
static uint8_t g9001_port_dir(G9001State *s, unsigned port);
static uint8_t g9001_port_pins(G9001State *s, unsigned port);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t g9001_current_pc(void)
{
    if (g9001_active == NULL || g9001_active->cpu == NULL) {
        return 0;
    }
    return g9001_active->cpu->env.pc;
}

static void g9001_trace_push(G9001State *s, uint16_t offset, unsigned size,
                             bool write, uint32_t value)
{
    G9001TraceEntry *e = &s->trace[s->trace_head];

    e->offset = offset;
    e->size = size;
    e->write = write;
    e->pc = g9001_current_pc();
    e->value = value;
    s->trace_head = (s->trace_head + 1) % G9001_TRACE_RING;
    if (s->trace_count < G9001_TRACE_RING) {
        s->trace_count++;
    }
}

static void g9001_trace_dump(G9001State *s, const char *reason)
{
    unsigned i;

    info_report("g9001: I/O history before %s", reason);
    for (i = 0; i < s->trace_count; i++) {
        unsigned idx = (s->trace_head + G9001_TRACE_RING - s->trace_count + i) %
                       G9001_TRACE_RING;
        G9001TraceEntry *e = &s->trace[idx];

        fprintf(stderr, "  pc=0x%08x %s off=0x%04x size=%u val=0x%08x\n",
                e->pc, e->write ? "wr" : "rd", e->offset, e->size, e->value);
    }
}

/* ------------------------------------------------------------------------- */
/* NAND backend                                                              */
/* ------------------------------------------------------------------------- */

static bool g9001_nand_open(G9001State *s, const char *path)
{
    G9001Nand *n = &s->nand;
    struct stat st;

    n->cached_page = -1;
    n->fd = -1;
    n->blocks = G9001_NAND_BLOCKS;

    if (path == NULL || path[0] == 0) {
        return false;
    }
    n->fd = open(path, O_RDONLY | O_BINARY);
    if (n->fd < 0) {
        error_report("g9001: cannot open NAND image '%s': %s", path,
                     strerror(errno));
        return false;
    }
    if (fstat(n->fd, &st) != 0 || st.st_size <= 0) {
        error_report("g9001: cannot stat NAND image '%s'", path);
        close(n->fd);
        n->fd = -1;
        return false;
    }
    n->size = st.st_size;
    n->blocks = n->size / G9001_NAND_BLOCK_SIZE;
    info_report("g9001: NAND image '%s' %" PRIu64 " bytes = %u blocks "
                "(%u x %u B pages)", path, n->size, n->blocks,
                G9001_NAND_PAGES_BLOCK, G9001_NAND_RAW_PAGE);
    return true;
}

static void g9001_nand_close(G9001State *s)
{
    if (s->nand.fd >= 0) {
        close(s->nand.fd);
        s->nand.fd = -1;
    }
}

static bool g9001_nand_load_page(G9001State *s, uint32_t page)
{
    G9001Nand *n = &s->nand;
    off_t off;
    ssize_t got;

    if (n->cached_page == (int64_t)page) {
        return true;
    }
    if (n->fd < 0) {
        memset(n->page, 0xff, sizeof(n->page));
        n->cached_page = page;
        return false;
    }
    off = (off_t)page * G9001_NAND_RAW_PAGE;
    if ((uint64_t)off >= n->size) {
        memset(n->page, 0xff, sizeof(n->page));
        n->cached_page = page;
        return false;
    }
    got = pread(n->fd, n->page, sizeof(n->page), off);
    if (got < 0) {
        got = 0;
    }
    if ((size_t)got < sizeof(n->page)) {
        memset(n->page + got, 0xff, sizeof(n->page) - got);
    }
    n->cached_page = page;
    n->page_dirty = false;
    /* "ECC always correct": expose the page's stored parity bytes. */
    if (s->nand_ecc_offset + sizeof(s->nand_ecc) <= sizeof(n->page)) {
        memcpy(s->nand_ecc, n->page + s->nand_ecc_offset,
               sizeof(s->nand_ecc));
    }
    return true;
}

static void g9001_nand_store_page(G9001State *s)
{
    G9001Nand *n = &s->nand;

    if (n->fd < 0 || n->cached_page < 0 || !n->page_dirty) {
        return;
    }
    if (s->machine != NULL &&
        G9001_MACHINE(s->machine)->nand_save) {
        ssize_t wrote = pwrite(n->fd, n->page, sizeof(n->page),
                               (off_t)n->cached_page * G9001_NAND_RAW_PAGE);

        (void)wrote;
    }
    n->page_dirty = false;
}

static uint32_t g9001_nand_page_of(uint32_t row)
{
    return row;
}

static void g9001_nand_reset(G9001State *s)
{
    G9001Nand *n = &s->nand;

    n->cmd = 0xff;
    n->phase = G9001_NAND_IDLE;
    n->addr_count = 0;
    n->col = 0;
    n->row = 0;
    n->data_ptr = 0;
    n->data_end = 0;
    n->program_loaded = false;
    n->ready_low = false;
    n->id_len = 4;
    n->id[0] = (s->machine != NULL ?
                G9001_MACHINE(s->machine)->nand_id : G9001_NAND_DEFAULT_ID) >> 24;
    n->id[1] = (s->machine != NULL ?
                G9001_MACHINE(s->machine)->nand_id : G9001_NAND_DEFAULT_ID) >> 16;
    n->id[2] = (s->machine != NULL ?
                G9001_MACHINE(s->machine)->nand_id : G9001_NAND_DEFAULT_ID) >> 8;
    n->id[3] = (s->machine != NULL ?
                G9001_MACHINE(s->machine)->nand_id : G9001_NAND_DEFAULT_ID);
    n->id_ptr = 0;
}

/*
 * Latch the address bytes collected so far into column/row.  The E08 boot ROM
 * uses 4 address cycles for a small-page device (1 column + 3 row bytes) and 5
 * cycles for a large-page device (2 column + 3 row bytes); the MBR reuses the
 * same convention, so the cycle count alone tells us how to split them.
 */
static void g9001_nand_latch_addr(G9001State *s)
{
    G9001Nand *n = &s->nand;

    switch (n->addr_count) {
    case 5:
        n->col = n->addr[0] | (n->addr[1] << 8);
        n->row = n->addr[2] | (n->addr[3] << 8) | ((n->addr[4] & 0x0f) << 16);
        break;
    case 4:
        n->col = n->addr[0];
        n->row = n->addr[1] | (n->addr[2] << 8) | (n->addr[3] << 16);
        break;
    case 3:
        n->col = n->addr[0];
        n->row = n->addr[1] | (n->addr[2] << 8);
        break;
    case 2:
        n->col = n->addr[0];
        n->row = n->addr[1];
        break;
    default:
        break;
    }
    if (s->trace_nand && n->addr_count) {
        info_report("g9001-nand: latch %u bytes -> col=%u row=%u "
                    "(byte offset %" PRIu64 ")",
                    n->addr_count, n->col, n->row,
                    (uint64_t)n->row * G9001_NAND_RAW_PAGE + n->col);
    }
    n->addr_count = 0;
}

/*
 * The E08 card interface reads a NAND page as 512-byte sectors, each followed
 * by its 8-byte ECC (the hardware ECC unit checks exactly that layout, see
 * manual V.4.6).  The ECC bytes of all sectors live at the start of the spare
 * area, so the *stream* the firmware sees is 4 x (512 data + 8 ECC) = 2080
 * bytes for a 2 KiB page.
 */
#define G9001_NAND_SECTOR_DATA  512
#define G9001_NAND_SECTOR_ECC   8
#define G9001_NAND_SECTOR_STRIDE \
    (G9001_NAND_SECTOR_DATA + G9001_NAND_SECTOR_ECC)
#define G9001_NAND_SECTORS      (G9001_NAND_PAGE_DATA / \
                                 G9001_NAND_SECTOR_DATA)
#define G9001_NAND_STREAM_SIZE \
    (G9001_NAND_SECTOR_STRIDE * G9001_NAND_SECTORS)
#define G9001_NAND_ECC_AREA     G9001_NAND_PAGE_DATA

static unsigned g9001_nand_stream_offset(unsigned pos)
{
    unsigned sector = pos / G9001_NAND_SECTOR_STRIDE;
    unsigned off = pos % G9001_NAND_SECTOR_STRIDE;

    if (sector >= G9001_NAND_SECTORS) {
        return G9001_NAND_STREAM_SIZE;
    }
    if (off < G9001_NAND_SECTOR_DATA) {
        return sector * G9001_NAND_SECTOR_DATA + off;
    }
    return G9001_NAND_ECC_AREA + sector * G9001_NAND_SECTOR_ECC +
           (off - G9001_NAND_SECTOR_DATA);
}

/*
 * Feed one byte into the NAND command/address/data state machine.
 * `command` is true when the CLE line is asserted, `address` when ALE is.
 */
static void g9001_nand_byte(G9001State *s, bool command, bool address,
                            bool write, uint8_t *value)
{
    G9001Nand *n = &s->nand;
    uint32_t page;

    if (command) {
        n->cmd = *value;
        n->addr_count = 0;
        n->data_ptr = 0;
        n->program_loaded = false;
        if (s->trace_nand) {
            info_report("g9001-nand: cmd 0x%02x pc=0x%08x", *value,
                        g9001_current_pc());
        }
        switch (n->cmd) {
        case 0x00:
            n->phase = G9001_NAND_READ;
            n->addr_count = 0;
            break;
        case 0x30:
            /* read confirm: latch address and load the page */
            g9001_nand_latch_addr(s);
            page = g9001_nand_page_of(n->row);
            g9001_nand_load_page(s, page);
            n->data_ptr = n->col;
            n->data_end = sizeof(n->page);
            n->phase = G9001_NAND_READ;
            break;
        case 0x80:
            n->phase = G9001_NAND_PROGRAM;
            n->addr_count = 0;
            break;
        case 0x10:
            g9001_nand_latch_addr(s);
            page = g9001_nand_page_of(n->row);
            g9001_nand_load_page(s, page);
            n->data_ptr = n->col;
            n->data_end = sizeof(n->page);
            n->phase = G9001_NAND_STATUS;
            break;
        case 0x60:
            n->phase = G9001_NAND_ERASE;
            n->addr_count = 0;
            break;
        case 0xd0:
            if (n->addr_count >= 3) {
                if (n->addr_count >= 5) {
                    n->row = n->addr[2] | (n->addr[3] << 8) |
                             ((n->addr[4] & 0x0f) << 16);
                } else {
                    n->row = n->addr[1] | (n->addr[2] << 8);
                }
            }
            page = (n->row & ~(G9001_NAND_PAGES_BLOCK - 1));
            g9001_nand_load_page(s, page);
            memset(n->page, 0xff, sizeof(n->page));
            n->page_dirty = true;
            g9001_nand_store_page(s);
            n->phase = G9001_NAND_STATUS;
            break;
        case 0x70:
            n->phase = G9001_NAND_STATUS;
            break;
        case 0x90:
            n->phase = G9001_NAND_ID;
            n->id_ptr = 0;
            break;
        case 0xff:
            g9001_nand_reset(s);
            break;
        default:
            break;
        }
        return;
    }

    if (address) {
        if (n->addr_count < ARRAY_SIZE(n->addr)) {
            n->addr[n->addr_count++] = *value;
        }
        if (s->trace_nand) {
            info_report("g9001-nand: addr[%u] = 0x%02x pc=0x%08x",
                        n->addr_count - 1, *value, g9001_current_pc());
        }
        return;
    }

    /* data port */
    if (write) {
        switch (n->phase) {
        case G9001_NAND_PROGRAM:
            if (n->addr_count >= 3) {
                g9001_nand_latch_addr(s);
                n->data_ptr = n->col;
                g9001_nand_load_page(s, g9001_nand_page_of(n->row));
            }
            if (n->data_ptr < G9001_NAND_STREAM_SIZE) {
                unsigned off = g9001_nand_stream_offset(n->data_ptr);

                if (off < sizeof(n->page)) {
                    n->page[off] = *value;
                    n->page_dirty = true;
                    n->program_loaded = true;
                }
                n->data_ptr++;
            }
            break;
        default:
            break;
        }
        return;
    }

    /*
     * Address bytes are followed by data even without a confirm command
     * (the small-page read sequence), so latch as soon as data starts.
     */
    if (n->addr_count != 0) {
        g9001_nand_latch_addr(s);
        g9001_nand_load_page(s, g9001_nand_page_of(n->row));
        n->data_ptr = n->col;
        n->data_end = G9001_NAND_STREAM_SIZE;
        if (n->phase == G9001_NAND_IDLE) {
            n->phase = G9001_NAND_READ;
        }
    }
    if ((n->data_ptr % G9001_NAND_SECTOR_STRIDE) ==
        G9001_NAND_SECTOR_DATA) {
        n->ecc_sector = n->data_ptr / G9001_NAND_SECTOR_STRIDE;
    }

    switch (n->phase) {
    case G9001_NAND_READ: {
        unsigned off;

        if (n->data_ptr < G9001_NAND_STREAM_SIZE) {
            off = g9001_nand_stream_offset(n->data_ptr);
            *value = off < sizeof(n->page) ? n->page[off] : 0xff;
            n->data_ptr++;
        } else {
            *value = 0xff;
        }
        break;
    }
    case G9001_NAND_STATUS:
        *value = n->ready_low ? 0x00 : 0xc0;
        break;
    case G9001_NAND_ID:
        *value = n->id_ptr < n->id_len ? n->id[n->id_ptr++] : 0xff;
        break;
    default:
        *value = 0xff;
        break;
    }
}

/*
 * The card window is a plain memory area. CLE/ALE are GPIO pins; when they
 * are not wired up we fall back to "every access is a data access", which is
 * enough for the boot ROM emulation and for tracing the real pin usage.
 */
static bool g9001_pin_level(G9001State *s, uint32_t pin)
{
    unsigned port;
    unsigned bit;

    if (pin == 0xffffffff || s->machine == NULL) {
        return false;
    }
    port = pin / 8;
    bit = pin % 8;
    if (port >= G9001_KEY_PORTS) {
        return false;
    }
    return (g9001_port_data(s, port) >> bit) & 1;
}

static uint64_t g9001_card_read(void *opaque, hwaddr offset, unsigned size)
{
    G9001State *s = opaque;
    G9001MachineState *gms = G9001_MACHINE(s->machine);
    uint64_t ret = 0;
    bool cle = g9001_pin_level(s, gms->nand_cle_pin);
    bool ale = g9001_pin_level(s, gms->nand_ale_pin);
    unsigned i;

    if (!s->trace_nand_window_seen) {
        s->trace_nand_window_seen = true;
        info_report("g9001: first card window read at 0x%08" HWADDR_PRIx
                    " pc=0x%08x (cle=%d ale=%d)",
                    (hwaddr)G9001_CARD4_BASE + offset,
                    g9001_current_pc(), cle, ale);
        g9001_trace_dump(s, "first card window read");
    }

    for (i = 0; i < size; i++) {
        uint8_t v = 0xff;

        g9001_nand_byte(s, cle, ale, false, &v);
        ret |= (uint64_t)v << (8 * i);
    }
    return ret;
}

static void g9001_card_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    G9001State *s = opaque;
    G9001MachineState *gms = G9001_MACHINE(s->machine);
    bool cle = g9001_pin_level(s, gms->nand_cle_pin);
    bool ale = g9001_pin_level(s, gms->nand_ale_pin);
    unsigned i;

    if (!s->trace_nand_window_seen) {
        s->trace_nand_window_seen = true;
        info_report("g9001: first card window write at 0x%08" HWADDR_PRIx
                    " value=0x%" PRIx64 " pc=0x%08x (cle=%d ale=%d)",
                    (hwaddr)G9001_CARD4_BASE + offset, value,
                    g9001_current_pc(), cle, ale);
        g9001_trace_dump(s, "first card window write");
    }

    for (i = 0; i < size; i++) {
        uint8_t v = (value >> (8 * i)) & 0xff;

        g9001_nand_byte(s, cle, ale, true, &v);
    }
}

static const MemoryRegionOps g9001_card_ops = {
    .read = g9001_card_read,
    .write = g9001_card_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* ------------------------------------------------------------------------- */
/* LCD                                                                       */
/* ------------------------------------------------------------------------- */

static uint32_t g9001_lcdc32(G9001State *s, hwaddr off)
{
    return ldl_le_p(&s->io[off]);
}

static bool g9001_lcd_on(G9001State *s)
{
    return (s->io[G9001_IO_LCDC_PS] & G9001_LCDC_PSAVE_MASK) ==
           G9001_LCDC_PSAVE_MASK;
}

static unsigned g9001_lcd_bpp(G9001State *s)
{
    uint32_t dmd = g9001_lcdc32(s, G9001_IO_LCDC_DMD);

    switch (dmd & G9001_LCDC_DMD_BPP_MASK) {
    case 0:
        return 1;
    case 1:
        return 2;
    case 2:
        return 4;
    case 3:
        return 8;
    default:
        return G9001_LCD_BPP;
    }
}

static unsigned g9001_lcd_stride(G9001State *s, unsigned bpp)
{
    uint32_t mladd = g9001_lcdc32(s, G9001_IO_LCDC_MLADD);

    if (mladd != 0 && mladd <= 4096) {
        return mladd;
    }
    return G9001_LCD_STRIDE;
}

static uint8_t g9001_lcd_gray(G9001State *s, uint8_t pixel, unsigned bpp)
{
    uint32_t dmd = g9001_lcdc32(s, G9001_IO_LCDC_DMD); 
    uint32_t lut;
    uint8_t raw;

    if (dmd & G9001_LCDC_DMD_LUTPASS) {
        /* LUT bypassed: show the raw level spread over the full range. */
        if (bpp == 0) {
            return 0;
        }
        return pixel * 255 / ((1u << bpp) - 1);
    }
    /* LUT entry: one 32-bit register per level at 0x301AA0 + 4 * level. */
    lut = g9001_lcdc32(s, 0x1aa0 + pixel * 4);
    raw = (lut >> 8) & 0x3f;
    return (raw << 2) | (raw >> 4);
}

static uint32_t g9001_lcd_pixel(DisplaySurface *surface, uint8_t level)
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

static void g9001_lcd_store_pixel(DisplaySurface *surface, int x, int y,
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

static const uint8_t *g9001_lcd_vram(G9001State *s)
{
    uint32_t madd = g9001_lcdc32(s, G9001_IO_LCDC_MADD);
    uint64_t off;

    if (s->sdram_ptr == NULL) {
        return NULL;
    }
    if (madd < G9001_SDRAM_BASE ||
        madd >= G9001_SDRAM_BASE + G9001_DEFAULT_RAM) {
        return NULL;
    }
    off = madd - G9001_SDRAM_BASE;
    return s->sdram_ptr + off;
}

static void g9001_lcd_update(void *opaque)
{
    G9001State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    const uint8_t *vram = g9001_lcd_vram(s);
    uint32_t colors[1 << G9001_LCD_MAX_BPP];
    unsigned bpp;
    unsigned stride;
    unsigned x;
    unsigned y;

    if (surface == NULL || surface_bits_per_pixel(surface) == 0 ||
        vram == NULL || !g9001_lcd_on(s)) {
        return;
    }
    bpp = g9001_lcd_bpp(s);
    if (bpp > G9001_LCD_MAX_BPP) {
        bpp = 4;
    }
    stride = g9001_lcd_stride(s, bpp);
    for (x = 0; x < (1u << bpp); x++) {
        colors[x] = g9001_lcd_pixel(surface, g9001_lcd_gray(s, x, bpp));
    }

    for (y = 0; y < G9001_LCD_HEIGHT; y++) {
        const uint8_t *row = vram + (size_t)y * stride;

        for (x = 0; x < G9001_LCD_WIDTH; x++) {
            unsigned bit = x * bpp;
            uint8_t byte = row[bit >> 3];
            unsigned shift = 8 - bpp - (bit & 7);
            uint8_t pixel = (byte >> shift) & ((1u << bpp) - 1);

            g9001_lcd_store_pixel(surface, x, y, colors[pixel]);
        }
    }
    dpy_gfx_update(s->con, 0, 0, G9001_LCD_WIDTH, G9001_LCD_HEIGHT);
    s->lcd_dirty = false;
}

static void g9001_lcd_invalidate(void *opaque)
{
    G9001State *s = opaque;

    dpy_gfx_update(s->con, 0, 0, G9001_LCD_WIDTH, G9001_LCD_HEIGHT);
}

static const GraphicHwOps g9001_lcd_ops = {
    .invalidate = g9001_lcd_invalidate,
    .gfx_update = g9001_lcd_update,
};

static void g9001_lcd_dump_pgm(G9001State *s, const char *path)
{
    const uint8_t *vram = g9001_lcd_vram(s);
    g_autofree char *header = NULL;
    g_autofree uint8_t *image = NULL;
    unsigned bpp;
    unsigned stride;
    unsigned x;
    unsigned y;
    unsigned used[1 << G9001_LCD_MAX_BPP] = { 0 };
    GError *gerr = NULL;

    if (path == NULL || path[0] == 0) {
        path = "g9001-lcd.pgm";
    }
    if (vram == NULL) {
        info_report("g9001-lcd: no VRAM (MADD=0x%08x); dumping white",
                    g9001_lcdc32(s, G9001_IO_LCDC_MADD));
    }
    bpp = g9001_lcd_bpp(s);
    if (bpp > G9001_LCD_MAX_BPP) {
        bpp = 4;
    }
    stride = g9001_lcd_stride(s, bpp);
    header = g_strdup_printf("P5\n%u %u\n255\n", G9001_LCD_WIDTH,
                             G9001_LCD_HEIGHT);
    image = g_malloc(G9001_LCD_WIDTH * G9001_LCD_HEIGHT);

    for (y = 0; y < G9001_LCD_HEIGHT; y++) {
        for (x = 0; x < G9001_LCD_WIDTH; x++) {
            uint8_t pixel = 0;

            if (vram != NULL) {
                const uint8_t *row = vram + (size_t)y * stride;
                unsigned bit = x * bpp;
                uint8_t byte = row[bit >> 3];
                unsigned shift = 8 - bpp - (bit & 7);

                pixel = (byte >> shift) & ((1u << bpp) - 1);
            }
            used[pixel & 0xf]++;
            image[y * G9001_LCD_WIDTH + x] = g9001_lcd_gray(s, pixel, bpp);
        }
    }

    {
        GByteArray *out = g_byte_array_sized_new(strlen(header) +
                                                 G9001_LCD_WIDTH *
                                                 G9001_LCD_HEIGHT);
        g_byte_array_append(out, (const uint8_t *)header, strlen(header));
        g_byte_array_append(out, image, G9001_LCD_WIDTH * G9001_LCD_HEIGHT);
        if (!g_file_set_contents(path, (const char *)out->data, out->len,
                                 &gerr)) {
            info_report("g9001-lcd: dump to %s failed: %s", path,
                        gerr ? gerr->message : "unknown");
            g_clear_error(&gerr);
        } else {
            info_report("g9001-lcd: dumped %ux%u %ubpp to %s (levels 0..%u)",
                        G9001_LCD_WIDTH, G9001_LCD_HEIGHT, bpp, path,
                        (1u << bpp) - 1);
        }
        g_byte_array_unref(out);
    }
}

static void g9001_lcd_dump_cb(void *opaque)
{
    G9001State *s = opaque;
    G9001MachineState *gms = G9001_MACHINE(s->machine);

    g9001_lcd_dump_pgm(s, gms->debug_lcd_dump_path);
}

static void g9001_pc_sample_cb(void *opaque)
{
    G9001State *s = opaque;
    G9001MachineState *gms = G9001_MACHINE(s->machine);

    info_report("g9001-pc: pc=0x%08x sp=0x%08x psr=0x%08x ttbr=0x%08x "
                "lcd_ps=0x%02x madd=0x%08x t16(ctl=0x%02x cnt=%u ca=%u cb=%u "
                "ie=0x%02x il=0x%02x fl=0x%02x)",
                g9001_current_pc(), s->cpu->env.sp, s->cpu->env.psr,
                s->cpu->env.ttbr, s->io[G9001_IO_LCDC_PS],
                g9001_lcdc32(s, G9001_IO_LCDC_MADD),
                s->t16.ctl, s->t16.counter, s->t16.ca, s->t16.cb,
                s->io[G9001_IO_INT_E16T01], s->io[0x0266],
                s->io[0x0282]);
    timer_mod(s->pc_sample_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
              gms->debug_pc_sample_ms);
}

/* ------------------------------------------------------------------------- */
/* GPIO / keyboard                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Key matrix from docs/G9001_显示与键盘分析.md section 2:
 *   columns (driven low one at a time): P1.3, P4.1, P4.2, P4.3,
 *                                       P3.2, P3.3, P3.4, P3.5
 *   rows    (read back, idle high)    : P6.0..P6.3, P7.0..P7.2, P6.4
 *   scan code = (column << 8) | row
 */
static const struct {
    uint8_t port;
    uint8_t bit;
} g9001_key_col_pin[G9001_KEY_COLS] = {
    { 1, 3 }, { 4, 1 }, { 4, 2 }, { 4, 3 },
    { 3, 2 }, { 3, 3 }, { 3, 4 }, { 3, 5 },
};

static const struct {
    uint8_t port;
    uint8_t bit;
} g9001_key_row_pin[G9001_KEY_ROWS] = {
    { 6, 0 }, { 6, 1 }, { 6, 2 }, { 6, 3 },
    { 7, 0 }, { 7, 1 }, { 7, 2 }, { 6, 4 },
};

static uint8_t g9001_port_data(G9001State *s, unsigned port)
{
    if (port >= G9001_KEY_PORTS) {
        return 0;
    }
    return s->io[G9001_IO_PORT_BASE + port * 2];
}

static uint8_t g9001_port_dir(G9001State *s, unsigned port)
{
    if (port >= G9001_KEY_PORTS) {
        return 0;
    }
    return s->io[G9001_IO_PORT_BASE + port * 2 + 1];
}

static unsigned g9001_driven_column(G9001State *s)
{
    unsigned col;

    for (col = 0; col < G9001_KEY_COLS; col++) {
        unsigned port = g9001_key_col_pin[col].port;
        unsigned bit = g9001_key_col_pin[col].bit;

        if (!(g9001_port_dir(s, port) & (1u << bit))) {
            continue;           /* not an output: cannot drive the matrix */
        }
        if (!(g9001_port_data(s, port) & (1u << bit))) {
            return col;         /* active low */
        }
    }
    return G9001_KEY_COLS;
}

static uint8_t g9001_row_inputs(G9001State *s)
{
    uint8_t rows = 0xff;
    unsigned col = g9001_driven_column(s);
    unsigned row;

    if (col >= G9001_KEY_COLS) {
        return rows;
    }
    for (row = 0; row < G9001_KEY_ROWS; row++) {
        if (s->key_state[col][row]) {
            rows &= ~(1u << row);
        }
    }
    return rows;
}

/* Map the eight row lines onto P6/P7 exactly like the scan routine does. */
static uint8_t g9001_port_pins(G9001State *s, unsigned port)
{
    G9001MachineState *gms = G9001_MACHINE(s->machine);
    uint8_t dir = g9001_port_dir(s, port);
    uint8_t out = g9001_port_data(s, port) & dir;
    uint8_t in = 0xff;
    uint8_t rows = g9001_row_inputs(s);
    uint32_t byby = gms->nand_ryby_pin;
    unsigned i;

    for (i = 0; i < G9001_KEY_ROWS; i++) {
        unsigned rport = g9001_key_row_pin[i].port;
        unsigned rbit = g9001_key_row_pin[i].bit;

        if (rport != port) {
            continue;
        }
        if (rows & (1u << i)) {
            in |= (1u << rbit);
        } else {
            in &= ~(1u << rbit);
        }
    }
    /* NAND RY/#BY: high while ready, low while the array is busy. */
    if (byby != 0xffffffff && byby / 8 == port) {
        if (s->nand.ready_low) {
            in &= ~(1u << (byby % 8));
        } else {
            in |= (1u << (byby % 8));
        }
    }
    return out | (in & ~dir);
}

/* ------------------------------------------------------------------------- */
/* interrupt controller + 16-bit timer 0                                     */
/* ------------------------------------------------------------------------- */

/*
 * Only the sources the firmware actually uses are wired up so far.  Flag and
 * enable registers share the bit layout; the ILC priority register holds two
 * 4-bit levels per byte.  Vector numbers follow the S1C33 family layout that
 * the BBK 9288 machine already uses (16TM0 compare B = 30, compare A = 31)
 * and still have to be confirmed against manual table III.2.1.1.1.
 */
typedef struct G9001IrqSource {
    uint16_t flag_off;
    uint16_t enable_off;
    uint16_t il_off;
    uint8_t il_shift;
    uint8_t bit;
    uint8_t vector;
    const char *name;
} G9001IrqSource;

static const G9001IrqSource g9001_irq_sources[] = {
    { 0x0282, 0x0272, 0x0266, 0, 0x04, 30, "16TM0B" },
    { 0x0282, 0x0272, 0x0266, 0, 0x08, 31, "16TM0A" },
};

static void g9001_update_irq(G9001State *s)
{
    unsigned i;
    int best_level = 0;
    uint8_t best_vector = 0;
    bool found = false;

    for (i = 0; i < ARRAY_SIZE(g9001_irq_sources); i++) {
        const G9001IrqSource *src = &g9001_irq_sources[i];
        uint8_t flags = s->io[src->flag_off];
        uint8_t enables = s->io[src->enable_off];
        unsigned level;

        if (!(flags & src->bit) || !(enables & src->bit)) {
            continue;
        }
        level = (s->io[src->il_off] >> src->il_shift) & 0x7;
        if (!found || level > (unsigned)best_level) {
            found = true;
            best_level = level;
            best_vector = src->vector;
        }
    }

    if (s->cpu == NULL) {
        return;
    }
    if (!found) {
        s1c33_cpu_set_irq(CPU(s->cpu), false, 0, 0);
        s->irq_active = false;
        return;
    }
    if (s->trace_irq) {
        info_report("g9001-irq: vector=%u level=%u pc=0x%08x", best_vector,
                    best_level, g9001_current_pc());
    }
    s->irq_active = true;
    s->irq_vector = best_vector;
    s->irq_prio = best_level;
    s1c33_cpu_set_irq(CPU(s->cpu), true, best_vector, best_level);
}

/* Advance the timer counter by the time elapsed since the last update. */
static void g9001_t16_advance(G9001State *s)
{
    G9001T16 *t = &s->t16;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t counts;
    uint16_t period;

    if (!t->running) {
        t->last_ns = now;
        return;
    }
    if (t->last_ns == 0 || now <= t->last_ns) {
        t->last_ns = now;
        return;
    }
    counts = (now - t->last_ns) * (uint64_t)s->t16_hz / 1000000000ull +
             t->frac;
    t->last_ns = now;
    if (counts == 0) {
        return;
    }
    t->frac = counts % (uint64_t)s->t16_hz / 2;
    counts = MIN(counts, 4 * 65536ull);

    period = t->cb != 0 ? t->cb : (t->ca != 0 ? t->ca : 0x10000);
    while (counts-- > 0) {
        t->counter++;
        if (t->counter == t->ca) {
            s->io[0x0282] |= 0x08;          /* compare A */
        }
        if (t->counter == t->cb) {
            s->io[0x0282] |= 0x04;          /* compare B */
        }
        if (t->counter >= period) {
            t->counter = 0;
        }
    }
    g9001_update_irq(s);
}

static void g9001_t16_cb(void *opaque)
{
    G9001State *s = opaque;

    g9001_t16_advance(s);
    timer_mod(s->t16_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
}

/* ------------------------------------------------------------------------- */
/* I/O register window                                                       */
/* ------------------------------------------------------------------------- */

static uint64_t g9001_io_read(void *opaque, hwaddr offset, unsigned size)
{
    G9001State *s = opaque;
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr off = offset + i;
        uint8_t v;

        if (off >= G9001_IO_SIZE) {
            v = 0xff;
        } else if (off >= G9001_IO_PORT_BASE &&
                   off < G9001_IO_PORT_BASE + G9001_KEY_PORTS * 2) {
            hwaddr rel = off - G9001_IO_PORT_BASE;
            unsigned port = rel / 2;

            v = (rel & 1) ? g9001_port_dir(s, port) :
                            g9001_port_pins(s, port);
        } else if (off == G9001_IO_SPI_STAT) {
            v = 0x02;           /* Tx empty: card transfers never block */
        } else if (off == G9001_IO_SPI_RXD) {
            v = 0xff;
        } else if (off >= G9001_IO_ECC0_BASE &&
                   off < G9001_IO_ECC0_BASE + ARRAY_SIZE(s->nand_ecc)) {
            /*
             * "ECC always correct": the byte the hardware would have computed
             * over the last completed sector is exactly the one the flash
             * stores for it, so echo that one back.
             */
            unsigned idx = off - G9001_IO_ECC0_BASE;
            unsigned area = G9001_NAND_ECC_AREA +
                            s->nand.ecc_sector * G9001_NAND_SECTOR_ECC + idx;

            if (s->nand.cached_page >= 0 && area < sizeof(s->nand.page)) {
                v = s->nand.page[area];
            } else {
                v = s->nand_ecc[idx];
            }
        } else if (off == G9001_IO_SDRAMC_INI) {
            /* The init bit and the ready/busy status bits are self-clearing
             * on real silicon; the boot loader polls bits 4 and 3 here. */
            v = s->io[off] | G9001_SDRAMC_INI_READY;
        } else if (off == G9001_IO_T16_0TC) {
            g9001_t16_advance(s);
            v = s->t16.counter & 0xff;
        } else if (off == G9001_IO_T16_0TC + 1) {
            g9001_t16_advance(s);
            v = s->t16.counter >> 8;
        } else {
            v = s->io[off];
        }
        value |= (uint64_t)v << (8 * i);
        g9001_trace_push(s, off, 1, false, v);
    }

    if (s->trace_io) {
        info_report("g9001-io rd off=0x%04" HWADDR_PRIx " size=%u val=0x%"
                    PRIx64 " pc=0x%08x", offset, size, value,
                    g9001_current_pc());
    }
    return value;
}

static void g9001_io_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    G9001State *s = opaque;
    unsigned i;

    if (s->trace_io) {
        info_report("g9001-io wr off=0x%04" HWADDR_PRIx " size=%u val=0x%"
                    PRIx64 " pc=0x%08x", offset, size, value,
                    g9001_current_pc());
    }

    for (i = 0; i < size; i++) {
        hwaddr off = offset + i;
        uint8_t v = (value >> (8 * i)) & 0xff;

        if (off >= G9001_IO_SIZE) {
            continue;
        }
        g9001_trace_push(s, off, 1, true, v);

        if (off >= G9001_IO_PORT_BASE &&
            off < G9001_IO_PORT_BASE + G9001_KEY_PORTS * 2) {
            hwaddr rel = off - G9001_IO_PORT_BASE;

            if (rel & 1) {
                s->io[off] = v;         /* direction */
            } else {
                s->io[off] = v;         /* output latch */
            }
            continue;
        }

        switch (off) {
        case G9001_IO_LCDC_PS:
            s->io[off] = v;
            s->lcd_dirty = true;
            break;
        case G9001_IO_LCDC_MADD:
        case G9001_IO_LCDC_MADD + 1:
        case G9001_IO_LCDC_MADD + 2:
        case G9001_IO_LCDC_MADD + 3:
            s->io[off] = v;
            s->lcd_dirty = true;
            break;
        case G9001_IO_LCDC_INT:
            /* write-1-to-clear status bits */
            s->io[off] &= ~v;
            break;
        case G9001_IO_ECC_RSTRDY:
            s->io[off] = 0x01;          /* ECC ready */
            break;
        case G9001_IO_INT_FK01_FP03:
        case G9001_IO_INT_F16T01:
        case G9001_IO_INT_FLCDC:
            g9001_t16_advance(s);
            s->io[off] &= ~v;
            g9001_update_irq(s);
            break;
        case G9001_IO_INT_E16T01:
            s->io[off] = v;
            g9001_update_irq(s);
            break;
        case G9001_IO_T16_0CA:
        case G9001_IO_T16_0CA + 1:
        case G9001_IO_T16_0CB:
        case G9001_IO_T16_0CB + 1:
        case G9001_IO_T16_0CTL:
        case G9001_IO_T16_0CTL + 1:
            g9001_t16_advance(s);
            s->io[off] = v;
            s->t16.ca = s->io[G9001_IO_T16_0CA] |
                        (s->io[G9001_IO_T16_0CA + 1] << 8);
            s->t16.cb = s->io[G9001_IO_T16_0CB] |
                        (s->io[G9001_IO_T16_0CB + 1] << 8);
            s->t16.ctl = s->io[G9001_IO_T16_0CTL];
            if ((s->t16.ctl & 0x01) != (s->t16.running ? 0x01 : 0)) {
                s->t16.running = s->t16.ctl & 0x01;
                s->t16.counter = 0;
                s->t16.last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            break;
        case G9001_IO_T16_0TC:
        case G9001_IO_T16_0TC + 1:
            s->io[off] = v;
            s->t16.counter = s->io[G9001_IO_T16_0TC] |
                             (s->io[G9001_IO_T16_0TC + 1] << 8);
            break;
        case G9001_IO_CMU_GATEDCLK0:
        case G9001_IO_CMU_GATEDCLK1:
        case G9001_IO_CMU_CLKCNTL:
        case G9001_IO_CMU_OPT:
            s->io[off] = v;
            break;
        case G9001_IO_SDRAMC_INI:
            s->io[off] = v;
            break;
        default:
            s->io[off] = v;
            break;
        }
    }
}

static const MemoryRegionOps g9001_io_ops = {
    .read = g9001_io_read,
    .write = g9001_io_write,
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

/* ------------------------------------------------------------------------- */
/* keyboard input                                                            */
/* ------------------------------------------------------------------------- */

/*
 * PC key -> matrix (column, row).  The G9001 layout is not fully known yet, so
 * this is a "first working set" that can be rewritten once key.dll's table is
 * recovered; it keeps the emulator usable for navigation meanwhile.
 */
static const struct {
    int qcode;
    uint8_t col;
    uint8_t row;
} g9001_keymap[] = {
    { Q_KEY_CODE_UP,        0, 0 },
    { Q_KEY_CODE_DOWN,      0, 1 },
    { Q_KEY_CODE_LEFT,      0, 2 },
    { Q_KEY_CODE_RIGHT,     0, 3 },
    { Q_KEY_CODE_RET,       1, 0 },
    { Q_KEY_CODE_ESC,       1, 1 },
    { Q_KEY_CODE_SPC,       1, 2 },
    { Q_KEY_CODE_BACKSPACE, 1, 3 },
};

static void g9001_keyboard_event(DeviceState *dev, QemuConsole *src,
                                 InputEvent *evt)
{
    G9001State *s = g9001_active;
    InputKeyEvent *key;
    int qcode;
    bool down;
    unsigned i;

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
    down = key->down;
    for (i = 0; i < ARRAY_SIZE(g9001_keymap); i++) {
        if (g9001_keymap[i].qcode == qcode) {
            s->key_state[g9001_keymap[i].col][g9001_keymap[i].row] =
                down ? 1 : 0;
            break;
        }
    }
}

static const QemuInputHandler g9001_keyboard_handler = {
    .name = "G9001 Keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = g9001_keyboard_event,
};

/* ------------------------------------------------------------------------- */
/* machine                                                                   */
/* ------------------------------------------------------------------------- */

static void g9001_boot_from_nand(G9001State *s)
{
    G9001Nand *n = &s->nand;
    G9001MachineState *gms;
    uint8_t *rom;
    unsigned config;
    uint32_t boot_pc = 0;

    if (s->machine == NULL) {
        return;
    }
    gms = G9001_MACHINE(s->machine);

    /* The E08 boot ROM reads page 0 with the small-page geometry. */
    n->small_page = gms->nand_small_page;
    if (!g9001_nand_load_page(s, 0)) {
        error_report("g9001: no NAND image loaded; nothing to boot");
        return;
    }

    rom = n->page;
    config = rom[0];
    info_report("g9001: boot config data %02x %02x %02x %02x "
                "(bus=%u bits, small page=%u B)",
                rom[0], rom[1], rom[2], rom[3],
                (config & 0x30) == 0x20 ? 8 : 16,
                gms->nand_small_page ? G9001_NAND_SMALL_PAGE : 
                                       G9001_NAND_PAGE_DATA);

    /*
     * Manual D.2.3 (figure D.2.3.1): the 512-byte MBR block in the flash is
     * laid out as [4 bytes configuration data][508 bytes executable code],
     * and only the executable code is loaded - from A0RAM 0x0 on.  The config
     * data itself is not copied to A0RAM.
     */
    memcpy(s->a0ram_ptr, rom + 4, G9001_NAND_SMALL_PAGE - 4);

    /*
     * Register state handed over by the boot ROM: r10/r11 point at the
     * external area 4 base and the internal I/O base (the MBR uses r11 as the
     * peripheral base), and SP has to point into A0RAM because the first
     * thing the MBR does is a call.
     */
    s->cpu->env.regs[10] = G9001_CARD4_BASE;
    s->cpu->env.regs[11] = G9001_IO_BASE;
    /* NAND address cycle counts as detected by the boot ROM (P57 pin). */
    s->cpu->env.regs[15] = gms->nand_small_page ? 1 : 2;   /* column bytes */
    s->cpu->env.regs[14] = 3;                              /* row bytes */
    s->cpu->env.sp = G9001_A0RAM_SIZE;
    s->cpu->env.ttbr = 0x00c00000;

    /*
     * Manual D.2.2 step (2): the boot ROM configures the card interface and
     * the port functions before handing over.  The MBR relies on that, so it
     * only ever toggles the port latches with xbset/xbclr instead of touching
     * the direction registers itself:
     *   P0.2 = CLE, P0.3 = ALE      -> outputs
     *   P5.6 = #CE10                -> output
     *   P3.0/P3.1 = #RE/#WE         -> outputs (bus side, latched 0)
     *   P4.0 = RY/#BY               -> input
     */
    s->io[G9001_IO_PORT_BASE + 0 * 2] = 0x00;       /* P0D  */
    s->io[G9001_IO_PORT_BASE + 0 * 2 + 1] = 0x0c;   /* P0 dir: CLE/ALE */
    s->io[G9001_IO_PORT_BASE + 3 * 2] = 0x00;       /* P3D  */
    s->io[G9001_IO_PORT_BASE + 3 * 2 + 1] = 0x03;   /* P3 dir: #RE/#WE */
    s->io[G9001_IO_PORT_BASE + 4 * 2 + 1] = 0x00;   /* P4 dir: RY/#BY in */
    s->io[G9001_IO_PORT_BASE + 5 * 2] = 0xc0;       /* P5D: #CE10 idle high */
    s->io[G9001_IO_PORT_BASE + 5 * 2 + 1] = 0x40;   /* P5 dir: #CE10 out */

    cpu_set_pc(CPU(s->cpu), 0);
    info_report("g9001: boot ROM hand-off to 0x%08x (r10=0x%08x r11=0x%08x "
                "sp=0x%08x ttbr=0x%08x)", 0, s->cpu->env.regs[10],
                s->cpu->env.regs[11], s->cpu->env.sp, s->cpu->env.ttbr);
}

static void g9001_init(MachineState *machine)
{
    G9001MachineState *gms = G9001_MACHINE(machine);
    G9001State *s = g_new0(G9001State, 1);
    MemoryRegion *sysmem = get_system_memory();
    S1C33CPU *cpu;

    s->machine = machine;
    s->nand.cached_page = -1;
    s->trace_io = gms->trace_io;
    s->trace_nand = gms->trace_nand;
    s->trace_irq = gms->trace_irq;
    s->t16_hz = gms->t16_hz;
    s->nand_ecc_offset = gms->nand_ecc_offset;

    memory_region_init_ram(&s->a0ram, NULL, "g9001.a0ram",
                           G9001_A0RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, G9001_A0RAM_BASE, &s->a0ram);
    s->a0ram_ptr = memory_region_get_ram_ptr(&s->a0ram);

    memory_region_init_ram(&s->area1, NULL, "g9001.area1",
                           G9001_AREA1_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, G9001_AREA1_BASE, &s->area1);

    memory_region_init_ram(&s->ivram, NULL, "g9001.ivram",
                           G9001_IVRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, G9001_IVRAM_BASE, &s->ivram);
    s->ivram_ptr = memory_region_get_ram_ptr(&s->ivram);

    memory_region_init_ram(&s->dstram, NULL, "g9001.dstram",
                           G9001_DSTRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, G9001_DSTRAM_BASE, &s->dstram);

    memory_region_init_io(&s->io_region, NULL, &g9001_io_ops, s,
                          "g9001.e08-io", G9001_IO_SIZE);
    memory_region_add_subregion(sysmem, G9001_IO_BASE, &s->io_region);

    memory_region_init_io(&s->card4, NULL, &g9001_card_ops, s,
                          "g9001.card-ce4", G9001_CARD4_SIZE);
    memory_region_add_subregion(sysmem, G9001_CARD4_BASE, &s->card4);

    memory_region_init_io(&s->card14, NULL, &g9001_card_ops, s,
                          "g9001.card-ce4-area14", G9001_CARD14_SIZE);
    memory_region_add_subregion(sysmem, G9001_CARD14_BASE, &s->card14);

    memory_region_add_subregion(sysmem, G9001_SDRAM_BASE, machine->ram);
    s->sdram_ptr = memory_region_get_ram_ptr(machine->ram);

    /*
     * The SDRAMC chip select (#CE7) is decoded into Area 7 (0x400000, 2 MiB)
     * at reset and only later moved to Area 19 (0x10000000) through the SRAMC
     * area-location register; the first stage of the boot loader loads its
     * second stage to 0x400000 and jumps there.  Alias the first 2 MiB of the
     * SDRAM over Area 7 so both views reach the same memory.
     */
    memory_region_init_alias(&s->sdram_alias, NULL, "g9001.sdram-area7",
                             machine->ram, 0,
                             MIN((uint64_t)G9001_AREA7_SIZE,
                                 (uint64_t)machine->ram_size));
    memory_region_add_subregion(sysmem, G9001_AREA7_BASE, &s->sdram_alias);

    g9001_active = s;
    g9001_nand_reset(s);
    if (!g9001_nand_open(s, gms->nand_image_path)) {
        info_report("g9001: running without a NAND image "
                    "(use -M g9001,nand-image=<dump>)");
    }

    s->con = graphic_console_init(NULL, 0, &g9001_lcd_ops, s);
    qemu_console_resize(s->con, G9001_LCD_WIDTH, G9001_LCD_HEIGHT);

    cpu = S1C33_CPU(cpu_create(machine->cpu_type));
    s->cpu = cpu;
    s->cpu->env.sp = G9001_BOOT_STACK_TOP;
    s->cpu->env.ttbr = 0;

    /*
     * The boot loader and the OS use SLP as a short "clock unit settled"
     * delay while PSR.IE is clear, so nothing could ever wake the CPU in the
     * model.  Treat SLP as a NOP; override with -cpu c33l05,slp-as-nop=off.
     */
    cpu->slp_as_nop = true;

    s->kbd_handler = qemu_input_handler_register(NULL,
                                                 &g9001_keyboard_handler);
    qemu_input_handler_activate(s->kbd_handler);

    s->t16_hz = gms->t16_hz ?: 32768;
    s->t16_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, g9001_t16_cb, s);
    timer_mod(s->t16_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);

    g9001_boot_from_nand(s);

    if (gms->debug_lcd_dump_ms != 0) {
        s->lcd_dump_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                         g9001_lcd_dump_cb, s);
        timer_mod(s->lcd_dump_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  gms->debug_lcd_dump_ms);
        info_report("g9001: LCD dump scheduled after %u ms to %s",
                    gms->debug_lcd_dump_ms,
                    gms->debug_lcd_dump_path ?: "g9001-lcd.pgm");
    }
    if (gms->debug_pc_sample_ms != 0) {
        s->pc_sample_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          g9001_pc_sample_cb, s);
        timer_mod(s->pc_sample_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  gms->debug_pc_sample_ms);
    }
}

static bool g9001_get_trace_io(Object *obj, Error **errp)
{
    return G9001_MACHINE(obj)->trace_io;
}

static void g9001_set_trace_io(Object *obj, bool value, Error **errp)
{
    G9001_MACHINE(obj)->trace_io = value;
}

static bool g9001_get_trace_nand(Object *obj, Error **errp)
{
    return G9001_MACHINE(obj)->trace_nand;
}

static void g9001_set_trace_nand(Object *obj, bool value, Error **errp)
{
    G9001_MACHINE(obj)->trace_nand = value;
}

static bool g9001_get_trace_irq(Object *obj, Error **errp)
{
    return G9001_MACHINE(obj)->trace_irq;
}

static void g9001_set_trace_irq(Object *obj, bool value, Error **errp)
{
    G9001_MACHINE(obj)->trace_irq = value;
}

static char *g9001_get_nand_image(Object *obj, Error **errp)
{
    return g_strdup(G9001_MACHINE(obj)->nand_image_path);
}

static void g9001_set_nand_image(Object *obj, const char *value, Error **errp)
{
    G9001MachineState *gms = G9001_MACHINE(obj);

    g_free(gms->nand_image_path);
    gms->nand_image_path = g_strdup(value);
}

static char *g9001_get_lcd_dump_path(Object *obj, Error **errp)
{
    return g_strdup(G9001_MACHINE(obj)->debug_lcd_dump_path);
}

static void g9001_set_lcd_dump_path(Object *obj, const char *value,
                                    Error **errp)
{
    G9001MachineState *gms = G9001_MACHINE(obj);

    g_free(gms->debug_lcd_dump_path);
    gms->debug_lcd_dump_path = g_strdup(value);
}

static bool g9001_get_nand_save(Object *obj, Error **errp)
{
    return G9001_MACHINE(obj)->nand_save;
}

static void g9001_set_nand_save(Object *obj, bool value, Error **errp)
{
    G9001_MACHINE(obj)->nand_save = value;
}

static void g9001_machine_instance_init(Object *obj)
{
    G9001MachineState *gms = G9001_MACHINE(obj);
    gms->debug_lcd_dump_path = g_strdup("g9001-lcd.pgm");
    gms->nand_id = G9001_NAND_DEFAULT_ID;
    gms->nand_small_page = true;
    gms->nand_save = false;
    gms->nand_cle_pin = 2;          /* P0.2 = CLE  */
    gms->nand_ale_pin = 3;          /* P0.3 = ALE  */
    gms->nand_ryby_pin = 4 * 8 + 0; /* P4.0 = RY/#BY */
    gms->nand_ecc_offset = G9001_NAND_RAW_PAGE - 8;

    object_property_add_str(obj, "nand-image",
                            g9001_get_nand_image, g9001_set_nand_image);
    object_property_set_description(obj, "nand-image",
                                    "Raw NAND dump (4096+128 byte pages, "
                                    "64 pages/block) to boot from");
    object_property_add_uint32_ptr(obj, "nand-id", &gms->nand_id,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-id",
                                    "Four NAND ID bytes, most significant "
                                    "byte first (default 0xecda1095)");
    object_property_add_bool(obj, "nand-save", g9001_get_nand_save,
                             g9001_set_nand_save);
    object_property_set_description(obj, "nand-save",
                                    "Write back programmed/erased NAND pages "
                                    "to the image file (default off)");
    object_property_add_str(obj, "debug-lcd-dump-path",
                            g9001_get_lcd_dump_path, g9001_set_lcd_dump_path);
    object_property_set_description(obj, "debug-lcd-dump-path",
                                    "Path for debug-lcd-dump-ms output PGM");
    object_property_add_uint32_ptr(obj, "debug-lcd-dump-ms",
                                   &gms->debug_lcd_dump_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-lcd-dump-ms",
                                    "Dump the LCD framebuffer as PGM after "
                                    "this many host milliseconds");
    object_property_add_uint32_ptr(obj, "debug-pc-sample-ms",
                                   &gms->debug_pc_sample_ms,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "debug-pc-sample-ms",
                                    "Log the guest PC/SP and LCD state every "
                                    "this many host milliseconds");
    object_property_add_bool(obj, "trace-io", g9001_get_trace_io,
                             g9001_set_trace_io);
    object_property_set_description(obj, "trace-io",
                                    "Log every E08 I/O and card window access");
    object_property_add_bool(obj, "trace-nand", g9001_get_trace_nand,
                             g9001_set_trace_nand);
    object_property_set_description(obj, "trace-nand",
                                    "Log NAND command/address/data activity");
    object_property_add_bool(obj, "trace-irq", g9001_get_trace_irq,
                             g9001_set_trace_irq);
    object_property_set_description(obj, "trace-irq",
                                    "Log every interrupt raised to the CPU");
    object_property_add_uint32_ptr(obj, "t16-hz", &gms->t16_hz,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "t16-hz",
                                    "16-bit timer 0 input clock in Hz "
                                    "(default 32768, the OSC1 rate)");
    object_property_add_uint32_ptr(obj, "nand-cle-pin", &gms->nand_cle_pin,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-cle-pin",
                                    "GPIO pin driving NAND CLE as port*8+bit "
                                    "(-1 = not wired)");
    object_property_add_uint32_ptr(obj, "nand-ale-pin", &gms->nand_ale_pin,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-ale-pin",
                                    "GPIO pin driving NAND ALE as port*8+bit "
                                    "(-1 = not wired)");
    object_property_add_uint32_ptr(obj, "nand-ryby-pin", &gms->nand_ryby_pin,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-ryby-pin",
                                    "GPIO pin reading NAND RY/#BY as "
                                    "port*8+bit (default 32 = P4.0)");
    object_property_add_uint32_ptr(obj, "nand-ecc-offset",
                                   &gms->nand_ecc_offset,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "nand-ecc-offset",
                                    "Offset inside the raw page whose last 8 "
                                    "bytes are echoed into the ECC registers "
                                    "(default 4216)");
}

static void g9001_machine_instance_finalize(Object *obj)
{
    G9001MachineState *gms = G9001_MACHINE(obj);

    g_free(gms->nand_image_path);
    g_free(gms->debug_lcd_dump_path);
    if (g9001_active != NULL) {
        g9001_nand_close(g9001_active);
        g9001_active = NULL;
    }
}

static void g9001_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SII G9001 (Epson S1C33E08, 640x480 mono TFT, research)";
    mc->init = g9001_init;
    mc->default_cpu_type = TYPE_C33L05_CPU;
    mc->default_ram_size = G9001_DEFAULT_RAM;
    mc->default_ram_id = "g9001.sdram";
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo g9001_machine_type_info = {
    .name = TYPE_G9001_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(G9001MachineState),
    .instance_init = g9001_machine_instance_init,
    .instance_finalize = g9001_machine_instance_finalize,
    .class_init = g9001_machine_class_init,
};

static void g9001_machine_register_types(void)
{
    type_register_static(&g9001_machine_type_info);
}

type_init(g9001_machine_register_types)
