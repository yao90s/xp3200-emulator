/*
 * Epson S1C33 CPU definitions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef S1C33_CPU_H
#define S1C33_CPU_H

#include "cpu-qom.h"
#include "disas/dis-asm.h"
#include "exec/cpu-common.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-interrupt.h"

#ifdef CONFIG_USER_ONLY
#error "S1C33 does not support user mode emulation"
#endif

enum {
    S1C33_NUM_REGS = 16,
    S1C33_RECENT_TB_LEN = 16,
};

#define S1C33_PSR_N          (1u << 0)
#define S1C33_PSR_Z          (1u << 1)
#define S1C33_PSR_V          (1u << 2)
#define S1C33_PSR_C          (1u << 3)
#define S1C33_PSR_IE         (1u << 4)
#define S1C33_PSR_DS         (1u << 6)
#define S1C33_PSR_MO         (1u << 7)
#define S1C33_PSR_IL_SHIFT   8
#define S1C33_PSR_IL_MASK    (0xfu << S1C33_PSR_IL_SHIFT)
/*
 * Trap table base is 1 KiB aligned: bits 9..0 read back as zero on both
 * cores.  They differ above that - the L05 keeps bits 31..28 at zero and
 * has a 28-bit internal address bus, while the E08 keeps the full 32-bit
 * address (its SDRAM sits at 0x10000000 and the firmware installs the trap
 * table at 0x11e84000).  The variant is picked per CPU model through the
 * "core-variant" property, so a machine change cannot silently alter the
 * address semantics of the other core.
 */
typedef enum S1C33CoreVariant {
    S1C33_CORE_L05 = 0,
    S1C33_CORE_E08 = 1,
} S1C33CoreVariant;

#define S1C33_TTBR_MASK_L05  0x0ffffc00u
#define S1C33_TTBR_MASK_E08  0xfffffc00u
#define S1C33_ADDR_MASK_L05  0x0ffffffeu
#define S1C33_ADDR_MASK_E08  0xfffffffeu

static inline uint32_t s1c33_ttbr_mask(uint32_t core_variant)
{
    return core_variant == S1C33_CORE_E08 ? S1C33_TTBR_MASK_E08
                                          : S1C33_TTBR_MASK_L05;
}

static inline uint32_t s1c33_addr_mask(uint32_t core_variant)
{
    return core_variant == S1C33_CORE_E08 ? S1C33_ADDR_MASK_E08
                                          : S1C33_ADDR_MASK_L05;
}
#define S1C33_TRAP_NMI_VECTOR 7
#define S1C33_CPU_INTERRUPT_NMI CPU_INTERRUPT_TGT_EXT_3

typedef struct CPUArchState {
    uint32_t regs[S1C33_NUM_REGS];
    uint32_t pc;
    uint32_t psr;
    uint32_t sp;
    uint32_t alr;
    uint32_t ahr;
    uint32_t ext[2];
    uint32_t ext_count;
    uint32_t recent_tbs[S1C33_RECENT_TB_LEN];
    uint32_t recent_tb_pos;
    uint32_t recent_tb_count;
    uint32_t ttbr;
    uint8_t irq_vector;
    uint8_t irq_level;
    bool irq_pending;
    bool debug_mode;

    struct {} end_reset_fields;

    bool in_sleep;
} CPUS1C33State;

struct ArchCPU {
    CPUState parent_obj;
    CPUS1C33State env;

    bool exit_on_halt;
    bool shutdown_on_disabled_halt;
    uint32_t core_variant;          /* S1C33CoreVariant */
    bool slp_as_nop;
    bool trace_psr;
    bool trace_calls;
    bool trace_exec;
    bool trace_mem;
    bool trace_int;
    uint32_t trace_calls_start;
    uint32_t trace_calls_end;
    uint32_t trace_exec_start;
    uint32_t trace_exec_end;
    uint32_t trace_mem_start;
    uint32_t trace_mem_end;
    uint32_t trace_mem_pc_start;
    uint32_t trace_mem_pc_end;
    uint32_t trace_trap_addr;
    uint32_t trace_trap_addr2;
    bool unimpl_as_nop;             /* log unimplemented opcodes, keep running */
    uint32_t trace_watch_addr;      /* guest address whose value is watched */
    uint32_t trace_watch_last;      /* last observed value */
    uint32_t trace_watch_size;      /* 1/2/4 bytes */
};

struct S1C33CPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define CPU_RESOLVING_TYPE TYPE_S1C33_CPU

void s1c33_cpu_do_interrupt(CPUState *cs);
bool s1c33_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
void s1c33_cpu_set_irq(CPUState *cs, bool pending, uint8_t vector,
                       uint8_t level);
void s1c33_cpu_raise_nmi(CPUState *cs);
hwaddr s1c33_cpu_get_phys_page_debug(CPUState *cs, vaddr addr);
void s1c33_cpu_dump_state(CPUState *cs, FILE *f, int flags);
int s1c33_cpu_gdb_read_register(CPUState *cpu, GByteArray *buf, int reg);
int s1c33_cpu_gdb_write_register(CPUState *cpu, uint8_t *buf, int reg);
int print_insn_s1c33(bfd_vma addr, disassemble_info *info);

void s1c33_translate_init(void);
CPUState *s1c33_cpu_create(const char *typename, uint32_t core_variant,
                           bool slp_as_nop);
void s1c33_translate_code(CPUState *cs, TranslationBlock *tb,
                          int *max_insns, vaddr pc, void *host_pc);

#endif
