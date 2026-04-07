// instrusments/cmp.hpp - CMP instruction implementation
// CMP performs SRC1 - SRC2, sets flags identically to SUB, discards the result.

static int decode_cmp_segment_override(uint8_t prefix) {
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

static uint64_t cmp_segment_base(CPU_CONTEXT* ctx, int segment_index) {
    return cpu_segment_base_for_addressing(ctx, segment_index);
}

// --- CMP execution helpers ---

// 3C ib - CMP AL, imm8
void cmp_al_imm8(CPU_CONTEXT* ctx, uint8_t imm) {
    uint8_t dst = (uint8_t)(ctx->regs[REG_RAX] & 0xFF);
    uint16_t result = (uint16_t)dst - (uint16_t)imm;
    update_flags_sub8(ctx, dst, imm, result);
}

// 3D iw - CMP AX, imm16
void cmp_ax_imm16(CPU_CONTEXT* ctx, uint16_t imm) {
    uint16_t dst = (uint16_t)(ctx->regs[REG_RAX] & 0xFFFF);
    uint32_t result = (uint32_t)dst - (uint32_t)imm;
    update_flags_sub16(ctx, dst, imm, result);
}

// 3D id - CMP EAX, imm32
void cmp_eax_imm32(CPU_CONTEXT* ctx, uint32_t imm) {
    uint32_t dst = (uint32_t)(ctx->regs[REG_RAX] & 0xFFFFFFFF);
    uint64_t result = (uint64_t)dst - (uint64_t)imm;
    update_flags_sub32(ctx, dst, imm, result);
}

// REX.W + 3D id - CMP RAX, imm32 (sign-extended to 64 bits)
void cmp_rax_imm32(CPU_CONTEXT* ctx, int32_t imm) {
    uint64_t dst = ctx->regs[REG_RAX];
    uint64_t src = (uint64_t)(int64_t)imm;
    uint64_t result = dst - src;
    update_flags_sub64(ctx, dst, src, result);
}

// 80 /7 ib - CMP r/m8, imm8
void cmp_rm8_imm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, uint8_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint8_t dst;
    if (mod == 3) {
        dst = get_reg8(ctx, rm);
    }
    else {
        dst = read_memory_byte(ctx, mem_addr);
    }

    uint16_t result = (uint16_t)dst - (uint16_t)imm;
    update_flags_sub8(ctx, dst, imm, result);
}

// 81 /7 iw - CMP r/m16, imm16
void cmp_rm16_imm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, uint16_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint16_t dst;
    if (mod == 3) {
        dst = get_reg16(ctx, rm);
    }
    else {
        dst = read_memory_word(ctx, mem_addr);
    }

    uint32_t result = (uint32_t)dst - (uint32_t)imm;
    update_flags_sub16(ctx, dst, imm, result);
}

// 81 /7 id - CMP r/m32, imm32
void cmp_rm32_imm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, uint32_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t dst;
    if (mod == 3) {
        dst = get_reg32(ctx, rm);
    }
    else {
        dst = read_memory_dword(ctx, mem_addr);
    }

    uint64_t result = (uint64_t)dst - (uint64_t)imm;
    update_flags_sub32(ctx, dst, imm, result);
}

// REX.W + 81 /7 id - CMP r/m64, imm32 (sign-extended to 64 bits)
void cmp_rm64_imm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, int32_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t dst;
    if (mod == 3) {
        dst = get_reg64(ctx, rm);
    }
    else {
        dst = read_memory_qword(ctx, mem_addr);
    }

    uint64_t src = (uint64_t)(int64_t)imm;
    uint64_t result = dst - src;
    update_flags_sub64(ctx, dst, src, result);
}

// 83 /7 ib - CMP r/m16, imm8 (sign-extended)
void cmp_rm16_imm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, int8_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint16_t dst;
    if (mod == 3) {
        dst = get_reg16(ctx, rm);
    }
    else {
        dst = read_memory_word(ctx, mem_addr);
    }

    uint16_t src = (uint16_t)(int16_t)imm;
    uint32_t result = (uint32_t)dst - (uint32_t)src;
    update_flags_sub16(ctx, dst, src, result);
}

// 83 /7 ib - CMP r/m32, imm8 (sign-extended)
void cmp_rm32_imm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, int8_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t dst;
    if (mod == 3) {
        dst = get_reg32(ctx, rm);
    }
    else {
        dst = read_memory_dword(ctx, mem_addr);
    }

    uint32_t src = (uint32_t)(int32_t)imm;
    uint64_t result = (uint64_t)dst - (uint64_t)src;
    update_flags_sub32(ctx, dst, src, result);
}

