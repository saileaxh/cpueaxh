// instrusments/jmp.hpp - JMP instruction implementation

// --- JMP execution helpers ---

// EB cb - JMP rel8 (short jump, sign-extended to operand size)
void jmp_rel8(CPU_CONTEXT* ctx, int8_t rel8, uint64_t next_rip, int operand_size) {
    if (operand_size == 64) {
        ctx->rip = next_rip + (int64_t)rel8;
    }
    else if (operand_size == 32) {
        ctx->rip = (uint32_t)(next_rip + (int32_t)rel8);
    }
    else {
        ctx->rip = (uint16_t)(next_rip + (int16_t)rel8);
    }
}

// E9 cd - JMP rel32 (near relative, sign-extended to 64 bits in 64-bit mode)
void jmp_rel32(CPU_CONTEXT* ctx, int32_t rel32, uint64_t next_rip, int operand_size) {
    if (operand_size == 64) {
        ctx->rip = next_rip + (int64_t)rel32;
    }
    else if (operand_size == 32) {
        ctx->rip = (uint32_t)(next_rip + rel32);
    }
    else {
        ctx->rip = (uint16_t)(next_rip + (int16_t)rel32);
    }
}

// E9 cw - JMP rel16 (near relative, compat/legacy mode only)
void jmp_rel16(CPU_CONTEXT* ctx, int16_t rel16, uint64_t next_rip) {
    ctx->rip = (uint16_t)(next_rip + rel16);
}

// FF /4 - JMP r/m64 (near absolute indirect, 64-bit mode)
void jmp_abs64(CPU_CONTEXT* ctx, uint64_t target) {
    ctx->rip = target;
}

// FF /4 - JMP r/m32 (near absolute indirect, compat mode)
void jmp_abs32(CPU_CONTEXT* ctx, uint32_t target) {
    ctx->rip = target;
}

// FF /4 - JMP r/m16 (near absolute indirect, 16-bit)
void jmp_abs16(CPU_CONTEXT* ctx, uint16_t target) {
    ctx->rip = target;
}

