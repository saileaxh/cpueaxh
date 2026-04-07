// instrusments/pcmpestrm.hpp - PCMPESTRM instruction implementation

static int decode_pcmpestrm_segment_override(uint8_t prefix) {
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

static uint64_t pcmpestrm_segment_base(CPU_CONTEXT* ctx, int segment_index) {
    return cpu_segment_base_for_addressing(ctx, segment_index);
}

static int decode_pcmpestrm_xmm_reg_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int reg = (modrm >> 3) & 0x07;
    if (ctx->rex_r) {
        reg |= 0x08;
    }
    return reg;
}

static int decode_pcmpestrm_xmm_rm_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int rm = modrm & 0x07;
    if (ctx->rex_b) {
        rm |= 0x08;
    }
    return rm;
}

static void decode_modrm_pcmpestrm(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
    if (*offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }

    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    uint8_t rm = inst->modrm & 0x07;

    if (mod != 3 && rm == 4 && inst->address_size != 16) {
        if (*offset >= code_size) {
            raise_gp_ctx(ctx, 0);
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
            raise_gp_ctx(ctx, 0);
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

static XMMRegister read_pcmpestrm_source(CPU_CONTEXT* ctx, const DecodedInstruction* inst) {
    if (((inst->modrm >> 6) & 0x03) == 3) {
        return get_xmm128(ctx, decode_pcmpestrm_xmm_rm_index(ctx, inst->modrm));
    }
    return read_xmm_memory(ctx, inst->mem_address);
}

bool is_pcmpestrm_instruction(const uint8_t* code, int len, int prefix_len) {
    if (prefix_len + 3 >= len) {
        return false;
    }
    return code[prefix_len] == 0x0F && code[prefix_len + 1] == 0x3A && code[prefix_len + 2] == 0x60;
}

DecodedInstruction decode_pcmpestrm_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;
    int segment_index = SEG_DS;
    bool has_segment_override = false;
    bool has_lock_prefix = false;
    uint8_t mandatory_prefix = 0;
    bool has_unsupported_prefix = false;

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
                has_unsupported_prefix = true;
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
        else if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E || prefix == 0x64 || prefix == 0x65) {
            int seg_override = decode_pcmpestrm_segment_override(prefix);
            if (seg_override >= 0) {
                segment_index = seg_override;
                has_segment_override = true;
            }
            offset++;
        }
        else {
            break;
        }
    }

    if (has_lock_prefix || has_unsupported_prefix || mandatory_prefix != 0x66) {
        raise_ud_ctx(ctx);
    }

    if (offset + 4 > code_size) {
        raise_gp_ctx(ctx, 0);
    }

    if (code[offset++] != 0x0F || code[offset++] != 0x3A) {
        raise_ud_ctx(ctx);
    }

    inst.opcode = code[offset++];
    if (inst.opcode != 0x60) {
        raise_ud_ctx(ctx);
    }

    if (ctx->cs.descriptor.long_mode) {
        inst.address_size = ctx->address_size_override ? 32 : 64;
    }
    else {
        inst.address_size = ctx->address_size_override ? 16 : 32;
    }

    decode_modrm_pcmpestrm(ctx, &inst, code, code_size, &offset);

    inst.imm_size = 1;
    if (offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }
    inst.immediate = code[offset++];

    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);

    if (inst.has_modrm && has_segment_override) {
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        if (mod != 3) {
            const int default_segment = cpu_default_segment_for_memory_operand(ctx, inst.modrm, inst.has_sib, inst.sib, inst.address_size);
            inst.mem_address += pcmpestrm_segment_base(ctx, segment_index) - pcmpestrm_segment_base(ctx, default_segment);
            cpu_validate_linear_address(ctx, inst.mem_address, segment_index);
        }
    }

    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_pcmpestrm(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_pcmpestrm_instruction(ctx, code, code_size);
    int lhs_index = decode_pcmpestrm_xmm_reg_index(ctx, inst.modrm);
    __m128i lhs = avx_xmm_to_m128i(get_xmm128(ctx, lhs_index));
    __m128i rhs = avx_xmm_to_m128i(read_pcmpestrm_source(ctx, &inst));
    int length_a = avx_pcmpestr_length(ctx, REG_RAX, ctx->rex_w, (uint8_t)inst.immediate);
    int length_b = avx_pcmpestr_length(ctx, REG_RDX, ctx->rex_w, (uint8_t)inst.immediate);
    AVXPcmpstrResult result = avx_execute_pcmpestr(lhs, length_a, rhs, length_b, (uint8_t)inst.immediate);
    update_avx_pcmpstr_flags(ctx, result.cf, result.zf, result.sf, result.of);
    set_xmm128(ctx, 0, result.mask);
}