// REX.W + 83 /7 ib - CMP r/m64, imm8 (sign-extended)
void cmp_rm64_imm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, int8_t imm) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t dst;
    if (mod == 3) {
        dst = get_reg64(ctx, rm);
    }
    else {
        dst = read_memory_qword(ctx, mem_addr);
    }

    uint64_t src = (uint64_t)(int64_t)imm;
    uint64_t result = dst - src;
    update_flags_sub64(ctx, dst, src, result);
}

// 38 /r - CMP r/m8, r8
void cmp_rm8_r8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint8_t src = get_reg8(ctx, reg);
    uint8_t dst;
    if (mod == 3) {
        dst = get_reg8(ctx, rm);
    }
    else {
        dst = read_memory_byte(ctx, mem_addr);
    }

    uint16_t result = (uint16_t)dst - (uint16_t)src;
    update_flags_sub8(ctx, dst, src, result);
}

// 39 /r - CMP r/m16, r16
void cmp_rm16_r16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint16_t src = get_reg16(ctx, reg);
    uint16_t dst;
    if (mod == 3) {
        dst = get_reg16(ctx, rm);
    }
    else {
        dst = read_memory_word(ctx, mem_addr);
    }

    uint32_t result = (uint32_t)dst - (uint32_t)src;
    update_flags_sub16(ctx, dst, src, result);
}

// 39 /r - CMP r/m32, r32
void cmp_rm32_r32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t src = get_reg32(ctx, reg);
    uint32_t dst;
    if (mod == 3) {
        dst = get_reg32(ctx, rm);
    }
    else {
        dst = read_memory_dword(ctx, mem_addr);
    }

    uint64_t result = (uint64_t)dst - (uint64_t)src;
    update_flags_sub32(ctx, dst, src, result);
}

// REX.W + 39 /r - CMP r/m64, r64
void cmp_rm64_r64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t src = get_reg64(ctx, reg);
    uint64_t dst;
    if (mod == 3) {
        dst = get_reg64(ctx, rm);
    }
    else {
        dst = read_memory_qword(ctx, mem_addr);
    }

    uint64_t result = dst - src;
    update_flags_sub64(ctx, dst, src, result);
}

// 3A /r - CMP r8, r/m8
void cmp_r8_rm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint8_t dst = get_reg8(ctx, reg);
    uint8_t src;
    if (mod == 3) {
        src = get_reg8(ctx, rm);
    }
    else {
        src = read_memory_byte(ctx, mem_addr);
    }

    uint16_t result = (uint16_t)dst - (uint16_t)src;
    update_flags_sub8(ctx, dst, src, result);
}

// 3B /r - CMP r16, r/m16
void cmp_r16_rm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint16_t dst = get_reg16(ctx, reg);
    uint16_t src;
    if (mod == 3) {
        src = get_reg16(ctx, rm);
    }
    else {
        src = read_memory_word(ctx, mem_addr);
    }

    uint32_t result = (uint32_t)dst - (uint32_t)src;
    update_flags_sub16(ctx, dst, src, result);
}

// 3B /r - CMP r32, r/m32
void cmp_r32_rm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t dst = get_reg32(ctx, reg);
    uint32_t src;
    if (mod == 3) {
        src = get_reg32(ctx, rm);
    }
    else {
        src = read_memory_dword(ctx, mem_addr);
    }

    uint64_t result = (uint64_t)dst - (uint64_t)src;
    update_flags_sub32(ctx, dst, src, result);
}

// REX.W + 3B /r - CMP r64, r/m64
void cmp_r64_rm64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_r) {
        reg |= 0x08;
    }
    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t dst = get_reg64(ctx, reg);
    uint64_t src;
    if (mod == 3) {
        src = get_reg64(ctx, rm);
    }
    else {
        src = read_memory_qword(ctx, mem_addr);
    }

    uint64_t result = dst - src;
    update_flags_sub64(ctx, dst, src, result);
}

// --- Helper: decode ModR/M for CMP ---