// FF /5 - JMP m16:16 / m16:32 / m16:64 (far absolute indirect via memory)
// EA cd/cp - JMP ptr16:16 / ptr16:32 (far direct, compat mode only)
void jmp_far(CPU_CONTEXT* ctx, uint16_t new_selector, uint64_t new_offset, int operand_size) {
    // NULL selector check
    if (is_null_selector(new_selector)) {
        raise_gp_ctx(ctx, 0);
    }

    SegmentDescriptor new_desc = load_descriptor_from_table(ctx, new_selector);

    // Must be a code segment (call gates not implemented here)
    if (!is_code_segment(new_desc.type)) {
        raise_gp_ctx(ctx, new_selector & 0xFFFC);
    }

    // L=1 and D=1 simultaneously is invalid in long mode
    if (cpu_long_mode_active(ctx) && new_desc.long_mode && new_desc.db) {
        raise_gp_ctx(ctx, new_selector & 0xFFFC);
    }

    uint8_t rpl = new_selector & 0x03;

    if (is_conforming_code_segment(new_desc.type)) {
        // Conforming: DPL must not be greater than CPL
        if (new_desc.dpl > ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
    }
    else {
        // Non-conforming: DPL must equal CPL; RPL must not exceed CPL
        // (Unlike CALL, JMP cannot change privilege level)
        if (new_desc.dpl != ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
        if (rpl > ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
    }

    if (!new_desc.present) {
        raise_np_ctx(ctx, new_selector & 0xFFFC);
    }

    // Truncate offset based on target mode
    uint64_t target_rip = new_offset;
    if (new_desc.long_mode) {
        target_rip = cpu_mask_code_offset(target_rip, operand_size);
    }
    else {
        target_rip = cpu_mask_code_offset(target_rip, operand_size);
    }

    // Load new CS (no stack changes ¡ª this is JMP, not CALL)
    ctx->cs.selector = (new_selector & 0xFFFC) | ctx->cpl; // CS.RPL = CPL
    ctx->cs.descriptor = new_desc;

    // Load new RIP
    ctx->rip = target_rip;
}

// --- Helper: decode ModR/M for JMP ---

void decode_modrm_jmp(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
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

// --- JMP instruction decoder ---

DecodedInstruction decode_jmp_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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
            // LOCK prefix is #UD for JMP
            raise_ud_ctx(ctx);
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

    // Determine operand size
    // In 64-bit mode: near JMP forced to 64; far JMP follows rex_w / override
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
    // EB cb - JMP rel8 (short jump)
    case 0xEB:
        inst.imm_size = 1;
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = code[offset++];
        break;

    // E9 cw/cd - JMP rel16 / JMP rel32
    case 0xE9:
        if (cpu_is_64bit_code(ctx)) {
            // rel16 not supported in 64-bit mode
            if (ctx->operand_size_override) {
                raise_ud_ctx(ctx);
            }
            inst.imm_size = 4;
        }
        else {
            inst.imm_size = ctx->operand_size_override ? 2 : 4;
        }
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        break;

    // FF /4 - JMP r/m16, r/m32, r/m64 (near absolute indirect)
    // FF /5 - JMP m16:16, m16:32, m16:64 (far absolute indirect)
    case 0xFF: {
        decode_modrm_jmp(ctx, &inst, code, code_size, &offset);
        uint8_t reg_field = (inst.modrm >> 3) & 0x07;

        if (reg_field == 4) {
            // Near indirect: in 64-bit mode forced to r/m64
            if (cpu_is_64bit_code(ctx)) {
                inst.operand_size = 64;
            }
        }
        else if (reg_field == 5) {
            // Far indirect: must be memory operand
            uint8_t mod = (inst.modrm >> 6) & 0x03;
            if (mod == 3) {
                raise_ud_ctx(ctx);
            }
            // Operand size for far pointer: REX.W ¡ú m16:64, default ¡ú m16:32, 0x66 ¡ú m16:16
            if (cpu_is_64bit_code(ctx)) {
                if (ctx->rex_w) {
                    inst.operand_size = 64;
                }
                else if (ctx->operand_size_override) {
                    inst.operand_size = 16;
                }
                else {
                    inst.operand_size = 32;
                }
            }
        }
        else {
            raise_ud_ctx(ctx);
        }
        break;
    }

    // EA cd/cp - JMP ptr16:16 / JMP ptr16:32 (far direct, invalid in 64-bit mode)
    case 0xEA:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        // Read far pointer: offset then selector
        if (inst.operand_size == 32) {
            // ptr16:32 ¡ª 4-byte offset + 2-byte selector
            if (offset + 6 > code_size) {
                raise_gp_ctx(ctx, 0);
            }
            inst.immediate = 0;
            for (int i = 0; i < 4; i++) {
                inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.mem_address = 0;
            for (int i = 0; i < 2; i++) {
                inst.mem_address |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.imm_size = 6;
        }
        else {
            // ptr16:16 ¡ª 2-byte offset + 2-byte selector
            if (offset + 4 > code_size) {
                raise_gp_ctx(ctx, 0);
            }
            inst.immediate = 0;
            for (int i = 0; i < 2; i++) {
                inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.mem_address = 0;
            for (int i = 0; i < 2; i++) {
                inst.mem_address |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.imm_size = 4;
        }
        break;

    default:
        raise_ud_ctx(ctx);
    }

    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- JMP instruction executor ---

void execute_jmp(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_jmp_instruction(ctx, code, code_size);

    // next_rip = address of instruction following this JMP
    uint64_t next_rip = ctx->rip + (uint64_t)inst.inst_size;

    switch (inst.opcode) {
    // EB cb - JMP rel8
    case 0xEB:
        jmp_rel8(ctx, (int8_t)inst.immediate, next_rip, inst.operand_size);
        break;

    // E9 cw/cd - JMP rel16 / JMP rel32
    case 0xE9:
        if (inst.imm_size == 2) {
            jmp_rel16(ctx, (int16_t)inst.immediate, next_rip);
        }
        else {
            jmp_rel32(ctx, (int32_t)inst.immediate, next_rip, inst.operand_size);
        }
        break;

    // FF /4 or FF /5
    case 0xFF: {
        uint8_t reg_field = (inst.modrm >> 3) & 0x07;
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        uint8_t rm = inst.modrm & 0x07;

        if (ctx->rex_b) {
            rm |= 0x08;
        }

        if (reg_field == 4) {
            // Near absolute indirect
            if (inst.operand_size == 64) {
                uint64_t target = (mod == 3) ? get_reg64(ctx, rm)
                                             : read_memory_qword(ctx, inst.mem_address);
                jmp_abs64(ctx, target);
            }
            else if (inst.operand_size == 32) {
                uint32_t target = (mod == 3) ? get_reg32(ctx, rm)
                                             : read_memory_dword(ctx, inst.mem_address);
                jmp_abs32(ctx, target);
            }
            else {
                uint16_t target = (mod == 3) ? get_reg16(ctx, rm)
                                             : read_memory_word(ctx, inst.mem_address);
                jmp_abs16(ctx, target);
            }
        }
        else {
            // FF /5 ¡ª far absolute indirect via memory
            uint64_t new_offset;
            uint16_t new_selector;

            if (inst.operand_size == 64) {
                new_offset   = read_memory_qword(ctx, inst.mem_address);
                new_selector = read_memory_word(ctx,  inst.mem_address + 8);
            }
            else if (inst.operand_size == 32) {
                new_offset   = read_memory_dword(ctx, inst.mem_address);
                new_selector = read_memory_word(ctx,  inst.mem_address + 4);
            }
            else {
                new_offset   = read_memory_word(ctx,  inst.mem_address);
                new_selector = read_memory_word(ctx,  inst.mem_address + 2);
            }

            jmp_far(ctx, new_selector, new_offset, inst.operand_size);
        }
        break;
    }

    // EA cd/cp - JMP ptr16:16 / JMP ptr16:32 (compat mode only)
    case 0xEA: {
        uint16_t new_selector = (uint16_t)inst.mem_address;
        uint64_t new_offset   = inst.immediate;
        jmp_far(ctx, new_selector, new_offset, inst.operand_size);
        break;
    }
    }
}
