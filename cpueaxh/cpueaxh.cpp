#include "cpueaxh.hpp"
#include "cpueaxh_internal.hpp"

#define CPUEAXH_MAX_HOOKS 64

#if defined(CPUEAXH_PLATFORM_KERNEL) || defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDM_INCLUDED_) || defined(_NTIFS_)
extern "C" int _fltused = 0;
#endif

extern "C" cpueaxh_err cpueaxh_host_call_asm(cpueaxh_x86_context* context, cpueaxh_cb_host_bridge_t bridge, void* bridge_block);

struct CPUEAXH_HOST_BRIDGE_BLOCK {
    uint64_t host_rsp;
    uint64_t context_ptr;
    uint64_t guest_rsp;
    uint64_t saved_stack0;
    uint64_t saved_stack1;
    uint64_t saved_stack2;
    uint64_t saved_rax;
};

struct CPUEAXH_HOOK_ENTRY {
    bool used;
    cpueaxh_hook handle;
    uint32_t type;
    cpueaxh_cb_hookcode_t code_callback;
    cpueaxh_cb_hookmem_t mem_callback;
    cpueaxh_cb_hookmem_invalid_t invalid_mem_callback;
    void* user_data;
    uint64_t begin;
    uint64_t end;
};

struct CPUEAXH_ESCAPE_ENTRY {
    cpueaxh_hook handle;
    cpueaxh_escape_insn_id instruction_id;
    cpueaxh_cb_escape_t callback;
    void* user_data;
    uint64_t begin;
    uint64_t end;
};

struct cpueaxh_engine {
    MEMORY_MANAGER memory_manager;
    CPU_CONTEXT context;
    CPUEAXH_HOOK_ENTRY hooks[CPUEAXH_MAX_HOOKS];
    uint16_t hook_type_counts[CPUEAXH_HOOK_MEM_FETCH_PROT + 1];
    uint8_t escape_index_by_id[CPUEAXH_ESCAPE_INSN_MAX + 1];
    CPUEAXH_ESCAPE_ENTRY* escapes;
    size_t escape_count;
    size_t escape_capacity;
    cpueaxh_hook next_hook;
    uint32_t memory_mode;
    int stop_requested;
    cpueaxh_err last_error;
};

static void cpueaxh_copy_segment_out(cpueaxh_x86_segment* out_segment, const SegmentRegister* in_segment) {
    out_segment->selector = in_segment->selector;
    out_segment->reserved0 = 0;
    out_segment->descriptor.base = in_segment->descriptor.base;
    out_segment->descriptor.limit = in_segment->descriptor.limit;
    out_segment->descriptor.type = in_segment->descriptor.type;
    out_segment->descriptor.dpl = in_segment->descriptor.dpl;
    out_segment->descriptor.present = in_segment->descriptor.present ? 1u : 0u;
    out_segment->descriptor.granularity = in_segment->descriptor.granularity ? 1u : 0u;
    out_segment->descriptor.db = in_segment->descriptor.db ? 1u : 0u;
    out_segment->descriptor.long_mode = in_segment->descriptor.long_mode ? 1u : 0u;
}

static void cpueaxh_copy_segment_in(SegmentRegister* out_segment, const cpueaxh_x86_segment* in_segment) {
    out_segment->selector = in_segment->selector;
    out_segment->descriptor.base = in_segment->descriptor.base;
    out_segment->descriptor.limit = in_segment->descriptor.limit;
    out_segment->descriptor.type = in_segment->descriptor.type;
    out_segment->descriptor.dpl = in_segment->descriptor.dpl;
    out_segment->descriptor.present = in_segment->descriptor.present != 0;
    out_segment->descriptor.granularity = in_segment->descriptor.granularity != 0;
    out_segment->descriptor.db = in_segment->descriptor.db != 0;
    out_segment->descriptor.long_mode = in_segment->descriptor.long_mode != 0;
}

static void cpueaxh_copy_xmm_out(cpueaxh_x86_xmm* out_xmm, const XMMRegister* in_xmm) {
    out_xmm->low = in_xmm->low;
    out_xmm->high = in_xmm->high;
}

static void cpueaxh_copy_xmm_in(XMMRegister* out_xmm, const cpueaxh_x86_xmm* in_xmm) {
    out_xmm->low = in_xmm->low;
    out_xmm->high = in_xmm->high;
}

static void cpueaxh_copy_ymm_out(cpueaxh_x86_ymm* out_ymm, const XMMRegister* lower, const XMMRegister* upper) {
    cpueaxh_copy_xmm_out(&out_ymm->lower, lower);
    cpueaxh_copy_xmm_out(&out_ymm->upper, upper);
}

static void cpueaxh_copy_ymm_in(XMMRegister* out_lower, XMMRegister* out_upper, const cpueaxh_x86_ymm* in_ymm) {
    cpueaxh_copy_xmm_in(out_lower, &in_ymm->lower);
    cpueaxh_copy_xmm_in(out_upper, &in_ymm->upper);
}

static void cpueaxh_copy_zmm_out(cpueaxh_x86_zmm* out_zmm, const XMMRegister* lower0, const XMMRegister* lower1, const ZMMUpperRegister* upper) {
    cpueaxh_copy_ymm_out(&out_zmm->lower, lower0, lower1);
    cpueaxh_copy_ymm_out(&out_zmm->upper, &upper->lower, &upper->upper);
}

static void cpueaxh_copy_zmm_in(XMMRegister* out_lower0, XMMRegister* out_lower1, ZMMUpperRegister* out_upper, const cpueaxh_x86_zmm* in_zmm) {
    cpueaxh_copy_ymm_in(out_lower0, out_lower1, &in_zmm->lower);
    cpueaxh_copy_ymm_in(&out_upper->lower, &out_upper->upper, &in_zmm->upper);
}

static SegmentRegister* cpueaxh_select_segment(CPU_CONTEXT* context, int regid) {
    switch (regid) {
    case CPUEAXH_X86_REG_ES_SELECTOR:
    case CPUEAXH_X86_REG_ES_BASE:
    case CPUEAXH_X86_REG_ES_LIMIT:
    case CPUEAXH_X86_REG_ES_TYPE:
    case CPUEAXH_X86_REG_ES_DPL:
    case CPUEAXH_X86_REG_ES_PRESENT:
    case CPUEAXH_X86_REG_ES_GRANULARITY:
    case CPUEAXH_X86_REG_ES_DB:
    case CPUEAXH_X86_REG_ES_LONG_MODE:
        return &context->es;
    case CPUEAXH_X86_REG_CS_SELECTOR:
    case CPUEAXH_X86_REG_CS_BASE:
    case CPUEAXH_X86_REG_CS_LIMIT:
    case CPUEAXH_X86_REG_CS_TYPE:
    case CPUEAXH_X86_REG_CS_DPL:
    case CPUEAXH_X86_REG_CS_PRESENT:
    case CPUEAXH_X86_REG_CS_GRANULARITY:
    case CPUEAXH_X86_REG_CS_DB:
    case CPUEAXH_X86_REG_CS_LONG_MODE:
        return &context->cs;
    case CPUEAXH_X86_REG_SS_SELECTOR:
    case CPUEAXH_X86_REG_SS_BASE:
    case CPUEAXH_X86_REG_SS_LIMIT:
    case CPUEAXH_X86_REG_SS_TYPE:
    case CPUEAXH_X86_REG_SS_DPL:
    case CPUEAXH_X86_REG_SS_PRESENT:
    case CPUEAXH_X86_REG_SS_GRANULARITY:
    case CPUEAXH_X86_REG_SS_DB:
    case CPUEAXH_X86_REG_SS_LONG_MODE:
        return &context->ss;
    case CPUEAXH_X86_REG_DS_SELECTOR:
    case CPUEAXH_X86_REG_DS_BASE:
    case CPUEAXH_X86_REG_DS_LIMIT:
    case CPUEAXH_X86_REG_DS_TYPE:
    case CPUEAXH_X86_REG_DS_DPL:
    case CPUEAXH_X86_REG_DS_PRESENT:
    case CPUEAXH_X86_REG_DS_GRANULARITY:
    case CPUEAXH_X86_REG_DS_DB:
    case CPUEAXH_X86_REG_DS_LONG_MODE:
        return &context->ds;
    case CPUEAXH_X86_REG_FS_SELECTOR:
    case CPUEAXH_X86_REG_FS_BASE:
    case CPUEAXH_X86_REG_FS_LIMIT:
    case CPUEAXH_X86_REG_FS_TYPE:
    case CPUEAXH_X86_REG_FS_DPL:
    case CPUEAXH_X86_REG_FS_PRESENT:
    case CPUEAXH_X86_REG_FS_GRANULARITY:
    case CPUEAXH_X86_REG_FS_DB:
    case CPUEAXH_X86_REG_FS_LONG_MODE:
        return &context->fs;
    case CPUEAXH_X86_REG_GS_SELECTOR:
    case CPUEAXH_X86_REG_GS_BASE:
    case CPUEAXH_X86_REG_GS_LIMIT:
    case CPUEAXH_X86_REG_GS_TYPE:
    case CPUEAXH_X86_REG_GS_DPL:
    case CPUEAXH_X86_REG_GS_PRESENT:
    case CPUEAXH_X86_REG_GS_GRANULARITY:
    case CPUEAXH_X86_REG_GS_DB:
    case CPUEAXH_X86_REG_GS_LONG_MODE:
        return &context->gs;
    default:
        return NULL;
    }
}