void decode_modrm_cmp(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
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

// --- CMP instruction decoder ---

DecodedInstruction decode_cmp_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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
        else if (prefix == 0xF0) {
            // LOCK prefix is #UD for CMP
            raise_ud_ctx(ctx);
        }
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
            prefix == 0x64 || prefix == 0x65 || prefix == 0xF2 || prefix == 0xF3) {
            int seg_override = decode_cmp_segment_override(prefix);
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

    // Determine operand size
    inst.operand_size = 32;
    if (ctx->rex_w) {
        inst.operand_size = 64;
    }
    else if (ctx->operand_size_override) {
        inst.operand_size = 16;
    }

    // Determine address size
    if (ctx->cs.descriptor.long_mode) {
        inst.address_size = ctx->address_size_override ? 32 : 64;
    }
    else {
        inst.address_size = ctx->address_size_override ? 16 : 32;
    }

    switch (inst.opcode) {
    // 3C ib - CMP AL, imm8
    case 0x3C:
        inst.operand_size = 8;
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // 3D iw/id - CMP AX, imm16 / CMP EAX, imm32 / REX.W + CMP RAX, imm32
    case 0x3D:
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

    // 80 /7 ib - CMP r/m8, imm8
    case 0x80:
        inst.operand_size = 8;
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        if (((inst.modrm >> 3) & 0x07) != 7) {
            raise_ud_ctx(ctx);
        }
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // 81 /7 iw/id - CMP r/m16, imm16 / CMP r/m32, imm32 / REX.W + CMP r/m64, imm32
    case 0x81:
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        if (((inst.modrm >> 3) & 0x07) != 7) {
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

    // 83 /7 ib - CMP r/m16, imm8 / CMP r/m32, imm8 / REX.W + CMP r/m64, imm8
    case 0x83:
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        if (((inst.modrm >> 3) & 0x07) != 7) {
            raise_ud_ctx(ctx);
        }
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // 38 /r - CMP r/m8, r8
    case 0x38:
        inst.operand_size = 8;
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        break;

    // 39 /r - CMP r/m16, r16 / CMP r/m32, r32 / REX.W + CMP r/m64, r64
    case 0x39:
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        break;

    // 3A /r - CMP r8, r/m8
    case 0x3A:
        inst.operand_size = 8;
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        break;

    // 3B /r - CMP r16, r/m16 / CMP r32, r/m32 / REX.W + CMP r64, r/m64
    case 0x3B:
        decode_modrm_cmp(ctx, &inst, code, code_size, &offset);
        break;

    default:
        raise_ud_ctx(ctx);
    }

    finalize_rip_relative_address(ctx, &inst, (int)offset);

    if (inst.has_modrm && has_segment_override) {
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        if (mod != 3) {
            const int default_segment = cpu_default_segment_for_memory_operand(ctx, inst.modrm, inst.has_sib, inst.sib, inst.address_size);
            inst.mem_address += cmp_segment_base(ctx, segment_index) - cmp_segment_base(ctx, default_segment);
            cpu_validate_linear_address(ctx, inst.mem_address, segment_index);
        }
    }

    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- CMP instruction executor ---

void execute_cmp(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_cmp_instruction(ctx, code, code_size);

    switch (inst.opcode) {
    // 3C ib - CMP AL, imm8
    case 0x3C:
        cmp_al_imm8(ctx, (uint8_t)inst.immediate);
        break;

    // 3D iw/id - CMP AX, imm16 / CMP EAX, imm32 / REX.W + CMP RAX, imm32
    case 0x3D:
        if (ctx->rex_w) {
            cmp_rax_imm32(ctx, (int32_t)inst.immediate);
        }
        else if (ctx->operand_size_override) {
            cmp_ax_imm16(ctx, (uint16_t)inst.immediate);
        }
        else {
            cmp_eax_imm32(ctx, (uint32_t)inst.immediate);
        }
        break;

    // 80 /7 ib - CMP r/m8, imm8
    case 0x80:
        cmp_rm8_imm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (uint8_t)inst.immediate);
        break;

    // 81 /7 iw/id - CMP r/m16, imm16 / CMP r/m32, imm32 / REX.W + CMP r/m64, imm32
    case 0x81:
        if (ctx->rex_w) {
            cmp_rm64_imm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (int32_t)inst.immediate);
        }
        else if (ctx->operand_size_override) {
            cmp_rm16_imm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (uint16_t)inst.immediate);
        }
        else {
            cmp_rm32_imm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (uint32_t)inst.immediate);
        }
        break;

    // 83 /7 ib - CMP r/m16, imm8 / CMP r/m32, imm8 / REX.W + CMP r/m64, imm8
    case 0x83:
        if (ctx->rex_w) {
            cmp_rm64_imm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (int8_t)inst.immediate);
        }
        else if (ctx->operand_size_override) {
            cmp_rm16_imm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (int8_t)inst.immediate);
        }
        else {
            cmp_rm32_imm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, (int8_t)inst.immediate);
        }
        break;

    // 38 /r - CMP r/m8, r8
    case 0x38:
        cmp_rm8_r8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        break;

    // 39 /r - CMP r/m16, r16 / CMP r/m32, r32 / REX.W + CMP r/m64, r64
    case 0x39:
        if (ctx->rex_w) {
            cmp_rm64_r64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            cmp_rm16_r16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            cmp_rm32_r32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 3A /r - CMP r8, r/m8
    case 0x3A:
        cmp_r8_rm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        break;

    // 3B /r - CMP r16, r/m16 / CMP r32, r/m32 / REX.W + CMP r64, r/m64
    case 0x3B:
        if (ctx->rex_w) {
            cmp_r64_rm64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (ctx->operand_size_override) {
            cmp_r16_rm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            cmp_r32_rm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;
    }
}
