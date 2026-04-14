// instrusments/leave.hpp - LEAVE instruction implementation

int decode_leave_operand_size(CPU_CONTEXT* ctx) {
    if (cpu_is_64bit_code(ctx)) {
        return ctx->operand_size_override ? 16 : 64;
    }
    return ctx->operand_size_override ? 16 : 32;
}

int decode_leave_stack_addr_size(CPU_CONTEXT* ctx) {
    if (cpu_is_64bit_code(ctx)) {
        return ctx->address_size_override ? 32 : 64;
    }

    int default_size = ctx->ss.descriptor.db ? 32 : 16;
    if (ctx->address_size_override) {
        return default_size == 32 ? 16 : 32;
    }
    return default_size;
}

void leave_set_stack_pointer(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 64) {
        ctx->regs[REG_RSP] = ctx->regs[REG_RBP];
    }
    else if (stack_addr_size == 32) {
        uint32_t ebp = (uint32_t)(ctx->regs[REG_RBP] & 0xFFFFFFFF);
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | ebp;
    }
    else {
        uint16_t bp = (uint16_t)(ctx->regs[REG_RBP] & 0xFFFF);
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | bp;
    }
}

uint16_t leave_pop16(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 64) {
        uint16_t value = read_memory_word(ctx, ctx->regs[REG_RSP]);
        ctx->regs[REG_RSP] += 2;
        return value;
    }
    if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        uint16_t value = read_memory_word(ctx, esp);
        esp += 2;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
        return value;
    }

    uint16_t sp = (uint16_t)(ctx->regs[REG_RSP] & 0xFFFF);
    uint16_t value = read_memory_word(ctx, sp);
    sp += 2;
    ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | sp;
    return value;
}

uint32_t leave_pop32(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 64) {
        uint32_t value = read_memory_dword(ctx, ctx->regs[REG_RSP]);
        ctx->regs[REG_RSP] += 4;
        return value;
    }
    if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        uint32_t value = read_memory_dword(ctx, esp);
        esp += 4;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
        return value;
    }

    uint16_t sp = (uint16_t)(ctx->regs[REG_RSP] & 0xFFFF);
    uint32_t value = read_memory_dword(ctx, sp);
    sp += 4;
    ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFULL) | sp;
    return value;
}

uint64_t leave_pop64(CPU_CONTEXT* ctx, int stack_addr_size) {
    if (stack_addr_size == 64) {
        uint64_t value = read_memory_qword(ctx, ctx->regs[REG_RSP]);
        ctx->regs[REG_RSP] += 8;
        return value;
    }
    if (stack_addr_size == 32) {
        uint32_t esp = (uint32_t)(ctx->regs[REG_RSP] & 0xFFFFFFFF);
        uint64_t value = read_memory_qword(ctx, esp);
        esp += 8;
        ctx->regs[REG_RSP] = (ctx->regs[REG_RSP] & ~0xFFFFFFFFULL) | esp;
        return value;
    }

    raise_gp_ctx(ctx, 0);
    return 0;
}

DecodedInstruction decode_leave_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
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
    if (inst.opcode != 0xC9) {
        raise_ud_ctx(ctx);
    }

    if (has_lock_prefix) {
        raise_ud_ctx(ctx);
    }

    inst.operand_size = decode_leave_operand_size(ctx);
    inst.address_size = decode_leave_stack_addr_size(ctx);
    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

void execute_leave(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_leave_instruction(ctx, code, code_size);

    leave_set_stack_pointer(ctx, inst.address_size);

    if (inst.operand_size == 16) {
        set_reg16(ctx, REG_RBP, leave_pop16(ctx, inst.address_size));
    }
    else if (inst.operand_size == 32) {
        set_reg32(ctx, REG_RBP, leave_pop32(ctx, inst.address_size));
    }
    else {
        set_reg64(ctx, REG_RBP, leave_pop64(ctx, inst.address_size));
    }
}
