// cpu/helper.hpp - CPU helper functions (segments, descriptors, register access, addressing)

SegmentDescriptor load_descriptor_from_table(CPU_CONTEXT* ctx, uint16_t selector) {
    SegmentDescriptor desc = {};
    uint16_t index = selector >> 3;
    bool ti = (selector >> 2) & 1;

    uint64_t table_base;
    uint16_t table_limit;

    if (ti) {
        table_base = ctx->ldtr_base;
        table_limit = ctx->ldtr_limit;
    }
    else {
        table_base = ctx->gdtr_base;
        table_limit = ctx->gdtr_limit;
    }

    uint64_t desc_offset = (uint64_t)index * 8;
    if (desc_offset + 7 > table_limit) {
        raise_gp_ctx(ctx, selector & 0xFFFC);
        return desc;
    }

    uint64_t desc_addr = table_base + desc_offset;

    uint32_t low = read_memory_dword(ctx, desc_addr);
    uint32_t high = read_memory_dword(ctx, desc_addr + 4);

    desc.base = (low >> 16) | ((high & 0xFF) << 16) | ((high >> 24) << 24);
    desc.limit = (low & 0xFFFF) | ((high & 0x000F0000));
    desc.type = (high >> 8) & 0x0F;
    desc.dpl = (high >> 13) & 0x03;
    desc.present = (high >> 15) & 0x01;
    desc.granularity = (high >> 23) & 0x01;
    desc.db = (high >> 22) & 0x01;
    desc.long_mode = (high >> 21) & 0x01;

    if (desc.granularity) {
        desc.limit = (desc.limit << 12) | 0xFFF;
    }

    return desc;
}

bool is_null_selector(uint16_t selector) {
    return (selector & 0xFFFC) == 0;
}

bool is_data_segment(uint8_t type) {
    return (type & 0x08) == 0;
}

bool is_writable_data_segment(uint8_t type) {
    return is_data_segment(type) && (type & 0x02);
}

bool is_code_segment(uint8_t type) {
    return (type & 0x08) != 0;
}

bool is_readable_code_segment(uint8_t type) {
    return is_code_segment(type) && (type & 0x02);
}

bool is_conforming_code_segment(uint8_t type) {
    return is_code_segment(type) && (type & 0x04);
}

void cpu_reset_prefix_state(CPU_CONTEXT* ctx) {
    if (!ctx) {
        return;
    }
    ctx->rex_present = false;
    ctx->rex_w = false;
    ctx->rex_r = false;
    ctx->rex_x = false;
    ctx->rex_b = false;
    ctx->operand_size_override = false;
    ctx->address_size_override = false;
}

bool cpu_try_apply_rex_prefix(CPU_CONTEXT* ctx, uint8_t prefix) {
    if (!ctx || !cpu_allows_rex_prefix(ctx) || prefix < 0x40 || prefix > 0x4F) {
        return false;
    }
    ctx->rex_present = true;
    ctx->rex_w = ((prefix >> 3) & 1) != 0;
    ctx->rex_r = ((prefix >> 2) & 1) != 0;
    ctx->rex_x = ((prefix >> 1) & 1) != 0;
    ctx->rex_b = (prefix & 1) != 0;
    return true;
}

SegmentRegister* get_segment_register(CPU_CONTEXT* ctx, int index) {
    static SegmentRegister dummy = {};

    switch (index) {
    case SEG_ES: return &ctx->es;
    case SEG_CS: return &ctx->cs;
    case SEG_SS: return &ctx->ss;
    case SEG_DS: return &ctx->ds;
    case SEG_FS: return &ctx->fs;
    case SEG_GS: return &ctx->gs;
    default:
        raise_gp_ctx(ctx, 0);
        return &dummy;
    }
}

bool is_valid_control_register(uint8_t index) {
    return index == REG_CR0 || index == REG_CR2 || index == REG_CR3 || index == REG_CR4 || index == REG_CR8;
}

uint64_t get_control_register(CPU_CONTEXT* ctx, uint8_t index) {
    if (!ctx || !is_valid_control_register(index)) {
        raise_ud_ctx(ctx);
        return 0;
    }

    return ctx->control_regs[index];
}

