// instrusments/mov.hpp - MOV instruction implementation

// --- MOV execution helpers ---

static int decode_mov_segment_override(uint8_t prefix) {
    switch (prefix) {
    case 0x26: return SEG_ES;
    case 0x2E: return SEG_CS;
    case 0x36: return SEG_SS;
    case 0x3E: return SEG_DS;
    case 0x64: return SEG_FS;
    case 0x65: return SEG_GS;
    default:   return -1;
    }
}

static uint64_t mov_segment_base(CPU_CONTEXT* ctx, int segment_index) {
    return cpu_segment_base_for_addressing(ctx, segment_index);
}

static bool mov_decode_control_register_operands(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t* control_register, uint8_t* general_register) {
    if (!ctx || !control_register || !general_register) {
        return false;
    }

    uint8_t decoded_control_register = (uint8_t)(((modrm >> 3) & 0x07) | (ctx->rex_r ? 0x08 : 0x00));
    uint8_t decoded_general_register = (uint8_t)((modrm & 0x07) | (ctx->rex_b ? 0x08 : 0x00));

    if (ctx->rex_r && decoded_control_register != REG_CR8) {
        raise_ud_ctx(ctx);
        return false;
    }

    if (!is_valid_control_register(decoded_control_register)) {
        raise_ud_ctx(ctx);
        return false;
    }

    if (ctx->cpl != 0) {
        raise_gp_ctx(ctx, 0);
        return false;
    }

    *control_register = decoded_control_register;
    *general_register = decoded_general_register;
    return true;
}

static bool mov_validate_control_register_write(CPU_CONTEXT* ctx, uint8_t control_register, uint64_t value) {
    if (!ctx) {
        return false;
    }

    switch (control_register) {
    case REG_CR0:
        if ((value & (1ull << 31)) == 0 || ((value & (1ull << 0)) == 0 && (value & (1ull << 31)) != 0) ||
            ((value & (1ull << 29)) != 0 && (value & (1ull << 30)) == 0)) {
            raise_gp_ctx(ctx, 0);
            return false;
        }
        break;
    case REG_CR4:
        if ((value & (1ull << 5)) == 0) {
            raise_gp_ctx(ctx, 0);
            return false;
        }
        break;
    case REG_CR8:
        if ((value & ~0xFull) != 0) {
            raise_gp_ctx(ctx, 0);
            return false;
        }
        break;
    default:
        break;
    }

    return true;
}

void mov_r64_cr(CPU_CONTEXT* ctx, uint8_t modrm) {
    uint8_t control_register = 0;
    uint8_t general_register = 0;
    if (!mov_decode_control_register_operands(ctx, modrm, &control_register, &general_register)) {
        return;
    }

    set_reg64(ctx, general_register, get_control_register(ctx, control_register));
}

void mov_cr_r64(CPU_CONTEXT* ctx, uint8_t modrm) {
    uint8_t control_register = 0;
    uint8_t general_register = 0;
    if (!mov_decode_control_register_operands(ctx, modrm, &control_register, &general_register)) {
        return;
    }

    uint64_t value = get_reg64(ctx, general_register);
    if (!mov_validate_control_register_write(ctx, control_register, value)) {
        return;
    }

    set_control_register(ctx, control_register, value);
}

void mov_r8_rm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint8_t value;
    if (mod == 3) {
        value = get_reg8(ctx, rm);
    }
    else {
        value = read_memory_byte(ctx, mem_addr);
    }

    set_reg8(ctx, reg, value);
}

void mov_rm8_r8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint8_t value = get_reg8(ctx, reg);

    if (mod == 3) {
        set_reg8(ctx, rm, value);
    }
    else {
        write_memory_byte(ctx, mem_addr, value);
    }
}

void mov_r16_rm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint16_t value;
    if (mod == 3) {
        value = get_reg16(ctx, rm);
    }
    else {
        value = read_memory_word(ctx, mem_addr);
    }

    set_reg16(ctx, reg, value);
}

void mov_rm16_r16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint16_t value = get_reg16(ctx, reg);

    if (mod == 3) {
        set_reg16(ctx, rm, value);
    }
    else {
        write_memory_word(ctx, mem_addr, value);
    }
}

void mov_r32_rm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t value;
    if (mod == 3) {
        value = get_reg32(ctx, rm);
    }
    else {
        value = read_memory_dword(ctx, mem_addr);
    }

    set_reg32(ctx, reg, value);
}

void mov_rm32_r32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t value = get_reg32(ctx, reg);

    if (mod == 3) {
        set_reg32(ctx, rm, value);
    }
    else {
        write_memory_dword(ctx, mem_addr, value);
    }
}

