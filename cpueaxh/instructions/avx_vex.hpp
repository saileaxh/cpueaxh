// instrusments/avx_vex.hpp - Minimal two-byte / three-byte VEX / AVX instruction support

struct AVXRegister256 {
    XMMRegister low;
    XMMRegister high;
};

struct AVXVexPrefix {
    bool three_byte;
    uint8_t byte2;
    uint8_t byte3;
    uint8_t opcode;
    size_t opcode_offset;
    size_t modrm_offset;
};

static void reset_avx_vex_state(CPU_CONTEXT* ctx) {
    ctx->rex_present = false;
    ctx->rex_w = false;
    ctx->rex_r = false;
    ctx->rex_x = false;
    ctx->rex_b = false;
    ctx->operand_size_override = false;
    ctx->address_size_override = false;
}

static AVXVexPrefix decode_avx_vex_prefix(uint8_t* code, size_t code_size) {
    if (code_size < 3) {
        raise_gp(0);
    }

    AVXVexPrefix prefix = {};
    if (code[0] == 0xC5) {
        prefix.three_byte = false;
        prefix.byte2 = code[1];
        prefix.byte3 = code[1];
        prefix.opcode = code[2];
        prefix.opcode_offset = 2;
        prefix.modrm_offset = 3;
        return prefix;
    }

    if (code[0] == 0xC4) {
        if (code_size < 4) {
            raise_gp(0);
        }
        prefix.three_byte = true;
        prefix.byte2 = code[1];
        prefix.byte3 = code[2];
        prefix.opcode = code[3];
        prefix.opcode_offset = 3;
        prefix.modrm_offset = 4;
        return prefix;
    }

    raise_ud();
    return prefix;
}

static uint8_t avx_vex_encoded_vvvv(const AVXVexPrefix* prefix) {
    uint8_t control = prefix->three_byte ? prefix->byte3 : prefix->byte2;
    return (control >> 3) & 0x0F;
}

static int avx_vex_source1_index(const AVXVexPrefix* prefix) {
    return (~avx_vex_encoded_vvvv(prefix)) & 0x0F;
}

static bool avx_vex_is_256(const AVXVexPrefix* prefix) {
    uint8_t control = prefix->three_byte ? prefix->byte3 : prefix->byte2;
    return (control & 0x04) != 0;
}

static uint8_t avx_vex_mandatory_prefix(const AVXVexPrefix* prefix) {
    uint8_t control = prefix->three_byte ? prefix->byte3 : prefix->byte2;
    return control & 0x03;
}

static uint8_t avx_vex_map_select(const AVXVexPrefix* prefix) {
    return prefix->three_byte ? (prefix->byte2 & 0x1F) : 0x01;
}

static void apply_avx_vex_state(CPU_CONTEXT* ctx, const AVXVexPrefix* prefix) {
    reset_avx_vex_state(ctx);
    ctx->rex_present = true;
    ctx->rex_w = prefix->three_byte && ((prefix->byte3 & 0x80) != 0);
    ctx->rex_r = (prefix->byte2 & 0x80) == 0;
    ctx->rex_x = prefix->three_byte && ((prefix->byte2 & 0x40) == 0);
    ctx->rex_b = prefix->three_byte && ((prefix->byte2 & 0x20) == 0);
}

static bool avx_vex_requires_reserved_vvvv(const AVXVexPrefix* prefix) {
    return avx_vex_encoded_vvvv(prefix) != 0x0F;
}

static int avx_vex_dest_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    return decode_xorps_xmm_reg_index(ctx, modrm);
}

static int avx_vex_rm_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    return decode_xorps_xmm_rm_index(ctx, modrm);
}

static int avx_vex_is4_source_index(CPU_CONTEXT* ctx, uint8_t imm8) {
    return ctx->cs.descriptor.long_mode ? ((imm8 >> 4) & 0x0F) : ((imm8 >> 4) & 0x07);
}

static void clear_all_avx_registers(CPU_CONTEXT* ctx) {
    CPUEAXH_MEMSET(ctx->xmm, 0, sizeof(ctx->xmm));
    clear_all_ymm_upper128(ctx);
}

static AVXRegister256 get_ymm256(CPU_CONTEXT* ctx, int reg) {
    AVXRegister256 value = {};
    value.low = get_xmm128(ctx, reg);
    value.high = get_ymm_upper128(ctx, reg);
    return value;
}

static void set_ymm256(CPU_CONTEXT* ctx, int reg, AVXRegister256 value) {
    set_xmm128(ctx, reg, value.low);
    set_ymm_upper128(ctx, reg, value.high);
}

static AVXRegister256 read_ymm_memory(CPU_CONTEXT* ctx, uint64_t address) {
    AVXRegister256 value = {};
    value.low = read_xmm_memory(ctx, address);
    value.high = read_xmm_memory(ctx, address + 16);
    return value;
}

static void write_ymm_memory(CPU_CONTEXT* ctx, uint64_t address, AVXRegister256 value) {
    write_xmm_memory(ctx, address, value.low);
    write_xmm_memory(ctx, address + 16, value.high);
}

static uint32_t get_ymm_lane_bits(const AVXRegister256& value, int lane) {
    if (lane < 4) {
        return get_xmm_lane_bits(value.low, lane);
    }
    return get_xmm_lane_bits(value.high, lane - 4);
}

static void set_ymm_lane_bits(AVXRegister256* value, int lane, uint32_t bits) {
    if (lane < 4) {
        set_xmm_lane_bits(&value->low, lane, bits);
        return;
    }
    set_xmm_lane_bits(&value->high, lane - 4, bits);
}

static uint64_t get_ymm_pd_lane_bits(const AVXRegister256& value, int lane) {
    if (lane < 2) {
        return get_sse2_arith_pd_lane_bits(value.low, lane);
    }
    return get_sse2_arith_pd_lane_bits(value.high, lane - 2);
}

static void set_ymm_pd_lane_bits(AVXRegister256* value, int lane, uint64_t bits) {
    if (lane < 2) {
        set_sse2_arith_pd_lane_bits(&value->low, lane, bits);
        return;
    }
    set_sse2_arith_pd_lane_bits(&value->high, lane - 2, bits);
}

static __m128i avx_xmm_to_m128i(XMMRegister value) {
    return _mm_set_epi64x((long long)value.high, (long long)value.low);
}

static XMMRegister avx_m128i_to_xmm(__m128i value) {
    XMMRegister result = {};
    alignas(16) uint64_t qwords[2] = {};
    _mm_storeu_si128((__m128i*)qwords, value);
    result.low = qwords[0];
    result.high = qwords[1];
    return result;
}

static void update_avx_pcmpstr_flags(CPU_CONTEXT* ctx, bool cf, bool zf, bool sf, bool of) {
    ctx->rflags &= ~(RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_OF);
    if (cf) {
        ctx->rflags |= RFLAGS_CF;
    }
    if (zf) {
        ctx->rflags |= RFLAGS_ZF;
    }
    if (sf) {
        ctx->rflags |= RFLAGS_SF;
    }
    if (of) {
        ctx->rflags |= RFLAGS_OF;
    }
}

static int avx_pcmpestr_length(CPU_CONTEXT* ctx, int reg_index, bool rex_w, uint8_t imm8) {
    int max_length = (imm8 & 0x01) != 0 ? 8 : 16;
    int64_t raw_length = rex_w ? (int64_t)get_reg64(ctx, reg_index)
                               : (int64_t)(int32_t)get_reg32(ctx, reg_index);
    if (raw_length > max_length) {
        return max_length;
    }
    if (raw_length < -max_length) {
        return -max_length;
    }
    return (int)raw_length;
}

struct AVXPcmpstrResult {
    uint32_t index;
    XMMRegister mask;
    bool cf;
    bool zf;
    bool sf;
    bool of;
};

static AVXPcmpstrResult avx_execute_pcmpestr(__m128i lhs, int length_a, __m128i rhs, int length_b, uint8_t imm8) {
    AVXPcmpstrResult result = {};

#define AVX_PCMPESTR_CASE(mode) \
    case mode: { \
        result.index = (uint32_t)_mm_cmpestri(lhs, length_a, rhs, length_b, mode); \
        result.mask = avx_m128i_to_xmm(_mm_cmpestrm(lhs, length_a, rhs, length_b, mode)); \
        result.cf = _mm_cmpestrc(lhs, length_a, rhs, length_b, mode) != 0; \
        result.zf = _mm_cmpestrz(lhs, length_a, rhs, length_b, mode) != 0; \
        result.sf = _mm_cmpestrs(lhs, length_a, rhs, length_b, mode) != 0; \
        result.of = _mm_cmpestro(lhs, length_a, rhs, length_b, mode) != 0; \
        return result; \
    }
#define AVX_PCMPESTR_CASE16(prefix) \
    AVX_PCMPESTR_CASE((prefix) | 0x0) \
    AVX_PCMPESTR_CASE((prefix) | 0x1) \
    AVX_PCMPESTR_CASE((prefix) | 0x2) \
    AVX_PCMPESTR_CASE((prefix) | 0x3) \
    AVX_PCMPESTR_CASE((prefix) | 0x4) \
    AVX_PCMPESTR_CASE((prefix) | 0x5) \
    AVX_PCMPESTR_CASE((prefix) | 0x6) \
    AVX_PCMPESTR_CASE((prefix) | 0x7) \
    AVX_PCMPESTR_CASE((prefix) | 0x8) \
    AVX_PCMPESTR_CASE((prefix) | 0x9) \
    AVX_PCMPESTR_CASE((prefix) | 0xA) \
    AVX_PCMPESTR_CASE((prefix) | 0xB) \
    AVX_PCMPESTR_CASE((prefix) | 0xC) \
    AVX_PCMPESTR_CASE((prefix) | 0xD) \
    AVX_PCMPESTR_CASE((prefix) | 0xE) \
    AVX_PCMPESTR_CASE((prefix) | 0xF)

    switch (imm8) {
        AVX_PCMPESTR_CASE16(0x00);
        AVX_PCMPESTR_CASE16(0x10);
        AVX_PCMPESTR_CASE16(0x20);
        AVX_PCMPESTR_CASE16(0x30);
        AVX_PCMPESTR_CASE16(0x40);
        AVX_PCMPESTR_CASE16(0x50);
        AVX_PCMPESTR_CASE16(0x60);
        AVX_PCMPESTR_CASE16(0x70);
        AVX_PCMPESTR_CASE16(0x80);
        AVX_PCMPESTR_CASE16(0x90);
        AVX_PCMPESTR_CASE16(0xA0);
        AVX_PCMPESTR_CASE16(0xB0);
        AVX_PCMPESTR_CASE16(0xC0);
        AVX_PCMPESTR_CASE16(0xD0);
        AVX_PCMPESTR_CASE16(0xE0);
        AVX_PCMPESTR_CASE16(0xF0);
    default:
        return result;
    }

#undef AVX_PCMPESTR_CASE16
#undef AVX_PCMPESTR_CASE
}

static AVXPcmpstrResult avx_execute_pcmpistr(__m128i lhs, __m128i rhs, uint8_t imm8) {
    AVXPcmpstrResult result = {};

#define AVX_PCMPISTR_CASE(mode) \
    case mode: { \
        result.index = (uint32_t)_mm_cmpistri(lhs, rhs, mode); \
        result.mask = avx_m128i_to_xmm(_mm_cmpistrm(lhs, rhs, mode)); \
        result.cf = _mm_cmpistrc(lhs, rhs, mode) != 0; \
        result.zf = _mm_cmpistrz(lhs, rhs, mode) != 0; \
        result.sf = _mm_cmpistrs(lhs, rhs, mode) != 0; \
        result.of = _mm_cmpistro(lhs, rhs, mode) != 0; \
        return result; \
    }
#define AVX_PCMPISTR_CASE16(prefix) \
    AVX_PCMPISTR_CASE((prefix) | 0x0) \
    AVX_PCMPISTR_CASE((prefix) | 0x1) \
    AVX_PCMPISTR_CASE((prefix) | 0x2) \
    AVX_PCMPISTR_CASE((prefix) | 0x3) \
    AVX_PCMPISTR_CASE((prefix) | 0x4) \
    AVX_PCMPISTR_CASE((prefix) | 0x5) \
    AVX_PCMPISTR_CASE((prefix) | 0x6) \
    AVX_PCMPISTR_CASE((prefix) | 0x7) \
    AVX_PCMPISTR_CASE((prefix) | 0x8) \
    AVX_PCMPISTR_CASE((prefix) | 0x9) \
    AVX_PCMPISTR_CASE((prefix) | 0xA) \
    AVX_PCMPISTR_CASE((prefix) | 0xB) \
    AVX_PCMPISTR_CASE((prefix) | 0xC) \
    AVX_PCMPISTR_CASE((prefix) | 0xD) \
    AVX_PCMPISTR_CASE((prefix) | 0xE) \
    AVX_PCMPISTR_CASE((prefix) | 0xF)

    switch (imm8) {
        AVX_PCMPISTR_CASE16(0x00);
        AVX_PCMPISTR_CASE16(0x10);
        AVX_PCMPISTR_CASE16(0x20);
        AVX_PCMPISTR_CASE16(0x30);
        AVX_PCMPISTR_CASE16(0x40);
        AVX_PCMPISTR_CASE16(0x50);
        AVX_PCMPISTR_CASE16(0x60);
        AVX_PCMPISTR_CASE16(0x70);
        AVX_PCMPISTR_CASE16(0x80);
        AVX_PCMPISTR_CASE16(0x90);
        AVX_PCMPISTR_CASE16(0xA0);
        AVX_PCMPISTR_CASE16(0xB0);
        AVX_PCMPISTR_CASE16(0xC0);
        AVX_PCMPISTR_CASE16(0xD0);
        AVX_PCMPISTR_CASE16(0xE0);
        AVX_PCMPISTR_CASE16(0xF0);
    default:
        return result;
    }

#undef AVX_PCMPISTR_CASE16
#undef AVX_PCMPISTR_CASE
}

static void validate_avx_movaps_alignment(const DecodedInstruction* inst, bool is_256) {
    if (((inst->modrm >> 6) & 0x03) == 3) {
        return;
    }
    uint64_t mask = is_256 ? 0x1FULL : 0x0FULL;
    if ((inst->mem_address & mask) != 0) {
        raise_gp(0);
    }
}

static uint32_t compute_avx_movmskpd128(XMMRegister value) {
    return (uint32_t)((value.low >> 63) & 0x1ULL)
        | (uint32_t)(((value.high >> 63) & 0x1ULL) << 1);
}

static uint32_t compute_avx_movmskpd256(AVXRegister256 value) {
    return compute_avx_movmskpd128(value.low)
        | (uint32_t)(((value.high.low >> 63) & 0x1ULL) << 2)
        | (uint32_t)(((value.high.high >> 63) & 0x1ULL) << 3);
}

static uint32_t compute_avx_movmskps128(XMMRegister value) {
    uint32_t mask = 0;
    for (int lane = 0; lane < 4; lane++) {
        mask |= ((get_xmm_lane_bits(value, lane) >> 31) & 0x1U) << lane;
    }
    return mask;
}

static uint32_t compute_avx_movmskps256(AVXRegister256 value) {
    uint32_t mask = 0;
    for (int lane = 0; lane < 8; lane++) {
        mask |= ((get_ymm_lane_bits(value, lane) >> 31) & 0x1U) << lane;
    }
    return mask;
}

static uint32_t compute_avx_pmovmskb128(XMMRegister value) {
    uint8_t bytes[16] = {};
    uint32_t mask = 0;
    sse2_misc_xmm_to_bytes(value, bytes);
    for (int index = 0; index < 16; index++) {
        mask |= (uint32_t)((bytes[index] >> 7) & 0x1U) << index;
    }
    return mask;
}

static uint32_t compute_avx_pmovmskb256(AVXRegister256 value) {
    uint8_t low_bytes[16] = {};
    uint8_t high_bytes[16] = {};
    uint32_t mask = 0;
    sse2_misc_xmm_to_bytes(value.low, low_bytes);
    sse2_misc_xmm_to_bytes(value.high, high_bytes);
    for (int index = 0; index < 16; index++) {
        mask |= (uint32_t)((low_bytes[index] >> 7) & 0x1U) << index;
        mask |= (uint32_t)((high_bytes[index] >> 7) & 0x1U) << (index + 16);
    }
    return mask;
}

static XMMRegister read_avx_int_rm128(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 3) {
        return get_xmm128(ctx, avx_vex_rm_index(ctx, inst->modrm));
    }
    return read_xmm_memory(ctx, inst->mem_address);
}

static XMMRegister read_avx_movdqa_rm128(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    validate_avx_movaps_alignment(inst, false);
    return read_avx_int_rm128(ctx, inst);
}

static void write_avx_int_rm128(CPU_CONTEXT* ctx, const DecodedInstruction* inst, XMMRegister value, bool clear_upper) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    int dest = avx_vex_rm_index(ctx, inst->modrm);
    if (mod == 3) {
        set_xmm128(ctx, dest, value);
        if (clear_upper) {
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }
    write_xmm_memory(ctx, inst->mem_address, value);
}

static void write_avx_movdqa_rm128(CPU_CONTEXT* ctx, const DecodedInstruction* inst, XMMRegister value, bool clear_upper) {
    validate_avx_movaps_alignment(inst, false);
    write_avx_int_rm128(ctx, inst, value, clear_upper);
}

