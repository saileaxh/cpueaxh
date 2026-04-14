// instrusments/push.hpp - PUSH instruction implementation

// --- PUSH execution helpers ---

// FF /6 - PUSH r/m16
void push_rm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

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

    push_value16(ctx, value);
}

// FF /6 - PUSH r/m32 (not encodable in 64-bit mode)
void push_rm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

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

    push_value32(ctx, value);
}

// FF /6 - PUSH r/m64
void push_rm64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t rm = modrm & 0x07;

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

    push_value64(ctx, value);
}

// 50+rd - PUSH r16
void push_r16(CPU_CONTEXT* ctx, int reg) {
    uint16_t value = get_reg16(ctx, reg);
    push_value16(ctx, value);
}

// 50+rd - PUSH r32 (not encodable in 64-bit mode)
void push_r32(CPU_CONTEXT* ctx, int reg) {
    uint32_t value = get_reg32(ctx, reg);
    push_value32(ctx, value);
}

// 50+rd - PUSH r64
void push_r64(CPU_CONTEXT* ctx, int reg) {
    uint64_t value = get_reg64(ctx, reg);
    push_value64(ctx, value);
}

// 6A ib - PUSH imm8 (sign-extended to operand size)
void push_imm8_16(CPU_CONTEXT* ctx, int8_t imm) {
    uint16_t value = (uint16_t)(int16_t)imm;
    push_value16(ctx, value);
}

void push_imm8_32(CPU_CONTEXT* ctx, int8_t imm) {
    uint32_t value = (uint32_t)(int32_t)imm;
    push_value32(ctx, value);
}

void push_imm8_64(CPU_CONTEXT* ctx, int8_t imm) {
    uint64_t value = (uint64_t)(int64_t)imm;
    push_value64(ctx, value);
}

// 68 iw - PUSH imm16
void push_imm16(CPU_CONTEXT* ctx, uint16_t imm) {
    push_value16(ctx, imm);
}

// 68 id - PUSH imm32 (sign-extended to 64 bits in 64-bit mode)
void push_imm32_32(CPU_CONTEXT* ctx, uint32_t imm) {
    push_value32(ctx, imm);
}

void push_imm32_64(CPU_CONTEXT* ctx, int32_t imm) {
    uint64_t value = (uint64_t)(int64_t)imm;
    push_value64(ctx, value);
}

// PUSH segment register
void push_sreg16(CPU_CONTEXT* ctx, int seg_index) {
    SegmentRegister* sreg = get_segment_register(ctx, seg_index);
    if (!sreg) {
        raise_gp_ctx(ctx, 0);
    }
    push_value16(ctx, sreg->selector);
}

void push_sreg32(CPU_CONTEXT* ctx, int seg_index) {
    SegmentRegister* sreg = get_segment_register(ctx, seg_index);
    if (!sreg) {
        raise_gp_ctx(ctx, 0);
    }
    // Zero-extended or 16-bit write; we follow the common behavior of zero-extending
    push_value32(ctx, (uint32_t)sreg->selector);
}

void push_sreg64(CPU_CONTEXT* ctx, int seg_index) {
    SegmentRegister* sreg = get_segment_register(ctx, seg_index);
    if (!sreg) {
        raise_gp_ctx(ctx, 0);
    }
    // Zero-extended to 64 bits
    push_value64(ctx, (uint64_t)sreg->selector);
}

// --- Helper: decode ModR/M for PUSH ---

