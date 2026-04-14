// instrusments/call.hpp - CALL instruction implementation

// --- CALL execution helpers ---

// E8 cd - Near relative CALL (64-bit mode: rel32 sign-extended to 64 bits)
void call_near_rel64(CPU_CONTEXT* ctx, int32_t rel32, uint64_t return_rip) {
    uint64_t target = return_rip + (int64_t)rel32;
    push_value64(ctx, return_rip);
    ctx->rip = target;
}

// E8 cd - Near relative CALL (32-bit mode: rel32)
void call_near_rel32(CPU_CONTEXT* ctx, int32_t rel32, uint64_t return_rip) {
    uint32_t target = (uint32_t)((uint32_t)return_rip + (uint32_t)rel32);
    push_value32(ctx, (uint32_t)return_rip);
    ctx->rip = target;
}

// E8 cw - Near relative CALL (16-bit mode: rel16)
void call_near_rel16(CPU_CONTEXT* ctx, int16_t rel16, uint64_t return_rip) {
    uint16_t target = (uint16_t)((uint16_t)return_rip + (uint16_t)rel16);
    push_value16(ctx, (uint16_t)return_rip);
    ctx->rip = target;
}

// FF /2 - Near absolute indirect CALL (64-bit operand)
void call_near_abs64(CPU_CONTEXT* ctx, uint64_t target, uint64_t return_rip) {
    push_value64(ctx, return_rip);
    ctx->rip = target;
}

// FF /2 - Near absolute indirect CALL (32-bit operand)
void call_near_abs32(CPU_CONTEXT* ctx, uint32_t target, uint64_t return_rip) {
    push_value32(ctx, (uint32_t)return_rip);
    ctx->rip = target;
}

// FF /2 - Near absolute indirect CALL (16-bit operand)
void call_near_abs16(CPU_CONTEXT* ctx, uint16_t target, uint64_t return_rip) {
    push_value16(ctx, (uint16_t)return_rip);
    ctx->rip = target;
}