static DecodedInstruction decode_avx_vex_modrm(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size, const AVXVexPrefix* prefix) {
    DecodedInstruction inst = {};
    uint8_t opcode = prefix->opcode;
    uint8_t mandatory_prefix = avx_vex_mandatory_prefix(prefix);
    uint8_t map_select = avx_vex_map_select(prefix);
    inst.opcode = opcode;
    inst.address_size = ctx->cs.descriptor.long_mode ? 64 : 32;

    size_t offset = prefix->modrm_offset;
    switch (opcode) {
    case 0x0E:
    case 0x0F:
        decode_modrm_sse2_logic_pd(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x00:
        if (map_select != 0x02 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x0C:
    case 0x0D:
    case 0x10:
    case 0x11:
        if (mandatory_prefix == 1 || mandatory_prefix == 3) {
            decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_movups(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x12:
    case 0x13:
    case 0x16:
    case 0x17:
    case 0x50:
        if (map_select == 0x01 && mandatory_prefix == 0) {
            decode_modrm_sse_mov_misc(ctx, &inst, code, code_size, &offset, false);
        }
        else if (opcode == 0x16 && map_select == 0x02 && mandatory_prefix == 1) {
            decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x28:
    case 0x29:
        if (map_select == 0x02 && mandatory_prefix == 1) {
            if (opcode == 0x28) {
                decode_modrm_sse2_int_arith(ctx, &inst, code, code_size, &offset, false);
            }
            else {
                decode_modrm_sse2_int_cmp(ctx, &inst, code, code_size, &offset, false);
            }
        }
        else if (mandatory_prefix == 1) {
            decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_movaps(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
        if (mandatory_prefix == 1) {
            decode_modrm_sse2_logic_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_logic(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x6F:
    case 0x7F:
    case 0xD7:
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x6E:
    case 0x7E:
        decode_modrm_movdq(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0xD0:
        if (mandatory_prefix == 1) {
            decode_modrm_sse2_math_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else if (mandatory_prefix == 3) {
            decode_modrm_sse_math(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            raise_ud();
        }
        break;
    case 0xAE:
        decode_sse_state_modrm(ctx, &inst, code, code_size, &offset);
        break;
    case 0xF0:
    case 0xF7:
        decode_modrm_sse2_misc(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0xDB:
    case 0xDF:
    case 0xEB:
    case 0xEF:
        decode_modrm_sse2_int_logic(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x38:
    case 0x39:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6B:
    case 0x6C:
    case 0x6D:
    case 0xDA:
    case 0xDE:
    case 0xEA:
    case 0xEE:
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x37:
        if (map_select != 0x02 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_int_cmp(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
        if (map_select != 0x02 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x8C:
    case 0x8E:
        if (map_select != 0x02 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x74:
    case 0x75:
    case 0x76:
        decode_modrm_sse2_int_cmp(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x70:
        decode_modrm_sse_shuffle(ctx, &inst, code, code_size, &offset, false);
        if (offset >= code_size) {
            raise_gp(0);
        }
        inst.immediate = code[offset++];
        inst.imm_size = 1;
        break;
    case 0xD4:
    case 0xD5:
    case 0x04:
    case 0x0B:
    case 0x40:
    case 0x41:
    case 0xD8:
    case 0xD9:
    case 0xDD:
    case 0xE0:
    case 0xE3:
    case 0xE4:
    case 0xE5:
    case 0xF6:
    case 0xE8:
    case 0xE9:
    case 0xEC:
    case 0xED:
    case 0xF4:
    case 0xF5:
    case 0xF8:
    case 0xF9:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFD:
    case 0xFE:
        decode_modrm_sse2_int_arith(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x71:
    case 0x72:
    case 0x73:
    case 0xD1:
    case 0xD2:
    case 0xD3:
    case 0xE1:
    case 0xE2:
    case 0xF1:
    case 0xF2:
    case 0xF3:
        decode_modrm_sse2_shift(ctx, &inst, code, code_size, &offset, false);
        if (opcode == 0x71 || opcode == 0x72) {
            uint8_t group = (inst.modrm >> 3) & 0x07;
            if (((inst.modrm >> 6) & 0x03) != 0x03) {
                raise_ud();
            }
            if (group != 2 && group != 4 && group != 6) {
                raise_ud();
            }
            if (offset >= code_size) {
                raise_gp(0);
            }
            inst.immediate = code[offset++];
            inst.imm_size = 1;
        }
        else if (opcode == 0x73) {
            uint8_t group = (inst.modrm >> 3) & 0x07;
            if (((inst.modrm >> 6) & 0x03) != 0x03) {
                raise_ud();
            }
            if (group != 2 && group != 3 && group != 6 && group != 7) {
                raise_ud();
            }
            if (offset >= code_size) {
                raise_gp(0);
            }
            inst.immediate = code[offset++];
            inst.imm_size = 1;
        }
        break;
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x5D:
    case 0x5F:
        if (mandatory_prefix == 1 || mandatory_prefix == 3) {
            decode_modrm_sse2_math_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_math(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x78:
    case 0x79:
        if (map_select != 0x02 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x7C:
    case 0x7D:
        if (mandatory_prefix == 1) {
            decode_modrm_sse2_math_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else if (mandatory_prefix == 3) {
            decode_modrm_sse_math(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            raise_ud();
        }
        break;
    case 0x58:
    case 0x59:
        if (map_select == 0x02 && mandatory_prefix == 1) {
            decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else if (mandatory_prefix == 1 || mandatory_prefix == 3) {
            decode_modrm_sse2_arith_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_arith(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x5C:
    case 0x5E:
        if (mandatory_prefix == 1 || mandatory_prefix == 3) {
            decode_modrm_sse2_arith_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_arith(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x5A:
        if (map_select == 0x02 && mandatory_prefix == 1) {
            decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse2_convert(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x5B:
    case 0xE6:
        decode_modrm_sse2_convert(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0xE7:
        if (map_select != 0x01 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x2A:
        if (map_select == 0x02 && mandatory_prefix == 1) {
            decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_convert(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    case 0x2B:
        if (map_select == 0x02 && mandatory_prefix == 1) {
            decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            if (map_select != 0x01) {
                raise_ud();
            }
            if (mandatory_prefix == 1) {
                decode_modrm_sse2_mov_pd(ctx, &inst, code, code_size, &offset, false);
            }
            else {
                decode_modrm_movups(ctx, &inst, code, code_size, &offset, false);
            }
        }
        break;
    case 0x2C:
    case 0x2D:
        decode_modrm_sse_convert(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x14:
    case 0x15:
        decode_modrm_sse_shuffle(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0xC6:
        decode_modrm_sse_shuffle(ctx, &inst, code, code_size, &offset, false);
        if (offset >= code_size) {
            raise_gp(0);
        }
        inst.immediate = code[offset++];
        inst.imm_size = 1;
        break;
    case 0xC4:
        if (map_select != 0x01 || mandatory_prefix != 1) {
            raise_ud();
        }
        decode_modrm_sse2_mov_int(ctx, &inst, code, code_size, &offset, false);
        if (offset >= code_size) {
            raise_gp(0);
        }
        inst.immediate = code[offset++];
        inst.imm_size = 1;
        break;
    case 0xC2:
        if (mandatory_prefix == 1 || mandatory_prefix == 3) {
            decode_modrm_sse2_cmp_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_cmp(ctx, &inst, code, code_size, &offset, false);
        }
        if (offset >= code_size) {
            raise_gp(0);
        }
        inst.immediate = code[offset++];
        inst.imm_size = 1;
        break;
    case 0x2E:
    case 0x2F:
        if (mandatory_prefix == 1) {
            decode_modrm_sse2_cmp_pd(ctx, &inst, code, code_size, &offset, false);
        }
        else {
            decode_modrm_sse_cmp(ctx, &inst, code, code_size, &offset, false);
        }
        break;
    default:
        raise_ud();
        break;
    }

    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

static DecodedInstruction decode_avx_vex_0f3a_modrm_imm(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size, const AVXVexPrefix* prefix) {
    DecodedInstruction inst = {};
    uint8_t opcode = prefix->opcode;
    uint8_t mandatory_prefix = avx_vex_mandatory_prefix(prefix);
    inst.opcode = opcode;
    inst.address_size = ctx->cs.descriptor.long_mode ? 64 : 32;

    if (mandatory_prefix != 1) {
        raise_ud();
    }

    size_t offset = prefix->modrm_offset;
    switch (opcode) {
    case 0x01:
    case 0x02:
    case 0x04:
    case 0x06:
    case 0x08:
    case 0x0A:
    case 0x00:
    case 0x0C:
    case 0x0E:
    case 0x0F:
    case 0x21:
    case 0x40:
    case 0x4A:
    case 0x4C:
        decode_modrm_sse_arith(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x17:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x20:
    case 0x22:
    case 0xDF:
    case 0x42:
        decode_modrm_sse2_mov_int(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
        decode_modrm_sse2_pack(ctx, &inst, code, code_size, &offset, false);
        break;
    case 0x05:
    case 0x09:
    case 0x0B:
    case 0x0D:
    case 0x18:
    case 0x19:
    case 0x38:
    case 0x39:
    case 0x41:
    case 0x46:
    case 0x4B:
        decode_modrm_sse2_arith_pd(ctx, &inst, code, code_size, &offset, false);
        break;
    default:
        raise_ud();
    }

    if (offset >= code_size) {
        raise_gp(0);
    }

    inst.immediate = code[offset++];
    inst.imm_size = 1;
    ctx->last_inst_size = (int)offset;
    return inst;
}

static AVXRegister256 read_avx_rm256(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 3) {
        return get_ymm256(ctx, avx_vex_rm_index(ctx, inst->modrm));
    }
    return read_ymm_memory(ctx, inst->mem_address);
}

static void write_avx_rm256(CPU_CONTEXT* ctx, const DecodedInstruction* inst, AVXRegister256 value) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 3) {
        set_ymm256(ctx, avx_vex_rm_index(ctx, inst->modrm), value);
        return;
    }
    write_ymm_memory(ctx, inst->mem_address, value);
}

static AVXRegister256 read_avx_movaps_rm256(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 3) {
        return get_ymm256(ctx, avx_vex_rm_index(ctx, inst->modrm));
    }
    validate_avx_movaps_alignment(inst, true);
    return read_ymm_memory(ctx, inst->mem_address);
}

static void write_avx_movaps_rm256(CPU_CONTEXT* ctx, const DecodedInstruction* inst, AVXRegister256 value) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 3) {
        set_ymm256(ctx, avx_vex_rm_index(ctx, inst->modrm), value);
        return;
    }
    validate_avx_movaps_alignment(inst, true);
    write_ymm_memory(ctx, inst->mem_address, value);
}

static XMMRegister apply_avx_logic128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    switch (opcode) {
    case 0x54:
        result.low = lhs.low & rhs.low;
        result.high = lhs.high & rhs.high;
        break;
    case 0x55:
        result.low = (~lhs.low) & rhs.low;
        result.high = (~lhs.high) & rhs.high;
        break;
    case 0x56:
        result.low = lhs.low | rhs.low;
        result.high = lhs.high | rhs.high;
        break;
    case 0x57:
        result.low = lhs.low ^ rhs.low;
        result.high = lhs.high ^ rhs.high;
        break;
    default:
        raise_ud();
        break;
    }
    return result;
}

static AVXRegister256 apply_avx_logic256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_logic128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_logic128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_unpack_ps128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    int base = (opcode == 0x15) ? 2 : 0;
    set_sse_shuffle_lane_bits(&result, 0, get_sse_shuffle_lane_bits(lhs, base + 0));
    set_sse_shuffle_lane_bits(&result, 1, get_sse_shuffle_lane_bits(rhs, base + 0));
    set_sse_shuffle_lane_bits(&result, 2, get_sse_shuffle_lane_bits(lhs, base + 1));
    set_sse_shuffle_lane_bits(&result, 3, get_sse_shuffle_lane_bits(rhs, base + 1));
    return result;
}

static AVXRegister256 apply_avx_unpack_ps256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    int lane_offset = (opcode == 0x15) ? 2 : 0;
    for (int block = 0; block < 2; ++block) {
        int base = block * 4;
        set_ymm_lane_bits(&result, base + 0, get_ymm_lane_bits(lhs, base + lane_offset + 0));
        set_ymm_lane_bits(&result, base + 1, get_ymm_lane_bits(rhs, base + lane_offset + 0));
        set_ymm_lane_bits(&result, base + 2, get_ymm_lane_bits(lhs, base + lane_offset + 1));
        set_ymm_lane_bits(&result, base + 3, get_ymm_lane_bits(rhs, base + lane_offset + 1));
    }
    return result;
}

static XMMRegister apply_avx_unpack_pd128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    int lane = (opcode == 0x15) ? 1 : 0;
    set_sse2_arith_pd_lane_bits(&result, 0, get_sse2_arith_pd_lane_bits(lhs, lane));
    set_sse2_arith_pd_lane_bits(&result, 1, get_sse2_arith_pd_lane_bits(rhs, lane));
    return result;
}

static AVXRegister256 apply_avx_unpack_pd256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    int lane = (opcode == 0x15) ? 1 : 0;
    for (int block = 0; block < 2; ++block) {
        int base = block * 2;
        set_ymm_pd_lane_bits(&result, base + 0, get_ymm_pd_lane_bits(lhs, base + lane));
        set_ymm_pd_lane_bits(&result, base + 1, get_ymm_pd_lane_bits(rhs, base + lane));
    }
    return result;
}

static XMMRegister apply_avx_shufps128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    XMMRegister result = {};
    set_sse_shuffle_lane_bits(&result, 0, get_sse_shuffle_lane_bits(lhs, imm8 & 0x03));
    set_sse_shuffle_lane_bits(&result, 1, get_sse_shuffle_lane_bits(lhs, (imm8 >> 2) & 0x03));
    set_sse_shuffle_lane_bits(&result, 2, get_sse_shuffle_lane_bits(rhs, (imm8 >> 4) & 0x03));
    set_sse_shuffle_lane_bits(&result, 3, get_sse_shuffle_lane_bits(rhs, (imm8 >> 6) & 0x03));
    return result;
}

static AVXRegister256 apply_avx_shufps256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t imm8) {
    AVXRegister256 result = {};
    for (int block = 0; block < 2; ++block) {
        int base = block * 4;
        set_ymm_lane_bits(&result, base + 0, get_ymm_lane_bits(lhs, base + (imm8 & 0x03)));
        set_ymm_lane_bits(&result, base + 1, get_ymm_lane_bits(lhs, base + ((imm8 >> 2) & 0x03)));
        set_ymm_lane_bits(&result, base + 2, get_ymm_lane_bits(rhs, base + ((imm8 >> 4) & 0x03)));
        set_ymm_lane_bits(&result, base + 3, get_ymm_lane_bits(rhs, base + ((imm8 >> 6) & 0x03)));
    }
    return result;
}

static XMMRegister apply_avx_shufpd128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    XMMRegister result = {};
    set_sse2_arith_pd_lane_bits(&result, 0, get_sse2_arith_pd_lane_bits(lhs, imm8 & 0x01));
    set_sse2_arith_pd_lane_bits(&result, 1, get_sse2_arith_pd_lane_bits(rhs, (imm8 >> 1) & 0x01));
    return result;
}

static AVXRegister256 apply_avx_shufpd256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t imm8) {
    AVXRegister256 result = {};
    set_ymm_pd_lane_bits(&result, 0, get_ymm_pd_lane_bits(lhs, imm8 & 0x01));
    set_ymm_pd_lane_bits(&result, 1, get_ymm_pd_lane_bits(rhs, (imm8 >> 1) & 0x01));
    set_ymm_pd_lane_bits(&result, 2, get_ymm_pd_lane_bits(lhs, 2 + ((imm8 >> 2) & 0x01)));
    set_ymm_pd_lane_bits(&result, 3, get_ymm_pd_lane_bits(rhs, 2 + ((imm8 >> 3) & 0x01)));
    return result;
}

static XMMRegister apply_avx_cvtps2pd128(uint64_t packed) {
    XMMRegister result = {};
    float lane0 = sse2_convert_bits_to_float((uint32_t)(packed & 0xFFFFFFFFU));
    float lane1 = sse2_convert_bits_to_float((uint32_t)(packed >> 32));
    result.low = sse2_convert_double_to_bits((double)lane0);
    result.high = sse2_convert_double_to_bits((double)lane1);
    return result;
}

static AVXRegister256 apply_avx_cvtps2pd256(XMMRegister source) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 4; ++lane) {
        float value = sse2_convert_bits_to_float(sse2_convert_get_ps_lane_bits(source, lane));
        set_ymm_pd_lane_bits(&result, lane, sse2_convert_double_to_bits((double)value));
    }
    return result;
}

static XMMRegister apply_avx_cvtpd2ps128(XMMRegister source) {
    XMMRegister result = {};
    sse2_convert_set_ps_lane_bits(&result, 0, sse2_convert_float_to_bits((float)sse2_convert_bits_to_double(source.low)));
    sse2_convert_set_ps_lane_bits(&result, 1, sse2_convert_float_to_bits((float)sse2_convert_bits_to_double(source.high)));
    return result;
}

static XMMRegister apply_avx_cvtpd2ps256(AVXRegister256 source) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; ++lane) {
        double value = sse2_convert_bits_to_double(get_ymm_pd_lane_bits(source, lane));
        sse2_convert_set_ps_lane_bits(&result, lane, sse2_convert_float_to_bits((float)value));
    }
    return result;
}

static XMMRegister apply_avx_cvtdq2ps128(XMMRegister source) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; ++lane) {
        int32_t value = sse2_convert_bits_to_i32(sse2_convert_get_ps_lane_bits(source, lane));
        sse2_convert_set_ps_lane_bits(&result, lane, sse2_convert_float_to_bits((float)value));
    }
    return result;
}

static AVXRegister256 apply_avx_cvtdq2ps256(AVXRegister256 source) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; ++lane) {
        int32_t value = (int32_t)get_ymm_lane_bits(source, lane);
        set_ymm_lane_bits(&result, lane, sse2_convert_float_to_bits((float)value));
    }
    return result;
}

static XMMRegister apply_avx_cvtps2dq128(XMMRegister source, bool truncate, uint32_t mxcsr) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; ++lane) {
        double value = (double)sse2_convert_bits_to_float(sse2_convert_get_ps_lane_bits(source, lane));
        int64_t converted = 0;
        bool success = sse2_convert_round_fp_to_integer(value, 32, truncate, mxcsr, &converted);
        sse2_convert_set_ps_lane_bits(&result, lane, success ? (uint32_t)((int32_t)converted) : 0x80000000U);
    }
    return result;
}

static AVXRegister256 apply_avx_cvtps2dq256(AVXRegister256 source, bool truncate, uint32_t mxcsr) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; ++lane) {
        double value = (double)sse2_convert_bits_to_float(get_ymm_lane_bits(source, lane));
        int64_t converted = 0;
        bool success = sse2_convert_round_fp_to_integer(value, 32, truncate, mxcsr, &converted);
        set_ymm_lane_bits(&result, lane, success ? (uint32_t)((int32_t)converted) : 0x80000000U);
    }
    return result;
}

static XMMRegister apply_avx_cvtdq2pd128(uint64_t packed) {
    XMMRegister result = {};
    int32_t lane0 = (int32_t)(packed & 0xFFFFFFFFU);
    int32_t lane1 = (int32_t)(packed >> 32);
    result.low = sse2_convert_double_to_bits((double)lane0);
    result.high = sse2_convert_double_to_bits((double)lane1);
    return result;
}

static AVXRegister256 apply_avx_cvtdq2pd256(XMMRegister source) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 4; ++lane) {
        int32_t value = sse2_convert_bits_to_i32(sse2_convert_get_ps_lane_bits(source, lane));
        set_ymm_pd_lane_bits(&result, lane, sse2_convert_double_to_bits((double)value));
    }
    return result;
}

static XMMRegister apply_avx_cvtpd2dq128(XMMRegister source, bool truncate, uint32_t mxcsr) {
    XMMRegister result = {};
    for (int lane = 0; lane < 2; ++lane) {
        double value = sse2_convert_bits_to_double(sse2_convert_get_pd_lane_bits(source, lane));
        int64_t converted = 0;
        bool success = sse2_convert_round_fp_to_integer(value, 32, truncate, mxcsr, &converted);
        sse2_convert_set_ps_lane_bits(&result, lane, success ? (uint32_t)((int32_t)converted) : 0x80000000U);
    }
    return result;
}

static XMMRegister apply_avx_cvtpd2dq256(AVXRegister256 source, bool truncate, uint32_t mxcsr) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; ++lane) {
        double value = sse2_convert_bits_to_double(get_ymm_pd_lane_bits(source, lane));
        int64_t converted = 0;
        bool success = sse2_convert_round_fp_to_integer(value, 32, truncate, mxcsr, &converted);
        sse2_convert_set_ps_lane_bits(&result, lane, success ? (uint32_t)((int32_t)converted) : 0x80000000U);
    }
    return result;
}

static XMMRegister apply_avx_cvtss2sd128(XMMRegister src1, uint32_t scalar_bits) {
    XMMRegister result = src1;
    result.low = sse2_convert_double_to_bits((double)sse_convert_bits_to_float(scalar_bits));
    return result;
}

static XMMRegister apply_avx_cvtsd2ss128(XMMRegister src1, uint64_t scalar_bits) {
    XMMRegister result = src1;
    set_xmm_lane_bits(&result, 0, sse_convert_float_to_bits((float)sse2_convert_bits_to_double(scalar_bits)));
    return result;
}

static XMMRegister apply_avx_cvtsi2ss128(XMMRegister src1, int64_t source_value) {
    XMMRegister result = src1;
    set_xmm_lane_bits(&result, 0, sse_convert_float_to_bits((float)source_value));
    return result;
}

static XMMRegister apply_avx_cvtsi2sd128(XMMRegister src1, int64_t source_value) {
    XMMRegister result = src1;
    result.low = sse2_convert_double_to_bits((double)source_value);
    return result;
}

static bool evaluate_avx_cmp_predicate_common(bool unordered, bool equal, bool less, bool greater, uint8_t predicate) {
    bool ordered = !unordered;
    bool less_equal = ordered && (less || equal);
    bool greater_equal = ordered && (greater || equal);
    bool not_equal = ordered && !equal;

    switch (predicate & 0x1F) {
    case 0x00: return ordered && equal;
    case 0x01: return ordered && less;
    case 0x02: return less_equal;
    case 0x03: return unordered;
    case 0x04: return unordered || not_equal;
    case 0x05: return unordered || !less;
    case 0x06: return unordered || !less_equal;
    case 0x07: return ordered;
    case 0x08: return unordered || equal;
    case 0x09: return unordered || !greater_equal;
    case 0x0A: return unordered || !greater;
    case 0x0B: return false;
    case 0x0C: return ordered && !equal;
    case 0x0D: return greater_equal;
    case 0x0E: return ordered && greater;
    case 0x0F: return true;
    case 0x10: return ordered && equal;
    case 0x11: return ordered && less;
    case 0x12: return less_equal;
    case 0x13: return unordered;
    case 0x14: return unordered || not_equal;
    case 0x15: return unordered || !less;
    case 0x16: return unordered || !less_equal;
    case 0x17: return ordered;
    case 0x18: return unordered || equal;
    case 0x19: return unordered || !greater_equal;
    case 0x1A: return unordered || !greater;
    case 0x1B: return false;
    case 0x1C: return ordered && !equal;
    case 0x1D: return greater_equal;
    case 0x1E: return ordered && greater;
    case 0x1F: return true;
    default:   return false;
    }
}

static bool evaluate_avx_cmp_predicate_ps(float lhs, float rhs, uint8_t predicate) {
    bool unordered = sse_cmp_is_nan(lhs) || sse_cmp_is_nan(rhs);
    bool equal = !unordered && lhs == rhs;
    bool less = !unordered && lhs < rhs;
    bool greater = !unordered && lhs > rhs;
    return evaluate_avx_cmp_predicate_common(unordered, equal, less, greater, predicate);
}

static bool evaluate_avx_cmp_predicate_pd(double lhs, double rhs, uint8_t predicate) {
    bool unordered = sse2_cmp_pd_is_nan(lhs) || sse2_cmp_pd_is_nan(rhs);
    bool equal = !unordered && lhs == rhs;
    bool less = !unordered && lhs < rhs;
    bool greater = !unordered && lhs > rhs;
    return evaluate_avx_cmp_predicate_common(unordered, equal, less, greater, predicate);
}

static XMMRegister apply_avx_cmpps128(XMMRegister lhs, XMMRegister rhs, uint8_t predicate) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        float lhs_lane = sse_cmp_bits_to_float(get_xmm_lane_bits(lhs, lane));
        float rhs_lane = sse_cmp_bits_to_float(get_xmm_lane_bits(rhs, lane));
        set_xmm_lane_bits(&result, lane, evaluate_avx_cmp_predicate_ps(lhs_lane, rhs_lane, predicate) ? 0xFFFFFFFFU : 0x00000000U);
    }
    return result;
}

static AVXRegister256 apply_avx_cmpps256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t predicate) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        float lhs_lane = sse_cmp_bits_to_float(get_ymm_lane_bits(lhs, lane));
        float rhs_lane = sse_cmp_bits_to_float(get_ymm_lane_bits(rhs, lane));
        set_ymm_lane_bits(&result, lane, evaluate_avx_cmp_predicate_ps(lhs_lane, rhs_lane, predicate) ? 0xFFFFFFFFU : 0x00000000U);
    }
    return result;
}

static XMMRegister apply_avx_cmppd128(XMMRegister lhs, XMMRegister rhs, uint8_t predicate) {
    XMMRegister result = {};
    for (int lane = 0; lane < 2; lane++) {
        double lhs_lane = sse2_cmp_pd_bits_to_double(get_sse2_arith_pd_lane_bits(lhs, lane));
        double rhs_lane = sse2_cmp_pd_bits_to_double(get_sse2_arith_pd_lane_bits(rhs, lane));
        set_sse2_arith_pd_lane_bits(&result, lane, evaluate_avx_cmp_predicate_pd(lhs_lane, rhs_lane, predicate) ? 0xFFFFFFFFFFFFFFFFULL : 0x0000000000000000ULL);
    }
    return result;
}

static AVXRegister256 apply_avx_cmppd256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t predicate) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 4; lane++) {
        double lhs_lane = sse2_cmp_pd_bits_to_double(get_ymm_pd_lane_bits(lhs, lane));
        double rhs_lane = sse2_cmp_pd_bits_to_double(get_ymm_pd_lane_bits(rhs, lane));
        set_ymm_pd_lane_bits(&result, lane, evaluate_avx_cmp_predicate_pd(lhs_lane, rhs_lane, predicate) ? 0xFFFFFFFFFFFFFFFFULL : 0x0000000000000000ULL);
    }
    return result;
}

static XMMRegister apply_avx_cmpss128(XMMRegister src1, uint32_t rhs_bits, uint8_t predicate) {
    XMMRegister result = src1;
    float lhs = sse_cmp_bits_to_float(get_xmm_lane_bits(src1, 0));
    float rhs = sse_cmp_bits_to_float(rhs_bits);
    set_xmm_lane_bits(&result, 0, evaluate_avx_cmp_predicate_ps(lhs, rhs, predicate) ? 0xFFFFFFFFU : 0x00000000U);
    return result;
}

static XMMRegister apply_avx_cmpsd128(XMMRegister src1, uint64_t rhs_bits, uint8_t predicate) {
    XMMRegister result = src1;
    double lhs = sse2_cmp_pd_bits_to_double(src1.low);
    double rhs = sse2_cmp_pd_bits_to_double(rhs_bits);
    result.low = evaluate_avx_cmp_predicate_pd(lhs, rhs, predicate) ? 0xFFFFFFFFFFFFFFFFULL : 0x0000000000000000ULL;
    return result;
}

static XMMRegister apply_avx_arith128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        float lhs_lane = sse_bits_to_float(get_xmm_lane_bits(lhs, lane));
        float rhs_lane = sse_bits_to_float(get_xmm_lane_bits(rhs, lane));
        set_xmm_lane_bits(&result, lane, sse_float_to_bits(apply_sse_arith_scalar(opcode, lhs_lane, rhs_lane)));
    }
    return result;
}

static AVXRegister256 apply_avx_arith256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        float lhs_lane = sse_bits_to_float(get_ymm_lane_bits(lhs, lane));
        float rhs_lane = sse_bits_to_float(get_ymm_lane_bits(rhs, lane));
        set_ymm_lane_bits(&result, lane, sse_float_to_bits(apply_sse_arith_scalar(opcode, lhs_lane, rhs_lane)));
    }
    return result;
}

static XMMRegister apply_avx_minmax128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    __m128 lhs_vec = sse_math_xmm_to_m128(lhs);
    __m128 rhs_vec = sse_math_xmm_to_m128(rhs);
    return sse_math_m128_to_xmm(opcode == 0x5D ? _mm_min_ps(lhs_vec, rhs_vec)
                                               : _mm_max_ps(lhs_vec, rhs_vec));
}

static AVXRegister256 apply_avx_minmax256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_minmax128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_minmax128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_sqrt128(XMMRegister rhs) {
    return sse_math_m128_to_xmm(_mm_sqrt_ps(sse_math_xmm_to_m128(rhs)));
}

static AVXRegister256 apply_avx_sqrt256(AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_sqrt128(rhs.low);
    result.high = apply_avx_sqrt128(rhs.high);
    return result;
}

static XMMRegister apply_avx_unary_math128(uint8_t opcode, XMMRegister rhs) {
    __m128 rhs_vec = sse_math_xmm_to_m128(rhs);
    switch (opcode) {
    case 0x51:
        return sse_math_m128_to_xmm(_mm_sqrt_ps(rhs_vec));
    case 0x52:
        return sse_math_m128_to_xmm(_mm_rsqrt_ps(rhs_vec));
    case 0x53:
        return sse_math_m128_to_xmm(_mm_rcp_ps(rhs_vec));
    default:
        raise_ud();
        return rhs;
    }
}

static AVXRegister256 apply_avx_unary_math256(uint8_t opcode, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_unary_math128(opcode, rhs.low);
    result.high = apply_avx_unary_math128(opcode, rhs.high);
    return result;
}

static XMMRegister apply_avx_unary_math_ss128(uint8_t opcode, XMMRegister src1, uint32_t rhs_bits) {
    XMMRegister result = src1;
    XMMRegister rhs_scalar = {};
    set_xmm_lane_bits(&rhs_scalar, 0, rhs_bits);
    __m128 rhs_vec = sse_math_xmm_to_m128(rhs_scalar);
    float low_result = 0.0f;

    switch (opcode) {
    case 0x51:
        low_result = _mm_cvtss_f32(_mm_sqrt_ss(rhs_vec));
        break;
    case 0x52:
        low_result = _mm_cvtss_f32(_mm_rsqrt_ss(rhs_vec));
        break;
    case 0x53:
        low_result = _mm_cvtss_f32(_mm_rcp_ss(rhs_vec));
        break;
    default:
        raise_ud();
    }

    set_xmm_lane_bits(&result, 0, sse_math_float_to_bits(low_result));
    return result;
}

static XMMRegister apply_avx_haddhsub_ps128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    __m128 lhs_vec = sse_math_xmm_to_m128(lhs);
    __m128 rhs_vec = sse_math_xmm_to_m128(rhs);
    return sse_math_m128_to_xmm(opcode == 0x7C ? _mm_hadd_ps(lhs_vec, rhs_vec)
                                                   : _mm_hsub_ps(lhs_vec, rhs_vec));
}

static AVXRegister256 apply_avx_haddhsub_ps256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_haddhsub_ps128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_haddhsub_ps128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_haddhsub_pd128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    __m128d lhs_vec = sse2_math_pd_xmm_to_m128d(lhs);
    __m128d rhs_vec = sse2_math_pd_xmm_to_m128d(rhs);
    return sse2_math_pd_m128d_to_xmm(opcode == 0x7C ? _mm_hadd_pd(lhs_vec, rhs_vec)
                                                    : _mm_hsub_pd(lhs_vec, rhs_vec));
}

