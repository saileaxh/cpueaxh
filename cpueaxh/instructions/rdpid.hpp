// instrusments/rdpid.hpp - RDPID instruction implementation

inline bool rdpid_has_required_prefix(const uint8_t* code, size_t code_size, int prefix_len) {
    if (!code || prefix_len < 0 || code_size < (size_t)prefix_len) {
        return false;
    }

    bool has_f3_prefix = false;
    for (int index = 0; index < prefix_len; index++) {
        if (code[index] == 0x66) {
            return false;
        }
        if (code[index] == 0xF3) {
            has_f3_prefix = true;
        }
    }

    return has_f3_prefix;
}

inline int decode_rdpid_rm_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int rm = modrm & 0x07;
    if (ctx->rex_b) {
        rm |= 0x08;
    }
    return rm;
}

inline bool is_rdpid_instruction(const uint8_t* code, size_t code_size, int prefix_len) {
    if (!code || code_size < (size_t)(prefix_len + 3)) {
        return false;
    }

    if (!rdpid_has_required_prefix(code, code_size, prefix_len)) {
        return false;
    }

    if (code[prefix_len] != 0x0F || code[prefix_len + 1] != 0xC7) {
        return false;
    }

    uint8_t modrm = code[prefix_len + 2];
    return (((modrm >> 3) & 0x07) == 7) && (((modrm >> 6) & 0x03) == 3);
}

inline DecodedInstruction decode_rdpid_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
    bool has_lock_prefix = false;
    bool has_f3_prefix = false;

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
        else if (prefix == 0xF3) {
            has_f3_prefix = true;
            offset++;
        }
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
                 prefix == 0x64 || prefix == 0x65 || prefix == 0xF2) {
            offset++;
        }
        else {
            break;
        }
    }

    if (has_lock_prefix || !has_f3_prefix || ctx->operand_size_override) {
        raise_ud();
        return inst;
    }

    if (offset + 3 > code_size) {
        raise_gp(0);
        return inst;
    }

    if (code[offset++] != 0x0F) {
        raise_ud();
        return inst;
    }

    inst.opcode = code[offset++];
    if (inst.opcode != 0xC7) {
        raise_ud();
        return inst;
    }

    inst.has_modrm = true;
    inst.modrm = code[offset++];
    if (((inst.modrm >> 3) & 0x07) != 7 || ((inst.modrm >> 6) & 0x03) != 3) {
        raise_ud();
        return inst;
    }

    inst.operand_size = ctx->rex_w ? 64 : 32;
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

inline void execute_rdpid(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_rdpid_instruction(ctx, code, code_size);
    if (cpu_has_exception(ctx)) {
        return;
    }

    int rm = decode_rdpid_rm_index(ctx, inst.modrm);
    if (inst.operand_size == 64) {
        set_reg64(ctx, rm, (uint64_t)ctx->processor_id);
        return;
    }

    set_reg32(ctx, rm, ctx->processor_id);
}