static const SegmentRegister* cpueaxh_select_segment(const CPU_CONTEXT* context, int regid) {
    return cpueaxh_select_segment(const_cast<CPU_CONTEXT*>(context), regid);
}

enum CPUEAXH_SEGMENT_FIELD_KIND {
    CPUEAXH_SEGMENT_FIELD_SELECTOR,
    CPUEAXH_SEGMENT_FIELD_BASE,
    CPUEAXH_SEGMENT_FIELD_LIMIT,
    CPUEAXH_SEGMENT_FIELD_TYPE,
    CPUEAXH_SEGMENT_FIELD_DPL,
    CPUEAXH_SEGMENT_FIELD_PRESENT,
    CPUEAXH_SEGMENT_FIELD_GRANULARITY,
    CPUEAXH_SEGMENT_FIELD_DB,
    CPUEAXH_SEGMENT_FIELD_LONG_MODE,
    CPUEAXH_SEGMENT_FIELD_NONE
};

static CPUEAXH_SEGMENT_FIELD_KIND cpueaxh_segment_field_kind_from_regid(int regid) {
    switch (regid) {
    case CPUEAXH_X86_REG_ES_SELECTOR:
    case CPUEAXH_X86_REG_CS_SELECTOR:
    case CPUEAXH_X86_REG_SS_SELECTOR:
    case CPUEAXH_X86_REG_DS_SELECTOR:
    case CPUEAXH_X86_REG_FS_SELECTOR:
    case CPUEAXH_X86_REG_GS_SELECTOR:
        return CPUEAXH_SEGMENT_FIELD_SELECTOR;
    case CPUEAXH_X86_REG_ES_BASE:
    case CPUEAXH_X86_REG_CS_BASE:
    case CPUEAXH_X86_REG_SS_BASE:
    case CPUEAXH_X86_REG_DS_BASE:
    case CPUEAXH_X86_REG_FS_BASE:
    case CPUEAXH_X86_REG_GS_BASE:
        return CPUEAXH_SEGMENT_FIELD_BASE;
    case CPUEAXH_X86_REG_ES_LIMIT:
    case CPUEAXH_X86_REG_CS_LIMIT:
    case CPUEAXH_X86_REG_SS_LIMIT:
    case CPUEAXH_X86_REG_DS_LIMIT:
    case CPUEAXH_X86_REG_FS_LIMIT:
    case CPUEAXH_X86_REG_GS_LIMIT:
        return CPUEAXH_SEGMENT_FIELD_LIMIT;
    case CPUEAXH_X86_REG_ES_TYPE:
    case CPUEAXH_X86_REG_CS_TYPE:
    case CPUEAXH_X86_REG_SS_TYPE:
    case CPUEAXH_X86_REG_DS_TYPE:
    case CPUEAXH_X86_REG_FS_TYPE:
    case CPUEAXH_X86_REG_GS_TYPE:
        return CPUEAXH_SEGMENT_FIELD_TYPE;
    case CPUEAXH_X86_REG_ES_DPL:
    case CPUEAXH_X86_REG_CS_DPL:
    case CPUEAXH_X86_REG_SS_DPL:
    case CPUEAXH_X86_REG_DS_DPL:
    case CPUEAXH_X86_REG_FS_DPL:
    case CPUEAXH_X86_REG_GS_DPL:
        return CPUEAXH_SEGMENT_FIELD_DPL;
    case CPUEAXH_X86_REG_ES_PRESENT:
    case CPUEAXH_X86_REG_CS_PRESENT:
    case CPUEAXH_X86_REG_SS_PRESENT:
    case CPUEAXH_X86_REG_DS_PRESENT:
    case CPUEAXH_X86_REG_FS_PRESENT:
    case CPUEAXH_X86_REG_GS_PRESENT:
        return CPUEAXH_SEGMENT_FIELD_PRESENT;
    case CPUEAXH_X86_REG_ES_GRANULARITY:
    case CPUEAXH_X86_REG_CS_GRANULARITY:
    case CPUEAXH_X86_REG_SS_GRANULARITY:
    case CPUEAXH_X86_REG_DS_GRANULARITY:
    case CPUEAXH_X86_REG_FS_GRANULARITY:
    case CPUEAXH_X86_REG_GS_GRANULARITY:
        return CPUEAXH_SEGMENT_FIELD_GRANULARITY;
    case CPUEAXH_X86_REG_ES_DB:
    case CPUEAXH_X86_REG_CS_DB:
    case CPUEAXH_X86_REG_SS_DB:
    case CPUEAXH_X86_REG_DS_DB:
    case CPUEAXH_X86_REG_FS_DB:
    case CPUEAXH_X86_REG_GS_DB:
        return CPUEAXH_SEGMENT_FIELD_DB;
    case CPUEAXH_X86_REG_ES_LONG_MODE:
    case CPUEAXH_X86_REG_CS_LONG_MODE:
    case CPUEAXH_X86_REG_SS_LONG_MODE:
    case CPUEAXH_X86_REG_DS_LONG_MODE:
    case CPUEAXH_X86_REG_FS_LONG_MODE:
    case CPUEAXH_X86_REG_GS_LONG_MODE:
        return CPUEAXH_SEGMENT_FIELD_LONG_MODE;
    default:
        return CPUEAXH_SEGMENT_FIELD_NONE;
    }
}

static void cpueaxh_context_out(cpueaxh_x86_context* out_context, const CPU_CONTEXT* in_context) {
    CPUEAXH_MEMSET(out_context, 0, sizeof(*out_context));
    CPUEAXH_MEMCPY(out_context->regs, in_context->regs, sizeof(out_context->regs));
    CPUEAXH_MEMCPY(out_context->xmm, in_context->xmm, sizeof(out_context->xmm));
    CPUEAXH_MEMCPY(out_context->ymm_upper, in_context->ymm_upper, sizeof(out_context->ymm_upper));
    CPUEAXH_MEMCPY(out_context->zmm_upper, in_context->zmm_upper, sizeof(out_context->zmm_upper));
    CPUEAXH_MEMCPY(out_context->opmask, in_context->opmask, sizeof(out_context->opmask));
    CPUEAXH_MEMCPY(out_context->mm, in_context->mm, sizeof(out_context->mm));
    out_context->rip = in_context->rip;
    out_context->rflags = in_context->rflags;
    out_context->mxcsr = in_context->mxcsr;
    cpueaxh_copy_segment_out(&out_context->es, &in_context->es);
    cpueaxh_copy_segment_out(&out_context->cs, &in_context->cs);
    cpueaxh_copy_segment_out(&out_context->ss, &in_context->ss);
    cpueaxh_copy_segment_out(&out_context->ds, &in_context->ds);
    cpueaxh_copy_segment_out(&out_context->fs, &in_context->fs);
    cpueaxh_copy_segment_out(&out_context->gs, &in_context->gs);
    out_context->gdtr_base = in_context->gdtr_base;
    out_context->gdtr_limit = in_context->gdtr_limit;
    out_context->ldtr_base = in_context->ldtr_base;
    out_context->ldtr_limit = in_context->ldtr_limit;
    out_context->cpl = in_context->cpl;
    out_context->code_exception = in_context->exception.code;
    out_context->error_code_exception = in_context->exception.error_code;
    CPUEAXH_MEMCPY(out_context->control_regs, in_context->control_regs, sizeof(out_context->control_regs));
    out_context->processor_id = in_context->processor_id;
}