static AVXRegister256 apply_avx_haddhsub_pd256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_haddhsub_pd128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_haddhsub_pd128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_dpps128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    float sum = 0.0f;
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        if ((imm8 & (1U << (lane + 4))) != 0) {
            float lhs_lane = sse_bits_to_float(get_xmm_lane_bits(lhs, lane));
            float rhs_lane = sse_bits_to_float(get_xmm_lane_bits(rhs, lane));
            sum += lhs_lane * rhs_lane;
        }
    }
    uint32_t sum_bits = sse_float_to_bits(sum);
    for (int lane = 0; lane < 4; lane++) {
        set_xmm_lane_bits(&result, lane, (imm8 & (1U << lane)) != 0 ? sum_bits : 0U);
    }
    return result;
}

static XMMRegister apply_avx_dppd128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    double sum = 0.0;
    XMMRegister result = {};
    if ((imm8 & 0x10) != 0) {
        sum += sse2_arith_pd_bits_to_double(lhs.low) * sse2_arith_pd_bits_to_double(rhs.low);
    }
    if ((imm8 & 0x20) != 0) {
        sum += sse2_arith_pd_bits_to_double(lhs.high) * sse2_arith_pd_bits_to_double(rhs.high);
    }
    uint64_t sum_bits = sse2_arith_pd_double_to_bits(sum);
    result.low = (imm8 & 0x01) != 0 ? sum_bits : 0ULL;
    result.high = (imm8 & 0x02) != 0 ? sum_bits : 0ULL;
    return result;
}

static unsigned int avx_host_rounding_mode(uint32_t mxcsr) {
    switch ((mxcsr >> 13) & 0x3U) {
    case 0: return _MM_ROUND_NEAREST;
    case 1: return _MM_ROUND_DOWN;
    case 2: return _MM_ROUND_UP;
    default: return _MM_ROUND_TOWARD_ZERO;
    }
}

static __m128 apply_avx_round_ps_intrinsic(__m128 value, uint8_t imm8, uint32_t mxcsr) {
    if ((imm8 & 0x04) != 0) {
        unsigned int old_mode = _MM_GET_ROUNDING_MODE();
        _MM_SET_ROUNDING_MODE(avx_host_rounding_mode(mxcsr));
        __m128 result = _mm_round_ps(value, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
        _MM_SET_ROUNDING_MODE(old_mode);
        return result;
    }
    switch (imm8 & 0x03) {
    case 0: return _mm_round_ps(value, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    case 1: return _mm_round_ps(value, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    case 2: return _mm_round_ps(value, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC);
    default: return _mm_round_ps(value, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    }
}

static __m128d apply_avx_round_pd_intrinsic(__m128d value, uint8_t imm8, uint32_t mxcsr) {
    if ((imm8 & 0x04) != 0) {
        unsigned int old_mode = _MM_GET_ROUNDING_MODE();
        _MM_SET_ROUNDING_MODE(avx_host_rounding_mode(mxcsr));
        __m128d result = _mm_round_pd(value, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
        _MM_SET_ROUNDING_MODE(old_mode);
        return result;
    }
    switch (imm8 & 0x03) {
    case 0: return _mm_round_pd(value, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    case 1: return _mm_round_pd(value, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    case 2: return _mm_round_pd(value, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC);
    default: return _mm_round_pd(value, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    }
}

static __m128 apply_avx_round_ss_intrinsic(__m128 src1, __m128 rhs, uint8_t imm8, uint32_t mxcsr) {
    if ((imm8 & 0x04) != 0) {
        unsigned int old_mode = _MM_GET_ROUNDING_MODE();
        _MM_SET_ROUNDING_MODE(avx_host_rounding_mode(mxcsr));
        __m128 result = _mm_round_ss(src1, rhs, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
        _MM_SET_ROUNDING_MODE(old_mode);
        return result;
    }
    switch (imm8 & 0x03) {
    case 0: return _mm_round_ss(src1, rhs, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    case 1: return _mm_round_ss(src1, rhs, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    case 2: return _mm_round_ss(src1, rhs, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC);
    default: return _mm_round_ss(src1, rhs, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    }
}

static __m128d apply_avx_round_sd_intrinsic(__m128d src1, __m128d rhs, uint8_t imm8, uint32_t mxcsr) {
    if ((imm8 & 0x04) != 0) {
        unsigned int old_mode = _MM_GET_ROUNDING_MODE();
        _MM_SET_ROUNDING_MODE(avx_host_rounding_mode(mxcsr));
        __m128d result = _mm_round_sd(src1, rhs, _MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC);
        _MM_SET_ROUNDING_MODE(old_mode);
        return result;
    }
    switch (imm8 & 0x03) {
    case 0: return _mm_round_sd(src1, rhs, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    case 1: return _mm_round_sd(src1, rhs, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    case 2: return _mm_round_sd(src1, rhs, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC);
    default: return _mm_round_sd(src1, rhs, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    }
}

static XMMRegister apply_avx_round_ps128(XMMRegister rhs, uint8_t imm8, uint32_t mxcsr) {
    return sse_math_m128_to_xmm(apply_avx_round_ps_intrinsic(sse_math_xmm_to_m128(rhs), imm8, mxcsr));
}

static AVXRegister256 apply_avx_round_ps256(AVXRegister256 rhs, uint8_t imm8, uint32_t mxcsr) {
    AVXRegister256 result = {};
    result.low = apply_avx_round_ps128(rhs.low, imm8, mxcsr);
    result.high = apply_avx_round_ps128(rhs.high, imm8, mxcsr);
    return result;
}

static XMMRegister apply_avx_round_pd128(XMMRegister rhs, uint8_t imm8, uint32_t mxcsr) {
    return sse2_math_pd_m128d_to_xmm(apply_avx_round_pd_intrinsic(sse2_math_pd_xmm_to_m128d(rhs), imm8, mxcsr));
}

static AVXRegister256 apply_avx_round_pd256(AVXRegister256 rhs, uint8_t imm8, uint32_t mxcsr) {
    AVXRegister256 result = {};
    result.low = apply_avx_round_pd128(rhs.low, imm8, mxcsr);
    result.high = apply_avx_round_pd128(rhs.high, imm8, mxcsr);
    return result;
}

static XMMRegister apply_avx_round_ss128(XMMRegister src1, uint32_t rhs_bits, uint8_t imm8, uint32_t mxcsr) {
    XMMRegister rhs_scalar = {};
    set_xmm_lane_bits(&rhs_scalar, 0, rhs_bits);
    return sse_math_m128_to_xmm(apply_avx_round_ss_intrinsic(sse_math_xmm_to_m128(src1), sse_math_xmm_to_m128(rhs_scalar), imm8, mxcsr));
}

static XMMRegister apply_avx_round_sd128(XMMRegister src1, uint64_t rhs_bits, uint8_t imm8, uint32_t mxcsr) {
    XMMRegister rhs_scalar = {};
    rhs_scalar.low = rhs_bits;
    return sse2_math_pd_m128d_to_xmm(apply_avx_round_sd_intrinsic(sse2_math_pd_xmm_to_m128d(src1), sse2_math_pd_xmm_to_m128d(rhs_scalar), imm8, mxcsr));
}

static XMMRegister apply_avx_blend_ps128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        bool use_rhs = (imm8 & (1U << lane)) != 0;
        set_xmm_lane_bits(&result, lane, use_rhs ? get_xmm_lane_bits(rhs, lane) : get_xmm_lane_bits(lhs, lane));
    }
    return result;
}

static AVXRegister256 apply_avx_blend_ps256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t imm8) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        bool use_rhs = (imm8 & (1U << lane)) != 0;
        set_ymm_lane_bits(&result, lane, use_rhs ? get_ymm_lane_bits(rhs, lane) : get_ymm_lane_bits(lhs, lane));
    }
    return result;
}

static XMMRegister apply_avx_blend_pd128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    XMMRegister result = {};
    result.low = (imm8 & 0x01) != 0 ? rhs.low : lhs.low;
    result.high = (imm8 & 0x02) != 0 ? rhs.high : lhs.high;
    return result;
}

static AVXRegister256 apply_avx_blend_pd256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t imm8) {
    AVXRegister256 result = {};
    result.low.low = (imm8 & 0x01) != 0 ? rhs.low.low : lhs.low.low;
    result.low.high = (imm8 & 0x02) != 0 ? rhs.low.high : lhs.low.high;
    result.high.low = (imm8 & 0x04) != 0 ? rhs.high.low : lhs.high.low;
    result.high.high = (imm8 & 0x08) != 0 ? rhs.high.high : lhs.high.high;
    return result;
}

static uint16_t get_xmm_word(XMMRegister value, int index) {
    if (index < 4) {
        return (uint16_t)((value.low >> (index * 16)) & 0xFFFFU);
    }
    return (uint16_t)((value.high >> ((index - 4) * 16)) & 0xFFFFU);
}

static void set_xmm_word(XMMRegister* value, int index, uint16_t word) {
    uint64_t mask = 0xFFFFULL;
    if (index < 4) {
        uint64_t shift = (uint64_t)index * 16;
        value->low = (value->low & ~(mask << shift)) | ((uint64_t)word << shift);
    }
    else {
        uint64_t shift = (uint64_t)(index - 4) * 16;
        value->high = (value->high & ~(mask << shift)) | ((uint64_t)word << shift);
    }
}

static uint8_t get_xmm_byte(XMMRegister value, int index) {
    if (index < 8) {
        return (uint8_t)((value.low >> (index * 8)) & 0xFFU);
    }
    return (uint8_t)((value.high >> ((index - 8) * 8)) & 0xFFU);
}

static void set_xmm_byte(XMMRegister* value, int index, uint8_t byte_value) {
    uint64_t mask = 0xFFULL;
    if (index < 8) {
        uint64_t shift = (uint64_t)index * 8;
        value->low = (value->low & ~(mask << shift)) | ((uint64_t)byte_value << shift);
    }
    else {
        uint64_t shift = (uint64_t)(index - 8) * 8;
        value->high = (value->high & ~(mask << shift)) | ((uint64_t)byte_value << shift);
    }
}

static uint16_t get_ymm_word(AVXRegister256 value, int index) {
    return index < 8 ? get_xmm_word(value.low, index) : get_xmm_word(value.high, index - 8);
}

static void set_ymm_word(AVXRegister256* value, int index, uint16_t word) {
    if (index < 8) {
        set_xmm_word(&value->low, index, word);
    }
    else {
        set_xmm_word(&value->high, index - 8, word);
    }
}

static uint8_t get_ymm_byte(AVXRegister256 value, int index) {
    return index < 16 ? get_xmm_byte(value.low, index) : get_xmm_byte(value.high, index - 16);
}

static void set_ymm_byte(AVXRegister256* value, int index, uint8_t byte_value) {
    if (index < 16) {
        set_xmm_byte(&value->low, index, byte_value);
    }
    else {
        set_xmm_byte(&value->high, index - 16, byte_value);
    }
}

static XMMRegister apply_avx2_blendw128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    XMMRegister result = {};
    for (int lane = 0; lane < 8; lane++) {
        bool use_rhs = (imm8 & (1U << lane)) != 0;
        set_xmm_word(&result, lane, use_rhs ? get_xmm_word(rhs, lane) : get_xmm_word(lhs, lane));
    }
    return result;
}

static AVXRegister256 apply_avx2_blendw256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t imm8) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 16; lane++) {
        bool use_rhs = (imm8 & (1U << (lane & 0x07))) != 0;
        set_ymm_word(&result, lane, use_rhs ? get_ymm_word(rhs, lane) : get_ymm_word(lhs, lane));
    }
    return result;
}

static XMMRegister apply_avx2_blendvb128(XMMRegister lhs, XMMRegister rhs, XMMRegister mask) {
    XMMRegister result = {};
    for (int byte_index = 0; byte_index < 16; byte_index++) {
        bool use_rhs = (get_xmm_byte(mask, byte_index) & 0x80U) != 0;
        set_xmm_byte(&result, byte_index, use_rhs ? get_xmm_byte(rhs, byte_index) : get_xmm_byte(lhs, byte_index));
    }
    return result;
}

static AVXRegister256 apply_avx2_blendvb256(AVXRegister256 lhs, AVXRegister256 rhs, AVXRegister256 mask) {
    AVXRegister256 result = {};
    for (int byte_index = 0; byte_index < 32; byte_index++) {
        bool use_rhs = (get_ymm_byte(mask, byte_index) & 0x80U) != 0;
        set_ymm_byte(&result, byte_index, use_rhs ? get_ymm_byte(rhs, byte_index) : get_ymm_byte(lhs, byte_index));
    }
    return result;
}

static XMMRegister apply_avx_blendv_ps128(XMMRegister lhs, XMMRegister rhs, XMMRegister mask) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        bool use_rhs = (get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0;
        set_xmm_lane_bits(&result, lane, use_rhs ? get_xmm_lane_bits(rhs, lane) : get_xmm_lane_bits(lhs, lane));
    }
    return result;
}

static AVXRegister256 apply_avx_blendv_ps256(AVXRegister256 lhs, AVXRegister256 rhs, AVXRegister256 mask) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        bool use_rhs = (get_ymm_lane_bits(mask, lane) & 0x80000000U) != 0;
        set_ymm_lane_bits(&result, lane, use_rhs ? get_ymm_lane_bits(rhs, lane) : get_ymm_lane_bits(lhs, lane));
    }
    return result;
}

static XMMRegister apply_avx_blendv_pd128(XMMRegister lhs, XMMRegister rhs, XMMRegister mask) {
    XMMRegister result = {};
    result.low = (mask.low & 0x8000000000000000ULL) != 0 ? rhs.low : lhs.low;
    result.high = (mask.high & 0x8000000000000000ULL) != 0 ? rhs.high : lhs.high;
    return result;
}

static AVXRegister256 apply_avx_blendv_pd256(AVXRegister256 lhs, AVXRegister256 rhs, AVXRegister256 mask) {
    AVXRegister256 result = {};
    result.low.low = (mask.low.low & 0x8000000000000000ULL) != 0 ? rhs.low.low : lhs.low.low;
    result.low.high = (mask.low.high & 0x8000000000000000ULL) != 0 ? rhs.low.high : lhs.low.high;
    result.high.low = (mask.high.low & 0x8000000000000000ULL) != 0 ? rhs.high.low : lhs.high.low;
    result.high.high = (mask.high.high & 0x8000000000000000ULL) != 0 ? rhs.high.high : lhs.high.high;
    return result;
}

static XMMRegister apply_avx_int_logic128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    switch (opcode) {
    case 0xDB:
        result.low = lhs.low & rhs.low;
        result.high = lhs.high & rhs.high;
        break;
    case 0xDF:
        result.low = (~lhs.low) & rhs.low;
        result.high = (~lhs.high) & rhs.high;
        break;
    case 0xEB:
        result.low = lhs.low | rhs.low;
        result.high = lhs.high | rhs.high;
        break;
    case 0xEF:
        result.low = lhs.low ^ rhs.low;
        result.high = lhs.high ^ rhs.high;
        break;
    default:
        raise_ud();
        break;
    }
    return result;
}

static AVXRegister256 apply_avx_int_logic256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_int_logic128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_int_logic128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_permilps_imm128(XMMRegister source, uint8_t imm8) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        int selector = (imm8 >> (lane * 2)) & 0x03;
        set_xmm_lane_bits(&result, lane, get_xmm_lane_bits(source, selector));
    }
    return result;
}

static AVXRegister256 apply_avx_permilps_imm256(AVXRegister256 source, uint8_t imm8) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        int lane_in_half = lane & 0x03;
        int half_base = lane < 4 ? 0 : 4;
        int selector = (imm8 >> (lane_in_half * 2)) & 0x03;
        set_ymm_lane_bits(&result, lane, get_ymm_lane_bits(source, half_base + selector));
    }
    return result;
}

static XMMRegister apply_avx_permilpd_imm128(XMMRegister source, uint8_t imm8) {
    XMMRegister result = {};
    result.low = ((imm8 & 0x01) != 0) ? source.high : source.low;
    result.high = ((imm8 & 0x02) != 0) ? source.high : source.low;
    return result;
}

static AVXRegister256 apply_avx_permilpd_imm256(AVXRegister256 source, uint8_t imm8) {
    AVXRegister256 result = {};
    result.low.low = ((imm8 & 0x01) != 0) ? source.low.high : source.low.low;
    result.low.high = ((imm8 & 0x02) != 0) ? source.low.high : source.low.low;
    result.high.low = ((imm8 & 0x04) != 0) ? source.high.high : source.high.low;
    result.high.high = ((imm8 & 0x08) != 0) ? source.high.high : source.high.low;
    return result;
}

static XMMRegister apply_avx_permilps_var128(XMMRegister source, XMMRegister control) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        int selector = (int)(get_xmm_lane_bits(control, lane) & 0x03U);
        set_xmm_lane_bits(&result, lane, get_xmm_lane_bits(source, selector));
    }
    return result;
}

static AVXRegister256 apply_avx_permilps_var256(AVXRegister256 source, AVXRegister256 control) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        int lane_in_half = lane & 0x03;
        int half_base = lane < 4 ? 0 : 4;
        int selector = (int)(get_ymm_lane_bits(control, lane) & 0x03U);
        set_ymm_lane_bits(&result, lane, get_ymm_lane_bits(source, half_base + selector));
    }
    return result;
}

static XMMRegister apply_avx_permilpd_var128(XMMRegister source, XMMRegister control) {
    XMMRegister result = {};
    result.low = (control.low & 0x01ULL) != 0 ? source.high : source.low;
    result.high = (control.high & 0x01ULL) != 0 ? source.high : source.low;
    return result;
}

static AVXRegister256 apply_avx_permilpd_var256(AVXRegister256 source, AVXRegister256 control) {
    AVXRegister256 result = {};
    result.low.low = (control.low.low & 0x01ULL) != 0 ? source.low.high : source.low.low;
    result.low.high = (control.low.high & 0x01ULL) != 0 ? source.low.high : source.low.low;
    result.high.low = (control.high.low & 0x01ULL) != 0 ? source.high.high : source.high.low;
    result.high.high = (control.high.high & 0x01ULL) != 0 ? source.high.high : source.high.low;
    return result;
}

static XMMRegister apply_avx2_pshufb128(XMMRegister source, XMMRegister control) {
    uint8_t source_bytes[16] = {};
    uint8_t control_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(source, source_bytes);
    sse2_pack_xmm_to_bytes(control, control_bytes);
    for (int index = 0; index < 16; index++) {
        uint8_t selector = control_bytes[index];
        result_bytes[index] = (selector & 0x80U) ? 0x00U : source_bytes[selector & 0x0FU];
    }
    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx2_pshufb256(AVXRegister256 source, AVXRegister256 control) {
    AVXRegister256 result = {};
    result.low = apply_avx2_pshufb128(source.low, control.low);
    result.high = apply_avx2_pshufb128(source.high, control.high);
    return result;
}

static XMMRegister apply_avx2_palignr128(XMMRegister src1, XMMRegister src2, uint8_t imm8) {
    uint8_t src1_bytes[16] = {};
    uint8_t src2_bytes[16] = {};
    uint8_t concat_bytes[32] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(src1, src1_bytes);
    sse2_pack_xmm_to_bytes(src2, src2_bytes);
    CPUEAXH_MEMCPY(concat_bytes, src2_bytes, 16);
    CPUEAXH_MEMCPY(concat_bytes + 16, src1_bytes, 16);
    for (int index = 0; index < 16; index++) {
        int source_index = (int)imm8 + index;
        result_bytes[index] = (source_index >= 0 && source_index < 32) ? concat_bytes[source_index] : 0x00U;
    }
    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx2_palignr256(AVXRegister256 src1, AVXRegister256 src2, uint8_t imm8) {
    AVXRegister256 result = {};
    result.low = apply_avx2_palignr128(src1.low, src2.low, imm8);
    result.high = apply_avx2_palignr128(src1.high, src2.high, imm8);
    return result;
}

static AVXRegister256 apply_avx2_perm32x8(AVXRegister256 data, AVXRegister256 control) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        uint32_t selector = get_ymm_lane_bits(control, lane) & 0x07U;
        set_ymm_lane_bits(&result, lane, get_ymm_lane_bits(data, (int)selector));
    }
    return result;
}

static AVXRegister256 apply_avx2_permq256(AVXRegister256 data, uint8_t imm8) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 4; lane++) {
        int selector = (imm8 >> (lane * 2)) & 0x03;
        set_ymm_pd_lane_bits(&result, lane, get_ymm_pd_lane_bits(data, selector));
    }
    return result;
}

static AVXRegister256 apply_avx_perm2f128_256(AVXRegister256 src1, AVXRegister256 src2, uint8_t imm8) {
    auto select_lane = [&](int selector) {
        switch (selector & 0x03) {
        case 0x00: return src1.low;
        case 0x01: return src1.high;
        case 0x02: return src2.low;
        default:   return src2.high;
        }
    };

    AVXRegister256 result = {};
    result.low = (imm8 & 0x08) != 0 ? XMMRegister{} : select_lane(imm8 & 0x03);
    result.high = (imm8 & 0x80) != 0 ? XMMRegister{} : select_lane((imm8 >> 4) & 0x03);
    return result;
}

static AVXRegister256 apply_avx_insertf128_256(AVXRegister256 src1, XMMRegister insert_value, uint8_t imm8) {
    AVXRegister256 result = src1;
    if ((imm8 & 0x01) != 0) {
        result.high = insert_value;
    }
    else {
        result.low = insert_value;
    }
    return result;
}

static XMMRegister apply_avx_extractf128_256(AVXRegister256 source, uint8_t imm8) {
    return (imm8 & 0x01) != 0 ? source.high : source.low;
}

static XMMRegister apply_avx_insertps_reg128(XMMRegister src1, XMMRegister src2, uint8_t imm8) {
    XMMRegister result = src1;
    int source_lane = (imm8 >> 6) & 0x03;
    int dest_lane = (imm8 >> 4) & 0x03;
    set_xmm_lane_bits(&result, dest_lane, get_xmm_lane_bits(src2, source_lane));
    for (int lane = 0; lane < 4; lane++) {
        if ((imm8 & (1U << lane)) != 0) {
            set_xmm_lane_bits(&result, lane, 0);
        }
    }
    return result;
}

static XMMRegister apply_avx_insertps_mem128(XMMRegister src1, uint32_t source_bits, uint8_t imm8) {
    XMMRegister result = src1;
    int dest_lane = (imm8 >> 4) & 0x03;
    set_xmm_lane_bits(&result, dest_lane, source_bits);
    for (int lane = 0; lane < 4; lane++) {
        if ((imm8 & (1U << lane)) != 0) {
            set_xmm_lane_bits(&result, lane, 0);
        }
    }
    return result;
}

static XMMRegister apply_avx_pinsrb128(XMMRegister src1, uint8_t source_value, uint8_t imm8) {
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(src1, result_bytes);
    result_bytes[imm8 & 0x0F] = source_value;
    return sse2_pack_bytes_to_xmm(result_bytes);
}

static XMMRegister apply_avx_pinsrw128(XMMRegister src1, uint16_t source_value, uint8_t imm8) {
    uint8_t result_bytes[16] = {};
    int dest_byte = (int)(imm8 & 0x07) * 2;
    sse2_pack_xmm_to_bytes(src1, result_bytes);
    result_bytes[dest_byte] = (uint8_t)(source_value & 0xFFU);
    result_bytes[dest_byte + 1] = (uint8_t)((source_value >> 8) & 0xFFU);
    return sse2_pack_bytes_to_xmm(result_bytes);
}

static XMMRegister apply_avx_pinsrd128(XMMRegister src1, uint32_t source_value, uint8_t imm8) {
    XMMRegister result = src1;
    set_xmm_lane_bits(&result, imm8 & 0x03, source_value);
    return result;
}

static XMMRegister apply_avx_pinsrq128(XMMRegister src1, uint64_t source_value, uint8_t imm8) {
    XMMRegister result = src1;
    if ((imm8 & 0x01) != 0) {
        result.high = source_value;
    }
    else {
        result.low = source_value;
    }
    return result;
}

static XMMRegister apply_avx_phminposuw128(XMMRegister source) {
    uint8_t source_bytes[16] = {};
    uint16_t minimum = 0;
    uint16_t minimum_index = 0;
    sse2_pack_xmm_to_bytes(source, source_bytes);

    minimum = (uint16_t)source_bytes[0] | (uint16_t)((uint16_t)source_bytes[1] << 8);
    for (uint16_t index = 1; index < 8; ++index) {
        int byte_offset = index * 2;
        uint16_t value = (uint16_t)source_bytes[byte_offset]
                       | (uint16_t)((uint16_t)source_bytes[byte_offset + 1] << 8);
        if (value < minimum) {
            minimum = value;
            minimum_index = index;
        }
    }

    XMMRegister result = {};
    result.low = (uint64_t)minimum | ((uint64_t)minimum_index << 16);
    return result;
}

