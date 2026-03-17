// instrusments/aesenc.hpp - AESENC/VAESENC implementation

static int decode_aesenc_xmm_reg_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int reg = (modrm >> 3) & 0x07;
    if (ctx->rex_r) {
        reg |= 0x08;
    }
    return reg;
}

static int decode_aesenc_xmm_rm_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int rm = modrm & 0x07;
    if (ctx->rex_b) {
        rm |= 0x08;
    }
    return rm;
}

static void decode_modrm_aesenc(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
    if (*offset >= code_size) {
        raise_gp(0);
    }

    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    uint8_t rm = inst->modrm & 0x07;

    if (mod != 3 && rm == 4 && inst->address_size != 16) {
        if (*offset >= code_size) {
            raise_gp(0);
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
            raise_gp(0);
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

static __m128i aesenc_xmm_to_m128i(XMMRegister value) {
    return _mm_set_epi64x((long long)value.high, (long long)value.low);
}

static XMMRegister aesenc_m128i_to_xmm(__m128i value) {
    XMMRegister result = {};
    alignas(16) uint64_t qwords[2] = {};
    _mm_storeu_si128((__m128i*)qwords, value);
    result.low = qwords[0];
    result.high = qwords[1];
    return result;
}

static XMMRegister apply_aesenc128(XMMRegister state, XMMRegister round_key) {
    return aesenc_m128i_to_xmm(_mm_aesenc_si128(aesenc_xmm_to_m128i(state), aesenc_xmm_to_m128i(round_key)));
}

static AVXRegister256 apply_vaesenc256(AVXRegister256 state, AVXRegister256 round_key) {
    AVXRegister256 result = {};
    result.low = apply_aesenc128(state.low, round_key.low);
    result.high = apply_aesenc128(state.high, round_key.high);
    return result;
}

static XMMRegister read_aesenc_source_operand(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    if (((inst->modrm >> 6) & 0x03) == 0x03) {
        return get_xmm128(ctx, decode_aesenc_xmm_rm_index(ctx, inst->modrm));
    }
    return read_xmm_memory(ctx, inst->mem_address);
}

inline bool is_aesenc_instruction(const uint8_t* code, int len, int prefix_len) {
    if (!code || prefix_len + 3 >= len) {
        return false;
    }
    return code[prefix_len] == 0x0F && code[prefix_len + 1] == 0x38 && code[prefix_len + 2] == 0xDC;
}

inline DecodedInstruction decode_aesenc_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
    bool has_lock_prefix = false;
    bool has_unsupported_simd_prefix = false;
    uint8_t mandatory_prefix = 0;

    ctx->rex_present = false;
    ctx->rex_w = false;
    ctx->rex_r = false;
    ctx->rex_x = false;
    ctx->rex_b = false;
    ctx->operand_size_override = false;
    ctx->address_size_override = false;

    while (offset < code_size) {
        uint8_t prefix = code[offset];
        if (prefix == 0x66 || prefix == 0xF2 || prefix == 0xF3) {
            if (mandatory_prefix == 0 || mandatory_prefix == prefix) {
                mandatory_prefix = prefix;
            }
            else {
                has_unsupported_simd_prefix = true;
            }
            if (prefix == 0x66) {
                ctx->operand_size_override = true;
            }
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
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E || prefix == 0x64 || prefix == 0x65) {
            offset++;
        }
        else {
            break;
        }
    }

    if (has_lock_prefix || has_unsupported_simd_prefix || mandatory_prefix != 0x66) {
        raise_ud();
        return inst;
    }

    if (offset + 4 > code_size) {
        raise_gp(0);
        return inst;
    }

    if (code[offset++] != 0x0F || code[offset++] != 0x38) {
        raise_ud();
        return inst;
    }

    inst.opcode = code[offset++];
    inst.mandatory_prefix = mandatory_prefix;
    if (inst.opcode != 0xDC) {
        raise_ud();
        return inst;
    }

    if (ctx->cs.descriptor.long_mode) {
        inst.address_size = ctx->address_size_override ? 32 : 64;
    }
    else {
        inst.address_size = ctx->address_size_override ? 16 : 32;
    }

    decode_modrm_aesenc(ctx, &inst, code, code_size, &offset);
    if (cpu_has_exception(ctx)) {
        return inst;
    }

    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, inst.inst_size);
    ctx->last_inst_size = inst.inst_size;
    return inst;
}

inline void execute_aesenc(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_aesenc_instruction(ctx, code, code_size);
    if (cpu_has_exception(ctx)) {
        return;
    }

    int dest = decode_aesenc_xmm_reg_index(ctx, inst.modrm);
    XMMRegister state = get_xmm128(ctx, dest);
    XMMRegister round_key = read_aesenc_source_operand(ctx, &inst);
    set_xmm128(ctx, dest, apply_aesenc128(state, round_key));
}