static void cpueaxh_context_in(CPU_CONTEXT* out_context, const cpueaxh_x86_context* in_context) {
    CPUEAXH_MEMCPY(out_context->regs, in_context->regs, sizeof(out_context->regs));
    CPUEAXH_MEMCPY(out_context->xmm, in_context->xmm, sizeof(out_context->xmm));
    CPUEAXH_MEMCPY(out_context->ymm_upper, in_context->ymm_upper, sizeof(out_context->ymm_upper));
    CPUEAXH_MEMCPY(out_context->zmm_upper, in_context->zmm_upper, sizeof(out_context->zmm_upper));
    CPUEAXH_MEMCPY(out_context->opmask, in_context->opmask, sizeof(out_context->opmask));
    CPUEAXH_MEMCPY(out_context->mm, in_context->mm, sizeof(out_context->mm));
    out_context->rip = in_context->rip;
    out_context->rflags = in_context->rflags;
    out_context->mxcsr = in_context->mxcsr;
    cpueaxh_copy_segment_in(&out_context->es, &in_context->es);
    cpueaxh_copy_segment_in(&out_context->cs, &in_context->cs);
    cpueaxh_copy_segment_in(&out_context->ss, &in_context->ss);
    cpueaxh_copy_segment_in(&out_context->ds, &in_context->ds);
    cpueaxh_copy_segment_in(&out_context->fs, &in_context->fs);
    cpueaxh_copy_segment_in(&out_context->gs, &in_context->gs);
    out_context->long_mode_active = true;
    out_context->gdtr_base = in_context->gdtr_base;
    out_context->gdtr_limit = in_context->gdtr_limit;
    out_context->ldtr_base = in_context->ldtr_base;
    out_context->ldtr_limit = in_context->ldtr_limit;
    out_context->cpl = in_context->cpl;
    out_context->exception.code = in_context->code_exception;
    out_context->exception.error_code = in_context->error_code_exception;
    CPUEAXH_MEMCPY(out_context->control_regs, in_context->control_regs, sizeof(out_context->control_regs));
    out_context->processor_id = in_context->processor_id;
}

static int cpueaxh_control_reg_index_from_regid(int regid) {
    switch (regid) {
    case CPUEAXH_X86_REG_CR0:
        return 0;
    case CPUEAXH_X86_REG_CR1:
        return 1;
    case CPUEAXH_X86_REG_CR2:
        return 2;
    case CPUEAXH_X86_REG_CR3:
        return 3;
    case CPUEAXH_X86_REG_CR4:
        return 4;
    case CPUEAXH_X86_REG_CR5:
        return 5;
    case CPUEAXH_X86_REG_CR6:
        return 6;
    case CPUEAXH_X86_REG_CR7:
        return 7;
    case CPUEAXH_X86_REG_CR8:
        return 8;
    case CPUEAXH_X86_REG_CR9:
        return 9;
    case CPUEAXH_X86_REG_CR10:
        return 10;
    case CPUEAXH_X86_REG_CR11:
        return 11;
    case CPUEAXH_X86_REG_CR12:
        return 12;
    case CPUEAXH_X86_REG_CR13:
        return 13;
    case CPUEAXH_X86_REG_CR14:
        return 14;
    case CPUEAXH_X86_REG_CR15:
        return 15;
    default:
        return -1;
    }
}

static bool cpueaxh_range_contains(uint64_t begin, uint64_t end, uint64_t address) {
    if (begin == 0 && end == 0) {
        return true;
    }
    if (end == 0) {
        return address >= begin;
    }
    return address >= begin && address <= end;
}

static bool cpueaxh_is_supported_hook_type(uint32_t type) {
    return type == CPUEAXH_HOOK_CODE_PRE || type == CPUEAXH_HOOK_CODE_POST ||
        type == CPUEAXH_HOOK_MEM_READ || type == CPUEAXH_HOOK_MEM_WRITE || type == CPUEAXH_HOOK_MEM_FETCH ||
        type == CPUEAXH_HOOK_MEM_READ_UNMAPPED || type == CPUEAXH_HOOK_MEM_WRITE_UNMAPPED || type == CPUEAXH_HOOK_MEM_FETCH_UNMAPPED ||
        type == CPUEAXH_HOOK_MEM_READ_PROT || type == CPUEAXH_HOOK_MEM_WRITE_PROT || type == CPUEAXH_HOOK_MEM_FETCH_PROT;
}

static bool cpueaxh_is_code_hook_type(uint32_t type) {
    return type == CPUEAXH_HOOK_CODE_PRE || type == CPUEAXH_HOOK_CODE_POST;
}

static bool cpueaxh_is_invalid_memory_hook_type(uint32_t type) {
    return type == CPUEAXH_HOOK_MEM_READ_UNMAPPED || type == CPUEAXH_HOOK_MEM_WRITE_UNMAPPED || type == CPUEAXH_HOOK_MEM_FETCH_UNMAPPED ||
        type == CPUEAXH_HOOK_MEM_READ_PROT || type == CPUEAXH_HOOK_MEM_WRITE_PROT || type == CPUEAXH_HOOK_MEM_FETCH_PROT;
}

static bool cpueaxh_is_hook_type_index_valid(uint32_t type) {
    return type <= CPUEAXH_HOOK_MEM_FETCH_PROT;
}

static bool cpueaxh_is_escape_index_valid(cpueaxh_escape_insn_id instruction_id) {
    return instruction_id > CPUEAXH_ESCAPE_INSN_NONE && instruction_id <= CPUEAXH_ESCAPE_INSN_MAX;
}

static void cpueaxh_rebuild_escape_index(cpueaxh_engine* engine) {
    if (!engine) {
        return;
    }

    CPUEAXH_MEMSET(engine->escape_index_by_id, 0, sizeof(engine->escape_index_by_id));
    for (size_t index = 0; index < engine->escape_count; index++) {
        const cpueaxh_escape_insn_id instruction_id = engine->escapes[index].instruction_id;
        if (cpueaxh_is_escape_index_valid(instruction_id) && index < 0xFF) {
            engine->escape_index_by_id[instruction_id] = (uint8_t)(index + 1);
        }
    }
}

static CPUEAXH_ESCAPE_ENTRY* cpueaxh_lookup_escape_entry(cpueaxh_engine* engine, cpueaxh_escape_insn_id instruction_id) {
    if (!engine || !cpueaxh_is_escape_index_valid(instruction_id)) {
        return NULL;
    }

    const uint8_t encoded_index = engine->escape_index_by_id[instruction_id];
    if (encoded_index == 0) {
        return NULL;
    }

    const size_t index = (size_t)(encoded_index - 1);
    if (index >= engine->escape_count) {
        return NULL;
    }

    CPUEAXH_ESCAPE_ENTRY* escape = &engine->escapes[index];
    return escape->instruction_id == instruction_id ? escape : NULL;
}

static void cpueaxh_adjust_hook_type_count(cpueaxh_engine* engine, uint32_t type, int delta) {
    if (!engine || !cpueaxh_is_hook_type_index_valid(type) || delta == 0) {
        return;
    }

    uint16_t& count = engine->hook_type_counts[type];
    if (delta > 0) {
        count = (uint16_t)(count + (uint16_t)delta);
    }
    else {
        const uint16_t amount = (uint16_t)(-delta);
        count = count > amount ? (uint16_t)(count - amount) : 0;
    }
}

bool cpu_has_hook_type(const CPU_CONTEXT* ctx, uint32_t type) {
    if (!ctx || !ctx->owner_engine || !cpueaxh_is_hook_type_index_valid(type)) {
        return false;
    }

    return ctx->owner_engine->hook_type_counts[type] != 0;
}

void cpu_notify_memory_hook(CPU_CONTEXT* ctx, uint32_t type, uint64_t address, size_t size, uint64_t value) {
    if (!cpu_has_hook_type(ctx, type)) {
        return;
    }

    cpueaxh_engine* engine = ctx->owner_engine;
    for (int index = 0; index < CPUEAXH_MAX_HOOKS; index++) {
        CPUEAXH_HOOK_ENTRY* hook = &engine->hooks[index];
        if (!hook->used || hook->type != type || !hook->mem_callback) {
            continue;
        }

        if (!cpueaxh_range_contains(hook->begin, hook->end, address)) {
            continue;
        }

        hook->mem_callback(engine, type, address, size, value, hook->user_data);
    }
}

bool cpu_notify_invalid_memory_hook(CPU_CONTEXT* ctx, uint32_t type, uint64_t address, size_t size, uint64_t value) {
    if (!cpu_has_hook_type(ctx, type)) {
        return false;
    }

    bool handled = false;
    cpueaxh_engine* engine = ctx->owner_engine;
    for (int index = 0; index < CPUEAXH_MAX_HOOKS; index++) {
        CPUEAXH_HOOK_ENTRY* hook = &engine->hooks[index];
        if (!hook->used || hook->type != type || !hook->invalid_mem_callback) {
            continue;
        }

        if (!cpueaxh_range_contains(hook->begin, hook->end, address)) {
            continue;
        }

        if (hook->invalid_mem_callback(engine, type, address, size, value, hook->user_data) != 0) {
            handled = true;
        }
    }

    return handled;
}