static AVXRegister256 broadcast_avx_ss256(uint32_t value) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        set_ymm_lane_bits(&result, lane, value);
    }
    return result;
}

static AVXRegister256 broadcast_avx_sd256(uint64_t value) {
    AVXRegister256 result = {};
    result.low.low = value;
    result.low.high = value;
    result.high.low = value;
    result.high.high = value;
    return result;
}

static AVXRegister256 broadcast_avx_f128_256(XMMRegister value) {
    AVXRegister256 result = {};
    result.low = value;
    result.high = value;
    return result;
}

static AVXRegister256 broadcast_avx2_byte256(uint8_t value) {
    AVXRegister256 result = {};
    uint64_t pattern = 0x0101010101010101ULL * (uint64_t)value;
    result.low.low = pattern;
    result.low.high = pattern;
    result.high.low = pattern;
    result.high.high = pattern;
    return result;
}

static AVXRegister256 broadcast_avx2_word256(uint16_t value) {
    AVXRegister256 result = {};
    uint64_t pattern = (uint64_t)value;
    pattern |= pattern << 16;
    pattern |= pattern << 32;
    result.low.low = pattern;
    result.low.high = pattern;
    result.high.low = pattern;
    result.high.high = pattern;
    return result;
}

static AVXRegister256 broadcast_avx2_dword256(uint32_t value) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        set_ymm_lane_bits(&result, lane, value);
    }
    return result;
}

static AVXRegister256 broadcast_avx2_qword256(uint64_t value) {
    AVXRegister256 result = {};
    result.low.low = value;
    result.low.high = value;
    result.high.low = value;
    result.high.high = value;
    return result;
}

static XMMRegister apply_avx_maskmov_load_ps128(XMMRegister mask, XMMRegister source) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            set_xmm_lane_bits(&result, lane, get_xmm_lane_bits(source, lane));
        }
    }
    return result;
}

static AVXRegister256 apply_avx_maskmov_load_ps256(AVXRegister256 mask, AVXRegister256 source) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        if ((get_ymm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            set_ymm_lane_bits(&result, lane, get_ymm_lane_bits(source, lane));
        }
    }
    return result;
}

static XMMRegister apply_avx_maskmov_load_pd128(XMMRegister mask, XMMRegister source) {
    XMMRegister result = {};
    result.low = (mask.low & 0x8000000000000000ULL) != 0 ? source.low : 0;
    result.high = (mask.high & 0x8000000000000000ULL) != 0 ? source.high : 0;
    return result;
}

static AVXRegister256 apply_avx_maskmov_load_pd256(AVXRegister256 mask, AVXRegister256 source) {
    AVXRegister256 result = {};
    result.low.low = (mask.low.low & 0x8000000000000000ULL) != 0 ? source.low.low : 0;
    result.low.high = (mask.low.high & 0x8000000000000000ULL) != 0 ? source.low.high : 0;
    result.high.low = (mask.high.low & 0x8000000000000000ULL) != 0 ? source.high.low : 0;
    result.high.high = (mask.high.high & 0x8000000000000000ULL) != 0 ? source.high.high : 0;
    return result;
}

static void execute_avx_maskmov_store_ps128(CPU_CONTEXT* ctx, uint64_t address, XMMRegister mask, XMMRegister source) {
    for (int lane = 0; lane < 4; lane++) {
        if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            write_memory_dword(ctx, address + (uint64_t)(lane * 4), get_xmm_lane_bits(source, lane));
        }
    }
}

static void execute_avx_maskmov_store_ps256(CPU_CONTEXT* ctx, uint64_t address, AVXRegister256 mask, AVXRegister256 source) {
    for (int lane = 0; lane < 8; lane++) {
        if ((get_ymm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            write_memory_dword(ctx, address + (uint64_t)(lane * 4), get_ymm_lane_bits(source, lane));
        }
    }
}

static void execute_avx_maskmov_store_pd128(CPU_CONTEXT* ctx, uint64_t address, XMMRegister mask, XMMRegister source) {
    if ((mask.low & 0x8000000000000000ULL) != 0) {
        write_memory_qword(ctx, address, source.low);
    }
    if ((mask.high & 0x8000000000000000ULL) != 0) {
        write_memory_qword(ctx, address + 8, source.high);
    }
}

static void execute_avx_maskmov_store_pd256(CPU_CONTEXT* ctx, uint64_t address, AVXRegister256 mask, AVXRegister256 source) {
    if ((mask.low.low & 0x8000000000000000ULL) != 0) {
        write_memory_qword(ctx, address, source.low.low);
    }
    if ((mask.low.high & 0x8000000000000000ULL) != 0) {
        write_memory_qword(ctx, address + 8, source.low.high);
    }
    if ((mask.high.low & 0x8000000000000000ULL) != 0) {
        write_memory_qword(ctx, address + 16, source.high.low);
    }
    if ((mask.high.high & 0x8000000000000000ULL) != 0) {
        write_memory_qword(ctx, address + 24, source.high.high);
    }
}

static XMMRegister apply_avx2_pmaskmov_load128(XMMRegister mask, XMMRegister source, bool is_qword) {
    XMMRegister result = {};
    if (is_qword) {
        result.low = (mask.low & 0x8000000000000000ULL) != 0 ? source.low : 0;
        result.high = (mask.high & 0x8000000000000000ULL) != 0 ? source.high : 0;
        return result;
    }

    for (int lane = 0; lane < 4; lane++) {
        if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            set_xmm_lane_bits(&result, lane, get_xmm_lane_bits(source, lane));
        }
    }
    return result;
}

static AVXRegister256 apply_avx2_pmaskmov_load256(AVXRegister256 mask, AVXRegister256 source, bool is_qword) {
    AVXRegister256 result = {};
    if (is_qword) {
        for (int lane = 0; lane < 4; lane++) {
            if ((get_ymm_pd_lane_bits(mask, lane) & 0x8000000000000000ULL) != 0) {
                set_ymm_pd_lane_bits(&result, lane, get_ymm_pd_lane_bits(source, lane));
            }
        }
        return result;
    }

    for (int lane = 0; lane < 8; lane++) {
        if ((get_ymm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            set_ymm_lane_bits(&result, lane, get_ymm_lane_bits(source, lane));
        }
    }
    return result;
}

static void execute_avx2_pmaskmov_store128(CPU_CONTEXT* ctx, uint64_t address, XMMRegister mask, XMMRegister source, bool is_qword) {
    if (is_qword) {
        if ((mask.low & 0x8000000000000000ULL) != 0) {
            write_memory_qword(ctx, address, source.low);
        }
        if ((mask.high & 0x8000000000000000ULL) != 0) {
            write_memory_qword(ctx, address + 8, source.high);
        }
        return;
    }

    for (int lane = 0; lane < 4; lane++) {
        if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            write_memory_dword(ctx, address + (uint64_t)(lane * 4), get_xmm_lane_bits(source, lane));
        }
    }
}

static void execute_avx2_pmaskmov_store256(CPU_CONTEXT* ctx, uint64_t address, AVXRegister256 mask, AVXRegister256 source, bool is_qword) {
    if (is_qword) {
        for (int lane = 0; lane < 4; lane++) {
            if ((get_ymm_pd_lane_bits(mask, lane) & 0x8000000000000000ULL) != 0) {
                write_memory_qword(ctx, address + (uint64_t)(lane * 8), get_ymm_pd_lane_bits(source, lane));
            }
        }
        return;
    }

    for (int lane = 0; lane < 8; lane++) {
        if ((get_ymm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            write_memory_dword(ctx, address + (uint64_t)(lane * 4), get_ymm_lane_bits(source, lane));
        }
    }
}

static void set_avx_test_flags(CPU_CONTEXT* ctx, bool zf, bool cf) {
    set_flag(ctx, RFLAGS_ZF, zf);
    set_flag(ctx, RFLAGS_CF, cf);
    set_flag(ctx, RFLAGS_OF, false);
    set_flag(ctx, RFLAGS_SF, false);
    set_flag(ctx, RFLAGS_AF, false);
    set_flag(ctx, RFLAGS_PF, false);
}

static bool compute_avx_test_zf128(XMMRegister lhs, XMMRegister rhs) {
    return ((lhs.low & rhs.low) == 0) && ((lhs.high & rhs.high) == 0);
}

static bool compute_avx_test_cf128(XMMRegister lhs, XMMRegister rhs) {
    return ((((~lhs.low) & rhs.low) == 0) && (((~lhs.high) & rhs.high) == 0));
}

static bool compute_avx_test_zf256(AVXRegister256 lhs, AVXRegister256 rhs) {
    return compute_avx_test_zf128(lhs.low, rhs.low) && compute_avx_test_zf128(lhs.high, rhs.high);
}

static bool compute_avx_test_cf256(AVXRegister256 lhs, AVXRegister256 rhs) {
    return compute_avx_test_cf128(lhs.low, rhs.low) && compute_avx_test_cf128(lhs.high, rhs.high);
}

static XMMRegister apply_avx_int_cmp128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_int_cmp_xmm_to_bytes(lhs, lhs_bytes);
    sse2_int_cmp_xmm_to_bytes(rhs, rhs_bytes);

    switch (opcode) {
    case 0x74:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = (lhs_bytes[index] == rhs_bytes[index]) ? 0xFFU : 0x00U;
        }
        break;
    case 0x75:
        for (int index = 0; index < 8; index++) {
            sse2_int_cmp_write_u16(result_bytes, index, sse2_int_cmp_read_u16(lhs_bytes, index) == sse2_int_cmp_read_u16(rhs_bytes, index) ? 0xFFFFU : 0x0000U);
        }
        break;
    case 0x76:
        for (int index = 0; index < 4; index++) {
            sse2_int_cmp_write_u32(result_bytes, index, sse2_int_cmp_read_u32(lhs_bytes, index) == sse2_int_cmp_read_u32(rhs_bytes, index) ? 0xFFFFFFFFU : 0x00000000U);
        }
        break;
    case 0x29:
        for (int index = 0; index < 2; index++) {
            sse2_int_arith_write_u64(result_bytes, index, sse2_int_arith_read_u64(lhs_bytes, index) == sse2_int_arith_read_u64(rhs_bytes, index) ? 0xFFFFFFFFFFFFFFFFULL : 0ULL);
        }
        break;
    case 0x37:
        for (int index = 0; index < 2; index++) {
            int64_t lhs_qword = (int64_t)sse2_int_arith_read_u64(lhs_bytes, index);
            int64_t rhs_qword = (int64_t)sse2_int_arith_read_u64(rhs_bytes, index);
            sse2_int_arith_write_u64(result_bytes, index, lhs_qword > rhs_qword ? 0xFFFFFFFFFFFFFFFFULL : 0ULL);
        }
        break;
    case 0x64:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = ((int8_t)lhs_bytes[index] > (int8_t)rhs_bytes[index]) ? 0xFFU : 0x00U;
        }
        break;
    case 0x65:
        for (int index = 0; index < 8; index++) {
            sse2_int_cmp_write_u16(result_bytes, index, sse2_int_cmp_read_i16(lhs_bytes, index) > sse2_int_cmp_read_i16(rhs_bytes, index) ? 0xFFFFU : 0x0000U);
        }
        break;
    case 0x66:
        for (int index = 0; index < 4; index++) {
            sse2_int_cmp_write_u32(result_bytes, index, sse2_int_cmp_read_i32(lhs_bytes, index) > sse2_int_cmp_read_i32(rhs_bytes, index) ? 0xFFFFFFFFU : 0x00000000U);
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_int_cmp_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx_int_cmp256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_int_cmp128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_int_cmp128(opcode, lhs.high, rhs.high);
    return result;
}

static uint16_t avx_pack_saturate_i32_to_u16(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > 65535) {
        return 65535;
    }
    return (uint16_t)value;
}

static XMMRegister apply_avx_pack128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(lhs, lhs_bytes);
    sse2_pack_xmm_to_bytes(rhs, rhs_bytes);

    switch (opcode) {
    case 0x60:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 1, false, result_bytes);
        break;
    case 0x61:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 2, false, result_bytes);
        break;
    case 0x62:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 4, false, result_bytes);
        break;
    case 0x63:
        for (int index = 0; index < 8; index++) {
            result_bytes[index] = (uint8_t)sse2_pack_saturate_i16_to_i8(sse2_pack_read_i16(lhs_bytes, index));
            result_bytes[8 + index] = (uint8_t)sse2_pack_saturate_i16_to_i8(sse2_pack_read_i16(rhs_bytes, index));
        }
        break;
    case 0x67:
        for (int index = 0; index < 8; index++) {
            result_bytes[index] = sse2_pack_saturate_i16_to_u8(sse2_pack_read_i16(lhs_bytes, index));
            result_bytes[8 + index] = sse2_pack_saturate_i16_to_u8(sse2_pack_read_i16(rhs_bytes, index));
        }
        break;
    case 0x68:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 1, true, result_bytes);
        break;
    case 0x69:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 2, true, result_bytes);
        break;
    case 0x6A:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 4, true, result_bytes);
        break;
    case 0x6B:
        for (int index = 0; index < 4; index++) {
            sse2_pack_write_i16(result_bytes, index, sse2_pack_saturate_i32_to_i16(sse2_pack_read_i32(lhs_bytes, index)));
            sse2_pack_write_i16(result_bytes, 4 + index, sse2_pack_saturate_i32_to_i16(sse2_pack_read_i32(rhs_bytes, index)));
        }
        break;
    case 0x2B:
        for (int index = 0; index < 4; index++) {
            sse2_int_arith_write_u16(result_bytes, index, avx_pack_saturate_i32_to_u16(sse2_pack_read_i32(lhs_bytes, index)));
            sse2_int_arith_write_u16(result_bytes, 4 + index, avx_pack_saturate_i32_to_u16(sse2_pack_read_i32(rhs_bytes, index)));
        }
        break;
    case 0x6C:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 8, false, result_bytes);
        break;
    case 0x6D:
        sse2_pack_interleave(lhs_bytes, rhs_bytes, 8, true, result_bytes);
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx_pack256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_pack128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_pack128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_pshufd128(XMMRegister src, uint8_t imm8) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        set_sse_shuffle_lane_bits(&result, lane, get_sse_shuffle_lane_bits(src, (imm8 >> (lane * 2)) & 0x03));
    }
    return result;
}

static AVXRegister256 apply_avx_pshufd256(AVXRegister256 src, uint8_t imm8) {
    AVXRegister256 result = {};
    result.low = apply_avx_pshufd128(src.low, imm8);
    result.high = apply_avx_pshufd128(src.high, imm8);
    return result;
}

static AVXRegister256 apply_avx_pshufhw256(AVXRegister256 src, uint8_t imm8) {
    AVXRegister256 result = {};
    result.low = apply_sse2_pshufhw128(src.low, imm8);
    result.high = apply_sse2_pshufhw128(src.high, imm8);
    return result;
}

static AVXRegister256 apply_avx_pshuflw256(AVXRegister256 src, uint8_t imm8) {
    AVXRegister256 result = {};
    result.low = apply_sse2_pshuflw128(src.low, imm8);
    result.high = apply_sse2_pshuflw128(src.high, imm8);
    return result;
}

static XMMRegister apply_avx_int_minmax128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(lhs, lhs_bytes);
    sse2_pack_xmm_to_bytes(rhs, rhs_bytes);

    switch (opcode) {
    case 0xDA:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = (lhs_bytes[index] < rhs_bytes[index]) ? lhs_bytes[index] : rhs_bytes[index];
        }
        break;
    case 0xDE:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = (lhs_bytes[index] > rhs_bytes[index]) ? lhs_bytes[index] : rhs_bytes[index];
        }
        break;
    case 0xEA:
        for (int index = 0; index < 8; index++) {
            int16_t lhs_word = sse2_pack_read_i16(lhs_bytes, index);
            int16_t rhs_word = sse2_pack_read_i16(rhs_bytes, index);
            sse2_pack_write_i16(result_bytes, index, lhs_word < rhs_word ? lhs_word : rhs_word);
        }
        break;
    case 0xEE:
        for (int index = 0; index < 8; index++) {
            int16_t lhs_word = sse2_pack_read_i16(lhs_bytes, index);
            int16_t rhs_word = sse2_pack_read_i16(rhs_bytes, index);
            sse2_pack_write_i16(result_bytes, index, lhs_word > rhs_word ? lhs_word : rhs_word);
        }
        break;
    case 0x38:
        for (int index = 0; index < 16; index++) {
            int8_t lhs_byte = (int8_t)lhs_bytes[index];
            int8_t rhs_byte = (int8_t)rhs_bytes[index];
            result_bytes[index] = (uint8_t)(lhs_byte < rhs_byte ? lhs_byte : rhs_byte);
        }
        break;
    case 0x39:
        for (int index = 0; index < 4; index++) {
            int32_t lhs_dword = sse2_pack_read_i32(lhs_bytes, index);
            int32_t rhs_dword = sse2_pack_read_i32(rhs_bytes, index);
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)(lhs_dword < rhs_dword ? lhs_dword : rhs_dword));
        }
        break;
    case 0x3A:
        for (int index = 0; index < 8; index++) {
            uint16_t lhs_word = sse2_int_arith_read_u16(lhs_bytes, index);
            uint16_t rhs_word = sse2_int_arith_read_u16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, lhs_word < rhs_word ? lhs_word : rhs_word);
        }
        break;
    case 0x3B:
        for (int index = 0; index < 4; index++) {
            uint32_t lhs_dword = sse2_int_arith_read_u32(lhs_bytes, index);
            uint32_t rhs_dword = sse2_int_arith_read_u32(rhs_bytes, index);
            sse2_int_arith_write_u32(result_bytes, index, lhs_dword < rhs_dword ? lhs_dword : rhs_dword);
        }
        break;
    case 0x3C:
        for (int index = 0; index < 16; index++) {
            int8_t lhs_byte = (int8_t)lhs_bytes[index];
            int8_t rhs_byte = (int8_t)rhs_bytes[index];
            result_bytes[index] = (uint8_t)(lhs_byte > rhs_byte ? lhs_byte : rhs_byte);
        }
        break;
    case 0x3D:
        for (int index = 0; index < 4; index++) {
            int32_t lhs_dword = sse2_pack_read_i32(lhs_bytes, index);
            int32_t rhs_dword = sse2_pack_read_i32(rhs_bytes, index);
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)(lhs_dword > rhs_dword ? lhs_dword : rhs_dword));
        }
        break;
    case 0x3E:
        for (int index = 0; index < 8; index++) {
            uint16_t lhs_word = sse2_int_arith_read_u16(lhs_bytes, index);
            uint16_t rhs_word = sse2_int_arith_read_u16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, lhs_word > rhs_word ? lhs_word : rhs_word);
        }
        break;
    case 0x3F:
        for (int index = 0; index < 4; index++) {
            uint32_t lhs_dword = sse2_int_arith_read_u32(lhs_bytes, index);
            uint32_t rhs_dword = sse2_int_arith_read_u32(rhs_bytes, index);
            sse2_int_arith_write_u32(result_bytes, index, lhs_dword > rhs_dword ? lhs_dword : rhs_dword);
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx_int_minmax256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_int_minmax128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_int_minmax128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx2_sign128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(lhs, lhs_bytes);
    sse2_pack_xmm_to_bytes(rhs, rhs_bytes);

    switch (opcode) {
    case 0x08:
        for (int index = 0; index < 16; index++) {
            int8_t value = (int8_t)lhs_bytes[index];
            int8_t sign = (int8_t)rhs_bytes[index];
            int8_t result = sign == 0 ? 0 : (sign < 0 ? (int8_t)(-value) : value);
            result_bytes[index] = (uint8_t)result;
        }
        break;
    case 0x09:
        for (int index = 0; index < 8; index++) {
            int16_t value = sse2_int_arith_read_i16(lhs_bytes, index);
            int16_t sign = sse2_int_arith_read_i16(rhs_bytes, index);
            int16_t result = sign == 0 ? 0 : (sign < 0 ? (int16_t)(-value) : value);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)result);
        }
        break;
    case 0x0A:
        for (int index = 0; index < 4; index++) {
            int32_t value = sse2_pack_read_i32(lhs_bytes, index);
            int32_t sign = sse2_pack_read_i32(rhs_bytes, index);
            int32_t result = sign == 0 ? 0 : (sign < 0 ? -value : value);
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)result);
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx2_sign256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx2_sign128(opcode, lhs.low, rhs.low);
    result.high = apply_avx2_sign128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx2_abs128(uint8_t opcode, XMMRegister value) {
    uint8_t src_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(value, src_bytes);

    switch (opcode) {
    case 0x1C:
        for (int index = 0; index < 16; index++) {
            int8_t element = (int8_t)src_bytes[index];
            result_bytes[index] = (uint8_t)(element < 0 ? (int8_t)(-element) : element);
        }
        break;
    case 0x1D:
        for (int index = 0; index < 8; index++) {
            int16_t element = sse2_int_arith_read_i16(src_bytes, index);
            int16_t result = element < 0 ? (int16_t)(-element) : element;
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)result);
        }
        break;
    case 0x1E:
        for (int index = 0; index < 4; index++) {
            int32_t element = sse2_pack_read_i32(src_bytes, index);
            int32_t result = element < 0 ? -element : element;
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)result);
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx2_abs256(uint8_t opcode, AVXRegister256 value) {
    AVXRegister256 result = {};
    result.low = apply_avx2_abs128(opcode, value.low);
    result.high = apply_avx2_abs128(opcode, value.high);
    return result;
}

static int avx2_pmov_source_size(uint8_t opcode, bool is_256) {
    switch (opcode & 0x0F) {
    case 0x00:
    case 0x03:
    case 0x05:
        return is_256 ? 16 : 8;
    case 0x01:
    case 0x04:
        return is_256 ? 8 : 4;
    case 0x02:
        return is_256 ? 4 : 2;
    default:
        raise_ud();
        return 0;
    }
}

static XMMRegister read_avx2_pmov_source(CPU_CONTEXT* ctx, const DecodedInstruction* inst, uint8_t opcode, bool is_256) {
    XMMRegister source = {};
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 0x03) {
        return get_xmm128(ctx, avx_vex_rm_index(ctx, inst->modrm));
    }

    switch (avx2_pmov_source_size(opcode, is_256)) {
    case 2:
        source.low = read_memory_word(ctx, inst->mem_address);
        break;
    case 4:
        source.low = read_memory_dword(ctx, inst->mem_address);
        break;
    case 8:
        source.low = read_memory_qword(ctx, inst->mem_address);
        break;
    case 16:
        source = read_xmm_memory(ctx, inst->mem_address);
        break;
    default:
        raise_ud();
        break;
    }
    return source;
}

static int avx_vsib_index_register(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    if (!inst->has_sib) {
        raise_ud();
    }

    int index = (inst->sib >> 3) & 0x07;
    if (ctx->rex_x && inst->address_size == 64) {
        index |= 0x08;
    }
    return index;
}

static uint64_t avx_vsib_base_address(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    if (!inst->has_sib) {
        raise_ud();
    }

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 0x03) {
        raise_ud();
    }

    uint8_t raw_base = inst->sib & 0x07;
    bool no_base = mod == 0x00 && raw_base == 0x05 && !(ctx->rex_b && inst->address_size == 64);
    uint64_t address = 0;

    if (no_base) {
        if (inst->address_size == 64) {
            address = (uint64_t)(int64_t)inst->displacement;
        }
        else {
            address = (uint32_t)inst->displacement;
        }
    }
    else {
        int base = raw_base;
        if (ctx->rex_b && inst->address_size == 64) {
            base |= 0x08;
        }
        address = ctx->regs[base];
        if (inst->address_size == 32) {
            address = (uint32_t)address;
        }
    }

    if (mod == 0x01) {
        address += (int8_t)(inst->displacement & 0xFF);
    }
    else if (mod == 0x02) {
        address += (int32_t)inst->displacement;
    }

    if (inst->address_size == 32) {
        address &= 0xFFFFFFFFULL;
    }
    return address;
}

static uint64_t avx_vsib_element_address(uint64_t base_address, uint8_t sib, int64_t index, int address_size) {
    int64_t scale = 1LL << ((sib >> 6) & 0x03);
    uint64_t address = base_address + (uint64_t)(index * scale);
    if (address_size == 32) {
        address &= 0xFFFFFFFFULL;
    }
    return address;
}