void decode_modrm_push(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
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

// --- PUSH instruction decoder ---

DecodedInstruction decode_push_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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
            // LOCK prefix is #UD for PUSH
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

    // In 64-bit mode, PUSH default operand size is 64 (not 32!)
    // REX.W promotes to 64, 0x66 demotes to 16, otherwise default is 64
    if (cpu_is_64bit_code(ctx)) {
        if (ctx->operand_size_override) {
            inst.operand_size = 16;
        }
        else {
            // In 64-bit mode, default is 64 for PUSH (REX.W has no additional effect)
            inst.operand_size = 64;
        }
    }
    else {
        // In 32-bit/compat mode
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
    // FF /6 - PUSH r/m16, r/m32, r/m64
    case 0xFF:
        decode_modrm_push(ctx, &inst, code, code_size, &offset);
        if (((inst.modrm >> 3) & 0x07) != 6) {
            raise_ud_ctx(ctx);
        }
        // In 64-bit mode, 32-bit operand size is not encodable
        if (cpu_is_64bit_code(ctx) && inst.operand_size == 32) {
            inst.operand_size = 64;
        }
        break;

    // 50+rd - PUSH r16, r32, r64
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        // In 64-bit mode, 32-bit operand size is not encodable
        if (cpu_is_64bit_code(ctx) && inst.operand_size == 32) {
            inst.operand_size = 64;
        }
        break;

    // 6A ib - PUSH imm8
    case 0x6A:
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // 68 iw/id - PUSH imm16, PUSH imm32
    case 0x68:
        if (ctx->operand_size_override) {
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

    // 0E - PUSH CS (invalid in 64-bit mode)
    case 0x0E:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 16 - PUSH SS (invalid in 64-bit mode)
    case 0x16:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 1E - PUSH DS (invalid in 64-bit mode)
    case 0x1E:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 06 - PUSH ES (invalid in 64-bit mode)
    case 0x06:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        break;

    // 0F xx - two-byte opcodes (PUSH FS / PUSH GS)
    case 0x0F:
        if (offset >= code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.opcode = code[offset++];
        // Mark as two-byte opcode by setting bit 8 in a local variable
        // We'll use 0x0FA0 and 0x0FA8 as combined opcode identifiers
        // Store second byte in opcode; we'll handle dispatch with a flag
        if (inst.opcode == 0xA0) {
            // PUSH FS
            inst.opcode = 0xA0; // we'll track the 0x0F prefix separately
        }
        else if (inst.opcode == 0xA8) {
            // PUSH GS
            inst.opcode = 0xA8;
        }
        else {
            raise_ud_ctx(ctx);
        }
        // Store a marker so executor knows this was a 0x0F-prefixed instruction
        inst.has_modrm = false;
        inst.imm_size = 0x0F; // reuse imm_size as a two-byte opcode marker
        break;

    default:
        raise_ud_ctx(ctx);
    }

    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- PUSH instruction executor ---

void execute_push(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_push_instruction(ctx, code, code_size);

    // Check for two-byte opcode (0x0F prefix)
    if (inst.imm_size == 0x0F) {
        // PUSH FS (0x0F A0) or PUSH GS (0x0F A8)
        int seg_index;
        if (inst.opcode == 0xA0) {
            seg_index = SEG_FS;
        }
        else {
            seg_index = SEG_GS;
        }

        if (cpu_is_64bit_code(ctx)) {
            if (ctx->operand_size_override) {
                push_sreg16(ctx, seg_index);
            }
            else {
                push_sreg64(ctx, seg_index);
            }
        }
        else {
            if (ctx->operand_size_override) {
                push_sreg16(ctx, seg_index);
            }
            else {
                push_sreg32(ctx, seg_index);
            }
        }
        return;
    }

    switch (inst.opcode) {
    // FF /6 - PUSH r/m16 / PUSH r/m32 / PUSH r/m64
    case 0xFF:
        if (inst.operand_size == 64) {
            push_rm64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else if (inst.operand_size == 16) {
            push_rm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        else {
            push_rm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address);
        }
        break;

    // 50+rd - PUSH r16 / PUSH r32 / PUSH r64
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {
        int reg = inst.opcode - 0x50;
        if (ctx->rex_b) {
            reg |= 0x08;
        }
        if (inst.operand_size == 64) {
            push_r64(ctx, reg);
        }
        else if (inst.operand_size == 16) {
            push_r16(ctx, reg);
        }
        else {
            push_r32(ctx, reg);
        }
        break;
    }

    // 6A ib - PUSH imm8 (sign-extended to operand size)
    case 0x6A:
        if (inst.operand_size == 64) {
            push_imm8_64(ctx, (int8_t)inst.immediate);
        }
        else if (inst.operand_size == 16) {
            push_imm8_16(ctx, (int8_t)inst.immediate);
        }
        else {
            push_imm8_32(ctx, (int8_t)inst.immediate);
        }
        break;

    // 68 iw/id - PUSH imm16 / PUSH imm32
    case 0x68:
        if (inst.operand_size == 16) {
            push_imm16(ctx, (uint16_t)inst.immediate);
        }
        else if (inst.operand_size == 64) {
            // In 64-bit mode, imm32 is sign-extended to 64 bits
            push_imm32_64(ctx, (int32_t)inst.immediate);
        }
        else {
            push_imm32_32(ctx, (uint32_t)inst.immediate);
        }
        break;

    // 0E - PUSH CS
    case 0x0E:
        if (inst.operand_size == 16) {
            push_sreg16(ctx, SEG_CS);
        }
        else {
            push_sreg32(ctx, SEG_CS);
        }
        break;

    // 16 - PUSH SS
    case 0x16:
        if (inst.operand_size == 16) {
            push_sreg16(ctx, SEG_SS);
        }
        else {
            push_sreg32(ctx, SEG_SS);
        }
        break;

    // 1E - PUSH DS
    case 0x1E:
        if (inst.operand_size == 16) {
            push_sreg16(ctx, SEG_DS);
        }
        else {
            push_sreg32(ctx, SEG_DS);
        }
        break;

    // 06 - PUSH ES
    case 0x06:
        if (inst.operand_size == 16) {
            push_sreg16(ctx, SEG_ES);
        }
        else {
            push_sreg32(ctx, SEG_ES);
        }
        break;
    }
}