static cpueaxh_escape_insn_id cpueaxh_classify_escape_instruction(const uint8_t* bytes, int fetched, uint32_t* instruction_size) {
    if (!bytes || !instruction_size || fetched <= 0) {
        return CPUEAXH_ESCAPE_INSN_NONE;
    }

    *instruction_size = 0;

    int prefix_len = 0;
    uint16_t opc = peek_opcode(NULL, bytes, fetched, &prefix_len);
    if (opc == 0xFFFF) {
        return CPUEAXH_ESCAPE_INSN_NONE;
    }

    if (opc == 0x00CC) {
        *instruction_size = (uint32_t)(prefix_len + 1);
        return CPUEAXH_ESCAPE_INSN_INT3;
    }
    if (opc == 0x00CD && fetched >= (prefix_len + 2)) {
        *instruction_size = (uint32_t)(prefix_len + 2);
        return CPUEAXH_ESCAPE_INSN_INT;
    }
    if (opc == 0x00F4) {
        *instruction_size = (uint32_t)(prefix_len + 1);
        return CPUEAXH_ESCAPE_INSN_HLT;
    }

    if (opc == 0x00E4 || opc == 0x00E5) {
        if (fetched >= (prefix_len + 2)) {
            *instruction_size = (uint32_t)(prefix_len + 2);
            return CPUEAXH_ESCAPE_INSN_IN;
        }
        return CPUEAXH_ESCAPE_INSN_NONE;
    }
    if (opc == 0x00EC || opc == 0x00ED) {
        *instruction_size = (uint32_t)(prefix_len + 1);
        return CPUEAXH_ESCAPE_INSN_IN;
    }
    if (opc == 0x006C || opc == 0x006D) {
        *instruction_size = (uint32_t)(prefix_len + 1);
        return CPUEAXH_ESCAPE_INSN_INS;
    }
    if (opc == 0x00E6 || opc == 0x00E7) {
        if (fetched >= (prefix_len + 2)) {
            *instruction_size = (uint32_t)(prefix_len + 2);
            return CPUEAXH_ESCAPE_INSN_OUT;
        }
        return CPUEAXH_ESCAPE_INSN_NONE;
    }
    if (opc == 0x00EE || opc == 0x00EF) {
        *instruction_size = (uint32_t)(prefix_len + 1);
        return CPUEAXH_ESCAPE_INSN_OUT;
    }

    if (opc == 0x0F05) {
        *instruction_size = (uint32_t)(prefix_len + 2);
        return CPUEAXH_ESCAPE_INSN_SYSCALL;
    }
    if (opc == 0x0F34) {
        *instruction_size = (uint32_t)(prefix_len + 2);
        return CPUEAXH_ESCAPE_INSN_SYSENTER;
    }
    if (opc == 0x0FA2) {
        *instruction_size = (uint32_t)(prefix_len + 2);
        return CPUEAXH_ESCAPE_INSN_CPUID;
    }
    if (opc == 0x0F31) {
        *instruction_size = (uint32_t)(prefix_len + 2);
        return CPUEAXH_ESCAPE_INSN_RDTSC;
    }
    if (opc == 0x0F01 && (prefix_len + 2) < fetched && bytes[prefix_len + 2] == 0xD0) {
        *instruction_size = (uint32_t)(prefix_len + 3);
        return CPUEAXH_ESCAPE_INSN_XGETBV;
    }
    if (opc == 0x0F01 && (prefix_len + 2) < fetched && bytes[prefix_len + 2] == 0xF9) {
        *instruction_size = (uint32_t)(prefix_len + 3);
        return CPUEAXH_ESCAPE_INSN_RDTSCP;
    }
    if (opc == 0x0FC7 && is_rdrand_instruction(bytes, (size_t)fetched)) {
        *instruction_size = (uint32_t)(prefix_len + 3);
        return CPUEAXH_ESCAPE_INSN_RDRAND;
    }
    if (opc == 0x0F20 && fetched >= (prefix_len + 3)) {
        *instruction_size = (uint32_t)(prefix_len + 3);
        return CPUEAXH_ESCAPE_INSN_READCRX;
    }
    if (opc == 0x0F22 && fetched >= (prefix_len + 3)) {
        *instruction_size = (uint32_t)(prefix_len + 3);
        return CPUEAXH_ESCAPE_INSN_WRITECRX;
    }
    bool is_rdssp_64 = false;
    if (opc == 0x0F1E && is_rdssp_instruction(bytes, (size_t)fetched, &is_rdssp_64, instruction_size)) {
        return is_rdssp_64 ? CPUEAXH_ESCAPE_INSN_RDSSPQ : CPUEAXH_ESCAPE_INSN_RDSSPD;
    }
    return CPUEAXH_ESCAPE_INSN_NONE;
}

static cpueaxh_err cpueaxh_ensure_escape_capacity(cpueaxh_engine* engine, size_t required_count) {
    if (!engine) {
        return CPUEAXH_ERR_ARG;
    }
    if (required_count <= engine->escape_capacity) {
        return CPUEAXH_ERR_OK;
    }

    size_t new_capacity = engine->escape_capacity == 0 ? 8 : engine->escape_capacity;
    while (new_capacity < required_count) {
        new_capacity *= 2;
    }

    CPUEAXH_ESCAPE_ENTRY* new_entries = reinterpret_cast<CPUEAXH_ESCAPE_ENTRY*>(
        CPUEAXH_ALLOC_ZEROED(new_capacity * sizeof(CPUEAXH_ESCAPE_ENTRY)));
    if (!new_entries) {
        return CPUEAXH_ERR_NOMEM;
    }

    if (engine->escapes && engine->escape_count != 0) {
        CPUEAXH_MEMCPY(new_entries, engine->escapes, engine->escape_count * sizeof(CPUEAXH_ESCAPE_ENTRY));
        CPUEAXH_FREE(engine->escapes);
    }

    engine->escapes = new_entries;
    engine->escape_capacity = new_capacity;
    return CPUEAXH_ERR_OK;
}

static cpueaxh_err cpueaxh_validate_engine(cpueaxh_engine* engine) {
    return engine ? CPUEAXH_ERR_OK : CPUEAXH_ERR_ARG;
}

static cpueaxh_err cpueaxh_apply_memory_mode(cpueaxh_engine* engine, uint32_t memory_mode) {
    if (!engine) {
        return CPUEAXH_ERR_ARG;
    }

    if (memory_mode != CPUEAXH_MEMORY_MODE_GUEST && memory_mode != CPUEAXH_MEMORY_MODE_HOST) {
        return CPUEAXH_ERR_MODE;
    }

    engine->memory_mode = memory_mode;
    mm_set_host_passthrough(
        &engine->memory_manager,
        MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXEC,
        memory_mode == CPUEAXH_MEMORY_MODE_HOST);
    return CPUEAXH_ERR_OK;
}

static cpueaxh_err cpueaxh_translate_access_error(MM_ACCESS_STATUS status, uint32_t perm) {
    if (status == MM_ACCESS_PROT) {
        switch (perm) {
        case MM_PROT_WRITE:
            return CPUEAXH_ERR_WRITE_PROT;
        case MM_PROT_EXEC:
            return CPUEAXH_ERR_FETCH_PROT;
        case MM_PROT_READ:
        default:
            return CPUEAXH_ERR_READ_PROT;
        }
    }

    switch (perm) {
    case MM_PROT_WRITE:
        return CPUEAXH_ERR_WRITE_UNMAPPED;
    case MM_PROT_EXEC:
        return CPUEAXH_ERR_FETCH_UNMAPPED;
    case MM_PROT_READ:
    default:
        return CPUEAXH_ERR_READ_UNMAPPED;
    }
}

static cpueaxh_err cpueaxh_mem_write_raw(cpueaxh_engine* engine, uint64_t address, const void* bytes, size_t size) {
    const uint8_t* source = reinterpret_cast<const uint8_t*>(bytes);
    for (size_t index = 0; index < size; index++) {
        MM_ACCESS_STATUS status = mm_write_byte_checked(&engine->memory_manager, address + index, source[index]);
        if (status != MM_ACCESS_OK) {
            return cpueaxh_translate_access_error(status, MM_PROT_WRITE);
        }
    }
    return CPUEAXH_ERR_OK;
}

static cpueaxh_err cpueaxh_mem_read_raw(cpueaxh_engine* engine, uint64_t address, void* bytes, size_t size) {
    uint8_t* target = reinterpret_cast<uint8_t*>(bytes);
    for (size_t index = 0; index < size; index++) {
        MM_ACCESS_STATUS status = mm_read_byte_checked(&engine->memory_manager, address + index, &target[index], MM_PROT_READ);
        if (status != MM_ACCESS_OK) {
            return cpueaxh_translate_access_error(status, MM_PROT_READ);
        }
    }
    return CPUEAXH_ERR_OK;
}

