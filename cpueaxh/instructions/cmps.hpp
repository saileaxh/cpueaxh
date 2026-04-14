// instrusments/cmps.hpp - CMPS instruction implementation

int decode_cmps_operand_size(CPU_CONTEXT* ctx, uint8_t opcode) {
    if (opcode == 0xA6) {
        return 8;
    }

    if (cpu_is_64bit_code(ctx)) {
        if (ctx->rex_w) {
            return 64;
        }
        return ctx->operand_size_override ? 16 : 32;
    }

    return ctx->operand_size_override ? 16 : 32;
}

int decode_cmps_address_size(CPU_CONTEXT* ctx) {
    if (cpu_is_64bit_code(ctx)) {
        return ctx->address_size_override ? 32 : 64;
    }
    return ctx->address_size_override ? 16 : 32;
}

uint64_t get_cmps_index(CPU_CONTEXT* ctx, int reg, int address_size) {
    switch (address_size) {
    case 16: return get_reg16(ctx, reg);
    case 32: return get_reg32(ctx, reg);
    case 64: return get_reg64(ctx, reg);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void set_cmps_index(CPU_CONTEXT* ctx, int reg, int address_size, uint64_t value) {
    switch (address_size) {
    case 16:
        set_reg16(ctx, reg, (uint16_t)value);
        break;
    case 32:
        set_reg32(ctx, reg, (uint32_t)value);
        break;
    case 64:
        set_reg64(ctx, reg, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

uint64_t cmps_segment_base(CPU_CONTEXT* ctx, int seg_index) {
    return cpu_segment_base_for_addressing(ctx, seg_index);
}

uint64_t read_cmps_value(CPU_CONTEXT* ctx, uint64_t address, int operand_size) {
    switch (operand_size) {
    case 8:  return read_memory_byte(ctx, address);
    case 16: return read_memory_word(ctx, address);
    case 32: return read_memory_dword(ctx, address);
    case 64: return read_memory_qword(ctx, address);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void update_cmps_flags(CPU_CONTEXT* ctx, int operand_size, uint64_t source_value, uint64_t dest_value) {
    switch (operand_size) {
    case 8: {
        uint8_t lhs = (uint8_t)source_value;
        uint8_t rhs = (uint8_t)dest_value;
        uint16_t result = (uint16_t)(lhs - rhs);
        update_flags_sub8(ctx, lhs, rhs, result);
        break;
    }
    case 16: {
        uint16_t lhs = (uint16_t)source_value;
        uint16_t rhs = (uint16_t)dest_value;
        uint32_t result = (uint32_t)(lhs - rhs);
        update_flags_sub16(ctx, lhs, rhs, result);
        break;
    }
    case 32: {
        uint32_t lhs = (uint32_t)source_value;
        uint32_t rhs = (uint32_t)dest_value;
        uint64_t result = (uint64_t)(lhs - rhs);
        update_flags_sub32(ctx, lhs, rhs, result);
        break;
    }
    case 64: {
        uint64_t lhs = source_value;
        uint64_t rhs = dest_value;
        uint64_t result = lhs - rhs;
        update_flags_sub64(ctx, lhs, rhs, result);
        break;
    }
    default:
        raise_ud_ctx(ctx);
    }
}

int decode_cmps_segment_override(uint8_t prefix) {
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

DecodedInstruction decode_cmps_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
    bool has_lock_prefix = false;

    cpu_reset_prefix_state(ctx);

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
        else if (cpu_decode_segment_override(prefix) >= 0) {
            offset++;
        }
        else if (prefix == 0xF2 || prefix == 0xF3) {
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
    if (inst.opcode != 0xA6 && inst.opcode != 0xA7) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    inst.operand_size = decode_cmps_operand_size(ctx, inst.opcode);
    inst.address_size = decode_cmps_address_size(ctx);
    inst.immediate = (uint64_t)cpu_effective_segment_override_or_default(ctx, SEG_DS);
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_cmps(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_cmps_instruction(ctx, code, code_size);
    int source_segment = (int)inst.immediate;

    uint64_t source_index = get_cmps_index(ctx, REG_RSI, inst.address_size);
    uint64_t dest_index = get_cmps_index(ctx, REG_RDI, inst.address_size);
    uint64_t source_addr = cmps_segment_base(ctx, source_segment) + source_index;
    uint64_t dest_addr = cmps_segment_base(ctx, SEG_ES) + dest_index;
    if (!cpu_validate_linear_address(ctx, source_addr, source_segment) ||
        !cpu_validate_linear_address(ctx, dest_addr, SEG_ES)) {
        return;
    }
    uint64_t source_value = read_cmps_value(ctx, source_addr, inst.operand_size);
    uint64_t dest_value = read_cmps_value(ctx, dest_addr, inst.operand_size);
    uint64_t step = (uint64_t)(inst.operand_size / 8);

    update_cmps_flags(ctx, inst.operand_size, source_value, dest_value);

    if (ctx->rflags & RFLAGS_DF) {
        set_cmps_index(ctx, REG_RSI, inst.address_size, source_index - step);
        set_cmps_index(ctx, REG_RDI, inst.address_size, dest_index - step);
    }
    else {
        set_cmps_index(ctx, REG_RSI, inst.address_size, source_index + step);
        set_cmps_index(ctx, REG_RDI, inst.address_size, dest_index + step);
    }
}
