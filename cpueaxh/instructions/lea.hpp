// instrusments/lea.hpp - LEA instruction implementation

int decode_lea_reg_index(CPU_CONTEXT* ctx, uint8_t modrm) {
    int reg = (modrm >> 3) & 0x07;
    if (ctx->rex_r) {
        reg |= 0x08;
    }
    return reg;
}

uint64_t read_lea_address_reg(CPU_CONTEXT* ctx, int reg, int address_size) {
    if (address_size == 64) {
        return get_reg64(ctx, reg);
    }
    if (address_size == 32) {
        return get_reg32(ctx, reg);
    }
    return get_reg16(ctx, reg);
}

uint64_t calculate_lea_address(CPU_CONTEXT* ctx, uint8_t modrm, bool has_sib, uint8_t sib, int32_t disp, int address_size, uint64_t next_rip) {
    uint8_t mod = (modrm >> 6) & 0x03;
    uint8_t raw_rm = modrm & 0x07;

    if (mod == 3) {
        raise_ud_ctx(ctx);
    }

    if (address_size == 64 || address_size == 32) {
        bool long_mode_addr = ctx->cs.descriptor.long_mode;
        uint64_t addr = 0;

        if (has_sib) {
            uint8_t scale = (sib >> 6) & 0x03;
            uint8_t raw_index = (sib >> 3) & 0x07;
            uint8_t raw_base = sib & 0x07;

            int index = raw_index;
            int base = raw_base;
            if (long_mode_addr && ctx->rex_x) {
                index |= 0x08;
            }
            if (long_mode_addr && ctx->rex_b) {
                base |= 0x08;
            }

            bool has_index = !(raw_index == 4 && !(long_mode_addr && ctx->rex_x));
            bool no_base = (mod == 0 && raw_base == 5 && !(long_mode_addr && ctx->rex_b));

            if (no_base) {
                if (address_size == 64) {
                    addr = (uint64_t)(int64_t)(int32_t)disp;
                }
                else {
                    addr = (uint32_t)disp;
                }
            }
            else {
                addr = read_lea_address_reg(ctx, base, address_size);
            }

            if (has_index) {
                addr += read_lea_address_reg(ctx, index, address_size) << scale;
            }
        }
        else if (mod == 0 && raw_rm == 5 && !(ctx->cs.descriptor.long_mode && ctx->rex_b)) {
            if (address_size == 64) {
                addr = next_rip + (uint64_t)(int64_t)(int32_t)disp;
            }
            else {
                addr = (uint32_t)disp;
            }
        }
        else {
            int rm = raw_rm;
            if (ctx->cs.descriptor.long_mode && ctx->rex_b) {
                rm |= 0x08;
            }
            addr = read_lea_address_reg(ctx, rm, address_size);
        }

        if (mod == 1) {
            addr += (int8_t)(disp & 0xFF);
        }
        else if (mod == 2) {
            addr += (int32_t)disp;
        }

        if (address_size == 32) {
            addr &= 0xFFFFFFFFULL;
        }
        return addr;
    }

    return get_effective_offset(ctx, modrm, &sib, &disp, address_size);
}

void write_lea_reg_operand(CPU_CONTEXT* ctx, uint8_t modrm, int operand_size, uint64_t value) {
    int reg = decode_lea_reg_index(ctx, modrm);

    switch (operand_size) {
    case 16:
        set_reg16(ctx, reg, (uint16_t)value);
        break;
    case 32:
        set_reg32(ctx, reg, (uint32_t)value);
        break;
    case 64:
        set_reg64(ctx, reg, value);
        break;
    default:
        raise_ud_ctx(ctx);
    }
}

void decode_modrm_lea(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset, bool has_lock_prefix) {
    if (*offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }

    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    uint8_t rm = inst->modrm & 0x07;

    if (mod == 3) {
        raise_ud_ctx(ctx);
    }

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

    uint64_t next_rip = ctx->rip + (uint64_t)(*offset);
    inst->mem_address = calculate_lea_address(ctx, inst->modrm, inst->has_sib, inst->sib, inst->displacement, inst->address_size, next_rip);

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }
}

DecodedInstruction decode_lea_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;

    ctx->rex_present = false;
    ctx->rex_w = false;
    ctx->rex_r = false;
    ctx->rex_x = false;
    ctx->rex_b = false;
    ctx->operand_size_override = false;
    ctx->address_size_override = false;

    bool has_lock_prefix = false;

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
                 prefix == 0x64 || prefix == 0x65 || prefix == 0xF2 || prefix == 0xF3) {
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
    if (inst.opcode != 0x8D) {
        raise_ud_ctx(ctx);
    }

    inst.operand_size = 32;
    if (ctx->rex_w) {
        inst.operand_size = 64;
    }
    else if (ctx->operand_size_override) {
        inst.operand_size = 16;
    }

    if (ctx->cs.descriptor.long_mode) {
        inst.address_size = ctx->address_size_override ? 32 : 64;
    }
    else {
        inst.address_size = ctx->address_size_override ? 16 : 32;
    }

    decode_modrm_lea(ctx, &inst, code, code_size, &offset, has_lock_prefix);

    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_lea(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_lea_instruction(ctx, code, code_size);
    write_lea_reg_operand(ctx, inst.modrm, inst.operand_size, inst.mem_address);
}
