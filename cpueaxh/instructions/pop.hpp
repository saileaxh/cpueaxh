// instrusments/pop.hpp - POP instruction implementation

// --- POP execution helpers ---

// 8F /0 - POP r/m16
void pop_rm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    // Read value from stack first, then increment RSP
    // Note: if destination uses ESP/RSP for address calculation, the effective address
    // was computed before the pop (already done during decode), but the POP ESP case
    // means RSP is incremented before the write.
    uint16_t value = pop_value16(ctx);

    if (mod == 3) {
        set_reg16(ctx, rm, value);
    }
    else {
        write_memory_word(ctx, mem_addr, value);
    }
}

// 8F /0 - POP r/m32 (not encodable in 64-bit mode)
void pop_rm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint32_t value = pop_value32(ctx);

    if (mod == 3) {
        set_reg32(ctx, rm, value);
    }
    else {
        write_memory_dword(ctx, mem_addr, value);
    }
}

// 8F /0 - POP r/m64
void pop_rm64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

    if (ctx->rex_b) {
        rm |= 0x08;
    }

    uint64_t value = pop_value64(ctx);

    if (mod == 3) {
        set_reg64(ctx, rm, value);
    }
    else {
        write_memory_qword(ctx, mem_addr, value);
    }
}

// 58+rd - POP r16
void pop_r16(CPU_CONTEXT* ctx, int reg) {
    uint16_t value = pop_value16(ctx);
    set_reg16(ctx, reg, value);
}

// 58+rd - POP r32 (not encodable in 64-bit mode)
void pop_r32(CPU_CONTEXT* ctx, int reg) {
    uint32_t value = pop_value32(ctx);
    set_reg32(ctx, reg, value);
}

// 58+rd - POP r64
void pop_r64(CPU_CONTEXT* ctx, int reg) {
    uint64_t value = pop_value64(ctx);
    set_reg64(ctx, reg, value);
}

// POP into segment register (16-bit selector popped, then loaded)
void pop_sreg16(CPU_CONTEXT* ctx, int seg_index) {
    uint16_t selector = pop_value16(ctx);
    load_segment_register(ctx, seg_index, selector);
}

void pop_sreg32(CPU_CONTEXT* ctx, int seg_index) {
    // Pop 32 bits but only lower 16 bits used as selector
    uint32_t value = pop_value32(ctx);
    uint16_t selector = (uint16_t)(value & 0xFFFF);
    load_segment_register(ctx, seg_index, selector);
}

void pop_sreg64(CPU_CONTEXT* ctx, int seg_index) {
    // Pop 64 bits but only lower 16 bits used as selector
    uint64_t value = pop_value64(ctx);
    uint16_t selector = (uint16_t)(value & 0xFFFF);
    load_segment_register(ctx, seg_index, selector);
}

// --- Helper: decode ModR/M for POP ---

