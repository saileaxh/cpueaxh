// instrusments/movs.hpp - MOVS instruction implementation

int decode_movs_operand_size(CPU_CONTEXT* ctx, uint8_t opcode) {
    if (opcode == 0xA4) {
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

int decode_movs_address_size(CPU_CONTEXT* ctx) {
    if (cpu_is_64bit_code(ctx)) {
        return ctx->address_size_override ? 32 : 64;
    }
    return ctx->address_size_override ? 16 : 32;
}

uint64_t get_movs_index(CPU_CONTEXT* ctx, int reg, int address_size) {
    switch (address_size) {
    case 16: return get_reg16(ctx, reg);
    case 32: return get_reg32(ctx, reg);
    case 64: return get_reg64(ctx, reg);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void set_movs_index(CPU_CONTEXT* ctx, int reg, int address_size, uint64_t value) {
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

uint64_t movs_segment_base(CPU_CONTEXT* ctx, int seg_index) {
    return cpu_segment_base_for_addressing(ctx, seg_index);
}

uint64_t read_movs_value(CPU_CONTEXT* ctx, uint64_t address, int operand_size) {
    switch (operand_size) {
    case 8:  return read_memory_byte(ctx, address);
    case 16: return read_memory_word(ctx, address);
    case 32: return read_memory_dword(ctx, address);
    case 64: return read_memory_qword(ctx, address);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void write_movs_value(CPU_CONTEXT* ctx, uint64_t address, int operand_size, uint64_t value) {
    switch (operand_size) {
    case 8:
        write_memory_byte(ctx, address, (uint8_t)value);
        break;
    case 16:
        write_memory_word(ctx, address, (uint16_t)value);
        break;
    case 32:
        write_memory_dword(ctx, address, (uint32_t)value);
        break;
    case 64:
        write_memory_qword(ctx, address, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

int decode_movs_segment_override(uint8_t prefix) {
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

DecodedInstruction decode_movs_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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
    if (inst.opcode != 0xA4 && inst.opcode != 0xA5) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    inst.operand_size = decode_movs_operand_size(ctx, inst.opcode);
    inst.address_size = decode_movs_address_size(ctx);
    inst.immediate = (uint64_t)cpu_effective_segment_override_or_default(ctx, SEG_DS);
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_movs(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_movs_instruction(ctx, code, code_size);
    const int source_segment = (int)inst.immediate;
    uint64_t source_index = get_movs_index(ctx, REG_RSI, inst.address_size);
    uint64_t dest_index = get_movs_index(ctx, REG_RDI, inst.address_size);
    uint64_t source_addr = movs_segment_base(ctx, source_segment) + source_index;
    uint64_t dest_addr = movs_segment_base(ctx, SEG_ES) + dest_index;
    if (!cpu_validate_linear_address(ctx, source_addr, source_segment) ||
        !cpu_validate_linear_address(ctx, dest_addr, SEG_ES)) {
        return;
    }
    uint64_t value = read_movs_value(ctx, source_addr, inst.operand_size);
    uint64_t step = (uint64_t)(inst.operand_size / 8);

    write_movs_value(ctx, dest_addr, inst.operand_size, value);

    if (ctx->rflags & RFLAGS_DF) {
        set_movs_index(ctx, REG_RSI, inst.address_size, source_index - step);
        set_movs_index(ctx, REG_RDI, inst.address_size, dest_index - step);
    }
    else {
        set_movs_index(ctx, REG_RSI, inst.address_size, source_index + step);
        set_movs_index(ctx, REG_RDI, inst.address_size, dest_index + step);
    }
}
