// instrusments/lods.hpp - LODS instruction implementation

int decode_lods_operand_size(CPU_CONTEXT* ctx, uint8_t opcode) {
    if (opcode == 0xAC) {
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

int decode_lods_address_size(CPU_CONTEXT* ctx) {
    if (ctx->cs.descriptor.long_mode) {
        return ctx->address_size_override ? 32 : 64;
    }
    return ctx->address_size_override ? 16 : 32;
}

int decode_lods_segment_override(uint8_t prefix) {
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

uint64_t get_lods_index(CPU_CONTEXT* ctx, int address_size) {
    switch (address_size) {
    case 16: return get_reg16(ctx, REG_RSI);
    case 32: return get_reg32(ctx, REG_RSI);
    case 64: return get_reg64(ctx, REG_RSI);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void set_lods_index(CPU_CONTEXT* ctx, int address_size, uint64_t value) {
    switch (address_size) {
    case 16:
        set_reg16(ctx, REG_RSI, (uint16_t)value);
        break;
    case 32:
        set_reg32(ctx, REG_RSI, (uint32_t)value);
        break;
    case 64:
        set_reg64(ctx, REG_RSI, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

uint64_t lods_segment_base(CPU_CONTEXT* ctx, int seg_index) {
    return cpu_segment_base_for_addressing(ctx, seg_index);
}

uint64_t read_lods_value(CPU_CONTEXT* ctx, uint64_t address, int operand_size) {
    switch (operand_size) {
    case 8:  return read_memory_byte(ctx, address);
    case 16: return read_memory_word(ctx, address);
    case 32: return read_memory_dword(ctx, address);
    case 64: return read_memory_qword(ctx, address);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void write_lods_accumulator(CPU_CONTEXT* ctx, int operand_size, uint64_t value) {
    switch (operand_size) {
    case 8:
        set_reg8(ctx, REG_RAX, (uint8_t)value);
        break;
    case 16:
        set_reg16(ctx, REG_RAX, (uint16_t)value);
        break;
    case 32:
        set_reg32(ctx, REG_RAX, (uint32_t)value);
        break;
    case 64:
        set_reg64(ctx, REG_RAX, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

DecodedInstruction decode_lods_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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
        int seg_override = decode_lods_segment_override(prefix);
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
    if (inst.opcode != 0xAC && inst.opcode != 0xAD) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    inst.operand_size = decode_lods_operand_size(ctx, inst.opcode);
    inst.address_size = decode_lods_address_size(ctx);
    inst.immediate = (uint64_t)source_segment;
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_lods(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_lods_instruction(ctx, code, code_size);
    uint64_t source_index = get_lods_index(ctx, inst.address_size);
    uint64_t source_addr = lods_segment_base(ctx, (int)inst.immediate) + source_index;
    uint64_t value = read_lods_value(ctx, source_addr, inst.operand_size);
    uint64_t step = (uint64_t)(inst.operand_size / 8);

    write_lods_accumulator(ctx, inst.operand_size, value);

    if (ctx->rflags & RFLAGS_DF) {
        set_lods_index(ctx, inst.address_size, source_index - step);
    }
    else {
        set_lods_index(ctx, inst.address_size, source_index + step);
    }
}
