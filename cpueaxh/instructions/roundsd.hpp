// instrusments/roundsd.hpp - ROUNDSD instruction implementation

#include <intrin.h>

static int decode_roundsd_xmm_reg_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int reg = (modrm >> 3) & 0x07;
    if (ctx->rex_r) {
        reg |= 0x08;
    }
    return reg;
}

static int decode_roundsd_xmm_rm_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int rm = modrm & 0x07;
    if (ctx->rex_b) {
        rm |= 0x08;
    }
    return rm;
}

static void decode_modrm_roundsd(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
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
        for (int index = 0; index < inst->disp_size; index++) {
            inst->displacement |= ((int32_t)code[(*offset)++]) << (index * 8);
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

static unsigned int roundsd_host_rounding_mode(uint32_t mxcsr) {
    switch ((mxcsr >> 13) & 0x3U) {
    case 0: return _MM_ROUND_NEAREST;
    case 1: return _MM_ROUND_DOWN;
    case 2: return _MM_ROUND_UP;
    default: return _MM_ROUND_TOWARD_ZERO;
    }
}

static __m128d apply_roundsd_intrinsic(__m128d destination, __m128d source, uint8_t imm8, uint32_t mxcsr) {
    if ((imm8 & 0x04) != 0) {
        unsigned int old_mode = _MM_GET_ROUNDING_MODE();
        _MM_SET_ROUNDING_MODE(roundsd_host_rounding_mode(mxcsr));
        __m128d result = _mm_round_sd(destination, source, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
        _MM_SET_ROUNDING_MODE(old_mode);
        return result;
    }

    switch (imm8 & 0x03) {
    case 0: return _mm_round_sd(destination, source, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    case 1: return _mm_round_sd(destination, source, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    case 2: return _mm_round_sd(destination, source, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC);
    default: return _mm_round_sd(destination, source, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    }
}

static XMMRegister apply_roundsd128(XMMRegister destination, uint64_t source_bits, uint8_t imm8, uint32_t mxcsr) {
    XMMRegister source_scalar = {};
    source_scalar.low = source_bits;
    return sse2_math_pd_m128d_to_xmm(apply_roundsd_intrinsic(sse2_math_pd_xmm_to_m128d(destination), sse2_math_pd_xmm_to_m128d(source_scalar), imm8, mxcsr));
}

inline bool is_roundsd_instruction(const uint8_t* code, int len, int prefix_len) {
    if (!code || prefix_len + 4 >= len) {
        return false;
    }
    return code[prefix_len] == 0x0F && code[prefix_len + 1] == 0x3A && code[prefix_len + 2] == 0x0B;
}

inline DecodedInstruction decode_roundsd_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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

    if (code[offset++] != 0x0F || code[offset++] != 0x3A) {
        raise_ud();
        return inst;
    }

    inst.opcode = code[offset++];
    if (inst.opcode != 0x0B) {
        raise_ud();
        return inst;
    }

    if (ctx->cs.descriptor.long_mode) {
        inst.address_size = ctx->address_size_override ? 32 : 64;
    }
    else {
        inst.address_size = ctx->address_size_override ? 16 : 32;
    }

    decode_modrm_roundsd(ctx, &inst, code, code_size, &offset);

    inst.imm_size = 1;
    if (offset >= code_size) {
        raise_gp(0);
        return inst;
    }
    inst.immediate = code[offset++];

    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

inline void execute_roundsd(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_roundsd_instruction(ctx, code, code_size);
    if (cpu_has_exception(ctx)) {
        return;
    }

    int dest = decode_roundsd_xmm_reg_index(ctx, inst.modrm);
    XMMRegister destination = get_xmm128(ctx, dest);
    uint64_t source_bits = 0;

    if (((inst.modrm >> 6) & 0x03) == 0x03) {
        source_bits = get_xmm128(ctx, decode_roundsd_xmm_rm_index(ctx, inst.modrm)).low;
    }
    else {
        source_bits = read_memory_qword(ctx, inst.mem_address);
    }

    if (cpu_has_exception(ctx)) {
        return;
    }

    set_xmm128(ctx, dest, apply_roundsd128(destination, source_bits, (uint8_t)inst.immediate, ctx->mxcsr));
}