static void validate_avx_gather_operands(CPU_CONTEXT* ctx, const AVXVexPrefix* prefix, const DecodedInstruction* inst) {
    uint8_t mod = (inst->modrm >> 6) & 0x03;
    if (mod == 0x03 || !inst->has_sib) {
        raise_ud();
    }

    int dest = avx_vex_dest_index(ctx, inst->modrm);
    int mask = avx_vex_source1_index(prefix);
    int index = avx_vsib_index_register(ctx, inst);
    if (dest == mask || dest == index || mask == index) {
        raise_ud();
    }
}

static void execute_avx_gather_dps(CPU_CONTEXT* ctx, const AVXVexPrefix* prefix, const DecodedInstruction* inst, bool is_256) {
    validate_avx_gather_operands(ctx, prefix, inst);

    int dest = avx_vex_dest_index(ctx, inst->modrm);
    int mask_reg = avx_vex_source1_index(prefix);
    int index_reg = avx_vsib_index_register(ctx, inst);
    uint64_t base_address = avx_vsib_base_address(ctx, inst);

    if (is_256) {
        AVXRegister256 result = get_ymm256(ctx, dest);
        AVXRegister256 mask = get_ymm256(ctx, mask_reg);
        AVXRegister256 indices = get_ymm256(ctx, index_reg);
        for (int lane = 0; lane < 8; lane++) {
            if ((get_ymm_lane_bits(mask, lane) & 0x80000000U) != 0) {
                uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)(int32_t)get_ymm_lane_bits(indices, lane), inst->address_size);
                set_ymm_lane_bits(&result, lane, read_memory_dword(ctx, address));
            }
        }
        set_ymm256(ctx, dest, result);
        set_ymm256(ctx, mask_reg, {});
        return;
    }

    XMMRegister result = get_xmm128(ctx, dest);
    XMMRegister mask = get_xmm128(ctx, mask_reg);
    XMMRegister indices = get_xmm128(ctx, index_reg);
    for (int lane = 0; lane < 4; lane++) {
        if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
            uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)(int32_t)get_xmm_lane_bits(indices, lane), inst->address_size);
            set_xmm_lane_bits(&result, lane, read_memory_dword(ctx, address));
        }
    }
    set_xmm128(ctx, dest, result);
    clear_ymm_upper128(ctx, dest);
    set_xmm128(ctx, mask_reg, {});
    clear_ymm_upper128(ctx, mask_reg);
}

static void execute_avx_gather_dpd(CPU_CONTEXT* ctx, const AVXVexPrefix* prefix, const DecodedInstruction* inst, bool is_256) {
    validate_avx_gather_operands(ctx, prefix, inst);

    int dest = avx_vex_dest_index(ctx, inst->modrm);
    int mask_reg = avx_vex_source1_index(prefix);
    int index_reg = avx_vsib_index_register(ctx, inst);
    uint64_t base_address = avx_vsib_base_address(ctx, inst);

    if (is_256) {
        AVXRegister256 result = get_ymm256(ctx, dest);
        AVXRegister256 mask = get_ymm256(ctx, mask_reg);
        XMMRegister indices = get_xmm128(ctx, index_reg);
        for (int lane = 0; lane < 4; lane++) {
            if ((get_ymm_pd_lane_bits(mask, lane) >> 63) != 0) {
                uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)(int32_t)get_xmm_lane_bits(indices, lane), inst->address_size);
                set_ymm_pd_lane_bits(&result, lane, read_memory_qword(ctx, address));
            }
        }
        set_ymm256(ctx, dest, result);
        set_ymm256(ctx, mask_reg, {});
        return;
    }

    XMMRegister result = get_xmm128(ctx, dest);
    XMMRegister mask = get_xmm128(ctx, mask_reg);
    XMMRegister indices = get_xmm128(ctx, index_reg);
    for (int lane = 0; lane < 2; lane++) {
        if ((get_sse2_arith_pd_lane_bits(mask, lane) >> 63) != 0) {
            uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)(int32_t)get_xmm_lane_bits(indices, lane), inst->address_size);
            set_sse2_arith_pd_lane_bits(&result, lane, read_memory_qword(ctx, address));
        }
    }
    set_xmm128(ctx, dest, result);
    clear_ymm_upper128(ctx, dest);
    set_xmm128(ctx, mask_reg, {});
    clear_ymm_upper128(ctx, mask_reg);
}

static void execute_avx_gather_qps(CPU_CONTEXT* ctx, const AVXVexPrefix* prefix, const DecodedInstruction* inst, bool is_256) {
    validate_avx_gather_operands(ctx, prefix, inst);

    int dest = avx_vex_dest_index(ctx, inst->modrm);
    int mask_reg = avx_vex_source1_index(prefix);
    int index_reg = avx_vsib_index_register(ctx, inst);
    uint64_t base_address = avx_vsib_base_address(ctx, inst);

    XMMRegister result = get_xmm128(ctx, dest);
    XMMRegister mask = get_xmm128(ctx, mask_reg);
    int active_lanes = is_256 ? 4 : 2;

    if (!is_256) {
        set_xmm_lane_bits(&result, 2, 0);
        set_xmm_lane_bits(&result, 3, 0);
    }

    if (is_256) {
        AVXRegister256 indices = get_ymm256(ctx, index_reg);
        for (int lane = 0; lane < active_lanes; lane++) {
            if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
                uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)get_ymm_pd_lane_bits(indices, lane), inst->address_size);
                set_xmm_lane_bits(&result, lane, read_memory_dword(ctx, address));
            }
        }
    }
    else {
        XMMRegister indices = get_xmm128(ctx, index_reg);
        for (int lane = 0; lane < active_lanes; lane++) {
            if ((get_xmm_lane_bits(mask, lane) & 0x80000000U) != 0) {
                uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)get_sse2_arith_pd_lane_bits(indices, lane), inst->address_size);
                set_xmm_lane_bits(&result, lane, read_memory_dword(ctx, address));
            }
        }
    }

    set_xmm128(ctx, dest, result);
    clear_ymm_upper128(ctx, dest);
    set_xmm128(ctx, mask_reg, {});
    clear_ymm_upper128(ctx, mask_reg);
}

static void execute_avx_gather_qpd(CPU_CONTEXT* ctx, const AVXVexPrefix* prefix, const DecodedInstruction* inst, bool is_256) {
    validate_avx_gather_operands(ctx, prefix, inst);

    int dest = avx_vex_dest_index(ctx, inst->modrm);
    int mask_reg = avx_vex_source1_index(prefix);
    int index_reg = avx_vsib_index_register(ctx, inst);
    uint64_t base_address = avx_vsib_base_address(ctx, inst);

    if (is_256) {
        AVXRegister256 result = get_ymm256(ctx, dest);
        AVXRegister256 mask = get_ymm256(ctx, mask_reg);
        AVXRegister256 indices = get_ymm256(ctx, index_reg);
        for (int lane = 0; lane < 4; lane++) {
            if ((get_ymm_pd_lane_bits(mask, lane) >> 63) != 0) {
                uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)get_ymm_pd_lane_bits(indices, lane), inst->address_size);
                set_ymm_pd_lane_bits(&result, lane, read_memory_qword(ctx, address));
            }
        }
        set_ymm256(ctx, dest, result);
        set_ymm256(ctx, mask_reg, {});
        return;
    }

    XMMRegister result = get_xmm128(ctx, dest);
    XMMRegister mask = get_xmm128(ctx, mask_reg);
    XMMRegister indices = get_xmm128(ctx, index_reg);
    for (int lane = 0; lane < 2; lane++) {
        if ((get_sse2_arith_pd_lane_bits(mask, lane) >> 63) != 0) {
            uint64_t address = avx_vsib_element_address(base_address, inst->sib, (int64_t)get_sse2_arith_pd_lane_bits(indices, lane), inst->address_size);
            set_sse2_arith_pd_lane_bits(&result, lane, read_memory_qword(ctx, address));
        }
    }
    set_xmm128(ctx, dest, result);
    clear_ymm_upper128(ctx, dest);
    set_xmm128(ctx, mask_reg, {});
    clear_ymm_upper128(ctx, mask_reg);
}

static void avx_write_u16_32(uint8_t bytes[32], int index, uint16_t value) {
    int offset = index * 2;
    bytes[offset] = (uint8_t)(value & 0xFFU);
    bytes[offset + 1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void avx_write_u32_32(uint8_t bytes[32], int index, uint32_t value) {
    int offset = index * 4;
    for (int byte_index = 0; byte_index < 4; byte_index++) {
        bytes[offset + byte_index] = (uint8_t)((value >> (byte_index * 8)) & 0xFFU);
    }
}

static void avx_write_u64_32(uint8_t bytes[32], int index, uint64_t value) {
    int offset = index * 8;
    for (int byte_index = 0; byte_index < 8; byte_index++) {
        bytes[offset + byte_index] = (uint8_t)((value >> (byte_index * 8)) & 0xFFU);
    }
}

static XMMRegister apply_avx2_pmovx128(uint8_t opcode, XMMRegister source) {
    uint8_t src_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    bool zero_extend = opcode >= 0x30;
    sse2_pack_xmm_to_bytes(source, src_bytes);

    switch (opcode) {
    case 0x20:
    case 0x30:
        for (int index = 0; index < 8; index++) {
            int16_t value = zero_extend ? (int16_t)src_bytes[index] : (int16_t)(int8_t)src_bytes[index];
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)value);
        }
        break;
    case 0x21:
    case 0x31:
        for (int index = 0; index < 4; index++) {
            int32_t value = zero_extend ? (int32_t)src_bytes[index] : (int32_t)(int8_t)src_bytes[index];
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)value);
        }
        break;
    case 0x22:
    case 0x32:
        for (int index = 0; index < 2; index++) {
            int64_t value = zero_extend ? (int64_t)src_bytes[index] : (int64_t)(int8_t)src_bytes[index];
            sse2_int_arith_write_u64(result_bytes, index, (uint64_t)value);
        }
        break;
    case 0x23:
    case 0x33:
        for (int index = 0; index < 4; index++) {
            int32_t value = zero_extend ? (int32_t)sse2_int_arith_read_u16(src_bytes, index) : (int32_t)sse2_int_arith_read_i16(src_bytes, index);
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)value);
        }
        break;
    case 0x24:
    case 0x34:
        for (int index = 0; index < 2; index++) {
            int64_t value = zero_extend ? (int64_t)sse2_int_arith_read_u16(src_bytes, index) : (int64_t)sse2_int_arith_read_i16(src_bytes, index);
            sse2_int_arith_write_u64(result_bytes, index, (uint64_t)value);
        }
        break;
    case 0x25:
    case 0x35:
        for (int index = 0; index < 2; index++) {
            int64_t value = zero_extend ? (int64_t)sse2_int_arith_read_u32(src_bytes, index) : (int64_t)(int32_t)sse2_int_arith_read_u32(src_bytes, index);
            sse2_int_arith_write_u64(result_bytes, index, (uint64_t)value);
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx2_pmovx256(uint8_t opcode, XMMRegister source) {
    uint8_t src_bytes[16] = {};
    uint8_t result_bytes[32] = {};
    bool zero_extend = opcode >= 0x30;
    sse2_pack_xmm_to_bytes(source, src_bytes);

    switch (opcode) {
    case 0x20:
    case 0x30:
        for (int index = 0; index < 16; index++) {
            int16_t value = zero_extend ? (int16_t)src_bytes[index] : (int16_t)(int8_t)src_bytes[index];
            avx_write_u16_32(result_bytes, index, (uint16_t)value);
        }
        break;
    case 0x21:
    case 0x31:
        for (int index = 0; index < 8; index++) {
            int32_t value = zero_extend ? (int32_t)src_bytes[index] : (int32_t)(int8_t)src_bytes[index];
            avx_write_u32_32(result_bytes, index, (uint32_t)value);
        }
        break;
    case 0x22:
    case 0x32:
        for (int index = 0; index < 4; index++) {
            int64_t value = zero_extend ? (int64_t)src_bytes[index] : (int64_t)(int8_t)src_bytes[index];
            avx_write_u64_32(result_bytes, index, (uint64_t)value);
        }
        break;
    case 0x23:
    case 0x33:
        for (int index = 0; index < 8; index++) {
            int32_t value = zero_extend ? (int32_t)sse2_int_arith_read_u16(src_bytes, index) : (int32_t)sse2_int_arith_read_i16(src_bytes, index);
            avx_write_u32_32(result_bytes, index, (uint32_t)value);
        }
        break;
    case 0x24:
    case 0x34:
        for (int index = 0; index < 4; index++) {
            int64_t value = zero_extend ? (int64_t)sse2_int_arith_read_u16(src_bytes, index) : (int64_t)sse2_int_arith_read_i16(src_bytes, index);
            avx_write_u64_32(result_bytes, index, (uint64_t)value);
        }
        break;
    case 0x25:
    case 0x35:
        for (int index = 0; index < 4; index++) {
            int64_t value = zero_extend ? (int64_t)sse2_int_arith_read_u32(src_bytes, index) : (int64_t)(int32_t)sse2_int_arith_read_u32(src_bytes, index);
            avx_write_u64_32(result_bytes, index, (uint64_t)value);
        }
        break;
    default:
        raise_ud();
        break;
    }

    AVXRegister256 result = {};
    result.low = sse2_pack_bytes_to_xmm(result_bytes);
    result.high = sse2_pack_bytes_to_xmm(result_bytes + 16);
    return result;
}

static XMMRegister apply_avx2_horizontal128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_pack_xmm_to_bytes(lhs, lhs_bytes);
    sse2_pack_xmm_to_bytes(rhs, rhs_bytes);

    switch (opcode) {
    case 0x01:
    case 0x05:
    case 0x03:
    case 0x07:
        for (int block = 0; block < 2; block++) {
            const uint8_t* source_bytes = block == 0 ? lhs_bytes : rhs_bytes;
            for (int index = 0; index < 4; index++) {
                int16_t first = sse2_pack_read_i16(source_bytes, index * 2);
                int16_t second = sse2_pack_read_i16(source_bytes, index * 2 + 1);
                int32_t value = (opcode == 0x05 || opcode == 0x07)
                    ? (int32_t)first - (int32_t)second
                    : (int32_t)first + (int32_t)second;
                if (opcode == 0x03 || opcode == 0x07) {
                    sse2_pack_write_i16(result_bytes, block * 4 + index, sse2_pack_saturate_i32_to_i16(value));
                }
                else {
                    sse2_pack_write_i16(result_bytes, block * 4 + index, (int16_t)value);
                }
            }
        }
        break;
    case 0x02:
    case 0x06:
        for (int block = 0; block < 2; block++) {
            const uint8_t* source_bytes = block == 0 ? lhs_bytes : rhs_bytes;
            for (int index = 0; index < 2; index++) {
                int32_t first = sse2_pack_read_i32(source_bytes, index * 2);
                int32_t second = sse2_pack_read_i32(source_bytes, index * 2 + 1);
                uint32_t value = (uint32_t)((opcode == 0x06) ? (first - second) : (first + second));
                sse2_int_arith_write_u32(result_bytes, block * 2 + index, value);
            }
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_pack_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx2_horizontal256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx2_horizontal128(opcode, lhs.low, rhs.low);
    result.high = apply_avx2_horizontal128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_int_arith128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_int_arith_xmm_to_bytes(lhs, lhs_bytes);
    sse2_int_arith_xmm_to_bytes(rhs, rhs_bytes);

    switch (opcode) {
    case 0xFC:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = (uint8_t)(lhs_bytes[index] + rhs_bytes[index]);
        }
        break;
    case 0xFD:
        for (int index = 0; index < 8; index++) {
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)(sse2_int_arith_read_u16(lhs_bytes, index) + sse2_int_arith_read_u16(rhs_bytes, index)));
        }
        break;
    case 0xFE:
        for (int index = 0; index < 4; index++) {
            sse2_int_arith_write_u32(result_bytes, index, sse2_int_arith_read_u32(lhs_bytes, index) + sse2_int_arith_read_u32(rhs_bytes, index));
        }
        break;
    case 0xD4:
        for (int index = 0; index < 2; index++) {
            sse2_int_arith_write_u64(result_bytes, index, sse2_int_arith_read_u64(lhs_bytes, index) + sse2_int_arith_read_u64(rhs_bytes, index));
        }
        break;
    case 0xDC:
        for (int index = 0; index < 16; index++) {
            uint16_t sum = (uint16_t)lhs_bytes[index] + (uint16_t)rhs_bytes[index];
            result_bytes[index] = (sum > 0xFFU) ? 0xFFU : (uint8_t)sum;
        }
        break;
    case 0xDD:
        for (int index = 0; index < 8; index++) {
            uint32_t sum = (uint32_t)sse2_int_arith_read_u16(lhs_bytes, index) + (uint32_t)sse2_int_arith_read_u16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)(sum > 0xFFFFU ? 0xFFFFU : sum));
        }
        break;
    case 0xE0:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = sse2_int_arith_average_u8(lhs_bytes[index], rhs_bytes[index]);
        }
        break;
    case 0xE3:
        for (int index = 0; index < 8; index++) {
            sse2_int_arith_write_u16(result_bytes, index, sse2_int_arith_average_u16(sse2_int_arith_read_u16(lhs_bytes, index), sse2_int_arith_read_u16(rhs_bytes, index)));
        }
        break;
    case 0xEC:
        for (int index = 0; index < 16; index++) {
            int16_t sum = (int16_t)(int8_t)lhs_bytes[index] + (int16_t)(int8_t)rhs_bytes[index];
            result_bytes[index] = (uint8_t)sse2_int_arith_saturate_i16_to_i8(sum);
        }
        break;
    case 0xED:
        for (int index = 0; index < 8; index++) {
            int32_t sum = (int32_t)sse2_int_arith_read_i16(lhs_bytes, index) + (int32_t)sse2_int_arith_read_i16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)sse2_int_arith_saturate_i32_to_i16(sum));
        }
        break;
    case 0xF8:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = (uint8_t)(lhs_bytes[index] - rhs_bytes[index]);
        }
        break;
    case 0xF9:
        for (int index = 0; index < 8; index++) {
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)(sse2_int_arith_read_u16(lhs_bytes, index) - sse2_int_arith_read_u16(rhs_bytes, index)));
        }
        break;
    case 0xFA:
        for (int index = 0; index < 4; index++) {
            sse2_int_arith_write_u32(result_bytes, index, sse2_int_arith_read_u32(lhs_bytes, index) - sse2_int_arith_read_u32(rhs_bytes, index));
        }
        break;
    case 0xFB:
        for (int index = 0; index < 2; index++) {
            sse2_int_arith_write_u64(result_bytes, index, sse2_int_arith_read_u64(lhs_bytes, index) - sse2_int_arith_read_u64(rhs_bytes, index));
        }
        break;
    case 0xD8:
        for (int index = 0; index < 16; index++) {
            result_bytes[index] = (lhs_bytes[index] < rhs_bytes[index]) ? 0 : (uint8_t)(lhs_bytes[index] - rhs_bytes[index]);
        }
        break;
    case 0xD9:
        for (int index = 0; index < 8; index++) {
            uint16_t lhs = sse2_int_arith_read_u16(lhs_bytes, index);
            uint16_t rhs = sse2_int_arith_read_u16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, lhs < rhs ? 0 : (uint16_t)(lhs - rhs));
        }
        break;
    case 0xE8:
        for (int index = 0; index < 16; index++) {
            int16_t diff = (int16_t)(int8_t)lhs_bytes[index] - (int16_t)(int8_t)rhs_bytes[index];
            result_bytes[index] = (uint8_t)sse2_int_arith_saturate_i16_to_i8(diff);
        }
        break;
    case 0xE9:
        for (int index = 0; index < 8; index++) {
            int32_t diff = (int32_t)sse2_int_arith_read_i16(lhs_bytes, index) - (int32_t)sse2_int_arith_read_i16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)sse2_int_arith_saturate_i32_to_i16(diff));
        }
        break;
    case 0x04:
        for (int index = 0; index < 8; index++) {
            int base = index * 2;
            int32_t sum = (int32_t)((uint32_t)lhs_bytes[base] * (int32_t)(int8_t)rhs_bytes[base])
                        + (int32_t)((uint32_t)lhs_bytes[base + 1] * (int32_t)(int8_t)rhs_bytes[base + 1]);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)sse2_int_arith_saturate_i32_to_i16(sum));
        }
        break;
    case 0x0B:
        for (int index = 0; index < 8; index++) {
            int32_t product = (int32_t)sse2_int_arith_read_i16(lhs_bytes, index) * (int32_t)sse2_int_arith_read_i16(rhs_bytes, index);
            int32_t rounded = (product + 0x4000) >> 15;
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)sse2_int_arith_saturate_i32_to_i16(rounded));
        }
        break;
    case 0x28:
        for (int index = 0; index < 2; index++) {
            int lane = index * 2;
            int64_t product = (int64_t)(int32_t)sse2_int_arith_read_u32(lhs_bytes, lane) * (int64_t)(int32_t)sse2_int_arith_read_u32(rhs_bytes, lane);
            sse2_int_arith_write_u64(result_bytes, index, (uint64_t)product);
        }
        break;
    case 0x40:
        for (int index = 0; index < 4; index++) {
            int64_t product = (int64_t)(int32_t)sse2_int_arith_read_u32(lhs_bytes, index) * (int64_t)(int32_t)sse2_int_arith_read_u32(rhs_bytes, index);
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)product);
        }
        break;
    case 0xD5:
        for (int index = 0; index < 8; index++) {
            int32_t product = (int32_t)sse2_int_arith_read_i16(lhs_bytes, index) * (int32_t)sse2_int_arith_read_i16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)product);
        }
        break;
    case 0xE4:
        for (int index = 0; index < 8; index++) {
            uint32_t product = (uint32_t)sse2_int_arith_read_u16(lhs_bytes, index) * (uint32_t)sse2_int_arith_read_u16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)(product >> 16));
        }
        break;
    case 0xE5:
        for (int index = 0; index < 8; index++) {
            int32_t product = (int32_t)sse2_int_arith_read_i16(lhs_bytes, index) * (int32_t)sse2_int_arith_read_i16(rhs_bytes, index);
            sse2_int_arith_write_u16(result_bytes, index, (uint16_t)(product >> 16));
        }
        break;
    case 0xF6:
        for (int block = 0; block < 2; block++) {
            uint64_t sum = 0;
            for (int index = 0; index < 8; index++) {
                int offset = block * 8 + index;
                sum += (uint64_t)((lhs_bytes[offset] > rhs_bytes[offset]) ? (lhs_bytes[offset] - rhs_bytes[offset]) : (rhs_bytes[offset] - lhs_bytes[offset]));
            }
            sse2_int_arith_write_u64(result_bytes, block, sum);
        }
        break;
    case 0xF4: {
        uint64_t product0 = (uint64_t)sse2_int_arith_read_u32(lhs_bytes, 0) * (uint64_t)sse2_int_arith_read_u32(rhs_bytes, 0);
        uint64_t product1 = (uint64_t)sse2_int_arith_read_u32(lhs_bytes, 2) * (uint64_t)sse2_int_arith_read_u32(rhs_bytes, 2);
        sse2_int_arith_write_u64(result_bytes, 0, product0);
        sse2_int_arith_write_u64(result_bytes, 1, product1);
        break;
    }
    case 0xF5:
        for (int index = 0; index < 4; index++) {
            int base = index * 2;
            int64_t sum = (int64_t)sse2_int_arith_read_i16(lhs_bytes, base) * (int64_t)sse2_int_arith_read_i16(rhs_bytes, base)
                        + (int64_t)sse2_int_arith_read_i16(lhs_bytes, base + 1) * (int64_t)sse2_int_arith_read_i16(rhs_bytes, base + 1);
            sse2_int_arith_write_u32(result_bytes, index, (uint32_t)sum);
        }
        break;
    default:
        raise_ud();
        break;
    }

    return sse2_int_arith_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx_int_arith256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_int_arith128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_int_arith128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_mpsadbw128(XMMRegister lhs, XMMRegister rhs, uint8_t imm8) {
    uint8_t lhs_bytes[16] = {};
    uint8_t rhs_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    int rhs_offset = (int)(imm8 & 0x03) * 4;
    int lhs_offset = (int)((imm8 >> 2) & 0x01) * 4;

    sse2_int_arith_xmm_to_bytes(lhs, lhs_bytes);
    sse2_int_arith_xmm_to_bytes(rhs, rhs_bytes);

    for (int index = 0; index < 8; index++) {
        uint16_t sum = 0;
        for (int element = 0; element < 4; element++) {
            int lhs_byte = lhs_bytes[lhs_offset + index + element];
            int rhs_byte = rhs_bytes[rhs_offset + element];
            sum = (uint16_t)(sum + (uint16_t)(lhs_byte > rhs_byte ? (lhs_byte - rhs_byte) : (rhs_byte - lhs_byte)));
        }
        sse2_int_arith_write_u16(result_bytes, index, sum);
    }

    return sse2_int_arith_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx_mpsadbw256(AVXRegister256 lhs, AVXRegister256 rhs, uint8_t imm8) {
    uint8_t lhs_bytes[32] = {};
    uint8_t rhs_bytes[32] = {};
    uint8_t result_bytes[32] = {};
    sse2_int_arith_xmm_to_bytes(lhs.low, lhs_bytes);
    sse2_int_arith_xmm_to_bytes(lhs.high, lhs_bytes + 16);
    sse2_int_arith_xmm_to_bytes(rhs.low, rhs_bytes);
    sse2_int_arith_xmm_to_bytes(rhs.high, rhs_bytes + 16);

    for (int lane = 0; lane < 2; lane++) {
        int lane_base = lane * 16;
        int rhs_offset = lane == 0
            ? (int)(imm8 & 0x03) * 4
            : 16 + (int)((imm8 >> 3) & 0x03) * 4;
        int lhs_offset = lane == 0
            ? (int)((imm8 >> 2) & 0x01) * 4
            : 16 + (int)((imm8 >> 5) & 0x01) * 4;

        for (int index = 0; index < 8; index++) {
            uint16_t sum = 0;
            for (int element = 0; element < 4; element++) {
                int lhs_byte = lhs_bytes[lhs_offset + index + element];
                int rhs_byte = rhs_bytes[rhs_offset + element];
                sum = (uint16_t)(sum + (uint16_t)(lhs_byte > rhs_byte ? (lhs_byte - rhs_byte) : (rhs_byte - lhs_byte)));
            }
            avx_write_u16_32(result_bytes, lane * 8 + index, sum);
        }
    }

    AVXRegister256 result = {};
    result.low = sse2_int_arith_bytes_to_xmm(result_bytes);
    result.high = sse2_int_arith_bytes_to_xmm(result_bytes + 16);
    return result;
}

static XMMRegister apply_avx_shift128(uint8_t opcode, uint8_t group, XMMRegister src, uint64_t count) {
    uint8_t src_bytes[16] = {};
    uint8_t result_bytes[16] = {};
    sse2_shift_xmm_to_bytes(src, src_bytes);

    if (opcode == 0x71 || opcode == 0x72 || opcode == 0x73) {
        if (opcode == 0x71) {
            if (group == 2) {
                sse2_shift_apply_psrlw(src_bytes, count, result_bytes);
            }
            else if (group == 4) {
                sse2_shift_apply_psraw(src_bytes, count, result_bytes);
            }
            else if (group == 6) {
                sse2_shift_apply_psllw(src_bytes, count, result_bytes);
            }
            else {
                raise_ud();
            }
        }
        else if (opcode == 0x72) {
            if (group == 2) {
                sse2_shift_apply_psrld(src_bytes, count, result_bytes);
            }
            else if (group == 4) {
                sse2_shift_apply_psrad(src_bytes, count, result_bytes);
            }
            else if (group == 6) {
                sse2_shift_apply_pslld(src_bytes, count, result_bytes);
            }
            else {
                raise_ud();
            }
        }
        else {
            if (group == 2) {
                sse2_shift_apply_psrlq(src_bytes, count, result_bytes);
            }
            else if (group == 3) {
                sse2_shift_apply_psrldq(src_bytes, count, result_bytes);
            }
            else if (group == 6) {
                sse2_shift_apply_psllq(src_bytes, count, result_bytes);
            }
            else if (group == 7) {
                sse2_shift_apply_pslldq(src_bytes, count, result_bytes);
            }
            else {
                raise_ud();
            }
        }
    }
    else {
        switch (opcode) {
        case 0xD1:
            sse2_shift_apply_psrlw(src_bytes, count, result_bytes);
            break;
        case 0xD2:
            sse2_shift_apply_psrld(src_bytes, count, result_bytes);
            break;
        case 0xD3:
            sse2_shift_apply_psrlq(src_bytes, count, result_bytes);
            break;
        case 0xE1:
            sse2_shift_apply_psraw(src_bytes, count, result_bytes);
            break;
        case 0xE2:
            sse2_shift_apply_psrad(src_bytes, count, result_bytes);
            break;
        case 0xF1:
            sse2_shift_apply_psllw(src_bytes, count, result_bytes);
            break;
        case 0xF2:
            sse2_shift_apply_pslld(src_bytes, count, result_bytes);
            break;
        case 0xF3:
            sse2_shift_apply_psllq(src_bytes, count, result_bytes);
            break;
        default:
            raise_ud();
            break;
        }
    }

    return sse2_shift_bytes_to_xmm(result_bytes);
}

static AVXRegister256 apply_avx_shift256(uint8_t opcode, uint8_t group, AVXRegister256 src, uint64_t count) {
    AVXRegister256 result = {};
    result.low = apply_avx_shift128(opcode, group, src.low, count);
    result.high = apply_avx_shift128(opcode, group, src.high, count);
    return result;
}

static XMMRegister apply_avx2_shiftvar_dword128(uint8_t opcode, XMMRegister src, XMMRegister counts) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        uint32_t value = get_xmm_lane_bits(src, lane);
        uint32_t count = get_xmm_lane_bits(counts, lane) & 0x1FU;
        uint32_t shifted = 0;
        if (opcode == 0x47) {
            shifted = value << count;
        }
        else if (opcode == 0x45) {
            shifted = value >> count;
        }
        else if (opcode == 0x46) {
            shifted = (uint32_t)(((int32_t)value) >> count);
        }
        else {
            raise_ud();
        }
        set_xmm_lane_bits(&result, lane, shifted);
    }
    return result;
}