void mov_r64_rm64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t value;
    if (mod == 3) {
        value = get_reg64(ctx, rm);
    }
    else {
        value = read_memory_qword(ctx, mem_addr);
    }

    set_reg64(ctx, reg, value);
}

void mov_rm64_r64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t value = get_reg64(ctx, reg);

    if (mod == 3) {
        set_reg64(ctx, rm, value);
    }
    else {
        write_memory_qword(ctx, mem_addr, value);
    }
}

void mov_rm16_sreg(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    SegmentRegister* sreg = get_segment_register(ctx, reg);
    if (!sreg) {
        raise_gp_ctx(ctx, 0);
    }

    uint16_t value = sreg->selector;

    if (mod == 3) {
        set_reg16(ctx, rm, value);
    }
    else {
        write_memory_word(ctx, mem_addr, value);
    }
}

void mov_r32_sreg(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    SegmentRegister* sreg = get_segment_register(ctx, reg);
    if (!sreg) {
        raise_gp_ctx(ctx, 0);
    }

    uint32_t value = sreg->selector;

    if (mod == 3) {
        set_reg32(ctx, rm, value);
    }
    else {
        write_memory_word(ctx, mem_addr, (uint16_t)value);
    }
}

void mov_r64_sreg(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    SegmentRegister* sreg = get_segment_register(ctx, reg);
    if (!sreg) {
        raise_gp_ctx(ctx, 0);
    }

    uint64_t value = sreg->selector;

    if (mod == 3) {
        set_reg64(ctx, rm, value);
    }
    else {
        write_memory_word(ctx, mem_addr, (uint16_t)value);
    }
}

void mov_sreg_rm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    if (reg == SEG_CS) {
        raise_ud_ctx(ctx);
    }

    uint16_t selector;
    if (mod == 3) {
        selector = get_reg16(ctx, rm);
    }
    else {
        selector = read_memory_word(ctx, mem_addr);
    }

    load_segment_register(ctx, reg, selector);
}

void mov_sreg_rm64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    if (reg == SEG_CS) {
        raise_ud_ctx(ctx);
    }

    uint16_t selector;
    if (mod == 3) {
        selector = get_reg64(ctx, rm) & 0xFFFF;
    }
    else {
        selector = read_memory_word(ctx, mem_addr);
    }

    load_segment_register(ctx, reg, selector);
}

void mov_al_moffs8(CPU_CONTEXT* ctx, uint64_t offset) {
    uint8_t value = read_memory_byte(ctx, offset);
    ctx->regs[REG_RAX] = (ctx->regs[REG_RAX] & ~0xFFULL) | value;
}

void mov_ax_moffs16(CPU_CONTEXT* ctx, uint64_t offset) {
    uint16_t value = read_memory_word(ctx, offset);
    ctx->regs[REG_RAX] = (ctx->regs[REG_RAX] & ~0xFFFFULL) | value;
}

void mov_eax_moffs32(CPU_CONTEXT* ctx, uint64_t offset) {
    uint32_t value = read_memory_dword(ctx, offset);
    ctx->regs[REG_RAX] = value;
}

void mov_rax_moffs64(CPU_CONTEXT* ctx, uint64_t offset) {
    uint64_t value = read_memory_qword(ctx, offset);
    ctx->regs[REG_RAX] = value;
}

void mov_moffs8_al(CPU_CONTEXT* ctx, uint64_t offset) {
    uint8_t value = ctx->regs[REG_RAX] & 0xFF;
    write_memory_byte(ctx, offset, value);
}

void mov_moffs16_ax(CPU_CONTEXT* ctx, uint64_t offset) {
    uint16_t value = ctx->regs[REG_RAX] & 0xFFFF;
    write_memory_word(ctx, offset, value);
}

void mov_moffs32_eax(CPU_CONTEXT* ctx, uint64_t offset) {
    uint32_t value = ctx->regs[REG_RAX] & 0xFFFFFFFF;
    write_memory_dword(ctx, offset, value);
}

void mov_moffs64_rax(CPU_CONTEXT* ctx, uint64_t offset) {
    uint64_t value = ctx->regs[REG_RAX];
    write_memory_qword(ctx, offset, value);
}

void mov_r8_imm8(CPU_CONTEXT* ctx, int reg, uint8_t imm) {
    set_reg8(ctx, reg, imm);
}

void mov_r16_imm16(CPU_CONTEXT* ctx, int reg, uint16_t imm) {
    set_reg16(ctx, reg, imm);
}

void mov_r32_imm32(CPU_CONTEXT* ctx, int reg, uint32_t imm) {
    set_reg32(ctx, reg, imm);
}

