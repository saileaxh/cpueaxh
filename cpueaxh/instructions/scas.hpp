// instrusments/scas.hpp - SCAS instruction implementation

int decode_scas_operand_size(CPU_CONTEXT* ctx, uint8_t opcode) {
    if (opcode == 0xAE) {
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

int decode_scas_address_size(CPU_CONTEXT* ctx) {
    if (ctx->cs.descriptor.long_mode) {
        return ctx->address_size_override ? 32 : 64;
    }
    return ctx->address_size_override ? 16 : 32;
}

uint64_t get_scas_index(CPU_CONTEXT* ctx, int address_size) {
    switch (address_size) {
    case 16: return get_reg16(ctx, REG_RDI);
    case 32: return get_reg32(ctx, REG_RDI);
    case 64: return get_reg64(ctx, REG_RDI);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void set_scas_index(CPU_CONTEXT* ctx, int address_size, uint64_t value) {
    switch (address_size) {
    case 16:
        set_reg16(ctx, REG_RDI, (uint16_t)value);
        break;
    case 32:
        set_reg32(ctx, REG_RDI, (uint32_t)value);
        break;
    case 64:
        set_reg64(ctx, REG_RDI, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

uint64_t scas_segment_base(CPU_CONTEXT* ctx) {
    return cpu_segment_base_for_addressing(ctx, SEG_ES);
}

uint64_t read_scas_value(CPU_CONTEXT* ctx, uint64_t address, int operand_size) {
    switch (operand_size) {
    case 8:  return read_memory_byte(ctx, address);
    case 16: return read_memory_word(ctx, address);
    case 32: return read_memory_dword(ctx, address);
    case 64: return read_memory_qword(ctx, address);
    default: raise_ud_ctx(ctx); return 0;
    }
}

uint64_t read_scas_accumulator(CPU_CONTEXT* ctx, int operand_size) {
    switch (operand_size) {
    case 8:  return get_reg8(ctx, REG_RAX);
    case 16: return get_reg16(ctx, REG_RAX);
    case 32: return get_reg32(ctx, REG_RAX);
    case 64: return get_reg64(ctx, REG_RAX);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void update_scas_flags(CPU_CONTEXT* ctx, int operand_size, uint64_t accumulator, uint64_t memory_value) {
    switch (operand_size) {
    case 8: {
        uint8_t lhs = (uint8_t)accumulator;
        uint8_t rhs = (uint8_t)memory_value;
        uint16_t result = (uint16_t)(lhs - rhs);
        update_flags_sub8(ctx, lhs, rhs, result);
        break;
    }
    case 16: {
        uint16_t lhs = (uint16_t)accumulator;
        uint16_t rhs = (uint16_t)memory_value;
        uint32_t result = (uint32_t)(lhs - rhs);
        update_flags_sub16(ctx, lhs, rhs, result);
        break;
    }
    case 32: {
        uint32_t lhs = (uint32_t)accumulator;
        uint32_t rhs = (uint32_t)memory_value;
        uint64_t result = (uint64_t)(lhs - rhs);
        update_flags_sub32(ctx, lhs, rhs, result);
        break;
    }
    case 64: {
        uint64_t lhs = accumulator;
        uint64_t rhs = memory_value;
        uint64_t result = lhs - rhs;
        update_flags_sub64(ctx, lhs, rhs, result);
        break;
    }
    default:
        raise_ud_ctx(ctx);
    }
}

DecodedInstruction decode_scas_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
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
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
                 prefix == 0x64 || prefix == 0x65 ||
                 prefix == 0xF2 || prefix == 0xF3) {
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
    if (inst.opcode != 0xAE && inst.opcode != 0xAF) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    inst.operand_size = decode_scas_operand_size(ctx, inst.opcode);
    inst.address_size = decode_scas_address_size(ctx);
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_scas(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_scas_instruction(ctx, code, code_size);
    uint64_t dest_index = get_scas_index(ctx, inst.address_size);
    uint64_t dest_addr = scas_segment_base(ctx) + dest_index;
    uint64_t accumulator = read_scas_accumulator(ctx, inst.operand_size);
    uint64_t memory_value = read_scas_value(ctx, dest_addr, inst.operand_size);
    uint64_t step = (uint64_t)(inst.operand_size / 8);

    update_scas_flags(ctx, inst.operand_size, accumulator, memory_value);

    if (ctx->rflags & RFLAGS_DF) {
        set_scas_index(ctx, inst.address_size, dest_index - step);
    }
    else {
        set_scas_index(ctx, inst.address_size, dest_index + step);
    }
}