static AVXRegister256 apply_avx2_shiftvar_dword256(uint8_t opcode, AVXRegister256 src, AVXRegister256 counts) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        uint32_t value = get_ymm_lane_bits(src, lane);
        uint32_t count = get_ymm_lane_bits(counts, lane) & 0x1FU;
        uint32_t shifted = 0;
        if (opcode == 0x47) {
            shifted = value << count;
        }
        else if (opcode == 0x45) {
            shifted = value >> count;
        }
        else if (opcode == 0x46) {
            shifted = (uint32_t)(((int32_t)value) >> count);
        }
        else {
            raise_ud();
        }
        set_ymm_lane_bits(&result, lane, shifted);
    }
    return result;
}

static XMMRegister apply_avx2_shiftvar_qword128(uint8_t opcode, XMMRegister src, XMMRegister counts) {
    XMMRegister result = {};
    uint64_t low_count = counts.low & 0x3FULL;
    uint64_t high_count = counts.high & 0x3FULL;
    if (opcode == 0x47) {
        result.low = src.low << low_count;
        result.high = src.high << high_count;
    }
    else if (opcode == 0x45) {
        result.low = src.low >> low_count;
        result.high = src.high >> high_count;
    }
    else {
        raise_ud();
    }
    return result;
}

static AVXRegister256 apply_avx2_shiftvar_qword256(uint8_t opcode, AVXRegister256 src, AVXRegister256 counts) {
    AVXRegister256 result = {};
    uint64_t count0 = counts.low.low & 0x3FULL;
    uint64_t count1 = counts.low.high & 0x3FULL;
    uint64_t count2 = counts.high.low & 0x3FULL;
    uint64_t count3 = counts.high.high & 0x3FULL;
    if (opcode == 0x47) {
        result.low.low = src.low.low << count0;
        result.low.high = src.low.high << count1;
        result.high.low = src.high.low << count2;
        result.high.high = src.high.high << count3;
    }
    else if (opcode == 0x45) {
        result.low.low = src.low.low >> count0;
        result.low.high = src.low.high >> count1;
        result.high.low = src.high.low >> count2;
        result.high.high = src.high.high >> count3;
    }
    else {
        raise_ud();
    }
    return result;
}

static XMMRegister apply_avx_arith_pd128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    for (int lane = 0; lane < 2; lane++) {
        double lhs_lane = sse2_arith_pd_bits_to_double(get_sse2_arith_pd_lane_bits(lhs, lane));
        double rhs_lane = sse2_arith_pd_bits_to_double(get_sse2_arith_pd_lane_bits(rhs, lane));
        set_sse2_arith_pd_lane_bits(&result, lane, sse2_arith_pd_double_to_bits(apply_sse2_arith_pd_scalar(opcode, lhs_lane, rhs_lane)));
    }
    return result;
}

static AVXRegister256 apply_avx_arith_pd256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 4; lane++) {
        double lhs_lane = sse2_arith_pd_bits_to_double(get_ymm_pd_lane_bits(lhs, lane));
        double rhs_lane = sse2_arith_pd_bits_to_double(get_ymm_pd_lane_bits(rhs, lane));
        set_ymm_pd_lane_bits(&result, lane, sse2_arith_pd_double_to_bits(apply_sse2_arith_pd_scalar(opcode, lhs_lane, rhs_lane)));
    }
    return result;
}

static XMMRegister apply_avx_addsub_pd128(XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    for (int lane = 0; lane < 2; lane++) {
        double lhs_lane = sse2_arith_pd_bits_to_double(get_sse2_arith_pd_lane_bits(lhs, lane));
        double rhs_lane = sse2_arith_pd_bits_to_double(get_sse2_arith_pd_lane_bits(rhs, lane));
        double value = (lane & 0x01) == 0 ? (lhs_lane - rhs_lane) : (lhs_lane + rhs_lane);
        set_sse2_arith_pd_lane_bits(&result, lane, sse2_arith_pd_double_to_bits(value));
    }
    return result;
}

static AVXRegister256 apply_avx_addsub_pd256(AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 4; lane++) {
        double lhs_lane = sse2_arith_pd_bits_to_double(get_ymm_pd_lane_bits(lhs, lane));
        double rhs_lane = sse2_arith_pd_bits_to_double(get_ymm_pd_lane_bits(rhs, lane));
        double value = (lane & 0x01) == 0 ? (lhs_lane - rhs_lane) : (lhs_lane + rhs_lane);
        set_ymm_pd_lane_bits(&result, lane, sse2_arith_pd_double_to_bits(value));
    }
    return result;
}

static XMMRegister apply_avx_addsub_ps128(XMMRegister lhs, XMMRegister rhs) {
    XMMRegister result = {};
    for (int lane = 0; lane < 4; lane++) {
        float lhs_lane = sse_bits_to_float(get_xmm_lane_bits(lhs, lane));
        float rhs_lane = sse_bits_to_float(get_xmm_lane_bits(rhs, lane));
        float value = (lane & 0x01) == 0 ? (lhs_lane - rhs_lane) : (lhs_lane + rhs_lane);
        set_xmm_lane_bits(&result, lane, sse_float_to_bits(value));
    }
    return result;
}

static AVXRegister256 apply_avx_addsub_ps256(AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane++) {
        float lhs_lane = sse_bits_to_float(get_ymm_lane_bits(lhs, lane));
        float rhs_lane = sse_bits_to_float(get_ymm_lane_bits(rhs, lane));
        float value = (lane & 0x01) == 0 ? (lhs_lane - rhs_lane) : (lhs_lane + rhs_lane);
        set_ymm_lane_bits(&result, lane, sse_float_to_bits(value));
    }
    return result;
}

static XMMRegister apply_avx_arith_sd128(uint8_t opcode, XMMRegister src1, uint64_t rhs_bits) {
    XMMRegister result = src1;
    double lhs_scalar = sse2_arith_pd_bits_to_double(src1.low);
    double rhs_scalar = sse2_arith_pd_bits_to_double(rhs_bits);
    result.low = sse2_arith_pd_double_to_bits(apply_sse2_arith_pd_scalar(opcode, lhs_scalar, rhs_scalar));
    return result;
}

static XMMRegister apply_avx_movss_load128(XMMRegister src1, uint32_t rhs_bits) {
    XMMRegister result = src1;
    set_xmm_lane_bits(&result, 0, rhs_bits);
    return result;
}

static XMMRegister apply_avx_movlps_load128(XMMRegister src1, uint64_t rhs_bits) {
    XMMRegister result = src1;
    result.low = rhs_bits;
    return result;
}

static XMMRegister apply_avx_movhps_load128(XMMRegister src1, uint64_t rhs_bits) {
    XMMRegister result = src1;
    result.high = rhs_bits;
    return result;
}

static XMMRegister apply_avx_movhlps128(XMMRegister src1, XMMRegister src2) {
    XMMRegister result = src1;
    result.low = src2.high;
    return result;
}

static XMMRegister apply_avx_movlhps128(XMMRegister src1, XMMRegister src2) {
    XMMRegister result = src1;
    result.high = src2.low;
    return result;
}

static XMMRegister apply_avx_movddup128(XMMRegister source) {
    XMMRegister result = {};
    result.low = source.low;
    result.high = source.low;
    return result;
}

static XMMRegister apply_avx_movddup_load128(uint64_t source_bits) {
    XMMRegister result = {};
    result.low = source_bits;
    result.high = source_bits;
    return result;
}

static AVXRegister256 apply_avx_movddup256(AVXRegister256 source) {
    AVXRegister256 result = {};
    result.low.low = source.low.low;
    result.low.high = source.low.low;
    result.high.low = source.high.low;
    result.high.high = source.high.low;
    return result;
}

static XMMRegister apply_avx_movsldup128(XMMRegister source) {
    XMMRegister result = {};
    set_xmm_lane_bits(&result, 0, get_xmm_lane_bits(source, 0));
    set_xmm_lane_bits(&result, 1, get_xmm_lane_bits(source, 0));
    set_xmm_lane_bits(&result, 2, get_xmm_lane_bits(source, 2));
    set_xmm_lane_bits(&result, 3, get_xmm_lane_bits(source, 2));
    return result;
}

static AVXRegister256 apply_avx_movsldup256(AVXRegister256 source) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane += 2) {
        uint32_t value = get_ymm_lane_bits(source, lane);
        set_ymm_lane_bits(&result, lane, value);
        set_ymm_lane_bits(&result, lane + 1, value);
    }
    return result;
}

static XMMRegister apply_avx_movshdup128(XMMRegister source) {
    XMMRegister result = {};
    set_xmm_lane_bits(&result, 0, get_xmm_lane_bits(source, 1));
    set_xmm_lane_bits(&result, 1, get_xmm_lane_bits(source, 1));
    set_xmm_lane_bits(&result, 2, get_xmm_lane_bits(source, 3));
    set_xmm_lane_bits(&result, 3, get_xmm_lane_bits(source, 3));
    return result;
}

static AVXRegister256 apply_avx_movshdup256(AVXRegister256 source) {
    AVXRegister256 result = {};
    for (int lane = 0; lane < 8; lane += 2) {
        uint32_t value = get_ymm_lane_bits(source, lane + 1);
        set_ymm_lane_bits(&result, lane, value);
        set_ymm_lane_bits(&result, lane + 1, value);
    }
    return result;
}

static XMMRegister apply_avx_arith_ss128(uint8_t opcode, XMMRegister src1, uint32_t rhs_bits) {
    XMMRegister result = src1;
    float lhs_scalar = sse_bits_to_float(get_xmm_lane_bits(src1, 0));
    float rhs_scalar = sse_bits_to_float(rhs_bits);
    set_xmm_lane_bits(&result, 0, sse_float_to_bits(apply_sse_arith_scalar(opcode, lhs_scalar, rhs_scalar)));
    return result;
}

static XMMRegister apply_avx_minmax_ss128(uint8_t opcode, XMMRegister src1, uint32_t rhs_bits) {
    XMMRegister rhs_scalar = {};
    set_xmm_lane_bits(&rhs_scalar, 0, rhs_bits);
    __m128 lhs_vec = sse_math_xmm_to_m128(src1);
    __m128 rhs_vec = sse_math_xmm_to_m128(rhs_scalar);
    return sse_math_m128_to_xmm(opcode == 0x5D ? _mm_min_ss(lhs_vec, rhs_vec)
                                                  : _mm_max_ss(lhs_vec, rhs_vec));
}

static XMMRegister apply_avx_sqrt_ss128(XMMRegister src1, uint32_t rhs_bits) {
    XMMRegister result = src1;
    XMMRegister rhs_scalar = {};
    set_xmm_lane_bits(&rhs_scalar, 0, rhs_bits);
    __m128 rhs_vec = sse_math_xmm_to_m128(rhs_scalar);
    set_xmm_lane_bits(&result, 0, sse_math_float_to_bits(_mm_cvtss_f32(_mm_sqrt_ss(rhs_vec))));
    return result;
}

static XMMRegister apply_avx_minmax_pd128(uint8_t opcode, XMMRegister lhs, XMMRegister rhs) {
    __m128d lhs_vec = sse2_math_pd_xmm_to_m128d(lhs);
    __m128d rhs_vec = sse2_math_pd_xmm_to_m128d(rhs);
    return sse2_math_pd_m128d_to_xmm(opcode == 0x5D ? _mm_min_pd(lhs_vec, rhs_vec)
                                                   : _mm_max_pd(lhs_vec, rhs_vec));
}

static AVXRegister256 apply_avx_minmax_pd256(uint8_t opcode, AVXRegister256 lhs, AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_minmax_pd128(opcode, lhs.low, rhs.low);
    result.high = apply_avx_minmax_pd128(opcode, lhs.high, rhs.high);
    return result;
}

static XMMRegister apply_avx_minmax_sd128(uint8_t opcode, XMMRegister src1, uint64_t rhs_bits) {
    XMMRegister rhs_scalar = {};
    rhs_scalar.low = rhs_bits;
    __m128d lhs_vec = sse2_math_pd_xmm_to_m128d(src1);
    __m128d rhs_vec = sse2_math_pd_xmm_to_m128d(rhs_scalar);
    return sse2_math_pd_m128d_to_xmm(opcode == 0x5D ? _mm_min_sd(lhs_vec, rhs_vec)
                                                   : _mm_max_sd(lhs_vec, rhs_vec));
}

static XMMRegister apply_avx_sqrt_pd128(XMMRegister rhs) {
    return sse2_math_pd_m128d_to_xmm(_mm_sqrt_pd(sse2_math_pd_xmm_to_m128d(rhs)));
}

static AVXRegister256 apply_avx_sqrt_pd256(AVXRegister256 rhs) {
    AVXRegister256 result = {};
    result.low = apply_avx_sqrt_pd128(rhs.low);
    result.high = apply_avx_sqrt_pd128(rhs.high);
    return result;
}

static XMMRegister apply_avx_sqrt_sd128(XMMRegister src1, uint64_t rhs_bits) {
    XMMRegister rhs_scalar = {};
    rhs_scalar.low = rhs_bits;
    return sse2_math_pd_m128d_to_xmm(_mm_sqrt_sd(sse2_math_pd_xmm_to_m128d(src1), sse2_math_pd_xmm_to_m128d(rhs_scalar)));
}