void set_control_register(CPU_CONTEXT* ctx, uint8_t index, uint64_t value) {
    if (!ctx || !is_valid_control_register(index)) {
        raise_ud_ctx(ctx);
        return;
    }

    ctx->control_regs[index] = value;
}

void load_segment_register(CPU_CONTEXT* ctx, int seg_index, uint16_t selector) {
    if (seg_index == SEG_CS) {
        raise_ud_ctx(ctx);
        return;
    }

    uint8_t rpl = selector & 0x03;

    if (seg_index == SEG_SS) {
        if (is_null_selector(selector)) {
            raise_gp_ctx(ctx, 0);
        }

        SegmentDescriptor desc = load_descriptor_from_table(ctx, selector);
        if (cpu_has_exception(ctx)) {
            return;
        }

        if (rpl != ctx->cpl) {
            raise_gp_ctx(ctx, selector & 0xFFFC);
        }

        if (!is_writable_data_segment(desc.type)) {
            raise_gp_ctx(ctx, selector & 0xFFFC);
        }

        if (desc.dpl != ctx->cpl) {
            raise_gp_ctx(ctx, selector & 0xFFFC);
        }

        if (!desc.present) {
            raise_ss_ctx(ctx, selector & 0xFFFC);
        }

        ctx->ss.selector = selector;
        ctx->ss.descriptor = desc;
    }
    else {
        SegmentRegister* seg = get_segment_register(ctx, seg_index);
        if (!seg) return;

        if (is_null_selector(selector)) {
            seg->selector = selector;
            seg->descriptor = {};
        }
        else {
            SegmentDescriptor desc = load_descriptor_from_table(ctx, selector);
            if (cpu_has_exception(ctx)) {
                return;
            }

            if (!is_data_segment(desc.type) && !is_readable_code_segment(desc.type)) {
                raise_gp_ctx(ctx, selector & 0xFFFC);
            }

            if (is_data_segment(desc.type) || !is_conforming_code_segment(desc.type)) {
                if (rpl > desc.dpl || ctx->cpl > desc.dpl) {
                    raise_gp_ctx(ctx, selector & 0xFFFC);
                }
            }

            if (!desc.present) {
                raise_np_ctx(ctx, selector & 0xFFFC);
            }

            seg->selector = selector;
            seg->descriptor = desc;
        }
    }
}

inline bool cpu_memory_base_register_uses_ss(int reg) {
    return reg == REG_RSP || reg == REG_RBP || reg == REG_R13;
}

inline int cpu_default_segment_for_memory_operand(const CPU_CONTEXT* ctx, uint8_t modrm, bool has_sib, uint8_t sib, int addr_size) {
    const uint8_t mod = (modrm >> 6) & 0x03;
    const uint8_t rm = modrm & 0x07;

    if (mod == 3) {
        return SEG_DS;
    }

    if (addr_size != 16) {
        const bool long_mode = cpu_allows_rex_prefix(ctx);
        if ((rm & 0x07) == 4 && has_sib) {
            uint8_t base = sib & 0x07;
            if (ctx && ctx->rex_b && long_mode) {
                base |= 0x08;
            }
            if (!(base == 5 && mod == 0) && cpu_memory_base_register_uses_ss(base)) {
                return SEG_SS;
            }
        }
        else {
            int decoded_rm = rm;
            if (ctx && ctx->rex_b && long_mode) {
                decoded_rm |= 0x08;
            }
            if (!(mod == 0 && (rm & 0x07) == 5 && !(ctx && ctx->rex_b && long_mode)) &&
                cpu_memory_base_register_uses_ss(decoded_rm)) {
                return SEG_SS;
            }
        }
    }
    else if (rm == 2 || rm == 3 || ((rm == 6) && mod != 0)) {
        return SEG_SS;
    }

    return SEG_DS;
}