static cpueaxh_err cpueaxh_translate_patch_error(MM_PATCH_STATUS status) {
    switch (status) {
    case MM_PATCH_OK:
        return CPUEAXH_ERR_OK;
    case MM_PATCH_ARG:
        return CPUEAXH_ERR_ARG;
    case MM_PATCH_NOMEM:
        return CPUEAXH_ERR_NOMEM;
    case MM_PATCH_CONFLICT:
    case MM_PATCH_NOT_FOUND:
    default:
        return CPUEAXH_ERR_PATCH;
    }
}

static cpueaxh_err cpueaxh_reg_read_raw(const CPU_CONTEXT* context, int regid, void* value) {
    if (!value) {
        return CPUEAXH_ERR_ARG;
    }

    if (regid >= CPUEAXH_X86_REG_RAX && regid <= CPUEAXH_X86_REG_R15) {
        *reinterpret_cast<uint64_t*>(value) = context->regs[regid];
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_XMM0 && regid <= CPUEAXH_X86_REG_XMM15) {
        cpueaxh_copy_xmm_out(reinterpret_cast<cpueaxh_x86_xmm*>(value), &context->xmm[regid - CPUEAXH_X86_REG_XMM0]);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_YMM0 && regid <= CPUEAXH_X86_REG_YMM15) {
        cpueaxh_x86_ymm* output = reinterpret_cast<cpueaxh_x86_ymm*>(value);
        const int ymm_index = regid - CPUEAXH_X86_REG_YMM0;
        cpueaxh_copy_ymm_out(output, &context->xmm[ymm_index], &context->ymm_upper[ymm_index]);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_ZMM0 && regid <= CPUEAXH_X86_REG_ZMM15) {
        cpueaxh_x86_zmm* output = reinterpret_cast<cpueaxh_x86_zmm*>(value);
        const int zmm_index = regid - CPUEAXH_X86_REG_ZMM0;
        cpueaxh_copy_zmm_out(output, &context->xmm[zmm_index], &context->ymm_upper[zmm_index], &context->zmm_upper[zmm_index]);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_K0 && regid <= CPUEAXH_X86_REG_K7) {
        *reinterpret_cast<uint64_t*>(value) = context->opmask[regid - CPUEAXH_X86_REG_K0];
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_MM0 && regid <= CPUEAXH_X86_REG_MM7) {
        *reinterpret_cast<uint64_t*>(value) = context->mm[regid - CPUEAXH_X86_REG_MM0];
        return CPUEAXH_ERR_OK;
    }

    const int control_reg_index = cpueaxh_control_reg_index_from_regid(regid);
    if (control_reg_index >= 0) {
        *reinterpret_cast<uint64_t*>(value) = context->control_regs[control_reg_index];
        return CPUEAXH_ERR_OK;
    }

    switch (regid) {
    case CPUEAXH_X86_REG_RIP:
        *reinterpret_cast<uint64_t*>(value) = context->rip;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_EFLAGS:
        *reinterpret_cast<uint64_t*>(value) = context->rflags;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_CPL:
        *reinterpret_cast<uint64_t*>(value) = context->cpl;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_MXCSR:
        *reinterpret_cast<uint32_t*>(value) = context->mxcsr;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_GDTR_BASE:
        *reinterpret_cast<uint64_t*>(value) = context->gdtr_base;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_GDTR_LIMIT:
        *reinterpret_cast<uint16_t*>(value) = context->gdtr_limit;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_LDTR_BASE:
        *reinterpret_cast<uint64_t*>(value) = context->ldtr_base;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_LDTR_LIMIT:
        *reinterpret_cast<uint16_t*>(value) = context->ldtr_limit;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_EXCEPTION_CODE:
        *reinterpret_cast<uint32_t*>(value) = context->exception.code;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_EXCEPTION_ERROR_CODE:
        *reinterpret_cast<uint32_t*>(value) = context->exception.error_code;
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_PROCESSOR_ID:
        *reinterpret_cast<uint32_t*>(value) = context->processor_id;
        return CPUEAXH_ERR_OK;
    default:
        break;
    }

    const SegmentRegister* segment = cpueaxh_select_segment(context, regid);
    if (segment) {
        switch (cpueaxh_segment_field_kind_from_regid(regid)) {
        case CPUEAXH_SEGMENT_FIELD_SELECTOR:
            *reinterpret_cast<uint16_t*>(value) = segment->selector;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_BASE:
            *reinterpret_cast<uint64_t*>(value) = segment->descriptor.base;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_LIMIT:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.limit;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_TYPE:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.type;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_DPL:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.dpl;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_PRESENT:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.present ? 1u : 0u;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_GRANULARITY:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.granularity ? 1u : 0u;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_DB:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.db ? 1u : 0u;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_LONG_MODE:
            *reinterpret_cast<uint32_t*>(value) = segment->descriptor.long_mode ? 1u : 0u;
            return CPUEAXH_ERR_OK;
        default:
            break;
        }
    }

    return CPUEAXH_ERR_ARG;
}

static cpueaxh_err cpueaxh_reg_write_raw(CPU_CONTEXT* context, int regid, const void* value) {
    if (!value) {
        return CPUEAXH_ERR_ARG;
    }

    if (regid >= CPUEAXH_X86_REG_RAX && regid <= CPUEAXH_X86_REG_R15) {
        context->regs[regid] = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_XMM0 && regid <= CPUEAXH_X86_REG_XMM15) {
        cpueaxh_copy_xmm_in(&context->xmm[regid - CPUEAXH_X86_REG_XMM0], reinterpret_cast<const cpueaxh_x86_xmm*>(value));
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_YMM0 && regid <= CPUEAXH_X86_REG_YMM15) {
        const cpueaxh_x86_ymm* input = reinterpret_cast<const cpueaxh_x86_ymm*>(value);
        const int ymm_index = regid - CPUEAXH_X86_REG_YMM0;
        cpueaxh_copy_ymm_in(&context->xmm[ymm_index], &context->ymm_upper[ymm_index], input);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_ZMM0 && regid <= CPUEAXH_X86_REG_ZMM15) {
        const cpueaxh_x86_zmm* input = reinterpret_cast<const cpueaxh_x86_zmm*>(value);
        const int zmm_index = regid - CPUEAXH_X86_REG_ZMM0;
        cpueaxh_copy_zmm_in(&context->xmm[zmm_index], &context->ymm_upper[zmm_index], &context->zmm_upper[zmm_index], input);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_K0 && regid <= CPUEAXH_X86_REG_K7) {
        context->opmask[regid - CPUEAXH_X86_REG_K0] = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    }
    if (regid >= CPUEAXH_X86_REG_MM0 && regid <= CPUEAXH_X86_REG_MM7) {
        context->mm[regid - CPUEAXH_X86_REG_MM0] = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    }

    const int control_reg_index = cpueaxh_control_reg_index_from_regid(regid);
    if (control_reg_index >= 0) {
        context->control_regs[control_reg_index] = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    }

    switch (regid) {
    case CPUEAXH_X86_REG_RIP:
        context->rip = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_EFLAGS:
        context->rflags = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_CPL:
        context->cpl = (uint8_t)(*reinterpret_cast<const uint64_t*>(value) & 0x3u);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_MXCSR:
        context->mxcsr = *reinterpret_cast<const uint32_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_GDTR_BASE:
        context->gdtr_base = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_GDTR_LIMIT:
        context->gdtr_limit = *reinterpret_cast<const uint16_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_LDTR_BASE:
        context->ldtr_base = *reinterpret_cast<const uint64_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_LDTR_LIMIT:
        context->ldtr_limit = *reinterpret_cast<const uint16_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_EXCEPTION_CODE:
        context->exception.code = *reinterpret_cast<const uint32_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_EXCEPTION_ERROR_CODE:
        context->exception.error_code = *reinterpret_cast<const uint32_t*>(value);
        return CPUEAXH_ERR_OK;
    case CPUEAXH_X86_REG_PROCESSOR_ID:
        context->processor_id = *reinterpret_cast<const uint32_t*>(value);
        return CPUEAXH_ERR_OK;
    default:
        break;
    }

    SegmentRegister* segment = cpueaxh_select_segment(context, regid);
    if (segment) {
        switch (cpueaxh_segment_field_kind_from_regid(regid)) {
        case CPUEAXH_SEGMENT_FIELD_SELECTOR:
            segment->selector = *reinterpret_cast<const uint16_t*>(value);
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_BASE:
            segment->descriptor.base = *reinterpret_cast<const uint64_t*>(value);
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_LIMIT:
            segment->descriptor.limit = *reinterpret_cast<const uint32_t*>(value);
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_TYPE:
            segment->descriptor.type = (uint8_t)(*reinterpret_cast<const uint32_t*>(value) & 0x1Fu);
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_DPL:
            segment->descriptor.dpl = (uint8_t)(*reinterpret_cast<const uint32_t*>(value) & 0x3u);
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_PRESENT:
            segment->descriptor.present = (*reinterpret_cast<const uint32_t*>(value) & 0x1u) != 0;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_GRANULARITY:
            segment->descriptor.granularity = (*reinterpret_cast<const uint32_t*>(value) & 0x1u) != 0;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_DB:
            segment->descriptor.db = (*reinterpret_cast<const uint32_t*>(value) & 0x1u) != 0;
            return CPUEAXH_ERR_OK;
        case CPUEAXH_SEGMENT_FIELD_LONG_MODE:
            segment->descriptor.long_mode = (*reinterpret_cast<const uint32_t*>(value) & 0x1u) != 0;
            return CPUEAXH_ERR_OK;
        default:
            break;
        }
    }

    return CPUEAXH_ERR_ARG;
}

