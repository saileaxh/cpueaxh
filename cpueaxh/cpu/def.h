// cpu/def.h - CPU definitions: enums, structs, status-based exceptions

#pragma once

#include "../cpueaxh_platform.hpp"

struct MEMORY_MANAGER;
struct CPU_CONTEXT;
struct cpueaxh_engine;

#ifndef CPUEAXH_HOOK_CODE_PRE
#define CPUEAXH_HOOK_CODE_PRE 1u
#define CPUEAXH_HOOK_CODE_POST 2u
#define CPUEAXH_HOOK_MEM_READ 3u
#define CPUEAXH_HOOK_MEM_WRITE 4u
#define CPUEAXH_HOOK_MEM_FETCH 5u
#define CPUEAXH_HOOK_MEM_READ_UNMAPPED 6u
#define CPUEAXH_HOOK_MEM_WRITE_UNMAPPED 7u
#define CPUEAXH_HOOK_MEM_FETCH_UNMAPPED 8u
#define CPUEAXH_HOOK_MEM_READ_PROT 9u
#define CPUEAXH_HOOK_MEM_WRITE_PROT 10u
#define CPUEAXH_HOOK_MEM_FETCH_PROT 11u
#endif

// RFLAGS bit definitions
#define RFLAGS_CF  (1ULL << 0)
#define RFLAGS_PF  (1ULL << 2)
#define RFLAGS_AF  (1ULL << 4)
#define RFLAGS_ZF  (1ULL << 6)
#define RFLAGS_SF  (1ULL << 7)
#define RFLAGS_DF  (1ULL << 10)
#define RFLAGS_OF  (1ULL << 11)

enum CPU_EXCEPTION_CODE : uint32_t {
    CPU_EXCEPTION_NONE = 0,
    CPU_EXCEPTION_DE   = 0xE0000000,
    CPU_EXCEPTION_GP   = 0xE0000001,
    CPU_EXCEPTION_SS   = 0xE0000002,
    CPU_EXCEPTION_NP   = 0xE0000003,
    CPU_EXCEPTION_UD   = 0xE0000004,
    CPU_EXCEPTION_PF   = 0xE0000005,
    CPU_EXCEPTION_AC   = 0xE0000006,
    CPU_EXCEPTION_BP   = 0xE0000007,
    CPU_EXCEPTION_DB   = 0xE0000008,
    CPU_EXCEPTION_OF   = 0xE0000009,
};

struct CPU_EXCEPTION_STATE {
    uint32_t code;
    uint32_t error_code;
};

inline bool cpu_is_exception_code(uint32_t code) {
    return code >= CPU_EXCEPTION_DE && code <= CPU_EXCEPTION_OF;
}

inline void cpu_exception_reset(CPU_EXCEPTION_STATE* state) {
    state->code = CPU_EXCEPTION_NONE;
    state->error_code = 0;
}

enum SegmentRegisterIndex {
    SEG_ES = 0,
    SEG_CS = 1,
    SEG_SS = 2,
    SEG_DS = 3,
    SEG_FS = 4,
    SEG_GS = 5
};

enum RegisterIndex {
    REG_RAX = 0,
    REG_RCX = 1,
    REG_RDX = 2,
    REG_RBX = 3,
    REG_RSP = 4,
    REG_RBP = 5,
    REG_RSI = 6,
    REG_RDI = 7,
    REG_R8 = 8,
    REG_R9 = 9,
    REG_R10 = 10,
    REG_R11 = 11,
    REG_R12 = 12,
    REG_R13 = 13,
    REG_R14 = 14,
    REG_R15 = 15
};

enum ControlRegisterIndex {
    REG_CR0 = 0,
    REG_CR2 = 2,
    REG_CR3 = 3,
    REG_CR4 = 4,
    REG_CR8 = 8
};

struct SegmentDescriptor {
    uint64_t base;
    uint32_t limit;
    uint8_t type;
    uint8_t dpl;
    bool present;
    bool granularity;
    bool db;
    bool long_mode;
};

struct SegmentRegister {
    uint16_t selector;
    SegmentDescriptor descriptor;
};

struct XMMRegister {
    uint64_t low;
    uint64_t high;
};

struct CPU_CONTEXT {
    uint64_t regs[16];
    uint64_t control_regs[16];
    uint32_t processor_id;
    XMMRegister xmm[16];
    XMMRegister ymm_upper[16];
    uint64_t mm[8];
    uint32_t mxcsr;
    SegmentRegister es;
    SegmentRegister cs;
    SegmentRegister ss;
    SegmentRegister ds;
    SegmentRegister fs;
    SegmentRegister gs;

    uint64_t rip;
    uint64_t rflags;

    uint8_t cpl;

    uint64_t gdtr_base;
    uint16_t gdtr_limit;
    uint64_t ldtr_base;
    uint16_t ldtr_limit;

    MEMORY_MANAGER* mem_mgr;
    cpueaxh_engine* owner_engine;

    bool rex_present;
    bool rex_w;
    bool rex_r;
    bool rex_x;
    bool rex_b;