uint64_t get_effective_offset(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t* sib, int32_t* disp, int addr_size, int inst_size = 0) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;
    const bool long_mode = cpu_allows_rex_prefix(ctx);

    if (ctx->rex_b && long_mode) {
        rm |= 0x08;
    }

    uint64_t addr = 0;

    if (addr_size == 32 || addr_size == 64) {
        if (mod == 3) {
            return 0;
        }

        if ((rm & 0x07) == 4) {
            uint8_t sib_byte = *sib;
            uint8_t scale = (sib_byte >> 6) & 0x03;
            uint8_t raw_index = (sib_byte >> 3) & 0x07;
            uint8_t raw_base = sib_byte & 0x07;
            uint8_t index = raw_index;
            uint8_t base = raw_base;

            if (ctx->rex_x && long_mode) {
                index |= 0x08;
            }
            if (ctx->rex_b && long_mode) {
                base |= 0x08;
            }

            bool has_index = !(raw_index == 4 && !(long_mode && ctx->rex_x));
            bool no_base = (mod == 0 && raw_base == 5 && !(long_mode && ctx->rex_b));

            if (no_base) {
                addr = (uint32_t)(*disp);
            }
            else {
                addr = ctx->regs[base];
            }

            if (has_index) {
                addr += ctx->regs[index] << scale;
            }
        }
        else if ((rm & 0x07) == 5 && mod == 0) {
            if (addr_size == 64) {
                addr = ctx->rip + (uint64_t)inst_size + (int64_t)(*disp);
            }
            else {
                addr = (uint32_t)(*disp);
            }
        }
        else {
            addr = ctx->regs[rm];
        }

        if (mod == 1) {
            addr += (int8_t)(*disp & 0xFF);
        }
        else if (mod == 2) {
            addr += *disp;
        }

        if (addr_size == 32) {
            addr &= 0xFFFFFFFF;
        }
    }
    else {
        if (mod == 3) {
            return 0;
        }

        switch (rm) {
        case 0: addr = (ctx->regs[REG_RBX] + ctx->regs[REG_RSI]) & 0xFFFF; break;
        case 1: addr = (ctx->regs[REG_RBX] + ctx->regs[REG_RDI]) & 0xFFFF; break;
        case 2: addr = (ctx->regs[REG_RBP] + ctx->regs[REG_RSI]) & 0xFFFF; break;
        case 3: addr = (ctx->regs[REG_RBP] + ctx->regs[REG_RDI]) & 0xFFFF; break;
        case 4: addr = ctx->regs[REG_RSI] & 0xFFFF; break;
        case 5: addr = ctx->regs[REG_RDI] & 0xFFFF; break;
        case 6:
            if (mod == 0) {
                addr = *disp & 0xFFFF;
            }
            else {
                addr = ctx->regs[REG_RBP] & 0xFFFF;
            }
            break;
        case 7: addr = ctx->regs[REG_RBX] & 0xFFFF; break;
        }

        if (mod == 1) {
            addr = (addr + (int8_t)(*disp & 0xFF)) & 0xFFFF;
        }
        else if (mod == 2) {
            addr = (addr + (*disp & 0xFFFF)) & 0xFFFF;
        }
    }

    return addr;
}

uint64_t get_effective_address(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t* sib, int32_t* disp, int addr_size, int inst_size = 0) {
    const uint8_t mod = (modrm >> 6) & 0x03;
    uint64_t addr = get_effective_offset(ctx, modrm, sib, disp, addr_size, inst_size);
    if (mod == 3) {
        return addr;
    }

    const int default_segment = cpu_default_segment_for_memory_operand(ctx, modrm, sib != NULL, sib ? *sib : 0, addr_size);
    const int segment_index = cpu_effective_segment_override_or_default(ctx, default_segment);
    addr += cpu_segment_base_for_addressing(ctx, segment_index);
    cpu_validate_linear_address(ctx, addr, segment_index);
    return addr;
}

