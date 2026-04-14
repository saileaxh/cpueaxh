// instrusments/dec.hpp - DEC instruction implementation

// --- DEC internal helpers ---

int decode_dec_rm_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int rm = modrm & 0x07;
    if (ctx->rex_b) {
        rm |= 0x08;
    }
    return rm;
}

uint64_t read_dec_rm_operand(CPU_CONTEXT* ctx, uint8_t modrm, uint64_t mem_addr, int operand_size) {
    uint8_t mod = (modrm >> 6) & 0x03;
    int rm = decode_dec_rm_index(ctx, modrm);

    if (mod == 3) {
        switch (operand_size) {
        case 8:  return get_reg8(ctx, rm);
        case 16: return get_reg16(ctx, rm);
        case 32: return get_reg32(ctx, rm);
        case 64: return get_reg64(ctx, rm);
        default: raise_ud_ctx(ctx); return 0;
        }
    }

    switch (operand_size) {
    case 8:  return read_memory_byte(ctx, mem_addr);
    case 16: return read_memory_word(ctx, mem_addr);
    case 32: return read_memory_dword(ctx, mem_addr);
    case 64: return read_memory_qword(ctx, mem_addr);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void write_dec_rm_operand(CPU_CONTEXT* ctx, uint8_t modrm, uint64_t mem_addr, int operand_size, uint64_t value) {
    uint8_t mod = (modrm >> 6) & 0x03;
    int rm = decode_dec_rm_index(ctx, modrm);

    if (mod == 3) {
        switch (operand_size) {
        case 8:  set_reg8(ctx, rm, (uint8_t)value); break;
        case 16: set_reg16(ctx, rm, (uint16_t)value); break;
        case 32: set_reg32(ctx, rm, (uint32_t)value); break;
        case 64: set_reg64(ctx, rm, value); break;
        default: raise_ud_ctx(ctx);
        }
        return;
    }

    switch (operand_size) {
    case 8:  write_memory_byte(ctx, mem_addr, (uint8_t)value); break;
    case 16: write_memory_word(ctx, mem_addr, (uint16_t)value); break;
    case 32: write_memory_dword(ctx, mem_addr, (uint32_t)value); break;
    case 64: write_memory_qword(ctx, mem_addr, value); break;
    default: raise_ud_ctx(ctx);
    }
}

// --- DEC execution helpers ---

bool dec_rm_atomic(CPU_CONTEXT* ctx, uint8_t modrm, uint64_t mem_addr, int operand_size) {
    if (((modrm >> 6) & 0x03) == 3) {
        return false;
    }

    uint64_t old_value = 0;
    uint64_t new_value = 0;
    if (!cpu_atomic_sub_memory(ctx, mem_addr, operand_size, 1, &old_value, &new_value)) {
        return true;
    }

    switch (operand_size) {
    case 8:  update_flags_dec8(ctx, (uint8_t)old_value, (uint8_t)new_value); break;
    case 16: update_flags_dec16(ctx, (uint16_t)old_value, (uint16_t)new_value); break;
    case 32: update_flags_dec32(ctx, (uint32_t)old_value, (uint32_t)new_value); break;
    case 64: update_flags_dec64(ctx, old_value, new_value); break;
    default: raise_ud_ctx(ctx); break;
    }
    return true;
}

// FE /1 - DEC r/m8
void dec_rm8(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, bool has_lock_prefix) {
    (void)sib;
    (void)disp;
    if (has_lock_prefix && dec_rm_atomic(ctx, modrm, mem_addr, 8)) {
        return;
    }
    uint8_t src = (uint8_t)read_dec_rm_operand(ctx, modrm, mem_addr, 8);
    uint8_t result = (uint8_t)(src - 1);
    update_flags_dec8(ctx, src, result);
    write_dec_rm_operand(ctx, modrm, mem_addr, 8, result);
}

// FF /1 - DEC r/m16
void dec_rm16(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, bool has_lock_prefix) {
    (void)sib;
    (void)disp;
    if (has_lock_prefix && dec_rm_atomic(ctx, modrm, mem_addr, 16)) {
        return;
    }
    uint16_t src = (uint16_t)read_dec_rm_operand(ctx, modrm, mem_addr, 16);
    uint16_t result = (uint16_t)(src - 1);
    update_flags_dec16(ctx, src, result);
    write_dec_rm_operand(ctx, modrm, mem_addr, 16, result);
}

// FF /1 - DEC r/m32
void dec_rm32(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, bool has_lock_prefix) {
    (void)sib;
    (void)disp;
    if (has_lock_prefix && dec_rm_atomic(ctx, modrm, mem_addr, 32)) {
        return;
    }
    uint32_t src = (uint32_t)read_dec_rm_operand(ctx, modrm, mem_addr, 32);
    uint32_t result = (uint32_t)(src - 1);
    update_flags_dec32(ctx, src, result);
    write_dec_rm_operand(ctx, modrm, mem_addr, 32, result);
}

// REX.W + FF /1 - DEC r/m64
void dec_rm64(CPU_CONTEXT* ctx, uint8_t modrm, uint8_t sib, int32_t disp, uint64_t mem_addr, bool has_lock_prefix) {
    (void)sib;
    (void)disp;
    if (has_lock_prefix && dec_rm_atomic(ctx, modrm, mem_addr, 64)) {
        return;
    }
    uint64_t src = read_dec_rm_operand(ctx, modrm, mem_addr, 64);
    uint64_t result = src - 1;
    update_flags_dec64(ctx, src, result);
    write_dec_rm_operand(ctx, modrm, mem_addr, 64, result);
}

// --- DEC decode helpers ---

void decode_modrm_dec(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset, bool has_lock_prefix) {
    if (*offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }

    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    uint8_t rm = inst->modrm & 0x07;

    if (mod != 3 && rm == 4 && inst->address_size != 16) {
        if (*offset >= code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst->has_sib = true;
        inst->sib = code[(*offset)++];
    }

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

    if (has_lock_prefix && mod == 3) {
        raise_ud_ctx(ctx);
    }
}

// --- DEC instruction decoder ---

DecodedInstruction decode_dec_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;

    cpu_reset_prefix_state(ctx);

    bool has_lock_prefix = false;

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
            has_lock_prefix = true;
            offset++;
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
    inst.operand_size = 32;
    if (ctx->rex_w) {
        inst.operand_size = 64;
    }
    else if (ctx->operand_size_override) {
        inst.operand_size = 16;
    }

    inst.address_size = ctx->address_size_override
        ? (cpu_default_address_size(ctx) == 64 ? 32 : 16)
        : cpu_default_address_size(ctx);
    inst.has_lock_prefix = has_lock_prefix;

    switch (inst.opcode) {
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
            break;
        }
        break;
    // FE /1 - DEC r/m8
    case 0xFE:
        inst.operand_size = 8;
        decode_modrm_dec(ctx, &inst, code, code_size, &offset, has_lock_prefix);
        if (((inst.modrm >> 3) & 0x07) != 1) {
            raise_ud_ctx(ctx);
        }
        break;

    // FF /1 - DEC r/m16 / DEC r/m32 / REX.W + DEC r/m64
    case 0xFF:
        decode_modrm_dec(ctx, &inst, code, code_size, &offset, has_lock_prefix);
        if (((inst.modrm >> 3) & 0x07) != 1) {
            raise_ud_ctx(ctx);
        }
        break;

    default:
        raise_ud_ctx(ctx);
    }

    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- DEC instruction executor ---

void execute_dec(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_dec_instruction(ctx, code, code_size);

    switch (inst.opcode) {
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        {
        const uint8_t reg_modrm = (uint8_t)(0xC8u | (inst.opcode - 0x48u));
        if (ctx->operand_size_override) {
            dec_rm16(ctx, reg_modrm, 0, 0, 0, false);
        }
        else {
            dec_rm32(ctx, reg_modrm, 0, 0, 0, false);
        }
        break;
        }
    // FE /1 - DEC r/m8
    case 0xFE:
        dec_rm8(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, inst.has_lock_prefix);
        break;

    // FF /1 - DEC r/m16 / DEC r/m32 / REX.W + DEC r/m64
    case 0xFF:
        if (ctx->rex_w) {
            dec_rm64(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, inst.has_lock_prefix);
        }
        else if (ctx->operand_size_override) {
            dec_rm16(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, inst.has_lock_prefix);
        }
        else {
            dec_rm32(ctx, inst.modrm, inst.sib, inst.displacement, inst.mem_address, inst.has_lock_prefix);
        }
        break;
    }
}
