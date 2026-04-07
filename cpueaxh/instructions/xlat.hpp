// instrusments/xlat.hpp - XLAT instruction implementation

int decode_xlat_address_size(CPU_CONTEXT* ctx) {
    if (ctx->cs.descriptor.long_mode) {
        return ctx->address_size_override ? 32 : 64;
    }
    return ctx->address_size_override ? 16 : 32;
}

int decode_xlat_segment_override(uint8_t prefix) {
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

uint64_t get_xlat_base_offset(CPU_CONTEXT* ctx, int address_size) {
    switch (address_size) {
    case 16:
        return get_reg16(ctx, REG_RBX);
    case 32:
        return get_reg32(ctx, REG_RBX);
    case 64:
        return get_reg64(ctx, REG_RBX);
    default:
        raise_ud_ctx(ctx);
        return 0;
    }
}

uint64_t get_xlat_linear_address(CPU_CONTEXT* ctx, int address_size, int segment_index) {
    uint64_t segment_base = cpu_segment_base_for_addressing(ctx, segment_index);
    uint64_t offset = get_xlat_base_offset(ctx, address_size) + (uint64_t)get_reg8(ctx, REG_RAX);

    if (address_size == 16) {
        offset = (uint16_t)offset;
    }
    else if (address_size == 32) {
        offset = (uint32_t)offset;
    }

    return segment_base + offset;
}

DecodedInstruction decode_xlat_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
    int segment_index = SEG_DS;
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
        int seg_override = decode_xlat_segment_override(prefix);
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
            segment_index = seg_override;
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
    if (inst.opcode != 0xD7) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    inst.address_size = decode_xlat_address_size(ctx);
    inst.immediate = (uint64_t)segment_index;
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_xlat(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_xlat_instruction(ctx, code, code_size);
    uint64_t address = get_xlat_linear_address(ctx, inst.address_size, (int)inst.immediate);
    set_reg8(ctx, REG_RAX, read_memory_byte(ctx, address));
}
