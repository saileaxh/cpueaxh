// instrusments/rep.hpp - REP/REPNE prefix implementation

uint8_t decode_rep_prefix(CPU_CONTEXT* ctx, const uint8_t* code, size_t code_size, size_t* prefix_len) {
    size_t offset = 0;
    uint8_t rep_prefix = 0;

    while (offset < code_size) {
        uint8_t prefix = code[offset];
        if (prefix == 0x26 || prefix == 0x2E || prefix == 0x36 || prefix == 0x3E ||
            prefix == 0x64 || prefix == 0x65 ||
            prefix == 0x66 || prefix == 0x67 ||
            prefix == 0xF0 ||
            cpu_try_apply_rex_prefix(ctx, prefix)) {
            offset++;
        }
        else if (prefix == 0xF2 || prefix == 0xF3) {
            rep_prefix = prefix;
            offset++;
        }
        else {
            break;
        }
    }

    if (prefix_len) {
        *prefix_len = offset;
    }
    return rep_prefix;
}

bool is_rep_string_opcode(uint8_t opcode) {
    return opcode == 0xA4 || opcode == 0xA5 ||
           opcode == 0xA6 || opcode == 0xA7 ||
           opcode == 0xAA || opcode == 0xAB ||
           opcode == 0xAC || opcode == 0xAD ||
           opcode == 0xAE || opcode == 0xAF;
}

uint64_t get_rep_count(CPU_CONTEXT* ctx, int address_size) {
    switch (address_size) {
    case 16: return get_reg16(ctx, REG_RCX);
    case 32: return get_reg32(ctx, REG_RCX);
    case 64: return get_reg64(ctx, REG_RCX);
    default: raise_ud_ctx(ctx); return 0;
    }
}

void set_rep_count(CPU_CONTEXT* ctx, int address_size, uint64_t value) {
    switch (address_size) {
    case 16:
        set_reg16(ctx, REG_RCX, (uint16_t)value);
        break;
    case 32:
        set_reg32(ctx, REG_RCX, (uint32_t)value);
        break;
    case 64:
        set_reg64(ctx, REG_RCX, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

DecodedInstruction decode_rep_target(CPU_CONTEXT* ctx, uint8_t opcode, uint8_t* code, size_t code_size) {
    switch (opcode) {
    case 0xA4:
    case 0xA5:
        return decode_movs_instruction(ctx, code, code_size);
    case 0xA6:
    case 0xA7:
        return decode_cmps_instruction(ctx, code, code_size);
    case 0xAA:
    case 0xAB:
        return decode_stos_instruction(ctx, code, code_size);
    case 0xAC:
    case 0xAD:
        return decode_lods_instruction(ctx, code, code_size);
    case 0xAE:
    case 0xAF:
        return decode_scas_instruction(ctx, code, code_size);
    default:
        raise_ud_ctx(ctx);
        return DecodedInstruction{};
    }
}

void execute_rep_iteration(CPU_CONTEXT* ctx, uint8_t opcode, uint8_t* code, size_t code_size) {
    switch (opcode) {
    case 0xA4:
    case 0xA5:
        execute_movs(ctx, code, code_size);
        break;
    case 0xA6:
    case 0xA7:
        execute_cmps(ctx, code, code_size);
        break;
    case 0xAA:
    case 0xAB:
        execute_stos(ctx, code, code_size);
        break;
    case 0xAC:
    case 0xAD:
        execute_lods(ctx, code, code_size);
        break;
    case 0xAE:
    case 0xAF:
        execute_scas(ctx, code, code_size);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

void execute_rep(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    size_t prefix_len = 0;
    cpu_reset_prefix_state(ctx);
    uint8_t rep_prefix = decode_rep_prefix(ctx, code, code_size, &prefix_len);
    if (rep_prefix != 0xF2 && rep_prefix != 0xF3) {
        raise_ud_ctx(ctx);
    }

    if (prefix_len >= code_size) {
        raise_gp_ctx(ctx, 0);
    }

    uint8_t opcode = code[prefix_len];
    if (!is_rep_string_opcode(opcode)) {
        raise_ud_ctx(ctx);
    }

    DecodedInstruction inst = decode_rep_target(ctx, opcode, code, code_size);
    uint64_t count = get_rep_count(ctx, inst.address_size);
    if (count == 0) {
        return;
    }

    bool conditional_scan = (opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF);

    while (count != 0) {
        execute_rep_iteration(ctx, opcode, code, code_size);

        count--;
        set_rep_count(ctx, inst.address_size, count);
        if (count == 0) {
            break;
        }

        if (conditional_scan) {
            bool zf = (ctx->rflags & RFLAGS_ZF) != 0;
            if (rep_prefix == 0xF3) {
                if (!zf) {
                    break;
                }
            }
            else {
                if (zf) {
                    break;
                }
            }
        }
    }
}