void execute_avx_vex(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    AVXVexPrefix prefix = decode_avx_vex_prefix(code, code_size);
    uint8_t opcode = prefix.opcode;
    uint8_t mandatory_prefix = avx_vex_mandatory_prefix(&prefix);
    bool is_256 = avx_vex_is_256(&prefix);
    uint8_t map_select = avx_vex_map_select(&prefix);
    apply_avx_vex_state(ctx, &prefix);

    if (map_select == 0x03) {
        if ((opcode == 0x14 || opcode == 0x15 || opcode == 0x16) && mandatory_prefix == 1) {
            if (is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }

            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            XMMRegister source = get_xmm128(ctx, avx_vex_dest_index(ctx, inst.modrm));
            uint8_t mod = (inst.modrm >> 6) & 0x03;
            uint8_t imm8 = (uint8_t)inst.immediate;
            int dest = decode_movdq_gpr_rm_index(ctx, inst.modrm);

            if (opcode == 0x14) {
                uint8_t source_bytes[16] = {};
                sse2_misc_xmm_to_bytes(source, source_bytes);
                uint8_t value = source_bytes[imm8 & 0x0F];
                if (mod == 0x03) {
                    if (ctx->cs.descriptor.long_mode) {
                        set_reg64(ctx, dest, (uint64_t)value);
                    }
                    else {
                        set_reg32(ctx, dest, (uint32_t)value);
                    }
                }
                else {
                    write_memory_byte(ctx, inst.mem_address, value);
                }
                return;
            }

            if (opcode == 0x15) {
                uint8_t source_bytes[16] = {};
                sse2_misc_xmm_to_bytes(source, source_bytes);
                uint8_t source_offset = (uint8_t)((imm8 & 0x07) * 2);
                uint16_t value = (uint16_t)source_bytes[source_offset]
                               | (uint16_t)(source_bytes[source_offset + 1] << 8);
                if (mod == 0x03) {
                    if (ctx->cs.descriptor.long_mode) {
                        set_reg64(ctx, dest, (uint64_t)value);
                    }
                    else {
                        set_reg32(ctx, dest, (uint32_t)value);
                    }
                }
                else {
                    write_memory_word(ctx, inst.mem_address, value);
                }
                return;
            }

            if (ctx->cs.descriptor.long_mode && ctx->rex_w) {
                uint64_t value = ((imm8 & 0x01) != 0) ? source.high : source.low;
                if (mod == 0x03) {
                    set_reg64(ctx, dest, value);
                }
                else {
                    write_memory_qword(ctx, inst.mem_address, value);
                }
            }
            else {
                uint32_t value = get_xmm_lane_bits(source, imm8 & 0x03);
                if (mod == 0x03) {
                    set_reg32(ctx, dest, value);
                }
                else {
                    write_memory_dword(ctx, inst.mem_address, value);
                }
            }
            return;
        }
        if (opcode == 0x04 || opcode == 0x05) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            uint8_t imm8 = (uint8_t)inst.immediate;
            if (opcode == 0x04) {
                if (is_256) {
                    set_ymm256(ctx, dest, apply_avx_permilps_imm256(read_avx_rm256(ctx, &inst), imm8));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx_permilps_imm128(read_avx_int_rm128(ctx, &inst), imm8));
                    clear_ymm_upper128(ctx, dest);
                }
            }
            else {
                if (is_256) {
                    set_ymm256(ctx, dest, apply_avx_permilpd_imm256(read_avx_rm256(ctx, &inst), imm8));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx_permilpd_imm128(read_avx_int_rm128(ctx, &inst), imm8));
                    clear_ymm_upper128(ctx, dest);
                }
            }
            return;
        }
        if (opcode == 0x00 || opcode == 0x01) {
            if (mandatory_prefix != 1 || !is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            set_ymm256(ctx, dest, apply_avx2_permq256(read_avx_rm256(ctx, &inst), (uint8_t)inst.immediate));
            return;
        }
        if (opcode == 0x0F) {
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            uint8_t imm8 = (uint8_t)inst.immediate;
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx2_palignr256(get_ymm256(ctx, avx_vex_source1_index(&prefix)), read_avx_rm256(ctx, &inst), imm8));
            }
            else {
                set_xmm128(ctx, dest, apply_avx2_palignr128(get_xmm128(ctx, avx_vex_source1_index(&prefix)), read_avx_int_rm128(ctx, &inst), imm8));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x06 || opcode == 0x46) {
            if (!is_256) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            AVXRegister256 src1 = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 src2 = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx_perm2f128_256(src1, src2, (uint8_t)inst.immediate));
            return;
        }
        if (opcode == 0x08 || opcode == 0x09 || opcode == 0x0A || opcode == 0x0B) {
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            uint8_t imm8 = (uint8_t)inst.immediate;
            if (opcode == 0x08) {
                if (avx_vex_requires_reserved_vvvv(&prefix)) {
                    raise_ud();
                }
                if (is_256) {
                    set_ymm256(ctx, dest, apply_avx_round_ps256(read_avx_rm256(ctx, &inst), imm8, ctx->mxcsr));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx_round_ps128(read_sse_arith_source_operand(ctx, &inst), imm8, ctx->mxcsr));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            if (opcode == 0x09) {
                if (avx_vex_requires_reserved_vvvv(&prefix)) {
                    raise_ud();
                }
                if (is_256) {
                    set_ymm256(ctx, dest, apply_avx_round_pd256(read_avx_rm256(ctx, &inst), imm8, ctx->mxcsr));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx_round_pd128(read_sse2_arith_pd_packed_source(ctx, &inst), imm8, ctx->mxcsr));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            if (opcode == 0x0A) {
                if (is_256) {
                    raise_ud();
                }
                XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                set_xmm128(ctx, dest, apply_avx_round_ss128(src1, sse_convert_read_scalar_float_bits(ctx, &inst), imm8, ctx->mxcsr));
                clear_ymm_upper128(ctx, dest);
                return;
            }
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_round_sd128(src1, read_sse2_arith_pd_scalar_source_bits(ctx, &inst), imm8, ctx->mxcsr));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (opcode == 0x02 || opcode == 0x0E || opcode == 0x0C || opcode == 0x0D || opcode == 0x4C) {
            if (opcode == 0x4C && ctx->rex_w) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            uint8_t imm8 = (uint8_t)inst.immediate;
            if (opcode == 0x02) {
                if (is_256) {
                    AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                    AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                    set_ymm256(ctx, dest, apply_avx_blend_ps256(lhs, rhs, imm8));
                }
                else {
                    XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                    XMMRegister rhs = read_avx_int_rm128(ctx, &inst);
                    set_xmm128(ctx, dest, apply_avx_blend_ps128(lhs, rhs, imm8));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            if (opcode == 0x0E) {
                if (is_256) {
                    AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                    AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                    set_ymm256(ctx, dest, apply_avx2_blendw256(lhs, rhs, imm8));
                }
                else {
                    XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                    XMMRegister rhs = read_avx_int_rm128(ctx, &inst);
                    set_xmm128(ctx, dest, apply_avx2_blendw128(lhs, rhs, imm8));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            if (opcode == 0x0C) {
                if (is_256) {
                    AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                    AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                    set_ymm256(ctx, dest, apply_avx_blend_ps256(lhs, rhs, imm8));
                }
                else {
                    XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                    XMMRegister rhs = read_sse_arith_source_operand(ctx, &inst);
                    set_xmm128(ctx, dest, apply_avx_blend_ps128(lhs, rhs, imm8));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            if (opcode == 0x0D) {
                if (is_256) {
                    AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                    AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                    set_ymm256(ctx, dest, apply_avx_blend_pd256(lhs, rhs, imm8));
                }
                else {
                    XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                    XMMRegister rhs = read_sse2_arith_pd_packed_source(ctx, &inst);
                    set_xmm128(ctx, dest, apply_avx_blend_pd128(lhs, rhs, imm8));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            int mask_reg = avx_vex_is4_source_index(ctx, imm8);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                AVXRegister256 mask = get_ymm256(ctx, mask_reg);
                set_ymm256(ctx, dest, apply_avx2_blendvb256(lhs, rhs, mask));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_avx_int_rm128(ctx, &inst);
                XMMRegister mask = get_xmm128(ctx, mask_reg);
                set_xmm128(ctx, dest, apply_avx2_blendvb128(lhs, rhs, mask));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x21) {
            if (is_256) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            uint8_t imm8 = (uint8_t)inst.immediate;
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                XMMRegister src2 = get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm));
                set_xmm128(ctx, dest, apply_avx_insertps_reg128(src1, src2, imm8));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_insertps_mem128(src1, read_memory_dword(ctx, inst.mem_address), imm8));
            }
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (opcode == 0xDF) {
            if (is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }

            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            XMMRegister source = read_avx_int_rm128(ctx, &inst);
            set_xmm128(ctx, dest, apply_aeskeygenassist128(source, (uint8_t)inst.immediate));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (opcode == 0x20 || opcode == 0x22) {
            if (is_256) {
                raise_ud();
            }

            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            uint8_t mod = (inst.modrm >> 6) & 0x03;
            uint8_t imm8 = (uint8_t)inst.immediate;
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            int src2 = decode_movdq_gpr_rm_index(ctx, inst.modrm);
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));

            if (opcode == 0x20) {
                uint8_t value = mod == 0x03
                    ? (uint8_t)get_reg64(ctx, src2)
                    : read_memory_byte(ctx, inst.mem_address);
                set_xmm128(ctx, dest, apply_avx_pinsrb128(src1, value, imm8));
                clear_ymm_upper128(ctx, dest);
                return;
            }

            if (ctx->rex_w) {
                if (!ctx->cs.descriptor.long_mode) {
                    raise_ud();
                }
                uint64_t value = mod == 0x03
                    ? get_reg64(ctx, src2)
                    : read_memory_qword(ctx, inst.mem_address);
                set_xmm128(ctx, dest, apply_avx_pinsrq128(src1, value, imm8));
            }
            else {
                uint32_t value = mod == 0x03
                    ? (uint32_t)get_reg64(ctx, src2)
                    : read_memory_dword(ctx, inst.mem_address);
                set_xmm128(ctx, dest, apply_avx_pinsrd128(src1, value, imm8));
            }
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (opcode == 0x17) {
            if (ctx->rex_w || is_256 || avx_vex_encoded_vvvv(&prefix) != 0x00) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            uint32_t value = get_xmm_lane_bits(get_xmm128(ctx, avx_vex_dest_index(ctx, inst.modrm)),
                                               (int)((uint8_t)inst.immediate & 0x03));
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                set_reg32(ctx, avx_vex_rm_index(ctx, inst.modrm), value);
            }
            else {
                write_memory_dword(ctx, inst.mem_address, value);
            }
            return;
        }
        if (opcode == 0x42) {
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            uint8_t imm8 = (uint8_t)inst.immediate;
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_mpsadbw256(get_ymm256(ctx, avx_vex_source1_index(&prefix)),
                                                           read_avx_rm256(ctx, &inst),
                                                           imm8));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_mpsadbw128(get_xmm128(ctx, avx_vex_source1_index(&prefix)),
                                                           read_avx_int_rm128(ctx, &inst),
                                                           imm8));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x60 || opcode == 0x61 || opcode == 0x62 || opcode == 0x63) {
            if (mandatory_prefix != 1 || is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }

            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            uint8_t imm8 = (uint8_t)inst.immediate;
            __m128i lhs = avx_xmm_to_m128i(get_xmm128(ctx, avx_vex_dest_index(ctx, inst.modrm)));
            __m128i rhs = avx_xmm_to_m128i(read_avx_int_rm128(ctx, &inst));

            if (opcode == 0x60 || opcode == 0x61) {
                int length_a = avx_pcmpestr_length(ctx, REG_RAX, ctx->rex_w, imm8);
                int length_b = avx_pcmpestr_length(ctx, REG_RDX, ctx->rex_w, imm8);
                AVXPcmpstrResult result = avx_execute_pcmpestr(lhs, length_a, rhs, length_b, imm8);
                update_avx_pcmpstr_flags(ctx, result.cf, result.zf, result.sf, result.of);
                if (opcode == 0x60) {
                    set_xmm128(ctx, 0, result.mask);
                    clear_ymm_upper128(ctx, 0);
                }
                else {
                    set_reg32(ctx, REG_RCX, result.index);
                }
                return;
            }

            AVXPcmpstrResult result = avx_execute_pcmpistr(lhs, rhs, imm8);
            update_avx_pcmpstr_flags(ctx, result.cf, result.zf, result.sf, result.of);
            if (opcode == 0x62) {
                set_xmm128(ctx, 0, result.mask);
                clear_ymm_upper128(ctx, 0);
            }
            else {
                set_reg32(ctx, REG_RCX, result.index);
            }
            return;
        }
        if (opcode == 0x18 || opcode == 0x38) {
            if (mandatory_prefix != 1 || !is_256) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            AVXRegister256 src1 = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            XMMRegister insert_value = ((inst.modrm >> 6) & 0x03) == 0x03
                ? get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm))
                : read_xmm_memory(ctx, inst.mem_address);
            set_ymm256(ctx, dest, apply_avx_insertf128_256(src1, insert_value, (uint8_t)inst.immediate));
            return;
        }
        if (opcode == 0x19 || opcode == 0x39) {
            if (mandatory_prefix != 1 || !is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            XMMRegister extracted = apply_avx_extractf128_256(get_ymm256(ctx, avx_vex_dest_index(ctx, inst.modrm)), (uint8_t)inst.immediate);
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                int dest = avx_vex_rm_index(ctx, inst.modrm);
                set_xmm128(ctx, dest, extracted);
                clear_ymm_upper128(ctx, dest);
            }
            else {
                write_xmm_memory(ctx, inst.mem_address, extracted);
            }
            return;
        }
        if (opcode == 0x4A || opcode == 0x4B) {
            if (ctx->rex_w) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            int mask_reg = avx_vex_is4_source_index(ctx, (uint8_t)inst.immediate);
            if (opcode == 0x4A) {
                if (is_256) {
                    AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                    AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                    AVXRegister256 mask = get_ymm256(ctx, mask_reg);
                    set_ymm256(ctx, dest, apply_avx_blendv_ps256(lhs, rhs, mask));
                }
                else {
                    XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                    XMMRegister rhs = read_sse_arith_source_operand(ctx, &inst);
                    XMMRegister mask = get_xmm128(ctx, mask_reg);
                    set_xmm128(ctx, dest, apply_avx_blendv_ps128(lhs, rhs, mask));
                    clear_ymm_upper128(ctx, dest);
                }
                return;
            }
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                AVXRegister256 mask = get_ymm256(ctx, mask_reg);
                set_ymm256(ctx, dest, apply_avx_blendv_pd256(lhs, rhs, mask));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_arith_pd_packed_source(ctx, &inst);
                XMMRegister mask = get_xmm128(ctx, mask_reg);
                set_xmm128(ctx, dest, apply_avx_blendv_pd128(lhs, rhs, mask));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x40 || opcode == 0x41) {
            if (is_256) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_0f3a_modrm_imm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (opcode == 0x40) {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_arith_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_dpps128(lhs, rhs, (uint8_t)inst.immediate));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_arith_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_dppd128(lhs, rhs, (uint8_t)inst.immediate));
            }
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (map_select == 0x02) {
        if (opcode == 0x41) {
            if (mandatory_prefix != 1 || is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }

            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            set_xmm128(ctx, dest, apply_avx_phminposuw128(read_avx_int_rm128(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (opcode == 0xDC) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }

            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 state = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 round_key = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_vaesenc256(state, round_key));
            }
            else {
                XMMRegister state = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister round_key = read_avx_int_rm128(ctx, &inst);
                set_xmm128(ctx, dest, apply_aesenc128(state, round_key));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x00) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx2_pshufb256(get_ymm256(ctx, avx_vex_source1_index(&prefix)), read_avx_rm256(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx2_pshufb128(get_xmm128(ctx, avx_vex_source1_index(&prefix)), read_avx_int_rm128(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x16 || opcode == 0x36) {
            if (mandatory_prefix != 1 || !is_256) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            AVXRegister256 control = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 data = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx2_perm32x8(data, control));
            return;
        }
        if (opcode == 0x45 || opcode == 0x46 || opcode == 0x47) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            bool is_qword = ctx->rex_w && opcode != 0x46;
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 src = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 counts = read_avx_rm256(ctx, &inst);
                if (is_qword) {
                    set_ymm256(ctx, dest, apply_avx2_shiftvar_qword256(opcode, src, counts));
                }
                else {
                    set_ymm256(ctx, dest, apply_avx2_shiftvar_dword256(opcode, src, counts));
                }
            }
            else {
                XMMRegister src = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister counts = read_avx_int_rm128(ctx, &inst);
                if (is_qword) {
                    set_xmm128(ctx, dest, apply_avx2_shiftvar_qword128(opcode, src, counts));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx2_shiftvar_dword128(opcode, src, counts));
                }
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x2A) {
            if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                raise_ud();
            }
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            validate_avx_movaps_alignment(&inst, is_256);
            if (is_256) {
                set_ymm256(ctx, dest, read_ymm_memory(ctx, inst.mem_address));
            }
            else {
                set_xmm128(ctx, dest, read_xmm_memory(ctx, inst.mem_address));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode >= 0x90 && opcode <= 0x93) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            if (opcode == 0x90 || opcode == 0x92) {
                if (ctx->rex_w) {
                    execute_avx_gather_dpd(ctx, &prefix, &inst, is_256);
                }
                else {
                    execute_avx_gather_dps(ctx, &prefix, &inst, is_256);
                }
            }
            else {
                if (ctx->rex_w) {
                    execute_avx_gather_qpd(ctx, &prefix, &inst, is_256);
                }
                else {
                    execute_avx_gather_qps(ctx, &prefix, &inst, is_256);
                }
            }
            return;
        }
        if (opcode == 0x8C || opcode == 0x8E) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                raise_ud();
            }

            int reg = avx_vex_dest_index(ctx, inst.modrm);
            int mask_reg = avx_vex_source1_index(&prefix);
            bool is_store = opcode == 0x8E;
            bool is_qword = ctx->rex_w;

            if (!is_store) {
                if (is_256) {
                    AVXRegister256 mask = get_ymm256(ctx, mask_reg);
                    AVXRegister256 memory_value = read_ymm_memory(ctx, inst.mem_address);
                    set_ymm256(ctx, reg, apply_avx2_pmaskmov_load256(mask, memory_value, is_qword));
                }
                else {
                    XMMRegister mask = get_xmm128(ctx, mask_reg);
                    XMMRegister memory_value = read_xmm_memory(ctx, inst.mem_address);
                    set_xmm128(ctx, reg, apply_avx2_pmaskmov_load128(mask, memory_value, is_qword));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    execute_avx2_pmaskmov_store256(ctx, inst.mem_address, get_ymm256(ctx, mask_reg), get_ymm256(ctx, reg), is_qword);
                }
                else {
                    execute_avx2_pmaskmov_store128(ctx, inst.mem_address, get_xmm128(ctx, mask_reg), get_xmm128(ctx, reg), is_qword);
                }
            }
            return;
        }
        if ((opcode >= 0x20 && opcode <= 0x25) || (opcode >= 0x30 && opcode <= 0x35)) {
            if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            XMMRegister source = read_avx2_pmov_source(ctx, &inst, opcode, is_256);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx2_pmovx256(opcode, source));
            }
            else {
                set_xmm128(ctx, dest, apply_avx2_pmovx128(opcode, source));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x58 || opcode == 0x59 || opcode == 0x78 || opcode == 0x79) {
            if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            uint8_t mod = (inst.modrm >> 6) & 0x03;
            XMMRegister src = mod == 0x03 ? get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm)) : XMMRegister{};
            AVXRegister256 result = {};
            if (opcode == 0x58) {
                uint32_t value = mod == 0x03 ? get_xmm_lane_bits(src, 0) : read_memory_dword(ctx, inst.mem_address);
                result = broadcast_avx2_dword256(value);
            }
            else if (opcode == 0x59) {
                uint64_t value = mod == 0x03 ? src.low : read_memory_qword(ctx, inst.mem_address);
                result = broadcast_avx2_qword256(value);
            }
            else if (opcode == 0x78) {
                uint8_t value = mod == 0x03 ? (uint8_t)(src.low & 0xFFU) : read_memory_byte(ctx, inst.mem_address);
                result = broadcast_avx2_byte256(value);
            }
            else {
                uint16_t value = mod == 0x03 ? (uint16_t)(src.low & 0xFFFFU) : read_memory_word(ctx, inst.mem_address);
                result = broadcast_avx2_word256(value);
            }
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                set_ymm256(ctx, dest, result);
            }
            else {
                set_xmm128(ctx, dest, result.low);
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x18 || opcode == 0x19 || opcode == 0x1A || opcode == 0x5A) {
            if (mandatory_prefix != 1 || !is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                raise_ud();
            }
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (opcode == 0x18) {
                set_ymm256(ctx, dest, broadcast_avx_ss256(read_memory_dword(ctx, inst.mem_address)));
            }
            else if (opcode == 0x19) {
                set_ymm256(ctx, dest, broadcast_avx_sd256(read_memory_qword(ctx, inst.mem_address)));
            }
            else {
                set_ymm256(ctx, dest, broadcast_avx_f128_256(read_xmm_memory(ctx, inst.mem_address)));
            }
            return;
        }
        if (opcode == 0x0C || opcode == 0x0D) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (opcode == 0x0C) {
                if (is_256) {
                    set_ymm256(ctx, dest, apply_avx_permilps_var256(get_ymm256(ctx, avx_vex_source1_index(&prefix)), read_avx_rm256(ctx, &inst)));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx_permilps_var128(get_xmm128(ctx, avx_vex_source1_index(&prefix)), read_avx_int_rm128(ctx, &inst)));
                    clear_ymm_upper128(ctx, dest);
                }
            }
            else {
                if (is_256) {
                    set_ymm256(ctx, dest, apply_avx_permilpd_var256(get_ymm256(ctx, avx_vex_source1_index(&prefix)), read_avx_rm256(ctx, &inst)));
                }
                else {
                    set_xmm128(ctx, dest, apply_avx_permilpd_var128(get_xmm128(ctx, avx_vex_source1_index(&prefix)), read_avx_int_rm128(ctx, &inst)));
                    clear_ymm_upper128(ctx, dest);
                }
            }
            return;
        }
        if (opcode == 0x2C || opcode == 0x2D || opcode == 0x2E || opcode == 0x2F) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            if (((inst.modrm >> 6) & 0x03) == 0x03) {
                raise_ud();
            }
            int reg = avx_vex_dest_index(ctx, inst.modrm);
            int mask_reg = avx_vex_source1_index(&prefix);
            bool is_store = opcode == 0x2E || opcode == 0x2F;
            bool is_pd = opcode == 0x2D || opcode == 0x2F;
            if (!is_store) {
                if (is_256) {
                    AVXRegister256 memory_value = read_ymm_memory(ctx, inst.mem_address);
                    AVXRegister256 mask = get_ymm256(ctx, mask_reg);
                    set_ymm256(ctx, reg, is_pd ? apply_avx_maskmov_load_pd256(mask, memory_value)
                                                : apply_avx_maskmov_load_ps256(mask, memory_value));
                }
                else {
                    XMMRegister memory_value = read_xmm_memory(ctx, inst.mem_address);
                    XMMRegister mask = get_xmm128(ctx, mask_reg);
                    set_xmm128(ctx, reg, is_pd ? apply_avx_maskmov_load_pd128(mask, memory_value)
                                               : apply_avx_maskmov_load_ps128(mask, memory_value));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    AVXRegister256 mask = get_ymm256(ctx, mask_reg);
                    AVXRegister256 source = get_ymm256(ctx, reg);
                    if (is_pd) {
                        execute_avx_maskmov_store_pd256(ctx, inst.mem_address, mask, source);
                    }
                    else {
                        execute_avx_maskmov_store_ps256(ctx, inst.mem_address, mask, source);
                    }
                }
                else {
                    XMMRegister mask = get_xmm128(ctx, mask_reg);
                    XMMRegister source = get_xmm128(ctx, reg);
                    if (is_pd) {
                        execute_avx_maskmov_store_pd128(ctx, inst.mem_address, mask, source);
                    }
                    else {
                        execute_avx_maskmov_store_ps128(ctx, inst.mem_address, mask, source);
                    }
                }
            }
            return;
        }
        if (opcode == 0x0E || opcode == 0x0F || opcode == 0x17) {
            if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int src1 = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, src1);
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_avx_test_flags(ctx, compute_avx_test_zf256(lhs, rhs), compute_avx_test_cf256(lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, src1);
                XMMRegister rhs = read_avx_int_rm128(ctx, &inst);
                set_avx_test_flags(ctx, compute_avx_test_zf128(lhs, rhs), compute_avx_test_cf128(lhs, rhs));
            }
            return;
        }
        if (opcode == 0x04 || opcode == 0x0B || opcode == 0x28 || opcode == 0x40) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_int_arith256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_int_arith_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_int_arith128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x01 || opcode == 0x02 || opcode == 0x03 || opcode == 0x05 || opcode == 0x06 || opcode == 0x07) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx2_horizontal256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_pack_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx2_horizontal128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x29 || opcode == 0x37) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_int_cmp256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_int_cmp_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_int_cmp128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x2B) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_pack256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_pack_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_pack128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x38 || opcode == 0x39 || opcode == 0x3A || opcode == 0x3B || opcode == 0x3C || opcode == 0x3D || opcode == 0x3E || opcode == 0x3F) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_int_minmax256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_pack_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_int_minmax128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x08 || opcode == 0x09 || opcode == 0x0A) {
            if (mandatory_prefix != 1) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx2_sign256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_pack_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx2_sign128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (opcode == 0x1C || opcode == 0x1D || opcode == 0x1E) {
            if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx2_abs256(opcode, read_avx_rm256(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx2_abs128(opcode, read_sse2_pack_source_operand(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    if (map_select != 0x01) {
        raise_ud();
    }

    if (opcode == 0x77) {
        if (mandatory_prefix != 0 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        if (is_256) {
            clear_all_avx_registers(ctx);
        }
        else {
            clear_all_ymm_upper128(ctx);
        }
        ctx->last_inst_size = (int)prefix.modrm_offset;
        return;
    }

    if (opcode == 0x10 || opcode == 0x11) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int reg = avx_vex_dest_index(ctx, inst.modrm);
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        if (mandatory_prefix == 0) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (opcode == 0x10) {
                if (is_256) {
                    set_ymm256(ctx, reg, read_avx_rm256(ctx, &inst));
                }
                else {
                    set_xmm128(ctx, reg, read_movups_rm128(ctx, &inst));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    write_avx_rm256(ctx, &inst, get_ymm256(ctx, reg));
                }
                else {
                    write_movups_rm128(ctx, &inst, get_xmm128(ctx, reg));
                }
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (opcode == 0x10) {
                if (is_256) {
                    set_ymm256(ctx, reg, read_avx_rm256(ctx, &inst));
                }
                else {
                    set_xmm128(ctx, reg, read_sse2_mov_pd_rm128(ctx, &inst));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    write_avx_rm256(ctx, &inst, get_ymm256(ctx, reg));
                }
                else {
                    write_sse2_mov_pd_rm128(ctx, &inst, get_xmm128(ctx, reg));
                }
            }
            return;
        }
        if (mandatory_prefix == 2) {
            if (is_256) {
                raise_ud();
            }
            if (opcode == 0x10) {
                XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                set_xmm128(ctx, reg, apply_avx_movss_load128(src1, sse_convert_read_scalar_float_bits(ctx, &inst)));
                clear_ymm_upper128(ctx, reg);
            }
            else {
                if (avx_vex_requires_reserved_vvvv(&prefix)) {
                    raise_ud();
                }
                XMMRegister src = get_xmm128(ctx, reg);
                if (mod == 3) {
                    int dest = avx_vex_rm_index(ctx, inst.modrm);
                    XMMRegister result = get_xmm128(ctx, dest);
                    set_xmm_lane_bits(&result, 0, get_xmm_lane_bits(src, 0));
                    set_xmm128(ctx, dest, result);
                }
                else {
                    write_memory_dword(ctx, inst.mem_address, get_xmm_lane_bits(src, 0));
                }
            }
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                raise_ud();
            }
            if (opcode == 0x10) {
                XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister result = {};
                result.low = (mod == 3)
                    ? get_xmm128(ctx, decode_sse2_mov_pd_xmm_rm_index(ctx, inst.modrm)).low
                    : read_memory_qword(ctx, inst.mem_address);
                result.high = src1.high;
                set_xmm128(ctx, reg, result);
                clear_ymm_upper128(ctx, reg);
            }
            else {
                if (avx_vex_requires_reserved_vvvv(&prefix)) {
                    raise_ud();
                }
                XMMRegister src = get_xmm128(ctx, reg);
                if (mod == 3) {
                    int dest = decode_sse2_mov_pd_xmm_rm_index(ctx, inst.modrm);
                    XMMRegister result = get_xmm128(ctx, dest);
                    result.low = src.low;
                    set_xmm128(ctx, dest, result);
                }
                else {
                    write_memory_qword(ctx, inst.mem_address, src.low);
                }
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0x12 || opcode == 0x13 || opcode == 0x16 || opcode == 0x17) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        int reg = avx_vex_dest_index(ctx, inst.modrm);

        if (map_select == 0x01 && (mandatory_prefix == 2 || mandatory_prefix == 3)) {
            if (opcode == 0x13 || opcode == 0x17 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }

            if (mandatory_prefix == 2) {
                if (opcode != 0x12) {
                    raise_ud();
                }
                if (is_256) {
                    set_ymm256(ctx, reg, apply_avx_movddup256(read_avx_rm256(ctx, &inst)));
                }
                else {
                    XMMRegister result = mod == 0x03
                        ? apply_avx_movddup128(get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm)))
                        : apply_avx_movddup_load128(read_memory_qword(ctx, inst.mem_address));
                    set_xmm128(ctx, reg, result);
                    clear_ymm_upper128(ctx, reg);
                }
                return;
            }

            if (opcode == 0x12) {
                if (is_256) {
                    set_ymm256(ctx, reg, apply_avx_movsldup256(read_avx_rm256(ctx, &inst)));
                }
                else {
                    set_xmm128(ctx, reg, apply_avx_movsldup128(read_avx_int_rm128(ctx, &inst)));
                    clear_ymm_upper128(ctx, reg);
                }
                return;
            }

            if (opcode == 0x16) {
                if (is_256) {
                    set_ymm256(ctx, reg, apply_avx_movshdup256(read_avx_rm256(ctx, &inst)));
                }
                else {
                    set_xmm128(ctx, reg, apply_avx_movshdup128(read_avx_int_rm128(ctx, &inst)));
                    clear_ymm_upper128(ctx, reg);
                }
                return;
            }

            raise_ud();
        }

        if (mandatory_prefix == 0) {
            if (is_256) {
                raise_ud();
            }
            if (opcode == 0x12) {
                XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister result = mod == 3
                    ? apply_avx_movhlps128(src1, get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm)))
                    : apply_avx_movlps_load128(src1, read_memory_qword(ctx, inst.mem_address));
                set_xmm128(ctx, reg, result);
                clear_ymm_upper128(ctx, reg);
                return;
            }
            if (opcode == 0x16) {
                XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister result = mod == 3
                    ? apply_avx_movlhps128(src1, get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm)))
                    : apply_avx_movhps_load128(src1, read_memory_qword(ctx, inst.mem_address));
                set_xmm128(ctx, reg, result);
                clear_ymm_upper128(ctx, reg);
                return;
            }
            if (mod == 3 || avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            XMMRegister src = get_xmm128(ctx, reg);
            write_memory_qword(ctx, inst.mem_address, opcode == 0x13 ? src.low : src.high);
            return;
        }

        if (mandatory_prefix != 1 || is_256) {
            raise_ud();
        }
        if (mod == 3) {
            raise_ud();
        }
        if (opcode == 0x12) {
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, reg, apply_avx_movlps_load128(src1, read_memory_qword(ctx, inst.mem_address)));
            clear_ymm_upper128(ctx, reg);
            return;
        }
        if (opcode == 0x16) {
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, reg, apply_avx_movhps_load128(src1, read_memory_qword(ctx, inst.mem_address)));
            clear_ymm_upper128(ctx, reg);
            return;
        }
        if (avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        XMMRegister src = get_xmm128(ctx, reg);
        write_memory_qword(ctx, inst.mem_address, opcode == 0x13 ? src.low : src.high);
        return;
    }

    if ((opcode == 0x28 || opcode == 0x29) && !(opcode == 0x28 && map_select == 0x02 && mandatory_prefix == 1)) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int reg = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (opcode == 0x28) {
                if (is_256) {
                    set_ymm256(ctx, reg, read_avx_movaps_rm256(ctx, &inst));
                }
                else {
                    set_xmm128(ctx, reg, read_movaps_rm128(ctx, &inst));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    write_avx_movaps_rm256(ctx, &inst, get_ymm256(ctx, reg));
                }
                else {
                    write_movaps_rm128(ctx, &inst, get_xmm128(ctx, reg));
                }
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (opcode == 0x28) {
                if (is_256) {
                    set_ymm256(ctx, reg, read_avx_movaps_rm256(ctx, &inst));
                }
                else {
                    validate_avx_movaps_alignment(&inst, false);
                    set_xmm128(ctx, reg, read_sse2_mov_pd_rm128(ctx, &inst));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    write_avx_movaps_rm256(ctx, &inst, get_ymm256(ctx, reg));
                }
                else {
                    validate_avx_movaps_alignment(&inst, false);
                    write_sse2_mov_pd_rm128(ctx, &inst, get_xmm128(ctx, reg));
                }
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0x6E || opcode == 0x7E) {
        if (map_select != 0x01 || mandatory_prefix != 1 || is_256) {
            raise_ud();
        }

        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int operand_size = (ctx->cs.descriptor.long_mode && ctx->rex_w) ? 64 : 32;

        if (opcode == 0x6E) {
            XMMRegister result = {};
            int dest = avx_vex_dest_index(ctx, inst.modrm);
            result.low = read_movdq_gpr_or_mem(ctx, &inst, operand_size);
            result.high = 0;
            set_xmm128(ctx, dest, result);
            clear_ymm_upper128(ctx, dest);
            return;
        }

        XMMRegister src = get_xmm128(ctx, avx_vex_dest_index(ctx, inst.modrm));
        write_movdq_gpr_or_mem(ctx, &inst, operand_size, src.low);
        return;
    }

    if (opcode == 0xAE) {
        if (map_select != 0x01 || mandatory_prefix != 0 || is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }

        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        uint8_t reg = (inst.modrm >> 3) & 0x07;
        if (((inst.modrm >> 6) & 0x03) == 0x03) {
            raise_ud();
        }

        if (reg == 2) {
            uint32_t value = read_memory_dword(ctx, inst.mem_address);
            sse_state_validate_mxcsr(value);
            ctx->mxcsr = value & 0x0000FFFFU;
            return;
        }

        if (reg == 3) {
            write_memory_dword(ctx, inst.mem_address, ctx->mxcsr & 0x0000FFFFU);
            return;
        }

        raise_ud();
    }

    if (opcode == 0xD0) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_addsub_pd256(lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_math_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_addsub_pd128(lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_addsub_ps256(lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_math_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_addsub_ps128(lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0x2B) {
        if (avx_vex_requires_reserved_vvvv(&prefix) || (mandatory_prefix != 0 && mandatory_prefix != 1)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        if (((inst.modrm >> 6) & 0x03) == 0x03) {
            raise_ud();
        }
        int src = avx_vex_dest_index(ctx, inst.modrm);
        validate_avx_movaps_alignment(&inst, is_256);
        if (is_256) {
            write_ymm_memory(ctx, inst.mem_address, get_ymm256(ctx, src));
        }
        else {
            write_xmm_memory(ctx, inst.mem_address, get_xmm128(ctx, src));
        }
        return;
    }
    if (opcode == 0x6F || opcode == 0x7F) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int reg = avx_vex_dest_index(ctx, inst.modrm);
        if (avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        if (mandatory_prefix == 1) {
            if (opcode == 0x6F) {
                if (is_256) {
                    set_ymm256(ctx, reg, read_avx_movaps_rm256(ctx, &inst));
                }
                else {
                    set_xmm128(ctx, reg, read_avx_movdqa_rm128(ctx, &inst));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    write_avx_movaps_rm256(ctx, &inst, get_ymm256(ctx, reg));
                }
                else {
                    write_avx_movdqa_rm128(ctx, &inst, get_xmm128(ctx, reg), true);
                }
            }
            return;
        }
        if (mandatory_prefix == 2) {
            if (opcode == 0x6F) {
                if (is_256) {
                    set_ymm256(ctx, reg, read_avx_rm256(ctx, &inst));
                }
                else {
                    set_xmm128(ctx, reg, read_avx_int_rm128(ctx, &inst));
                    clear_ymm_upper128(ctx, reg);
                }
            }
            else {
                if (is_256) {
                    write_avx_rm256(ctx, &inst, get_ymm256(ctx, reg));
                }
                else {
                    write_avx_int_rm128(ctx, &inst, get_xmm128(ctx, reg), true);
                }
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0xE7) {
        if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        if (((inst.modrm >> 6) & 0x03) == 0x03) {
            raise_ud();
        }
        int src = avx_vex_dest_index(ctx, inst.modrm);
        validate_avx_movaps_alignment(&inst, is_256);
        if (is_256) {
            write_ymm_memory(ctx, inst.mem_address, get_ymm256(ctx, src));
        }
        else {
            write_xmm_memory(ctx, inst.mem_address, get_xmm128(ctx, src));
        }
        return;
    }

    if (opcode == 0x50) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        if (((inst.modrm >> 6) & 0x03) != 0x03) {
            raise_ud();
        }
        int dest = decode_sse2_mov_pd_gpr_reg_index(ctx, inst.modrm);
        int src = avx_vex_rm_index(ctx, inst.modrm);
        uint32_t mask = 0;
        if (mandatory_prefix == 0) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            mask = is_256 ? compute_avx_movmskps256(get_ymm256(ctx, src))
                          : compute_avx_movmskps128(get_xmm128(ctx, src));
        }
        else if (mandatory_prefix == 1) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            mask = is_256 ? compute_avx_movmskpd256(get_ymm256(ctx, src))
                          : compute_avx_movmskpd128(get_xmm128(ctx, src));
        }
        else {
            raise_ud();
        }
        set_reg32(ctx, dest, mask);
        return;
    }

    if (opcode == 0xD7) {
        if (mandatory_prefix != 1 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        if (((inst.modrm >> 6) & 0x03) != 0x03) {
            raise_ud();
        }
        int dest = decode_sse2_mov_pd_gpr_reg_index(ctx, inst.modrm);
        int src = avx_vex_rm_index(ctx, inst.modrm);
        set_reg32(ctx, dest, is_256 ? compute_avx_pmovmskb256(get_ymm256(ctx, src))
                                    : compute_avx_pmovmskb128(get_xmm128(ctx, src)));
        return;
    }

    if (opcode == 0xF0) {
        if (mandatory_prefix != 3 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (is_256) {
            set_ymm256(ctx, dest, read_ymm_memory(ctx, inst.mem_address));
        }
        else {
            set_xmm128(ctx, dest, read_xmm_memory(ctx, inst.mem_address));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0xF7) {
        if (mandatory_prefix != 1 || is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        if (((inst.modrm >> 6) & 0x03) != 0x03) {
            raise_ud();
        }
        int src = avx_vex_dest_index(ctx, inst.modrm);
        int mask = avx_vex_rm_index(ctx, inst.modrm);
        uint8_t src_bytes[16] = {};
        uint8_t mask_bytes[16] = {};
        uint64_t address = get_sse2_misc_maskmovdqu_address(ctx, &inst);
        sse2_misc_xmm_to_bytes(get_xmm128(ctx, src), src_bytes);
        sse2_misc_xmm_to_bytes(get_xmm128(ctx, mask), mask_bytes);
        for (int index = 0; index < 16; index++) {
            if (mask_bytes[index] & 0x80U) {
                write_memory_byte(ctx, address + index, src_bytes[index]);
            }
        }
        return;
    }

    if (opcode == 0x14 || opcode == 0x15) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_unpack_ps256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_shuffle_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_unpack_ps128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_unpack_pd256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_arith_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_unpack_pd128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0xC6) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        uint8_t imm8 = (uint8_t)inst.immediate;
        if (mandatory_prefix == 0) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_shufps256(lhs, rhs, imm8));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_shuffle_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_shufps128(lhs, rhs, imm8));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_shufpd256(lhs, rhs, imm8));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_arith_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_shufpd128(lhs, rhs, imm8));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0xC4) {
        if (map_select != 0x01 || mandatory_prefix != 1 || is_256) {
            raise_ud();
        }

        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        int src2 = decode_movdq_gpr_rm_index(ctx, inst.modrm);
        uint16_t value = mod == 0x03
            ? (uint16_t)get_reg64(ctx, src2)
            : read_memory_word(ctx, inst.mem_address);

        set_xmm128(ctx, dest, apply_avx_pinsrw128(get_xmm128(ctx, avx_vex_source1_index(&prefix)),
                                                  value,
                                                  (uint8_t)inst.immediate));
        clear_ymm_upper128(ctx, dest);
        return;
    }

    if (opcode == 0x54 || opcode == 0x55 || opcode == 0x56 || opcode == 0x57) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_logic256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = (opcode == 0x57) ? read_xorps_source_operand(ctx, &inst) : read_sse_logic_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_logic128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_logic256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_logic_pd_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_logic128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0xDB || opcode == 0xDF || opcode == 0xEB || opcode == 0xEF) {
        if (mandatory_prefix != 1) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (is_256) {
            AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx_int_logic256(opcode, lhs, rhs));
        }
        else {
            XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            XMMRegister rhs = read_sse2_int_logic_source_operand(ctx, &inst);
            set_xmm128(ctx, dest, apply_avx_int_logic128(opcode, lhs, rhs));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0x5A) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_cvtps2pd256(sse2_convert_read_xmm_source(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_cvtps2pd128(sse2_convert_read_low64_source(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (is_256) {
                set_xmm128(ctx, dest, apply_avx_cvtpd2ps256(read_avx_rm256(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_cvtpd2ps128(sse2_convert_read_xmm_source(ctx, &inst)));
            }
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 2) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_cvtss2sd128(src1, sse_convert_read_scalar_float_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_cvtsd2ss128(src1, sse2_scalar_convert_read_scalar_double_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x5B) {
        if (avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_cvtdq2ps256(read_avx_rm256(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_cvtdq2ps128(sse2_convert_read_xmm_source(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1 || mandatory_prefix == 2) {
            bool truncate = mandatory_prefix == 2;
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_cvtps2dq256(read_avx_rm256(ctx, &inst), truncate, ctx->mxcsr));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_cvtps2dq128(sse2_convert_read_xmm_source(ctx, &inst), truncate, ctx->mxcsr));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    if (opcode == 0xE6) {
        if (avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 2) {
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_cvtdq2pd256(sse2_convert_read_xmm_source(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_cvtdq2pd128(sse2_convert_read_low64_source(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1 || mandatory_prefix == 3) {
            bool truncate = mandatory_prefix == 1;
            if (is_256) {
                set_xmm128(ctx, dest, apply_avx_cvtpd2dq256(read_avx_rm256(ctx, &inst), truncate, ctx->mxcsr));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_cvtpd2dq128(sse2_convert_read_xmm_source(ctx, &inst), truncate, ctx->mxcsr));
            }
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x2A) {
        if (is_256) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        int source_bits = ctx->rex_w ? 64 : 32;
        if (mandatory_prefix == 2) {
            int64_t source_value = sse_convert_read_signed_integer_source(ctx, &inst, source_bits);
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_cvtsi2ss128(src1, source_value));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 3) {
            int64_t source_value = sse2_scalar_convert_read_signed_integer_source(ctx, &inst, source_bits);
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_cvtsi2sd128(src1, source_value));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x2C || opcode == 0x2D) {
        if (is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest_bits = ctx->rex_w ? 64 : 32;
        bool truncate = opcode == 0x2C;
        if (mandatory_prefix == 2) {
            int dest = decode_sse_convert_gpr_reg_index(ctx, inst.modrm);
            float source = sse_convert_bits_to_float(sse_convert_read_scalar_float_bits(ctx, &inst));
            int64_t integer_value = 0;
            bool success = sse_convert_round_float_to_integer(source, dest_bits, truncate, ctx->mxcsr, &integer_value);
            sse_convert_write_integer_destination(ctx, dest, dest_bits, success, integer_value);
            return;
        }
        if (mandatory_prefix == 3) {
            int dest = decode_sse2_scalar_convert_gpr_reg_index(ctx, inst.modrm);
            double source = sse2_convert_bits_to_double(sse2_scalar_convert_read_scalar_double_bits(ctx, &inst));
            int64_t integer_value = 0;
            bool success = sse2_convert_round_fp_to_integer(source, dest_bits, truncate, ctx->mxcsr, &integer_value);
            sse2_scalar_convert_write_integer_destination(ctx, dest, dest_bits, success, integer_value);
            return;
        }
        raise_ud();
    }

    if (opcode == 0xC2) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        uint8_t predicate = (uint8_t)inst.immediate;
        if (mandatory_prefix == 0) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_cmpps256(lhs, rhs, predicate));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_cmp_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_cmpps128(lhs, rhs, predicate));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_cmppd256(lhs, rhs, predicate));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_cmp_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_cmppd128(lhs, rhs, predicate));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 2) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_cmpss128(src1, read_sse_cmp_scalar_source_bits(ctx, &inst), predicate));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_cmpsd128(src1, read_sse2_cmp_pd_scalar_source_bits(ctx, &inst), predicate));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x2E || opcode == 0x2F) {
        if (is_256 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            float lhs = sse_cmp_bits_to_float(get_xmm_lane_bits(get_xmm128(ctx, dest), 0));
            float rhs = sse_cmp_bits_to_float(read_sse_cmp_scalar_source_bits(ctx, &inst));
            update_flags_comiss_ucomiss(ctx, lhs, rhs);
            return;
        }
        if (mandatory_prefix == 1) {
            double lhs = sse2_cmp_pd_bits_to_double(get_xmm128(ctx, dest).low);
            double rhs = sse2_cmp_pd_bits_to_double(read_sse2_cmp_pd_scalar_source_bits(ctx, &inst));
            update_flags_comisd_ucomisd(ctx, lhs, rhs);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x60 || opcode == 0x61 || opcode == 0x62 || opcode == 0x63 || opcode == 0x67 || opcode == 0x68 || opcode == 0x69 || opcode == 0x6A || opcode == 0x6B || opcode == 0x6C || opcode == 0x6D) {
        if (mandatory_prefix != 1) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (is_256) {
            AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx_pack256(opcode, lhs, rhs));
        }
        else {
            XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            XMMRegister rhs = read_sse2_pack_source_operand(ctx, &inst);
            set_xmm128(ctx, dest, apply_avx_pack128(opcode, lhs, rhs));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0x64 || opcode == 0x65 || opcode == 0x66 || opcode == 0x74 || opcode == 0x75 || opcode == 0x76) {
        if (mandatory_prefix != 1) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (is_256) {
            AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx_int_cmp256(opcode, lhs, rhs));
        }
        else {
            XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            XMMRegister rhs = read_sse2_int_cmp_source_operand(ctx, &inst);
            set_xmm128(ctx, dest, apply_avx_int_cmp128(opcode, lhs, rhs));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0x70) {
        if (mandatory_prefix < 1 || mandatory_prefix > 3 || avx_vex_requires_reserved_vvvv(&prefix)) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        uint8_t imm8 = (uint8_t)inst.immediate;
        if (is_256) {
            AVXRegister256 source = read_avx_rm256(ctx, &inst);
            if (mandatory_prefix == 1) {
                set_ymm256(ctx, dest, apply_avx_pshufd256(source, imm8));
            }
            else if (mandatory_prefix == 2) {
                set_ymm256(ctx, dest, apply_avx_pshufhw256(source, imm8));
            }
            else {
                set_ymm256(ctx, dest, apply_avx_pshuflw256(source, imm8));
            }
        }
        else {
            XMMRegister source = read_sse2_pack_source_operand(ctx, &inst);
            if (mandatory_prefix == 1) {
                set_xmm128(ctx, dest, apply_avx_pshufd128(source, imm8));
            }
            else if (mandatory_prefix == 2) {
                set_xmm128(ctx, dest, apply_sse2_pshufhw128(source, imm8));
            }
            else {
                set_xmm128(ctx, dest, apply_sse2_pshuflw128(source, imm8));
            }
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0xDA || opcode == 0xDE || opcode == 0xEA || opcode == 0xEE) {
        if (mandatory_prefix != 1) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (is_256) {
            AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx_int_minmax256(opcode, lhs, rhs));
        }
        else {
            XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            XMMRegister rhs = read_sse2_pack_source_operand(ctx, &inst);
            set_xmm128(ctx, dest, apply_avx_int_minmax128(opcode, lhs, rhs));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0xD4 || opcode == 0xD5 || opcode == 0xD8 || opcode == 0xD9 || opcode == 0xDC || opcode == 0xDD || opcode == 0xE0 || opcode == 0xE3 || opcode == 0xE4 || opcode == 0xE5 || opcode == 0xE8 || opcode == 0xE9 || opcode == 0xEC || opcode == 0xED || opcode == 0xF4 || opcode == 0xF5 || opcode == 0xF6 || opcode == 0xF8 || opcode == 0xF9 || opcode == 0xFA || opcode == 0xFB || opcode == 0xFC || opcode == 0xFD || opcode == 0xFE) {
        if (mandatory_prefix != 1) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (is_256) {
            AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
            set_ymm256(ctx, dest, apply_avx_int_arith256(opcode, lhs, rhs));
        }
        else {
            XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            XMMRegister rhs = read_sse2_int_arith_source_operand(ctx, &inst);
            set_xmm128(ctx, dest, apply_avx_int_arith128(opcode, lhs, rhs));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0x71 || opcode == 0x72 || opcode == 0x73 || opcode == 0xD1 || opcode == 0xD2 || opcode == 0xD3 || opcode == 0xE1 || opcode == 0xE2 || opcode == 0xF1 || opcode == 0xF2 || opcode == 0xF3) {
        if (mandatory_prefix != 1) {
            raise_ud();
        }
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        uint64_t count = 0;
        uint8_t group = (inst.modrm >> 3) & 0x07;
        if (opcode == 0x71 || opcode == 0x72 || opcode == 0x73) {
            bool is_shift_dq = opcode == 0x73 && (group == 3 || group == 7);
            int dest = is_shift_dq ? avx_vex_source1_index(&prefix) : avx_vex_rm_index(ctx, inst.modrm);
            count = (uint8_t)inst.immediate;
            if (is_256) {
                AVXRegister256 src = is_shift_dq ? get_ymm256(ctx, avx_vex_rm_index(ctx, inst.modrm))
                                                 : get_ymm256(ctx, avx_vex_source1_index(&prefix));
                set_ymm256(ctx, dest, apply_avx_shift256(opcode, group, src, count));
            }
            else {
                XMMRegister src = is_shift_dq ? get_xmm128(ctx, avx_vex_rm_index(ctx, inst.modrm))
                                              : get_xmm128(ctx, avx_vex_source1_index(&prefix));
                set_xmm128(ctx, dest, apply_avx_shift128(opcode, group, src, count));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }

        int dest = avx_vex_dest_index(ctx, inst.modrm);
        XMMRegister count_source = read_sse2_shift_source_operand(ctx, &inst);
        count = count_source.low;
        if (is_256) {
            AVXRegister256 src = get_ymm256(ctx, avx_vex_source1_index(&prefix));
            set_ymm256(ctx, dest, apply_avx_shift256(opcode, group, src, count));
        }
        else {
            XMMRegister src = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_shift128(opcode, group, src, count));
            clear_ymm_upper128(ctx, dest);
        }
        return;
    }

    if (opcode == 0x51 || opcode == 0x52 || opcode == 0x53) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_unary_math256(opcode, read_avx_rm256(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_unary_math128(opcode, read_sse_math_packed_source(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (opcode != 0x51) {
                raise_ud();
            }
            if (avx_vex_requires_reserved_vvvv(&prefix)) {
                raise_ud();
            }
            if (is_256) {
                set_ymm256(ctx, dest, apply_avx_sqrt_pd256(read_avx_rm256(ctx, &inst)));
            }
            else {
                set_xmm128(ctx, dest, apply_avx_sqrt_pd128(read_sse2_math_pd_packed_source(ctx, &inst)));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 2) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_unary_math_ss128(opcode, src1, read_sse_math_scalar_source_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 3) {
            if (opcode != 0x51) {
                raise_ud();
            }
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_sqrt_sd128(src1, read_sse2_math_pd_scalar_source_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x5D || opcode == 0x5F) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_minmax256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_math_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_minmax128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_minmax_pd256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_math_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_minmax_pd128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 2) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_minmax_ss128(opcode, src1, read_sse_math_scalar_source_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_minmax_sd128(opcode, src1, read_sse2_math_pd_scalar_source_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x58 || opcode == 0x59 || opcode == 0x5C || opcode == 0x5E) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 0) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_arith256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_arith_source_operand(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_arith128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_arith_pd256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_arith_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_arith_pd128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 2) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_arith_ss128(opcode, src1, sse_convert_read_scalar_float_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                raise_ud();
            }
            XMMRegister src1 = get_xmm128(ctx, avx_vex_source1_index(&prefix));
            set_xmm128(ctx, dest, apply_avx_arith_sd128(opcode, src1, read_sse2_arith_pd_scalar_source_bits(ctx, &inst)));
            clear_ymm_upper128(ctx, dest);
            return;
        }
        raise_ud();
    }

    if (opcode == 0x7C || opcode == 0x7D) {
        DecodedInstruction inst = decode_avx_vex_modrm(ctx, code, code_size, &prefix);
        int dest = avx_vex_dest_index(ctx, inst.modrm);
        if (mandatory_prefix == 1) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_haddhsub_pd256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse2_math_pd_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_haddhsub_pd128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        if (mandatory_prefix == 3) {
            if (is_256) {
                AVXRegister256 lhs = get_ymm256(ctx, avx_vex_source1_index(&prefix));
                AVXRegister256 rhs = read_avx_rm256(ctx, &inst);
                set_ymm256(ctx, dest, apply_avx_haddhsub_ps256(opcode, lhs, rhs));
            }
            else {
                XMMRegister lhs = get_xmm128(ctx, avx_vex_source1_index(&prefix));
                XMMRegister rhs = read_sse_math_packed_source(ctx, &inst);
                set_xmm128(ctx, dest, apply_avx_haddhsub_ps128(opcode, lhs, rhs));
                clear_ymm_upper128(ctx, dest);
            }
            return;
        }
        raise_ud();
    }

    raise_ud();
}