void mov_r64_imm64(CPU_CONTEXT* ctx, int reg, uint64_t imm) {
    set_reg64(ctx, reg, imm);
}

void mov_rm8_imm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, uint8_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    if (mod == 3) {
        set_reg8(ctx, rm, imm);
    }
    else {
        write_memory_byte(ctx, mem_addr, imm);
    }
}

void mov_rm16_imm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, uint16_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    if (mod == 3) {
        set_reg16(ctx, rm, imm);
    }
    else {
        write_memory_word(ctx, mem_addr, imm);
    }
}

void mov_rm32_imm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, uint32_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    if (mod == 3) {
        set_reg32(ctx, rm, imm);
    }
    else {
        write_memory_dword(ctx, mem_addr, imm);
    }
}

void mov_rm64_imm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, int32_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    int64_t sign_extended = (int64_t)imm;

    if (mod == 3) {
        set_reg64(ctx, rm, (uint64_t)sign_extended);
    }
    else {
        write_memory_qword(ctx, mem_addr, (uint64_t)sign_extended);
    }
}

// --- Helper: decode ModR/M for MOV ---

void decode_modrm_mov(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
    if (*offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }
    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    uint8_t rm = inst->modrm & 0x07;

    // SIB byte present when rm==4 and mod!=3 (not in 16-bit addressing)
    if (mod != 3 && rm == 4 && inst->address_size != 16) {
        if (*offset >= code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst->has_sib = true;
        inst->sib = code[(*offset)++];
    }

    // Determine displacement size
    if (mod == 0 && rm == 5) {
        inst->disp_size = (inst->address_size == 16) ? 2 : 4;
    }
    else if (mod == 0 && inst->has_sib && (inst->sib & 0x07) == 5) {
        inst->disp_size = 4;
    }
    else if (mod == 1) {
        inst->disp_size = 1;
    }
    else if (mod == 2) {
        inst->disp_size = (inst->address_size == 16) ? 2 : 4;
    }

    if (inst->disp_size > 0) {
        if (*offset + inst->disp_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst->displacement = 0;
        for (int i = 0; i < inst->disp_size; i++) {
            inst->displacement |= ((int32_t)code[(*offset)++]) << (i * 8);
        }
        if (inst->disp_size == 1) {
            inst->displacement = (int8_t)inst->displacement;
        }
        else if (inst->disp_size == 2) {
            inst->displacement = (int16_t)inst->displacement;
        }
    }

    if (mod != 3) {
        inst->mem_address = get_effective_address(ctx, inst->modrm, &inst->sib, &inst->displacement, inst->address_size);
    }
}

void decode_modrm_mov_cr(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
    if (*offset >= code_size) {
        raise_gp_ctx(ctx, 0);
        return;
    }

    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];
}

// --- MOV instruction decoder ---

DecodedInstruction decode_mov_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
    int segment_index = SEG_DS;
    bool has_segment_override = false;

    ctx->rex_present = false;
    ctx->rex_w = false;
    ctx->rex_r = false;
    ctx->rex_x = false;
    ctx->rex_b = false;
    ctx->operand_size_override = false;
    ctx->address_size_override = false;

    // Decode prefixes
    while (offset < code_size) {
        uint8_t prefix = code[offset];
        if (prefix == 0x66) {
            ctx->operand_size_override = true;
            offset++;
        }
        else if (prefix == 0x67) {
            ctx->address_size_override = true;
            offset++;
        }
        else if (prefix >= 0x40 && prefix <= 0x4F) {
            ctx->rex_present = true;
            ctx->rex_w = (prefix >> 3) & 1;
            ctx->rex_r = (prefix >> 2) & 1;
            ctx->rex_x = (prefix >> 1) & 1;
            ctx->rex_b = prefix & 1;
            offset++;
        }
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
            prefix == 0x64 || prefix == 0x65 || prefix == 0xF0 || prefix == 0xF2 || prefix == 0xF3) {
            // LOCK prefix is #UD for MOV
            if (prefix == 0xF0) {
                raise_ud_ctx(ctx);
            }

            int seg_override = decode_mov_segment_override(prefix);
            if (seg_override >= 0) {
                segment_index = seg_override;
                has_segment_override = true;
            }
            offset++;
        }
        else {
            break;
        }
    }

    if (offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }

    inst.opcode = code[offset++];
    if (inst.opcode == 0x0F) {
        if (offset >= code_size) {
            raise_gp_ctx(ctx, 0);
            ctx->last_inst_size = (int)offset;
            return inst;
        }
        inst.opcode = code[offset++];
    }

    // Determine operand size: in 64-bit mode, default is 32; REX.W promotes to 64; 0x66 demotes to 16
    inst.operand_size = 32;
    if (ctx->rex_w) {
        inst.operand_size = 64;
    }
    else if (ctx->operand_size_override) {
        inst.operand_size = 16;
    }

    // Determine address size: in 64-bit mode (CS.L=1), default is 64; 0x67 override to 32
    // In 32-bit mode (CS.L=0), default is 32; 0x67 override to 16
    if (ctx->cs.descriptor.long_mode) {
        inst.address_size = ctx->address_size_override ? 32 : 64;
    }
    else {
        inst.address_size = ctx->address_size_override ? 16 : 32;
    }

    switch (inst.opcode) {
    // 88 /r - MOV r/m8, r8
    case 0x88:
        inst.operand_size = 8;
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        break;

    // 89 /r - MOV r/m16, r16 / MOV r/m32, r32 / REX.W + MOV r/m64, r64
    case 0x89:
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        break;

    // 8A /r - MOV r8, r/m8
    case 0x8A:
        inst.operand_size = 8;
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        break;

    // 8B /r - MOV r16, r/m16 / MOV r32, r/m32 / REX.W + MOV r64, r/m64
    case 0x8B:
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        break;

    // 8C /r - MOV r/m16, Sreg / MOV r16/r32/m16, Sreg / REX.W + MOV r64/m16, Sreg
    case 0x8C:
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        break;

    // 8E /r - MOV Sreg, r/m16 / REX.W + MOV Sreg, r/m64
    case 0x8E:
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        break;

    // 0F 20 /r - MOV r64, CRn
    case 0x20:
        decode_modrm_mov_cr(ctx, &inst, code, code_size, &offset);
        break;

    // 0F 22 /r - MOV CRn, r64
    case 0x22:
        decode_modrm_mov_cr(ctx, &inst, code, code_size, &offset);
        break;

    // A0 - MOV AL, moffs8
    case 0xA0:
        inst.operand_size = 8;
        inst.imm_size = inst.address_size / 8;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        inst.mem_address = inst.immediate;
        break;

    // A1 - MOV AX, moffs16 / MOV EAX, moffs32 / REX.W + MOV RAX, moffs64
    case 0xA1:
        inst.imm_size = inst.address_size / 8;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        inst.mem_address = inst.immediate;
        break;

    // A2 - MOV moffs8, AL
    case 0xA2:
        inst.operand_size = 8;
        inst.imm_size = inst.address_size / 8;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        inst.mem_address = inst.immediate;
        break;

    // A3 - MOV moffs16, AX / MOV moffs32, EAX / REX.W + MOV moffs64, RAX
    case 0xA3:
        inst.imm_size = inst.address_size / 8;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        inst.mem_address = inst.immediate;
        break;

    // B0+rb ib - MOV r8, imm8
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        inst.operand_size = 8;
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // B8+rw iw / B8+rd id / REX.W + B8+rd io - MOV r16/r32/r64, imm16/imm32/imm64
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if (ctx->rex_w) {
            inst.imm_size = 8;
        }
        else if (ctx->operand_size_override) {
            inst.imm_size = 2;
        }
        else {
            inst.imm_size = 4;
        }
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        break;

    // C6 /0 ib - MOV r/m8, imm8
    case 0xC6:
        inst.operand_size = 8;
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        // The manual specifies C6 /0: the reg field of ModRM must be 0
        if (((inst.modrm >> 3) & 0x07) != 0) {
            raise_ud_ctx(ctx);
        }
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // C7 /0 iw/id - MOV r/m16, imm16 / MOV r/m32, imm32 / REX.W + MOV r/m64, imm32
    case 0xC7:
        decode_modrm_mov(ctx, &inst, code, code_size, &offset);
        // The manual specifies C7 /0: the reg field of ModRM must be 0
        if (((inst.modrm >> 3) & 0x07) != 0) {
            raise_ud_ctx(ctx);
        }
        if (ctx->rex_w) {
            inst.imm_size = 4;
        }
        else if (ctx->operand_size_override) {
            inst.imm_size = 2;
        }
        else {
            inst.imm_size = 4;
        }
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        break;

    default:
        raise_ud_ctx(ctx);
    }

    finalize_rip_relative_address(ctx, &inst, (int)offset);

    if (inst.opcode >= 0xA0 && inst.opcode <= 0xA3) {
        inst.mem_address += mov_segment_base(ctx, segment_index);
        cpu_validate_linear_address(ctx, inst.mem_address, segment_index);
    }
    else if (inst.has_modrm && has_segment_override) {
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        if (mod != 3) {
            const int default_segment = cpu_default_segment_for_memory_operand(ctx, inst.modrm, inst.has_sib, inst.sib, inst.address_size);
            inst.mem_address += mov_segment_base(ctx, segment_index) - mov_segment_base(ctx, default_segment);
            cpu_validate_linear_address(ctx, inst.mem_address, segment_index);
        }
    }

    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- MOV instruction executor ---