void finalize_rip_relative_address(CPU_CONTEXT* ctx, DecodedInstruction* inst, int inst_size) {
    if (!inst->has_modrm) {
        return;
    }

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 3 || inst->address_size != 64) {
        return;
    }

    uint8_t rm = inst->modrm & 0x07;
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    bool rip_relative = false;
    if ((rm & 0x07) != 4 || !inst->has_sib) {
        rip_relative = ((rm & 0x07) == 5 && mod == 0);
    }

    if (rip_relative) {
        inst->mem_address = get_effective_address(ctx, inst->modrm, &inst->sib, &inst->displacement, inst->address_size, inst_size);
    }
}

uint8_t get_reg8(CPU_CONTEXT* ctx, int reg) {
    if (ctx->rex_present) {
        return ctx->regs[reg] & 0xFF;
    }
    else {
        if (reg < 4) {
            return ctx->regs[reg] & 0xFF;
        }
        else {
            return (ctx->regs[reg - 4] >> 8) & 0xFF;
        }
    }
}

void set_reg8(CPU_CONTEXT* ctx, int reg, uint8_t value) {
    if (ctx->rex_present) {
        ctx->regs[reg] = (ctx->regs[reg] & ~0xFFULL) | value;
    }
    else {
        if (reg < 4) {
            ctx->regs[reg] = (ctx->regs[reg] & ~0xFFULL) | value;
        }
        else {
            ctx->regs[reg - 4] = (ctx->regs[reg - 4] & ~0xFF00ULL) | ((uint64_t)value << 8);
        }
    }
}

uint16_t get_reg16(CPU_CONTEXT* ctx, int reg) {
    return ctx->regs[reg] & 0xFFFF;
}

void set_reg16(CPU_CONTEXT* ctx, int reg, uint16_t value) {
    ctx->regs[reg] = (ctx->regs[reg] & ~0xFFFFULL) | value;
}

uint32_t get_reg32(CPU_CONTEXT* ctx, int reg) {
    return ctx->regs[reg] & 0xFFFFFFFF;
}

void set_reg32(CPU_CONTEXT* ctx, int reg, uint32_t value) {
    ctx->regs[reg] = value;
}

uint64_t get_reg64(CPU_CONTEXT* ctx, int reg) {
    return ctx->regs[reg];
}

void set_reg64(CPU_CONTEXT* ctx, int reg, uint64_t value) {
    ctx->regs[reg] = value;
}

XMMRegister get_xmm128(CPU_CONTEXT* ctx, int reg) {
    return ctx->xmm[reg];
}

void set_xmm128(CPU_CONTEXT* ctx, int reg, XMMRegister value) {
    ctx->xmm[reg] = value;
}

void set_xmm128_parts(CPU_CONTEXT* ctx, int reg, uint64_t low, uint64_t high) {
    ctx->xmm[reg].low = low;
    ctx->xmm[reg].high = high;
}

XMMRegister get_ymm_upper128(CPU_CONTEXT* ctx, int reg) {
    return ctx->ymm_upper[reg];
}

void set_ymm_upper128(CPU_CONTEXT* ctx, int reg, XMMRegister value) {
    ctx->ymm_upper[reg] = value;
}

void set_ymm_upper128_parts(CPU_CONTEXT* ctx, int reg, uint64_t low, uint64_t high) {
    ctx->ymm_upper[reg].low = low;
    ctx->ymm_upper[reg].high = high;
}

ZMMUpperRegister get_zmm_upper256(CPU_CONTEXT* ctx, int reg);
void set_zmm_upper256(CPU_CONTEXT* ctx, int reg, ZMMUpperRegister value);
void clear_zmm_upper256(CPU_CONTEXT* ctx, int reg);
void clear_all_zmm_upper256(CPU_CONTEXT* ctx);

void clear_ymm_upper128(CPU_CONTEXT* ctx, int reg) {
    ctx->ymm_upper[reg] = {};
    clear_zmm_upper256(ctx, reg);
}

void clear_all_ymm_upper128(CPU_CONTEXT* ctx) {
    CPUEAXH_MEMSET(ctx->ymm_upper, 0, sizeof(ctx->ymm_upper));
    clear_all_zmm_upper256(ctx);
}

ZMMUpperRegister get_zmm_upper256(CPU_CONTEXT* ctx, int reg) {
    return ctx->zmm_upper[reg];
}