void decode_modrm_pop(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
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

// --- POP instruction decoder ---

DecodedInstruction decode_pop_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;

    cpu_reset_prefix_state(ctx);

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
        else if (cpu_try_apply_rex_prefix(ctx, prefix)) {
            offset++;
        }
        else if (prefix == 0xF0) {
            // LOCK prefix is #UD for POP
            raise_ud_ctx(ctx);
            return inst;
        }
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
            prefix == 0x64 || prefix == 0x65 || prefix == 0xF2 || prefix == 0xF3) {
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

    // In 64-bit mode, POP default operand size is 64 (not 32!)
    // 0x66 demotes to 16, otherwise default is 64
    if (cpu_is_64bit_code(ctx)) {
        if (ctx->operand_size_override) {
            inst.operand_size = 16;
        }
        else {
            inst.operand_size = 64;
        }
    }
    else {
        inst.operand_size = 32;
        if (ctx->rex_w) {
            inst.operand_size = 64;
        }
        else if (ctx->operand_size_override) {
            inst.operand_size = 16;
        }
    }

    // Determine address size
    inst.address_size = ctx->address_size_override
        ? (cpu_default_address_size(ctx) == 64 ? 32 : 16)
        : cpu_default_address_size(ctx);

    switch (inst.opcode) {
    // 8F /0 - POP r/m16, r/m32, r/m64
    case 0x8F:
        decode_modrm_pop(ctx, &inst, code, code_size, &offset);
        if (((inst.modrm >> 3) & 0x07) != 0) {
            raise_ud_ctx(ctx);
        }
        // In 64-bit mode, 32-bit operand size is not encodable
        if (cpu_is_64bit_code(ctx) && inst.operand_size == 32) {
            inst.operand_size = 64;
        }
        break;

    // 58+rd - POP r16, r32, r64
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        // In 64-bit mode, 32-bit operand size is not encodable
        if (cpu_is_64bit_code(ctx) && inst.operand_size == 32) {
            inst.operand_size = 64;
        }
        break;

    // 1F - POP DS (invalid in 64-bit mode)
    case 0x1F:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 07 - POP ES (invalid in 64-bit mode)
    case 0x07:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 17 - POP SS (invalid in 64-bit mode)
    case 0x17:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 0F xx - two-byte opcodes (POP FS / POP GS)
    case 0x0F:
        if (offset >= code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.opcode = code[offset++];
        if (inst.opcode == 0xA1) {
            // POP FS
        }
        else if (inst.opcode == 0xA9) {
            // POP GS
        }
        else {
            raise_ud_ctx(ctx);
        }
        // Store a marker so executor knows this was a 0x0F-prefixed instruction
        inst.has_modrm = false;
        inst.imm_size = 0x0F; // reuse imm_size as two-byte opcode marker
        break;

    default:
        raise_ud_ctx(ctx);
    }

    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- POP instruction executor ---

void execute_pop(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_pop_instruction(ctx, code, code_size);

    // Check for two-byte opcode (0x0F prefix)
    if (inst.imm_size == 0x0F) {
        // POP FS (0x0F A1) or POP GS (0x0F A9)
        int seg_index;
        if (inst.opcode == 0xA1) {
            seg_index = SEG_FS;
        }
        else {
            seg_index = SEG_GS;
        }

        if (cpu_is_64bit_code(ctx)) {
            if (ctx->operand_size_override) {
                pop_sreg16(ctx, seg_index);
            }
            else {
                pop_sreg64(ctx, seg_index);
            }
        }
        else {
            if (ctx->operand_size_override) {
                pop_sreg16(ctx, seg_index);
            }
            else {
                pop_sreg32(ctx, seg_index);
            }
        }
        return;
    }

    switch (inst.opcode) {
    // 8F /0 - POP r/m16 / POP r/m32 / POP r/m64
    case 0x8F:
        if (inst.operand_size == 64) {
            pop_rm64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (inst.operand_size == 16) {
            pop_rm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            pop_rm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 58+rd - POP r16 / POP r32 / POP r64
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int reg = inst.opcode - 0x58;
        if (ctx->rex_b) {
            reg |= 0x08;
        }
        if (inst.operand_size == 64) {
            pop_r64(ctx, reg);
        }
        else if (inst.operand_size == 16) {
            pop_r16(ctx, reg);
        }
        else {
            pop_r32(ctx, reg);
        }
        break;
    }

    // 1F - POP DS
    case 0x1F:
        if (inst.operand_size == 16) {
            pop_sreg16(ctx, SEG_DS);
        }
        else {
            pop_sreg32(ctx, SEG_DS);
        }
        break;

    // 07 - POP ES
    case 0x07:
        if (inst.operand_size == 16) {
            pop_sreg16(ctx, SEG_ES);
        }
        else {
            pop_sreg32(ctx, SEG_ES);
        }
        break;

    // 17 - POP SS
    case 0x17:
        if (inst.operand_size == 16) {
            pop_sreg16(ctx, SEG_SS);
        }
        else {
            pop_sreg32(ctx, SEG_SS);
        }
        break;
    }
}