void execute_mov(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_mov_instruction(ctx, code, code_size);

    switch (inst.opcode) {
    // 88 /r - MOV r/m8, r8
    case 0x88:
        mov_rm8_r8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        break;

    // 89 /r - MOV r/m16, r16 / MOV r/m32, r32 / REX.W + MOV r/m64, r64
    case 0x89:
        if (ctx->rex_w) {
            mov_rm64_r64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            mov_rm16_r16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            mov_rm32_r32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 8A /r - MOV r8, r/m8
    case 0x8A:
        mov_r8_rm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        break;

    // 8B /r - MOV r16, r/m16 / MOV r32, r/m32 / REX.W + MOV r64, r/m64
    case 0x8B:
        if (ctx->rex_w) {
            mov_r64_rm64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            mov_r16_rm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            mov_r32_rm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 8C /r - MOV r/m16, Sreg / MOV r16/r32/m16, Sreg / REX.W + MOV r64/m16, Sreg
    case 0x8C:
        if (ctx->rex_w) {
            mov_r64_sreg(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            mov_rm16_sreg(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            mov_r32_sreg(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 8E /r - MOV Sreg, r/m16 / REX.W + MOV Sreg, r/m64
    case 0x8E:
        if (ctx->rex_w) {
            mov_sreg_rm64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            mov_sreg_rm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 0F 20 /r - MOV r64, CRn
    case 0x20:
        mov_r64_cr(ctx, inst.modrm);
        break;

    // 0F 22 /r - MOV CRn, r64
    case 0x22:
        mov_cr_r64(ctx, inst.modrm);
        break;

    // A0 - MOV AL, moffs8
    case 0xA0:
        mov_al_moffs8(ctx, inst.mem_address);
        break;

    // A1 - MOV AX, moffs16 / MOV EAX, moffs32 / REX.W + MOV RAX, moffs64
    case 0xA1:
        if (ctx->rex_w) {
            mov_rax_moffs64(ctx, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            mov_ax_moffs16(ctx, inst.mem_address);
        }
        else {
            mov_eax_moffs32(ctx, inst.mem_address);
        }
        break;

    // A2 - MOV moffs8, AL
    case 0xA2:
        mov_moffs8_al(ctx, inst.mem_address);
        break;

    // A3 - MOV moffs16, AX / MOV moffs32, EAX / REX.W + MOV moffs64, RAX
    case 0xA3:
        if (ctx->rex_w) {
            mov_moffs64_rax(ctx, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            mov_moffs16_ax(ctx, inst.mem_address);
        }
        else {
            mov_moffs32_eax(ctx, inst.mem_address);
        }
        break;

    // B0+rb ib - MOV r8, imm8
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        int reg = inst.opcode - 0xB0;
        if (ctx->rex_b) {
            reg |= 0x08;
        }
        mov_r8_imm8(ctx, reg, (uint8_t)inst.immediate);
        break;
    }

    // B8+rw/rd/io - MOV r16, imm16 / MOV r32, imm32 / REX.W + MOV r64, imm64
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int reg = inst.opcode - 0xB8;
        if (ctx->rex_b) {
            reg |= 0x08;
        }
        if (ctx->rex_w) {
            mov_r64_imm64(ctx, reg, inst.immediate);
        }
        else if (ctx->operand_size_override) {
            mov_r16_imm16(ctx, reg, (uint16_t)inst.immediate);
        }
        else {
            mov_r32_imm32(ctx, reg, (uint32_t)inst.immediate);
        }
        break;
    }

    // C6 /0 ib - MOV r/m8, imm8
    case 0xC6:
        mov_rm8_imm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (uint8_t)inst.immediate);
        break;

    // C7 /0 iw/id - MOV r/m16, imm16 / MOV r/m32, imm32 / REX.W + MOV r/m64, imm32 (sign-extended)
    case 0xC7:
        if (ctx->rex_w) {
            mov_rm64_imm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (int32_t)inst.immediate);
        }
        else if (ctx->operand_size_override) {
            mov_rm16_imm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (uint16_t)inst.immediate);
        }
        else {
            mov_rm32_imm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (uint32_t)inst.immediate);
        }
        break;
    }
}