static bool cpueaxh_is_optional_escape_instruction(cpueaxh_escape_insn_id instruction_id) {
    return instruction_id == CPUEAXH_ESCAPE_INSN_RDSSPD ||
        instruction_id == CPUEAXH_ESCAPE_INSN_RDSSPQ;
}

static void cpueaxh_dispatch_code_hooks(cpueaxh_engine* engine, uint32_t type, uint64_t address) {
    if (!engine || !cpueaxh_is_hook_type_index_valid(type) || engine->hook_type_counts[type] == 0) {
        return;
    }

    for (int index = 0; index < CPUEAXH_MAX_HOOKS; index++) {
        CPUEAXH_HOOK_ENTRY* hook = &engine->hooks[index];
        if (!hook->used || hook->type != type || !hook->code_callback) {
            continue;
        }

        if (!cpueaxh_range_contains(hook->begin, hook->end, address)) {
            continue;
        }

        hook->code_callback(engine, address, hook->user_data);
    }
}

static bool cpueaxh_try_dispatch_escape(cpueaxh_engine* engine, uint64_t address, uint8_t* bytes, int fetched, uint32_t* escaped_size, cpueaxh_err* callback_error, CPUEAXH_HOST_BRIDGE_BLOCK* bridge_block) {
    if (!engine || !bytes || fetched <= 0 || !escaped_size || !callback_error) {
        return false;
    }

    uint32_t instruction_size = 0;
    cpueaxh_escape_insn_id instruction_id = cpueaxh_classify_escape_instruction(bytes, fetched, &instruction_size);
    if (instruction_id == CPUEAXH_ESCAPE_INSN_NONE || instruction_size == 0) {
        return false;
    }

    CPUEAXH_ESCAPE_ENTRY* escape = cpueaxh_lookup_escape_entry(engine, instruction_id);
    if (escape && escape->callback && cpueaxh_range_contains(escape->begin, escape->end, address)) {
        cpueaxh_x86_context context = {};
        uint64_t rip_before = engine->context.rip;
        cpueaxh_context_out(&context, &engine->context);
        context.internal_bridge_block = reinterpret_cast<uint64_t>(bridge_block);

        cpueaxh_err error = escape->callback(engine, &context, bytes, escape->user_data);
        if (error != CPUEAXH_ERR_OK) {
            *callback_error = error;
            return true;
        }

        cpueaxh_context_in(&engine->context, &context);
        if (engine->context.rip == rip_before) {
            engine->context.rip = rip_before + (uint64_t)instruction_size;
        }
        engine->context.last_inst_size = (int)instruction_size;
        *escaped_size = instruction_size;
        if (cpu_has_exception(&engine->context)) {
            *callback_error = CPUEAXH_ERR_EXCEPTION;
        }
        else {
            *callback_error = CPUEAXH_ERR_OK;
        }
        return true;
    }

    if (cpueaxh_is_optional_escape_instruction(instruction_id)) {
        return false;
    }

    if (instruction_id == CPUEAXH_ESCAPE_INSN_INT3) {
        cpu_raise_exception(&engine->context, CPU_EXCEPTION_BP, 0);
    }
    else {
        cpu_raise_exception(&engine->context, CPU_EXCEPTION_UD, 0);
    }
    *callback_error = CPUEAXH_ERR_EXCEPTION;
    return true;
}

static cpueaxh_err cpueaxh_translate_step_error(cpueaxh_engine* engine, int status) {
    switch (status) {
    case CPU_STEP_OK:
    case CPU_STEP_HALT:
        return CPUEAXH_ERR_OK;
    case CPU_STEP_FETCH_ERR:
        return CPUEAXH_ERR_FETCH_UNMAPPED;
    case CPU_STEP_UD:
        return CPUEAXH_ERR_EXCEPTION;
    case CPU_STEP_EXCEPTION:
        if (engine->context.exception.code == CPU_EXCEPTION_PF) {
            const uint32_t error_code = engine->context.exception.error_code;
            const bool protection_violation = (error_code & 0x1u) != 0;
            const bool write_access = (error_code & 0x2u) != 0;
            const bool exec_access = (error_code & 0x10u) != 0;

            if (exec_access) {
                return protection_violation ? CPUEAXH_ERR_FETCH_PROT : CPUEAXH_ERR_FETCH_UNMAPPED;
            }
            if (write_access) {
                return protection_violation ? CPUEAXH_ERR_WRITE_PROT : CPUEAXH_ERR_WRITE_UNMAPPED;
            }
            return protection_violation ? CPUEAXH_ERR_READ_PROT : CPUEAXH_ERR_READ_UNMAPPED;
        }
        return CPUEAXH_ERR_EXCEPTION;
    default:
        return CPUEAXH_ERR_EXCEPTION;
    }
}

extern "C" cpueaxh_err cpueaxh_open(uint32_t arch, uint32_t mode, cpueaxh_engine** out_engine) {
    if (!out_engine) {
        return CPUEAXH_ERR_ARG;
    }
    if (arch != CPUEAXH_ARCH_X86) {
        *out_engine = NULL;
        return CPUEAXH_ERR_ARCH;
    }
    if (mode != CPUEAXH_MODE_64 && mode != CPUEAXH_MODE_COMPAT32) {
        *out_engine = NULL;
        return CPUEAXH_ERR_MODE;
    }

    cpueaxh_engine* engine = (cpueaxh_engine*)CPUEAXH_ALLOC_ZEROED(sizeof(cpueaxh_engine));
    if (!engine) {
        *out_engine = NULL;
        return CPUEAXH_ERR_NOMEM;
    }

    mm_init(&engine->memory_manager);
    init_cpu_context(&engine->context, &engine->memory_manager, mode == CPUEAXH_MODE_COMPAT32);
    engine->context.owner_engine = engine;
    engine->next_hook = 1;
    engine->memory_mode = CPUEAXH_MEMORY_MODE_GUEST;
    cpueaxh_apply_memory_mode(engine, CPUEAXH_MEMORY_MODE_GUEST);
    engine->last_error = CPUEAXH_ERR_OK;
    *out_engine = engine;
    return CPUEAXH_ERR_OK;
}

extern "C" void cpueaxh_close(cpueaxh_engine* engine) {
    if (!engine) {
        return;
    }
    mm_destroy(&engine->memory_manager);
    CPUEAXH_FREE(engine->escapes);
    CPUEAXH_FREE(engine);
}

extern "C" cpueaxh_err cpueaxh_set_memory_mode(cpueaxh_engine* engine, uint32_t memory_mode) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    return cpueaxh_apply_memory_mode(engine, memory_mode);
}

extern "C" cpueaxh_err cpueaxh_mem_map(cpueaxh_engine* engine, uint64_t address, size_t size, uint32_t perms) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || size == 0) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }

    if (!mm_is_page_aligned(address) || !mm_is_page_aligned((uint64_t)size) ||
        mm_range_overflows(address, (uint64_t)size) || !mm_is_valid_perms(perms)) {
        return CPUEAXH_ERR_ARG;
    }

    if (!mm_map_internal(&engine->memory_manager, address, (uint64_t)size, perms)) {
        return CPUEAXH_ERR_MAP;
    }
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_mem_map_ptr(cpueaxh_engine* engine, uint64_t address, size_t size, uint32_t perms, void* host_ptr) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || size == 0 || !host_ptr) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }

    if (!mm_is_page_aligned(address) || !mm_is_page_aligned((uint64_t)size) ||
        mm_range_overflows(address, (uint64_t)size) || !mm_is_valid_perms(perms)) {
        return CPUEAXH_ERR_ARG;
    }

    if (!mm_map_host(&engine->memory_manager, address, (uint64_t)size, perms, host_ptr)) {
        return CPUEAXH_ERR_MAP;
    }
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_mem_unmap(cpueaxh_engine* engine, uint64_t address, size_t size) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    if (size == 0) {
        return CPUEAXH_ERR_OK;
    }
    if (!mm_is_page_aligned(address) || !mm_is_page_aligned((uint64_t)size) || mm_range_overflows(address, (uint64_t)size)) {
        return CPUEAXH_ERR_ARG;
    }
    if (!mm_check_range_mapped(&engine->memory_manager, address, (uint64_t)size)) {
        return CPUEAXH_ERR_MAP;
    }

    return mm_unmap(&engine->memory_manager, address, (uint64_t)size) ? CPUEAXH_ERR_OK : CPUEAXH_ERR_MAP;
}