void set_zmm_upper256(CPU_CONTEXT* ctx, int reg, ZMMUpperRegister value) {
    ctx->zmm_upper[reg] = value;
}

void clear_zmm_upper256(CPU_CONTEXT* ctx, int reg) {
    ctx->zmm_upper[reg] = {};
}

void clear_all_zmm_upper256(CPU_CONTEXT* ctx) {
    CPUEAXH_MEMSET(ctx->zmm_upper, 0, sizeof(ctx->zmm_upper));
}

ZMMRegister get_zmm512(CPU_CONTEXT* ctx, int reg) {
    ZMMRegister value = {};
    value.xmm0 = ctx->xmm[reg];
    value.xmm1 = ctx->ymm_upper[reg];
    value.xmm2 = ctx->zmm_upper[reg].lower;
    value.xmm3 = ctx->zmm_upper[reg].upper;
    return value;
}

void set_zmm512(CPU_CONTEXT* ctx, int reg, ZMMRegister value) {
    ctx->xmm[reg] = value.xmm0;
    ctx->ymm_upper[reg] = value.xmm1;
    ctx->zmm_upper[reg].lower = value.xmm2;
    ctx->zmm_upper[reg].upper = value.xmm3;
}

void clear_zmm_above_128(CPU_CONTEXT* ctx, int reg) {
    clear_ymm_upper128(ctx, reg);
    clear_zmm_upper256(ctx, reg);
}

void clear_zmm_above_256(CPU_CONTEXT* ctx, int reg) {
    clear_zmm_upper256(ctx, reg);
}

uint64_t get_opmask64(CPU_CONTEXT* ctx, int reg) {
    return ctx->opmask[reg & 0x07];
}

void set_opmask64(CPU_CONTEXT* ctx, int reg, uint64_t value) {
    ctx->opmask[reg & 0x07] = value;
}

uint64_t get_mm64(CPU_CONTEXT* ctx, int reg) {
    return ctx->mm[reg & 0x07];
}

void set_mm64(CPU_CONTEXT* ctx, int reg, uint64_t value) {
    ctx->mm[reg & 0x07] = value;
}

XMMRegister read_xmm_memory(CPU_CONTEXT* ctx, uint64_t address) {
    XMMRegister value = {};
    value.low = read_memory_qword(ctx, address);
    value.high = read_memory_qword(ctx, address + 8);
    return value;
}

void write_xmm_memory(CPU_CONTEXT* ctx, uint64_t address, XMMRegister value) {
    write_memory_qword(ctx, address, value.low);
    write_memory_qword(ctx, address + 8, value.high);
}

ZMMUpperRegister read_zmm_upper_memory(CPU_CONTEXT* ctx, uint64_t address) {
    ZMMUpperRegister value = {};
    value.lower = read_xmm_memory(ctx, address);
    value.upper = read_xmm_memory(ctx, address + 16);
    return value;
}

void write_zmm_upper_memory(CPU_CONTEXT* ctx, uint64_t address, ZMMUpperRegister value) {
    write_xmm_memory(ctx, address, value.lower);
    write_xmm_memory(ctx, address + 16, value.upper);
}

ZMMRegister read_zmm_memory(CPU_CONTEXT* ctx, uint64_t address) {
    ZMMRegister value = {};
    value.xmm0 = read_xmm_memory(ctx, address);
    value.xmm1 = read_xmm_memory(ctx, address + 16);
    value.xmm2 = read_xmm_memory(ctx, address + 32);
    value.xmm3 = read_xmm_memory(ctx, address + 48);
    return value;
}

void write_zmm_memory(CPU_CONTEXT* ctx, uint64_t address, ZMMRegister value) {
    write_xmm_memory(ctx, address, value.xmm0);
    write_xmm_memory(ctx, address + 16, value.xmm1);
    write_xmm_memory(ctx, address + 32, value.xmm2);
    write_xmm_memory(ctx, address + 48, value.xmm3);
}

// --- Stack operation helpers ---

