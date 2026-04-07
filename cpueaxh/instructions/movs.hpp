// instrusments/movs.hpp - MOVS instruction implementation

int decode_movs_operand_size(CPU_CONTEXT* ctx, uint8_t opcode) {
    if (opcode == 0xA4) {
        return 8;
    }

    if (ctx->cs.descriptor.long_mode) {
        if (ctx->rex_w) {
            return 64;
        }
        return ctx->operand_size_override ? 16 : 32;
    }

    return ctx->operand_size_override ? 16 : 32;
}

int decode_movs_address_size(CPU_CONTEXT* ctx) {
    if (ctx->cs.descriptor.long_mode) {
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
    int source_segment = SEG_DS;
    bool has_lock_prefix = false;

    ctx->rex_present = false;
    ctx->rex_w = false;
    ctx->rex_r = false;
    ctx->rex_x = false;
    ctx->rex_b = false;
    ctx->operand_size_override = false;
    ctx->address_size_override = false;

    while (offset < code_size) {
        uint8_t prefix = code[offset];
        int seg_override = decode_movs_segment_override(prefix);
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
            has_lock_prefix = true;
            offset++;
        }
        else if (seg_override >= 0) {
            source_segment = seg_override;
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
    inst.immediate = (uint64_t)source_segment;
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_movs(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_movs_instruction(ctx, code, code_size);
    uint64_t source_index = get_movs_index(ctx, REG_RSI, inst.address_size);
    uint64_t dest_index = get_movs_index(ctx, REG_RDI, inst.address_size);
    uint64_t source_addr = movs_segment_base(ctx, (int)inst.immediate) + source_index;
    uint64_t dest_addr = movs_segment_base(ctx, SEG_ES) + dest_index;
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