extern "C" cpueaxh_err cpueaxh_mem_protect(cpueaxh_engine* engine, uint64_t address, size_t size, uint32_t perms) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    if (size == 0) {
        return CPUEAXH_ERR_OK;
    }
    if (!mm_is_page_aligned(address) || !mm_is_page_aligned((uint64_t)size) ||
        mm_range_overflows(address, (uint64_t)size) || !mm_is_valid_perms(perms)) {
        return CPUEAXH_ERR_ARG;
    }
    if (!mm_check_range_mapped(&engine->memory_manager, address, (uint64_t)size)) {
        return CPUEAXH_ERR_MAP;
    }

    return mm_protect(&engine->memory_manager, address, (uint64_t)size, perms) ? CPUEAXH_ERR_OK : CPUEAXH_ERR_MAP;
}

extern "C" cpueaxh_err cpueaxh_mem_set_cpu_attrs(cpueaxh_engine* engine, uint64_t address, size_t size, uint32_t attrs) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    if (size == 0) {
        return CPUEAXH_ERR_OK;
    }
    if (!mm_is_page_aligned(address) || !mm_is_page_aligned((uint64_t)size) ||
        mm_range_overflows(address, (uint64_t)size) || !mm_is_valid_cpu_attrs(attrs)) {
        return CPUEAXH_ERR_ARG;
    }
    if (!mm_check_range_mapped(&engine->memory_manager, address, (uint64_t)size)) {
        return CPUEAXH_ERR_MAP;
    }

    return mm_set_cpu_attrs(&engine->memory_manager, address, (uint64_t)size, attrs) ? CPUEAXH_ERR_OK : CPUEAXH_ERR_MAP;
}

extern "C" cpueaxh_err cpueaxh_mem_regions(const cpueaxh_engine* engine, cpueaxh_mem_region** regions, uint32_t* count) {
    if (!regions || !count) {
        return CPUEAXH_ERR_ARG;
    }

    *regions = NULL;
    *count = 0;

    if (!engine) {
        return CPUEAXH_ERR_ARG;
    }

    const size_t region_count = engine->memory_manager.region_count;
    if (region_count == 0) {
        return CPUEAXH_ERR_OK;
    }

    if (region_count > 0xFFFFFFFFu) {
        return CPUEAXH_ERR_NOMEM;
    }

    cpueaxh_mem_region* result = reinterpret_cast<cpueaxh_mem_region*>(
        CPUEAXH_ALLOC_ZEROED(region_count * sizeof(cpueaxh_mem_region)));
    if (!result) {
        return CPUEAXH_ERR_NOMEM;
    }

    for (size_t i = 0; i < region_count; i++) {
        const MEMORY_REGION* source = &engine->memory_manager.regions[i];
        result[i].begin = source->base;
        result[i].end = source->base + source->size - 1;
        result[i].perms = source->perms;
        result[i].cpu_attrs = source->cpu_attrs;
    }

    *regions = result;
    *count = (uint32_t)region_count;
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_mem_write(cpueaxh_engine* engine, uint64_t address, const void* bytes, size_t size) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || (!bytes && size != 0)) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }
    return cpueaxh_mem_write_raw(engine, address, bytes, size);
}

extern "C" cpueaxh_err cpueaxh_mem_read(cpueaxh_engine* engine, uint64_t address, void* bytes, size_t size) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || (!bytes && size != 0)) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }
    return cpueaxh_mem_read_raw(engine, address, bytes, size);
}

extern "C" cpueaxh_err cpueaxh_mem_patch_add(cpueaxh_engine* engine, cpueaxh_mem_patch_handle* out_patch, uint64_t address, const void* bytes, size_t size) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || !out_patch || (!bytes && size != 0)) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }
    if (engine->memory_mode != CPUEAXH_MEMORY_MODE_HOST) {
        return CPUEAXH_ERR_MODE;
    }

    return cpueaxh_translate_patch_error(mm_add_patch(&engine->memory_manager, out_patch, address, bytes, (uint64_t)size));
}

extern "C" cpueaxh_err cpueaxh_mem_patch_del(cpueaxh_engine* engine, cpueaxh_mem_patch_handle patch) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    return cpueaxh_translate_patch_error(mm_del_patch(&engine->memory_manager, patch));
}

extern "C" cpueaxh_err cpueaxh_reg_write(cpueaxh_engine* engine, int regid, const void* value) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    return cpueaxh_reg_write_raw(&engine->context, regid, value);
}

extern "C" cpueaxh_err cpueaxh_reg_read(cpueaxh_engine* engine, int regid, void* value) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    return cpueaxh_reg_read_raw(&engine->context, regid, value);
}

extern "C" cpueaxh_err cpueaxh_context_write(cpueaxh_engine* engine, const cpueaxh_x86_context* context) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    if (!context) {
        return CPUEAXH_ERR_ARG;
    }

    cpueaxh_context_in(&engine->context, context);
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_context_read(const cpueaxh_engine* engine, cpueaxh_x86_context* context) {
    cpueaxh_err error = cpueaxh_validate_engine(const_cast<cpueaxh_engine*>(engine));
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    if (!context) {
        return CPUEAXH_ERR_ARG;
    }

    cpueaxh_context_out(context, &engine->context);
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_set_processor_id(cpueaxh_engine* engine, uint32_t processor_id) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    engine->context.processor_id = processor_id;
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_emu_start(cpueaxh_engine* engine, uint64_t begin, uint64_t until, uint64_t timeout, size_t count) {
    (void)timeout;
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    if (begin != 0) {
        engine->context.rip = begin;
    }

    engine->stop_requested = 0;
    cpu_clear_exception(&engine->context);
    CPUEAXH_HOST_BRIDGE_BLOCK host_bridge_block = {};
    cpueaxh_err result = CPUEAXH_ERR_OK;
    const bool has_registered_escapes = engine->escape_count != 0;
    bool has_any_hook = false;
    for (uint32_t hook_type = CPUEAXH_HOOK_CODE_PRE; hook_type <= CPUEAXH_HOOK_MEM_FETCH_PROT; hook_type++) {
        if (engine->hook_type_counts[hook_type] != 0) {
            has_any_hook = true;
            break;
        }
    }

    size_t executed = 0;
    if (!has_any_hook && !has_registered_escapes) {
        for (;;) {
            if (count != 0 && executed >= count) {
                break;
            }
            if (until != 0 && engine->context.rip == until) {
                break;
            }
            if (engine->stop_requested) {
                break;
            }

            int status = cpu_step(&engine->context);
            if (status == CPU_STEP_OK) {
                executed++;
                continue;
            }
            if (status == CPU_STEP_HALT) {
                engine->last_error = CPUEAXH_ERR_OK;
                goto cpueaxh_emu_start_finish;
            }

            error = cpueaxh_translate_step_error(engine, status);
            engine->last_error = error;
            result = error;
            goto cpueaxh_emu_start_finish;
        }

        engine->last_error = CPUEAXH_ERR_OK;
        goto cpueaxh_emu_start_finish;
    }

    for (;;) {
        if (count != 0 && executed >= count) {
            break;
        }
        if (until != 0 && engine->context.rip == until) {
            break;
        }
        if (engine->stop_requested) {
            break;
        }

        uint64_t address = engine->context.rip;
        if (engine->hook_type_counts[CPUEAXH_HOOK_CODE_PRE] != 0) {
            cpueaxh_dispatch_code_hooks(engine, CPUEAXH_HOOK_CODE_PRE, address);
        }
        if (engine->stop_requested) {
            break;
        }

        uint8_t bytes[MAX_INST_FETCH];
        int fetched = fetch_instruction_bytes(&engine->context, address, bytes);

        uint32_t escaped_size = 0;
        if (cpueaxh_try_dispatch_escape(engine, address, bytes, fetched, &escaped_size, &error, &host_bridge_block)) {
            if (error != CPUEAXH_ERR_OK) {
                engine->last_error = error;
                result = error;
                goto cpueaxh_emu_start_finish;
            }

            executed++;
            if (engine->hook_type_counts[CPUEAXH_HOOK_CODE_POST] != 0) {
                cpueaxh_dispatch_code_hooks(engine, CPUEAXH_HOOK_CODE_POST, address);
            }
            continue;
        }

        int status = cpu_step_with_prefetch(&engine->context, bytes, fetched);
        if (status == CPU_STEP_OK) {
            executed++;
            if (engine->hook_type_counts[CPUEAXH_HOOK_CODE_POST] != 0) {
                cpueaxh_dispatch_code_hooks(engine, CPUEAXH_HOOK_CODE_POST, address);
            }
            continue;
        }
        if (status == CPU_STEP_HALT) {
            if (engine->hook_type_counts[CPUEAXH_HOOK_CODE_POST] != 0) {
                cpueaxh_dispatch_code_hooks(engine, CPUEAXH_HOOK_CODE_POST, address);
            }
            engine->last_error = CPUEAXH_ERR_OK;
            goto cpueaxh_emu_start_finish;
        }

        error = cpueaxh_translate_step_error(engine, status);
        engine->last_error = error;
        result = error;
        goto cpueaxh_emu_start_finish;
    }

    engine->last_error = CPUEAXH_ERR_OK;
cpueaxh_emu_start_finish:
    return result;
}

extern "C" cpueaxh_err cpueaxh_emu_start_function(cpueaxh_engine* engine, uint64_t begin, uint64_t timeout, size_t count) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    uint64_t stack_address = 0;
    error = cpueaxh_reg_read_raw(&engine->context, CPUEAXH_X86_REG_RSP, &stack_address);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    uint64_t original_return = 0;
    error = cpueaxh_mem_read_raw(engine, stack_address, &original_return, sizeof(original_return));
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    const uint64_t magic_return = CPUEAXH_EMU_RETURN_MAGIC;
    error = cpueaxh_mem_write_raw(engine, stack_address, &magic_return, sizeof(magic_return));
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    cpueaxh_err run_error = cpueaxh_emu_start(engine, begin, magic_return, timeout, count);

    cpueaxh_err restore_error = cpueaxh_mem_write_raw(engine, stack_address, &original_return, sizeof(original_return));
    if (run_error != CPUEAXH_ERR_OK) {
        return run_error;
    }
    if (restore_error != CPUEAXH_ERR_OK) {
        return restore_error;
    }

    return CPUEAXH_ERR_OK;
}