// FF /3 - Far indirect CALL via memory (same privilege level, near-simplified)
// In 64-bit mode: reads m16:16, m16:32, or m16:64 from memory
// Pushes old CS and RIP, loads new CS:RIP
void call_far_mem(CPU_CONTEXT* ctx, uint64_t mem_addr, int operand_size, uint64_t return_rip) {
    uint64_t new_offset;
    uint16_t new_selector;

    // Read offset then selector from memory (little-endian: offset first, then selector)
    if (operand_size == 64) {
        new_offset = read_memory_qword(ctx, mem_addr);
        new_selector = read_memory_word(ctx, mem_addr + 8);
    }
    else if (operand_size == 32) {
        new_offset = read_memory_dword(ctx, mem_addr);
        new_selector = read_memory_word(ctx, mem_addr + 4);
    }
    else {
        // 16-bit
        new_offset = read_memory_word(ctx, mem_addr);
        new_selector = read_memory_word(ctx, mem_addr + 2);
    }

    // Validate new selector
    if (is_null_selector(new_selector)) {
        raise_gp_ctx(ctx, 0);
    }

    // Load the descriptor for the new code segment
    SegmentDescriptor new_desc = load_descriptor_from_table(ctx, new_selector);

    // In 64-bit mode (IA32_EFER.LMA=1): must be a code segment or 64-bit call gate
    // We handle code segment case here; call gates are not implemented
    if (!is_code_segment(new_desc.type)) {
        // Could be a call gate - but we only handle code segments
        raise_gp_ctx(ctx, new_selector & 0xFFFC);
    }

    // L=1 and D=1 is invalid in long mode
    if (cpu_long_mode_active(ctx) && new_desc.long_mode && new_desc.db) {
        raise_gp_ctx(ctx, new_selector & 0xFFFC);
    }

    uint8_t rpl = new_selector & 0x03;

    if (is_conforming_code_segment(new_desc.type)) {
        // Conforming: DPL must not be greater than CPL
        if (new_desc.dpl > ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
    }
    else {
        // Non-conforming: DPL must equal CPL, RPL must not be greater than CPL
        if (new_desc.dpl != ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
        if (rpl > ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
    }

    if (!new_desc.present) {
        raise_np_ctx(ctx, new_selector & 0xFFFC);
    }

    // Truncate offset based on target mode
    if (new_desc.long_mode) {
        // 64-bit target: offset is full 64-bit (or sign-extended 32-bit)
        // Already read as appropriate size above
    }
    else {
        // Compatibility mode target
        if (operand_size == 16) {
            new_offset &= 0xFFFF;
        }
        else {
            new_offset &= 0xFFFFFFFF;
        }
    }

    // Push old CS and RIP (return address)
    // In 64-bit mode, CS is pushed as 64-bit (padded with 48 high-order bits of 0)
    if (cpu_is_64bit_code(ctx)) {
        push_value64(ctx, (uint64_t)ctx->cs.selector);
        push_value64(ctx, return_rip);
    }
    else if (operand_size == 32) {
        push_value32(ctx, (uint32_t)ctx->cs.selector);
        push_value32(ctx, (uint32_t)return_rip);
    }
    else {
        push_value16(ctx, ctx->cs.selector);
        push_value16(ctx, (uint16_t)return_rip);
    }

    // Load new CS
    ctx->cs.selector = (new_selector & 0xFFFC) | ctx->cpl; // CS.RPL = CPL
    ctx->cs.descriptor = new_desc;

    // Load new RIP
    ctx->rip = new_offset;
}

// 9A - Far direct CALL (ptr16:16 or ptr16:32) - not valid in 64-bit mode
void call_far_ptr(CPU_CONTEXT* ctx, uint16_t new_selector, uint32_t new_offset, int operand_size, uint64_t return_rip) {
    // Validate new selector
    if (is_null_selector(new_selector)) {
        raise_gp_ctx(ctx, 0);
    }

    SegmentDescriptor new_desc = load_descriptor_from_table(ctx, new_selector);

    if (!is_code_segment(new_desc.type)) {
        raise_gp_ctx(ctx, new_selector & 0xFFFC);
    }

    uint8_t rpl = new_selector & 0x03;

    if (is_conforming_code_segment(new_desc.type)) {
        if (new_desc.dpl > ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
    }
    else {
        if (new_desc.dpl != ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
        if (rpl > ctx->cpl) {
            raise_gp_ctx(ctx, new_selector & 0xFFFC);
        }
    }

    if (!new_desc.present) {
        raise_np_ctx(ctx, new_selector & 0xFFFC);
    }

    uint32_t target_eip = new_offset;
    if (operand_size == 16) {
        target_eip &= 0xFFFF;
    }

    // Push old CS and EIP
    if (operand_size == 32) {
        push_value32(ctx, (uint32_t)ctx->cs.selector);
        push_value32(ctx, (uint32_t)return_rip);
    }
    else {
        push_value16(ctx, ctx->cs.selector);
        push_value16(ctx, (uint16_t)return_rip);
    }

    // Load new CS
    ctx->cs.selector = (new_selector & 0xFFFC) | ctx->cpl;
    ctx->cs.descriptor = new_desc;

    // Load new EIP
    ctx->rip = target_eip;
}

// --- Helper: decode ModR/M for CALL ---

void decode_modrm_call(CPU_CONTEXT* ctx, DecodedInstruction* inst, uint8_t* code, size_t code_size, size_t* offset) {
    if (*offset >= code_size) {
        raise_gp_ctx(ctx, 0);
    }
    inst->has_modrm = true;
    inst->modrm = code[(*offset)++];

    uint8_t mod = (inst->modrm >> 6) & 0x03;
    uint8_t rm = inst->modrm & 0x07;

    // SIB byte present when rm==4 and mod!=3 (not in 16-bit addressing)
    if (mod != 3 && rm == 4 && inst->address_size != 16) {
        if (*offset >= code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst->has_sib = true;
        inst->sib = code[(*offset)++];
    }

    // Determine displacement size
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

// --- CALL instruction decoder ---

DecodedInstruction decode_call_instruction(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = {};
    size_t offset = 0;

    cpu_reset_prefix_state(ctx);

    // Decode prefixes
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
            // LOCK prefix is #UD for CALL
            raise_ud_ctx(ctx);
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

    // In 64-bit mode, near branches (E8, FF /2) force operand size to 64
    // 0x66 can demote to 16 for E8 rel16 (but it's N.S. - not supported)
    // For FF /2 in 64-bit mode, operand size is forced to 64
    // For FF /3 (far call), operand size follows normal rules
    if (cpu_is_64bit_code(ctx)) {
        // Default for near calls is 64 in 64-bit mode
        if (ctx->operand_size_override) {
            inst.operand_size = 16;
        }
        else {
            inst.operand_size = 64;
        }
    }
    else {
        inst.operand_size = 32;
        if (ctx->rex_w) {
            inst.operand_size = 64;
        }
        else if (ctx->operand_size_override) {
            inst.operand_size = 16;
        }
    }

    // Determine address size
    inst.address_size = ctx->address_size_override
        ? (cpu_default_address_size(ctx) == 64 ? 32 : 16)
        : cpu_default_address_size(ctx);

    switch (inst.opcode) {
    // E8 cw/cd - CALL rel16/rel32
    case 0xE8:
        if (cpu_is_64bit_code(ctx)) {
            // In 64-bit mode: always rel32, sign-extended to 64 bits
            // 0x66 prefix makes it rel16 but it's N.S. (not supported);
            // we still handle it for completeness
            if (ctx->operand_size_override) {
                inst.imm_size = 2;
                inst.operand_size = 16;
            }
            else {
                inst.imm_size = 4;
                inst.operand_size = 64;
            }
        }
        else {
            if (ctx->operand_size_override) {
                inst.imm_size = 2;
                inst.operand_size = 16;
            }
            else {
                inst.imm_size = 4;
                inst.operand_size = 32;
            }
        }
        if (offset + inst.imm_size > code_size) {
            raise_gp_ctx(ctx, 0);
        }
        inst.immediate = 0;
        for (int i = 0; i < inst.imm_size; i++) {
            inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
        }
        break;

    // FF /2 - CALL r/m16, r/m32, r/m64 (near absolute indirect)
    // FF /3 - CALL m16:16, m16:32, m16:64 (far absolute indirect)
    case 0xFF:
        decode_modrm_call(ctx, &inst, code, code_size, &offset);
        {
            uint8_t reg_field = (inst.modrm >> 3) & 0x07;
            if (reg_field == 2) {
                // Near indirect call FF /2
                // In 64-bit mode, operand size is forced to 64
                if (cpu_is_64bit_code(ctx)) {
                    inst.operand_size = 64;
                }
            }
            else if (reg_field == 3) {
                // Far indirect call FF /3
                uint8_t mod = (inst.modrm >> 6) & 0x03;
                // Far call must use memory operand (mod != 3)
                if (mod == 3) {
                    raise_ud_ctx(ctx);
                }
                // In 64-bit mode: if REX.W, operand size = 64 (m16:64)
                // otherwise if 0x66, operand size = 16 (m16:16)
                // otherwise operand size = 32 (m16:32)
                if (cpu_is_64bit_code(ctx)) {
                    if (ctx->rex_w) {
                        inst.operand_size = 64;
                    }
                    else if (ctx->operand_size_override) {
                        inst.operand_size = 16;
                    }
                    else {
                        inst.operand_size = 32;
                    }
                }
            }
            else {
                raise_ud_ctx(ctx);
            }
        }
        break;

    // 9A cd/cp - CALL ptr16:16 / CALL ptr16:32 (invalid in 64-bit mode)
    case 0x9A:
        if (cpu_is_64bit_code(ctx)) {
            raise_ud_ctx(ctx);
        }
        // Read immediate far pointer: offset then selector
        if (inst.operand_size == 32) {
            // ptr16:32 - 6 bytes: 4-byte offset + 2-byte selector
            if (offset + 6 > code_size) {
                raise_gp_ctx(ctx, 0);
            }
            inst.immediate = 0;
            for (int i = 0; i < 4; i++) {
                inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.mem_address = 0;
            for (int i = 0; i < 2; i++) {
                inst.mem_address |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.imm_size = 6;
        }
        else {
            // ptr16:16 - 4 bytes: 2-byte offset + 2-byte selector
            if (offset + 4 > code_size) {
                raise_gp_ctx(ctx, 0);
            }
            inst.immediate = 0;
            for (int i = 0; i < 2; i++) {
                inst.immediate |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.mem_address = 0;
            for (int i = 0; i < 2; i++) {
                inst.mem_address |= ((uint64_t)code[offset++]) << (i * 8);
            }
            inst.imm_size = 4;
        }
        break;

    default:
        raise_ud_ctx(ctx);
    }

    inst.inst_size = (int)offset;
    finalize_rip_relative_address(ctx, &inst, (int)offset);
    ctx->last_inst_size = (int)offset;
    return inst;
}

// --- CALL instruction executor ---

void execute_call(CPU_CONTEXT* ctx, uint8_t* code, size_t code_size) {
    DecodedInstruction inst = decode_call_instruction(ctx, code, code_size);

    // Return address = RIP pointing to next instruction after this CALL
    uint64_t return_rip = ctx->rip + inst.inst_size;

    switch (inst.opcode) {
    // E8 cw/cd - CALL rel16/rel32 (near relative)
    case 0xE8:
        if (inst.operand_size == 64) {
            // 64-bit mode: rel32 sign-extended to 64
            call_near_rel64(ctx, (int32_t)(uint32_t)inst.immediate, return_rip);
        }
        else if (inst.operand_size == 32) {
            call_near_rel32(ctx, (int32_t)(uint32_t)inst.immediate, return_rip);
        }
        else {
            // 16-bit
            call_near_rel16(ctx, (int16_t)(uint16_t)inst.immediate, return_rip);
        }
        break;

    // FF /2 - CALL r/m16 / r/m32 / r/m64 (near absolute indirect)
    // FF /3 - CALL m16:16 / m16:32 / m16:64 (far absolute indirect)
    case 0xFF: {
        uint8_t reg_field = (inst.modrm >> 3) & 0x07;
        uint8_t mod = (inst.modrm >> 6) & 0x03;
        uint8_t rm = inst.modrm & 0x07;

        if (ctx->rex_b) {
            rm |= 0x08;
        }

        if (reg_field == 2) {
            // Near absolute indirect
            uint64_t target;
            if (inst.operand_size == 64) {
                if (mod == 3) {
                    target = get_reg64(ctx, rm);
                }
                else {
                    target = read_memory_qword(ctx, inst.mem_address);
                }
                call_near_abs64(ctx, target, return_rip);
            }
            else if (inst.operand_size == 32) {
                if (mod == 3) {
                    target = get_reg32(ctx, rm);
                }
                else {
                    target = read_memory_dword(ctx, inst.mem_address);
                }
                call_near_abs32(ctx, (uint32_t)target, return_rip);
            }
            else {
                // 16-bit
                if (mod == 3) {
                    target = get_reg16(ctx, rm);
                }
                else {
                    target = read_memory_word(ctx, inst.mem_address);
                }
                call_near_abs16(ctx, (uint16_t)target, return_rip);
            }
        }
        else {
            // reg_field == 3: Far absolute indirect via memory
            // mod == 3 was already rejected in decoder
            call_far_mem(ctx, inst.mem_address, inst.operand_size, return_rip);
        }
        break;
    }

    // 9A cd/cp - CALL ptr16:16 / ptr16:32 (far direct, not valid in 64-bit mode)
    case 0x9A: {
        uint16_t new_selector = (uint16_t)inst.mem_address;
        uint32_t new_offset = (uint32_t)inst.immediate;
        call_far_ptr(ctx, new_selector, new_offset, inst.operand_size, return_rip);
        break;
    }
    }
}