    bool operand_size_override;
    bool address_size_override;

    // Set by every decoder to the byte-length of the decoded instruction.
    // cpu_step uses this to advance RIP for non-branch instructions.
    int last_inst_size;

    CPU_EXCEPTION_STATE exception;
};

struct CPU_SCALAR_SNAPSHOT {
    uint64_t regs[16];
    uint64_t control_regs[16];
    SegmentRegister es;
    SegmentRegister cs;
    SegmentRegister ss;
    SegmentRegister ds;
    SegmentRegister fs;
    SegmentRegister gs;
    uint64_t rip;
    uint64_t rflags;
    uint8_t cpl;
    uint64_t gdtr_base;
    uint16_t gdtr_limit;
    uint64_t ldtr_base;
    uint16_t ldtr_limit;
    bool rex_present;
    bool rex_w;
    bool rex_r;
    bool rex_x;
    bool rex_b;
    bool operand_size_override;
    bool address_size_override;
    int last_inst_size;
};

struct CPU_VECTOR_SNAPSHOT {
    XMMRegister xmm[16];
    XMMRegister ymm_upper[16];
    uint64_t mm[8];
    uint32_t mxcsr;
};

inline void cpu_capture_scalar_snapshot(CPU_SCALAR_SNAPSHOT* out, const CPU_CONTEXT* ctx) {
    if (!out || !ctx) {
        return;
    }

    CPUEAXH_MEMCPY(out->regs, ctx->regs, sizeof(out->regs));
    CPUEAXH_MEMCPY(out->control_regs, ctx->control_regs, sizeof(out->control_regs));
    out->es = ctx->es;
    out->cs = ctx->cs;
    out->ss = ctx->ss;
    out->ds = ctx->ds;
    out->fs = ctx->fs;
    out->gs = ctx->gs;
    out->rip = ctx->rip;
    out->rflags = ctx->rflags;
    out->cpl = ctx->cpl;
    out->gdtr_base = ctx->gdtr_base;
    out->gdtr_limit = ctx->gdtr_limit;
    out->ldtr_base = ctx->ldtr_base;
    out->ldtr_limit = ctx->ldtr_limit;
    out->rex_present = ctx->rex_present;
    out->rex_w = ctx->rex_w;
    out->rex_r = ctx->rex_r;
    out->rex_x = ctx->rex_x;
    out->rex_b = ctx->rex_b;
    out->operand_size_override = ctx->operand_size_override;
    out->address_size_override = ctx->address_size_override;
    out->last_inst_size = ctx->last_inst_size;
}

inline void cpu_restore_scalar_snapshot(CPU_CONTEXT* ctx, const CPU_SCALAR_SNAPSHOT* snapshot) {
    if (!ctx || !snapshot) {
        return;
    }

    CPUEAXH_MEMCPY(ctx->regs, snapshot->regs, sizeof(snapshot->regs));
    CPUEAXH_MEMCPY(ctx->control_regs, snapshot->control_regs, sizeof(snapshot->control_regs));
    ctx->es = snapshot->es;
    ctx->cs = snapshot->cs;
    ctx->ss = snapshot->ss;
    ctx->ds = snapshot->ds;
    ctx->fs = snapshot->fs;
    ctx->gs = snapshot->gs;
    ctx->rip = snapshot->rip;
    ctx->rflags = snapshot->rflags;
    ctx->cpl = snapshot->cpl;
    ctx->gdtr_base = snapshot->gdtr_base;
    ctx->gdtr_limit = snapshot->gdtr_limit;
    ctx->ldtr_base = snapshot->ldtr_base;
    ctx->ldtr_limit = snapshot->ldtr_limit;
    ctx->rex_present = snapshot->rex_present;
    ctx->rex_w = snapshot->rex_w;
    ctx->rex_r = snapshot->rex_r;
    ctx->rex_x = snapshot->rex_x;
    ctx->rex_b = snapshot->rex_b;
    ctx->operand_size_override = snapshot->operand_size_override;
    ctx->address_size_override = snapshot->address_size_override;
    ctx->last_inst_size = snapshot->last_inst_size;
}

inline void cpu_capture_vector_snapshot(CPU_VECTOR_SNAPSHOT* out, const CPU_CONTEXT* ctx) {
    if (!out || !ctx) {
        return;
    }

    CPUEAXH_MEMCPY(out->xmm, ctx->xmm, sizeof(out->xmm));
    CPUEAXH_MEMCPY(out->ymm_upper, ctx->ymm_upper, sizeof(out->ymm_upper));
    CPUEAXH_MEMCPY(out->mm, ctx->mm, sizeof(out->mm));
    out->mxcsr = ctx->mxcsr;
}

inline void cpu_restore_vector_snapshot(CPU_CONTEXT* ctx, const CPU_VECTOR_SNAPSHOT* snapshot) {
    if (!ctx || !snapshot) {
        return;
    }

    CPUEAXH_MEMCPY(ctx->xmm, snapshot->xmm, sizeof(snapshot->xmm));
    CPUEAXH_MEMCPY(ctx->ymm_upper, snapshot->ymm_upper, sizeof(snapshot->ymm_upper));
    CPUEAXH_MEMCPY(ctx->mm, snapshot->mm, sizeof(snapshot->mm));
    ctx->mxcsr = snapshot->mxcsr;
}

