// instrusments/endbr.hpp - ENDBR32/ENDBR64 instruction implementation

inline bool probe_endbr_instruction(const uint8_t* code, size_t code_size) {
    if (!code) {
        return false;
    }

    size_t offset = 0;
    bool has_lock_prefix = false;
    bool has_f3_prefix = false;

    while (offset < code_size) {
        uint8_t prefix = code[offset];
        if (prefix == 0x66 || prefix == 0x67 ||
            (prefix >= 0x40 && prefix <= 0x4F) ||
            prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
            prefix == 0x64 || prefix == 0x65 || prefix == 0xF2) {
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
        else {
            break;
        }
    }

    if (offset + 3 > code_size) {
        return false;
    }

    return !has_lock_prefix && has_f3_prefix &&
        code[offset] == 0x0F && code[offset + 1] == 0x1E &&
        (code[offset + 2] == 0xFA || code[offset + 2] == 0xFB);
}

inline bool decode_endbr_common(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size, DecodedInstruction* out_inst) {
    if (!ctx || !code || !out_inst) {
        return false;
    }

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
            ctx->rex_w = ((prefix >> 3) & 1) != 0;
            ctx->rex_r = ((prefix >> 2) & 1) != 0;
            ctx->rex_x = ((prefix >> 1) & 1) != 0;
            ctx->rex_b = (prefix & 1) != 0;
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

    if (offset + 3 > code_size) {
        raise_gp(0);
        return false;
    }

    if (code[offset++] != 0x0F || code[offset++] != 0x1E) {
        raise_ud();
        return false;
    }

    uint8_t opcode3 = code[offset++];
    if (has_lock_prefix || !has_f3_prefix || (opcode3 != 0xFA && opcode3 != 0xFB)) {
        raise_ud();
        return false;
    }

    inst.inst_size = (int)offset;
    ctx->last_inst_size = (int)offset;
    *out_inst = inst;
    return !cpu_has_exception(ctx);
}

inline bool is_endbr_instruction(const uint8_t* code, size_t code_size) {
    return probe_endbr_instruction(code, code_size);
}

inline DecodedInstruction decode_endbr_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    if (!decode_endbr_common(ctx, code, code_size, &inst)) {
        return inst;
    }

    finalize_rip_relative_address(ctx, &inst, inst.inst_size);
    ctx->last_inst_size = inst.inst_size;
    return inst;
}

inline void execute_endbr(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    decode_endbr_instruction(ctx, code, code_size);
}