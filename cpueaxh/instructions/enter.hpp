// instrusments/enter.hpp - ENTER instruction implementation

int decode_enter_operand_size(CPU_CONTEXT* ctx) {
    if (cpu_is_64bit_code(ctx)) {
        return ctx->operand_size_override ? 16 : 64;
    }
    return ctx->operand_size_override ? 16 : 32;
}

int decode_enter_stack_addr_size(CPU_CONTEXT* ctx) {
    if (cpu_is_64bit_code(ctx)) {
        return ctx->address_size_override ? 32 : 64;
    }

    int default_size = ctx->ss.descriptor.db ? 32 : 16;
    if (ctx->address_size_override) {
        return default_size == 32 ? 16 : 32;
    }
    return default_size;
}

uint64_t enter_get_stack_pointer_value(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 64) {
        return ctx->regs[REG_RSP];
    }
    if (stack_addr_size == 32) {
        return (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFFULL);
    }
    return (uint16_t)(ctx->regs[REG_RSP] & 0xFFFFULL);
}

void enter_set_stack_pointer_value(CPU_CONTEXT* ctx, int stack_addr_size, uint64_t value) {
    if (stack_addr_size == 64) {
        ctx->regs[REG_RSP] = value;
    }
    else if (stack_addr_size == 32) {
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | (uint32_t)value;
    }
    else {
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | (uint16_t)value;
    }
}

uint64_t enter_get_frame_pointer_address(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 64) {
        return ctx->regs[REG_RBP];
    }
    if (stack_addr_size == 32) {
        return (uint32_t)(ctx->regs[REG_RBP] & 0xFFFFFFFFULL);
    }
    return (uint16_t)(ctx->regs[REG_RBP] & 0xFFFFULL);
}

void enter_push16(CPU_CONTEXT* ctx, int stack_addr_size, uint16_t value) {
    uint64_t sp = enter_get_stack_pointer_value(ctx, stack_addr_size);
    sp -= 2;
    enter_set_stack_pointer_value(ctx, stack_addr_size, sp);
    write_memory_word(ctx, enter_get_stack_pointer_value(ctx, stack_addr_size), value);
}

void enter_push32(CPU_CONTEXT* ctx, int stack_addr_size, uint32_t value) {
    uint64_t sp = enter_get_stack_pointer_value(ctx, stack_addr_size);
    sp -= 4;
    enter_set_stack_pointer_value(ctx, stack_addr_size, sp);
    write_memory_dword(ctx, enter_get_stack_pointer_value(ctx, stack_addr_size), value);
}

void enter_push64(CPU_CONTEXT* ctx, int stack_addr_size, uint64_t value) {
    if (stack_addr_size == 16) {
        raise_gp_ctx(ctx, 0);
    }

    uint64_t sp = enter_get_stack_pointer_value(ctx, stack_addr_size);
    sp -= 8;
    enter_set_stack_pointer_value(ctx, stack_addr_size, sp);
    write_memory_qword(ctx, enter_get_stack_pointer_value(ctx, stack_addr_size), value);
}

uint16_t enter_read_frame16(CPU_CONTEXT* ctx, int stack_addr_size) {
    return read_memory_word(ctx, enter_get_frame_pointer_address(ctx, stack_addr_size));
}

uint32_t enter_read_frame32(CPU_CONTEXT* ctx, int stack_addr_size) {
    return read_memory_dword(ctx, enter_get_frame_pointer_address(ctx, stack_addr_size));
}

uint64_t enter_read_frame64(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 16) {
        raise_gp_ctx(ctx, 0);
    }
    return read_memory_qword(ctx, enter_get_frame_pointer_address(ctx, stack_addr_size));
}

DecodedInstruction decode_enter_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;

    cpu_reset_prefix_state(ctx);

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
        else if (cpu_try_apply_rex_prefix(ctx, prefix)) {
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
    if (inst.opcode != 0xC8) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    if (offset + 3 > code_size) {
        raise_gp_ctx(ctx, 0);
    }

    uint16_t alloc_size = (uint16_t)code[offset] | ((uint16_t)code[offset + 1] << 8);
    uint8_t nesting_level = code[offset + 2];
    offset += 3;

    inst.immediate = (uint64_t)alloc_size | ((uint64_t)nesting_level << 16);
    inst.imm_size = 3;
    inst.operand_size = decode_enter_operand_size(ctx);
    inst.address_size = decode_enter_stack_addr_size(ctx);
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_enter(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_enter_instruction(ctx, code, code_size);
    uint16_t alloc_size = (uint16_t)(inst.immediate & 0xFFFFULL);
    uint8_t nesting_level = (uint8_t)((inst.immediate >> 16) & 0x1FULL);

    if (inst.operand_size == 16) {
        enter_push16(ctx, inst.address_size, get_reg16(ctx, REG_RBP));
    }
    else if (inst.operand_size == 32) {
        enter_push32(ctx, inst.address_size, get_reg32(ctx, REG_RBP));
    }
    else {
        enter_push64(ctx, inst.address_size, get_reg64(ctx, REG_RBP));
    }

    uint64_t frame_temp = enter_get_stack_pointer_value(ctx, inst.address_size);

    if (nesting_level > 0) {
        for (uint8_t level = 1; level < nesting_level; level++) {
            if (inst.operand_size == 16) {
                uint16_t frame = get_reg16(ctx, REG_RBP);
                frame = (uint16_t)(frame - 2);
                set_reg16(ctx, REG_RBP, frame);
                enter_push16(ctx, inst.address_size, enter_read_frame16(ctx, inst.address_size));
            }
            else if (inst.operand_size == 32) {
                uint32_t frame = get_reg32(ctx, REG_RBP);
                frame -= 4;
                set_reg32(ctx, REG_RBP, frame);
                enter_push32(ctx, inst.address_size, enter_read_frame32(ctx, inst.address_size));
            }
            else {
                set_reg64(ctx, REG_RBP, get_reg64(ctx, REG_RBP) - 8);
                enter_push64(ctx, inst.address_size, enter_read_frame64(ctx, inst.address_size));
            }
        }

        if (inst.operand_size == 16) {
            enter_push16(ctx, inst.address_size, (uint16_t)frame_temp);
        }
        else if (inst.operand_size == 32) {
            enter_push32(ctx, inst.address_size, (uint32_t)frame_temp);
        }
        else {
            enter_push64(ctx, inst.address_size, frame_temp);
        }
    }

    if (inst.operand_size == 16) {
        set_reg16(ctx, REG_RBP, (uint16_t)frame_temp);
    }
    else if (inst.operand_size == 32) {
        set_reg32(ctx, REG_RBP, (uint32_t)frame_temp);
    }
    else {
        set_reg64(ctx, REG_RBP, frame_temp);
    }

    uint64_t stack_ptr = enter_get_stack_pointer_value(ctx, inst.address_size);
    enter_set_stack_pointer_value(ctx, inst.address_size, stack_ptr - alloc_size);
}