void cpu_notify_memory_hook(CPU_CONTEXT* ctx, uint32_t type, uint64_t address, size_t size, uint64_t value);
bool cpu_notify_invalid_memory_hook(CPU_CONTEXT* ctx, uint32_t type, uint64_t address, size_t size, uint64_t value);
bool cpu_has_hook_type(const CPU_CONTEXT* ctx, uint32_t type);

inline bool cpu_has_exception(const CPU_CONTEXT* ctx) {
    return ctx && ctx->exception.code != CPU_EXCEPTION_NONE;
}

inline void cpu_clear_exception(CPU_CONTEXT* ctx) {
    cpu_exception_reset(&ctx->exception);
}

inline void cpu_raise_exception(CPU_CONTEXT* ctx, uint32_t code, uint32_t error_code = 0) {
    if (!ctx || cpu_has_exception(ctx)) {
        return;
    }

    ctx->exception.code = code;
    ctx->exception.error_code = error_code;
}

inline void raise_de_ctx(CPU_CONTEXT* ctx) { cpu_raise_exception(ctx, CPU_EXCEPTION_DE, 0); }
inline void raise_gp_ctx(CPU_CONTEXT* ctx, uint32_t error_code) { cpu_raise_exception(ctx, CPU_EXCEPTION_GP, error_code); }
inline void raise_ss_ctx(CPU_CONTEXT* ctx, uint32_t error_code) { cpu_raise_exception(ctx, CPU_EXCEPTION_SS, error_code); }
inline void raise_np_ctx(CPU_CONTEXT* ctx, uint32_t error_code) { cpu_raise_exception(ctx, CPU_EXCEPTION_NP, error_code); }
inline void raise_ud_ctx(CPU_CONTEXT* ctx) { cpu_raise_exception(ctx, CPU_EXCEPTION_UD, 0); }
inline void raise_pf_ctx(CPU_CONTEXT* ctx, uint32_t error_code) { cpu_raise_exception(ctx, CPU_EXCEPTION_PF, error_code); }
inline void raise_ac_ctx(CPU_CONTEXT* ctx) { cpu_raise_exception(ctx, CPU_EXCEPTION_AC, 0); }
inline void raise_bp_ctx(CPU_CONTEXT* ctx) { cpu_raise_exception(ctx, CPU_EXCEPTION_BP, 0); }
inline void raise_db_ctx(CPU_CONTEXT* ctx) { cpu_raise_exception(ctx, CPU_EXCEPTION_DB, 0); }
inline void raise_of_ctx(CPU_CONTEXT* ctx) { cpu_raise_exception(ctx, CPU_EXCEPTION_OF, 0); }

inline bool cpu_is_canonical_address(uint64_t address) {
    const uint64_t sign_bit = (address >> 47) & 0x1ULL;
    const uint64_t upper_bits = address >> 48;
    return upper_bits == (sign_bit ? 0xFFFFULL : 0x0000ULL);
}

inline uint64_t cpu_segment_base_for_addressing(const CPU_CONTEXT* ctx, int segment_index) {
    if (!ctx) {
        return 0;
    }

    const SegmentRegister* seg = NULL;
    switch (segment_index) {
    case SEG_ES: seg = &ctx->es; break;
    case SEG_CS: seg = &ctx->cs; break;
    case SEG_SS: seg = &ctx->ss; break;
    case SEG_DS: seg = &ctx->ds; break;
    case SEG_FS: seg = &ctx->fs; break;
    case SEG_GS: seg = &ctx->gs; break;
    default:     return 0;
    }

    if (ctx->cs.descriptor.long_mode && segment_index != SEG_FS && segment_index != SEG_GS) {
        return 0;
    }

    return seg->descriptor.base;
}

inline void cpu_raise_noncanonical_address(CPU_CONTEXT* ctx, int segment_index) {
    if (segment_index == SEG_SS) {
        raise_ss_ctx(ctx, 0);
        return;
    }
    raise_gp_ctx(ctx, 0);
}

inline bool cpu_validate_linear_address(CPU_CONTEXT* ctx, uint64_t address, int segment_index) {
    if (!ctx || !ctx->cs.descriptor.long_mode) {
        return true;
    }
    if (cpu_is_canonical_address(address)) {
        return true;
    }
    cpu_raise_noncanonical_address(ctx, segment_index);
    return false;
}

struct DecodedInstruction {
    uint8_t opcode;
    uint8_t mandatory_prefix;
    uint8_t modrm;
    uint8_t sib;
    int32_t displacement;
    uint64_t immediate;
    uint64_t mem_address;
    int operand_size;
    int address_size;
    bool has_modrm;
    bool has_sib;
    bool has_lock_prefix;
    int disp_size;
    int imm_size;
    int inst_size;  // total instruction size in bytes (used by CALL/JMP for return address calculation)
};