extern "C" void cpueaxh_emu_stop(cpueaxh_engine* engine) {
    if (!engine) {
        return;
    }
    engine->stop_requested = 1;
}

extern "C" cpueaxh_err cpueaxh_exception_raise(cpueaxh_engine* engine, uint32_t code, uint32_t error_code) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    if (!cpu_is_exception_code(code)) {
        return CPUEAXH_ERR_ARG;
    }
    cpu_raise_exception(&engine->context, code, error_code);
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_exception_clear(cpueaxh_engine* engine) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }
    cpu_clear_exception(&engine->context);
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_host_call(cpueaxh_x86_context* context, cpueaxh_cb_host_bridge_t bridge) {
    if (!context || !bridge || context->internal_bridge_block == 0) {
        return CPUEAXH_ERR_ARG;
    }

    return cpueaxh_host_call_asm(context, bridge, reinterpret_cast<void*>(context->internal_bridge_block));
}

extern "C" cpueaxh_err cpueaxh_hook_add(cpueaxh_engine* engine, cpueaxh_hook* out_hook, uint32_t type, void* callback, void* user_data, uint64_t begin, uint64_t end) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || !out_hook || !callback) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }
    if (!cpueaxh_is_supported_hook_type(type)) {
        return CPUEAXH_ERR_HOOK;
    }

    for (int index = 0; index < CPUEAXH_MAX_HOOKS; index++) {
        if (engine->hooks[index].used) {
            continue;
        }

        engine->hooks[index].used = true;
        engine->hooks[index].handle = engine->next_hook++;
        engine->hooks[index].type = type;
        engine->hooks[index].code_callback = 0;
        engine->hooks[index].mem_callback = 0;
        engine->hooks[index].invalid_mem_callback = 0;
        if (cpueaxh_is_code_hook_type(type)) {
            engine->hooks[index].code_callback = (cpueaxh_cb_hookcode_t)callback;
        }
        else if (cpueaxh_is_invalid_memory_hook_type(type)) {
            engine->hooks[index].invalid_mem_callback = (cpueaxh_cb_hookmem_invalid_t)callback;
        }
        else {
            engine->hooks[index].mem_callback = (cpueaxh_cb_hookmem_t)callback;
        }
        engine->hooks[index].user_data = user_data;
        engine->hooks[index].begin = begin;
        engine->hooks[index].end = end;
        cpueaxh_adjust_hook_type_count(engine, type, 1);
        *out_hook = engine->hooks[index].handle;
        return CPUEAXH_ERR_OK;
    }

    return CPUEAXH_ERR_HOOK;
}

extern "C" cpueaxh_err cpueaxh_hook_add_address(cpueaxh_engine* engine, cpueaxh_hook* out_hook, uint32_t type, void* callback, void* user_data, uint64_t address) {
    return cpueaxh_hook_add(engine, out_hook, type, callback, user_data, address, address);
}

extern "C" cpueaxh_err cpueaxh_hook_del(cpueaxh_engine* engine, cpueaxh_hook hook) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    for (int index = 0; index < CPUEAXH_MAX_HOOKS; index++) {
        if (engine->hooks[index].used && engine->hooks[index].handle == hook) {
            cpueaxh_adjust_hook_type_count(engine, engine->hooks[index].type, -1);
            CPUEAXH_MEMSET(&engine->hooks[index], 0, sizeof(engine->hooks[index]));
            return CPUEAXH_ERR_OK;
        }
    }

    for (size_t index = 0; index < engine->escape_count; index++) {
        if (engine->escapes[index].handle == hook) {
            if (index + 1 < engine->escape_count) {
                CPUEAXH_MEMMOVE(
                    &engine->escapes[index],
                    &engine->escapes[index + 1],
                    (engine->escape_count - index - 1) * sizeof(engine->escapes[index]));
            }
            engine->escape_count--;
            if (engine->escape_count < engine->escape_capacity) {
                CPUEAXH_MEMSET(&engine->escapes[engine->escape_count], 0, sizeof(engine->escapes[engine->escape_count]));
            }
            cpueaxh_rebuild_escape_index(engine);
            return CPUEAXH_ERR_OK;
        }
    }

    return CPUEAXH_ERR_HOOK;
}

extern "C" cpueaxh_err cpueaxh_escape_add(cpueaxh_engine* engine, cpueaxh_escape_handle* out_hook, cpueaxh_escape_insn_id instruction_id, void* callback, void* user_data, uint64_t begin, uint64_t end) {
    cpueaxh_err error = cpueaxh_validate_engine(engine);
    if (error != CPUEAXH_ERR_OK || !out_hook || !callback || instruction_id == CPUEAXH_ESCAPE_INSN_NONE) {
        return error != CPUEAXH_ERR_OK ? error : CPUEAXH_ERR_ARG;
    }

    if (cpueaxh_lookup_escape_entry(engine, instruction_id)) {
        return CPUEAXH_ERR_HOOK;
    }

    error = cpueaxh_ensure_escape_capacity(engine, engine->escape_count + 1);
    if (error != CPUEAXH_ERR_OK) {
        return error;
    }

    CPUEAXH_ESCAPE_ENTRY* escape = &engine->escapes[engine->escape_count++];
    escape->handle = engine->next_hook++;
    escape->callback = (cpueaxh_cb_escape_t)callback;
    escape->user_data = user_data;
    escape->begin = begin;
    escape->end = end;
    escape->instruction_id = instruction_id;
    cpueaxh_rebuild_escape_index(engine);
    *out_hook = escape->handle;
    return CPUEAXH_ERR_OK;
}

extern "C" cpueaxh_err cpueaxh_escape_del(cpueaxh_engine* engine, cpueaxh_escape_handle hook) {
    return cpueaxh_hook_del(engine, hook);
}

extern "C" uint32_t cpueaxh_code_exception(const cpueaxh_engine* engine) {
    return engine ? engine->context.exception.code : CPU_EXCEPTION_NONE;
}

extern "C" uint32_t cpueaxh_error_code_exception(const cpueaxh_engine* engine) {
    return engine ? engine->context.exception.error_code : 0;
}

extern "C" void cpueaxh_free(void* ptr) {
    if (!ptr) {
        return;
    }
    CPUEAXH_FREE(ptr);
}