// Determine stack address size: in 64-bit mode always 64; otherwise based on SS.B flag
int get_stack_addr_size(CPU_CONTEXT* ctx) {
    return cpu_stack_address_size(ctx);
}

// Push a 16-bit value onto the stack
void push_value16(CPU_CONTEXT* ctx, uint16_t value) {
    int stack_addr_size = get_stack_addr_size(ctx);
    if (stack_addr_size == 64) {
        ctx->regs[REG_RSP] -= 2;
        write_memory_word(ctx, ctx->regs[REG_RSP], value);
    }
    else if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        esp -= 2;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
        write_memory_word(ctx, esp, value);
    }
    else {
        uint16_t sp = (uint16_t)(ctx->regs[REG_RSP] & 0xFFFF);
        sp -= 2;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | sp;
        write_memory_word(ctx, sp, value);
    }
}

// Push a 32-bit value onto the stack
void push_value32(CPU_CONTEXT* ctx, uint32_t value) {
    int stack_addr_size = get_stack_addr_size(ctx);
    if (stack_addr_size == 64) {
        ctx->regs[REG_RSP] -= 4;
        write_memory_dword(ctx, ctx->regs[REG_RSP], value);
    }
    else if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        esp -= 4;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
        write_memory_dword(ctx, esp, value);
    }
    else {
        uint16_t sp = (uint16_t)(ctx->regs[REG_RSP] & 0xFFFF);
        sp -= 4;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | sp;
        write_memory_dword(ctx, sp, value);
    }
}

// Push a 64-bit value onto the stack
void push_value64(CPU_CONTEXT* ctx, uint64_t value) {
    int stack_addr_size = get_stack_addr_size(ctx);
    if (stack_addr_size == 64) {
        ctx->regs[REG_RSP] -= 8;
        write_memory_qword(ctx, ctx->regs[REG_RSP], value);
    }
    else if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        esp -= 8;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
        write_memory_qword(ctx, esp, value);
    }
    else {
        // 16-bit stack address doesn't support 64-bit pushes
        raise_gp_ctx(ctx, 0);
    }
}

// Pop a 16-bit value from the stack
uint16_t pop_value16(CPU_CONTEXT* ctx) {
    int stack_addr_size = get_stack_addr_size(ctx);
    uint16_t value;
    if (stack_addr_size == 64) {
        value = read_memory_word(ctx, ctx->regs[REG_RSP]);
        ctx->regs[REG_RSP] += 2;
    }
    else if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        value = read_memory_word(ctx, esp);
        esp += 2;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
    }
    else {
        uint16_t sp = (uint16_t)(ctx->regs[REG_RSP] & 0xFFFF);
        value = read_memory_word(ctx, sp);
        sp += 2;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | sp;
    }
    return value;
}

// Pop a 32-bit value from the stack
uint32_t pop_value32(CPU_CONTEXT* ctx) {
    int stack_addr_size = get_stack_addr_size(ctx);
    uint32_t value;
    if (stack_addr_size == 64) {
        value = read_memory_dword(ctx, ctx->regs[REG_RSP]);
        ctx->regs[REG_RSP] += 4;
    }
    else if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        value = read_memory_dword(ctx, esp);
        esp += 4;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
    }
    else {
        uint16_t sp = (uint16_t)(ctx->regs[REG_RSP] & 0xFFFF);
        value = read_memory_dword(ctx, sp);
        sp += 4;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | sp;
    }
    return value;
}

// Pop a 64-bit value from the stack
uint64_t pop_value64(CPU_CONTEXT* ctx) {
    int stack_addr_size = get_stack_addr_size(ctx);
    uint64_t value;
    if (stack_addr_size == 64) {
        value = read_memory_qword(ctx, ctx->regs[REG_RSP]);
        ctx->regs[REG_RSP] += 8;
    }
    else if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        value = read_memory_qword(ctx, esp);
        esp += 8;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
    }
    else {
        // 16-bit stack address doesn't support 64-bit pops
        raise_gp_ctx(ctx, 0);
        value = 0; // unreachable, suppress warning
    }
    return value;
}
