// cpu/executor.hpp - Instruction fetch-decode-dispatch main loop

// Maximum bytes we fetch ahead per instruction (Intel max is 15 bytes)
#define MAX_INST_FETCH 15

// Result codes for cpu_step / cpu_run
#define CPU_STEP_OK        0
#define CPU_STEP_HALT      1   // HLT encountered
#define CPU_STEP_FETCH_ERR 2   // no bytes readable at rip
#define CPU_STEP_UD        3   // unrecognised opcode
#define CPU_STEP_EXCEPTION 4   // pending CPU exception

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Fetch up to MAX_INST_FETCH bytes from virtual memory starting at addr.
// Returns the number of bytes actually read.
static int fetch_instruction_bytes(CPU_CONTEXT* ctx, uint64_t addr, uint8_t* buf) {
    if (!ctx || !buf || cpu_has_exception(ctx)) {
        return 0;
    }

    if (cpu_can_use_page_fast_path(ctx)) {
        const bool notify_fetch = cpu_has_hook_type(ctx, CPUEAXH_HOOK_MEM_FETCH);
        int n = 0;

        while (n < MAX_INST_FETCH) {
            const uint64_t current_address = addr + (uint64_t)n;
            uint8_t* ptr = NULL;
            if (cpu_resolve_memory_access(ctx, current_address, MM_PROT_EXEC, &ptr, current_address, 1, 0) != MM_ACCESS_OK) {
                break;
            }

            size_t chunk = cpu_page_bytes_remaining(current_address);
            const size_t remaining = (size_t)(MAX_INST_FETCH - n);
            if (chunk > remaining) {
                chunk = remaining;
            }

            CPUEAXH_MEMCPY(buf + n, ptr, chunk);
            if (notify_fetch) {
                for (size_t offset = 0; offset < chunk; offset++) {
                    cpu_notify_memory_hook(ctx, CPUEAXH_HOOK_MEM_FETCH, current_address + offset, 1, buf[n + (int)offset]);
                }
            }

            n += (int)chunk;
        }
        return n;
    }

    int n = 0;
    for (int i = 0; i < MAX_INST_FETCH; i++) {
        uint8_t b = read_memory_exec_byte(ctx, addr + i);
        if (cpu_has_exception(ctx)) {
            break;
        }
        buf[i] = b;
        n++;
    }
    return n;
}

// Skip legacy prefixes and one optional REX byte.
// Returns the index of the first opcode byte in buf[].
// *prefix_len is set to that index.
// For a two-byte 0x0F escape, returns 0x0F00 | second_byte.
// Returns 0xFFFF if the buffer is truncated before any opcode byte.
static uint16_t peek_opcode(const uint8_t* buf, int len, int* prefix_len) {
    int i = 0;
    while (i < len) {
        uint8_t b = buf[i];
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x64 || b == 0x65 ||
            b == 0x66 || b == 0x67 ||
            b == 0xF0 || b == 0xF2 || b == 0xF3) {
            i++; continue;
        }
        if (b >= 0x40 && b <= 0x4F) { i++; continue; } // REX
        break;
    }
    *prefix_len = i;
    if (i >= len) return 0xFFFF;
    uint8_t opc = buf[i];
    if (opc == 0x0F && i + 1 < len)
        return (uint16_t)(0x0F00 | buf[i + 1]);
    return opc;
}

static uint8_t peek_repeat_prefix(const uint8_t* buf, int prefix_len) {
    uint8_t repeat_prefix = 0;
    for (int i = 0; i < prefix_len; i++) {
        if (buf[i] == 0xF2 || buf[i] == 0xF3) {
            repeat_prefix = buf[i];
        }
    }
    return repeat_prefix;
}

static uint8_t peek_mandatory_prefix(const uint8_t* buf, int prefix_len) {
    uint8_t mandatory_prefix = 0;
    for (int i = 0; i < prefix_len; i++) {
        if (buf[i] == 0x66 || buf[i] == 0xF2 || buf[i] == 0xF3) {
            mandatory_prefix = buf[i];
        }
    }
    return mandatory_prefix;
}

// Return true for opcodes whose execute_* handler updates ctx->rip directly
// (branches). For all others the executor advances rip by last_inst_size.
static bool is_branch_opcode(uint16_t opc) {
    // Near Jcc (0F 80-8F)
    if (opc >= 0x0F80 && opc <= 0x0F8F) return true;
    if (opc > 0x00FF) return false;
    switch (opc & 0xFF) {
    case 0x70: case 0x71: case 0x72: case 0x73:   // short Jcc
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
    case 0xE3:                                     // JRCXZ/JECXZ/JCXZ
    case 0xEB: case 0xE9: case 0xEA:              // JMP
    case 0xE8: case 0x9A:                          // CALL
    case 0xC2: case 0xC3: case 0xCA: case 0xCB:    // RET
    case 0xCF:                                     // IRET/IRETQ
    case 0xFF:                                     // CALL/JMP r/m (all reg fields)
        return true;
    default:
        return false;
    }
}

static uint8_t peek_modrm_reg_field(const uint8_t* buf, int fetched, int prefix_len, int opc_offset) {
    int pos = prefix_len + opc_offset;
    return (pos < fetched) ? (uint8_t)((buf[pos] >> 3) & 0x07) : 0xFF;
}

static bool cpu_step_may_touch_vector_state(const uint8_t* buf, int fetched, int prefix_len, uint16_t opc, uint8_t mandatory_prefix) {
    if (fetched <= 0 || !buf) {
        return false;
    }

    if (buf[0] == 0xC5 || buf[0] == 0xC4) {
        return true;
    }

    if (opc <= 0x00FF) {
        return false;
    }

    if ((opc >= 0x0F40 && opc <= 0x0F4F) ||
        (opc >= 0x0F80 && opc <= 0x0F8F) ||
        (opc >= 0x0F90 && opc <= 0x0F9F) ||
        opc == 0x0FA0 || opc == 0x0FA1 || opc == 0x0FA2 || opc == 0x0FA8 || opc == 0x0FA9 ||
        opc == 0x0FAF ||
        opc == 0x0FB0 || opc == 0x0FB1 || opc == 0x0FB6 || opc == 0x0FB7 || opc == 0x0FBC || opc == 0x0FBD || opc == 0x0FBE || opc == 0x0FBF ||
        opc == 0x0FA3 || opc == 0x0FAB || opc == 0x0FB3 || opc == 0x0FBB || opc == 0x0FBA ||
        opc == 0x0FC0 || opc == 0x0FC1 ||
        (opc >= 0x0FC8 && opc <= 0x0FCF) ||
        opc == 0x0F00 || opc == 0x0F01 || opc == 0x0F05 || opc == 0x0F20 || opc == 0x0F22 || opc == 0x0F31 || opc == 0x0F34 || opc == 0x0F1F) {
        return false;
    }

    if (opc == 0x0FC7 || opc == 0x0F1E) {
        return false;
    }

    if ((opc == 0x0F0D || opc == 0x0F18) ||
        (opc == 0x0F2B && mandatory_prefix == 0x00)) {
        return opc == 0x0F2B;
    }

    return true;
}

// ---------------------------------------------------------------------------
// cpu_step - execute one instruction at ctx->rip
// ---------------------------------------------------------------------------
static int cpu_step_with_prefetch(CPU_CONTEXT* ctx, const uint8_t* prefetched_bytes, int prefetched_size) {
    if (cpu_has_exception(ctx)) {
        return CPU_STEP_EXCEPTION;
    }

    CPU_SCALAR_SNAPSHOT saved_scalar;
    CPU_VECTOR_SNAPSHOT saved_vector;
    bool scalar_snapshot_valid = false;
    bool vector_snapshot_valid = false;
    cpu_clear_exception(ctx);

    uint8_t local_buf[MAX_INST_FETCH];
    uint8_t* buf = local_buf;
    int result_code = CPU_STEP_OK;
    int fetched = 0;
    int prefix_len = 0;
    uint16_t opc = 0;
    bool is_rdtscp = false;
    bool is_xgetbv = false;
    uint8_t last_rex_prefix = 0;
    bool rex_b_prefix = false;
    uint8_t raw_opc = 0;
    uint8_t repeat_prefix = 0;
    uint8_t mandatory_prefix = 0;
    bool branch = false;
    uint64_t rip_pre = ctx->rip;
    uint8_t group_reg = 0xFF;
    uint8_t group3_reg = 0xFF;
    uint8_t fe_reg = 0xFF;
    uint8_t ff_reg = 0xFF;
    uint8_t group2_reg = 0xFF;

    if (prefetched_bytes && prefetched_size > 0) {
        fetched = prefetched_size > MAX_INST_FETCH ? MAX_INST_FETCH : prefetched_size;
        buf = const_cast<uint8_t*>(prefetched_bytes);
    }
    else {
        fetched = fetch_instruction_bytes(ctx, ctx->rip, local_buf);
    }
    if (fetched == 0) {
        result_code = cpu_has_exception(ctx) ? CPU_STEP_EXCEPTION : CPU_STEP_FETCH_ERR;
        goto cpu_step_finish;
    }

    if (buf[0] == 0xC5 || buf[0] == 0xC4) {
        cpu_capture_scalar_snapshot(&saved_scalar, ctx);
        scalar_snapshot_valid = true;
        cpu_capture_vector_snapshot(&saved_vector, ctx);
        vector_snapshot_valid = true;

        execute_avx_vex(ctx, buf, (size_t)fetched);
        if (cpu_has_exception(ctx)) {
            result_code = CPU_STEP_EXCEPTION;
            goto cpu_step_finish;
        }
        ctx->rip = rip_pre + (uint64_t)ctx->last_inst_size;
        goto cpu_step_finish;
    }

    opc = peek_opcode(buf, fetched, &prefix_len);
    if (opc == 0xFFFF) {
        result_code = cpu_has_exception(ctx) ? CPU_STEP_EXCEPTION : CPU_STEP_FETCH_ERR;
        goto cpu_step_finish;
    }

    is_rdtscp = opc == 0x0F01 && (prefix_len + 2) < fetched && buf[prefix_len + 2] == 0xF9;
    is_xgetbv = opc == 0x0F01 && (prefix_len + 2) < fetched && buf[prefix_len + 2] == 0xD0;

    for (int prefix_index = 0; prefix_index < prefix_len; prefix_index++) {
        if (buf[prefix_index] >= 0x40 && buf[prefix_index] <= 0x4F) {
            last_rex_prefix = buf[prefix_index];
        }
    }
    rex_b_prefix = (last_rex_prefix & 0x01) != 0;

    raw_opc = (uint8_t)(opc & 0xFF);
    repeat_prefix = peek_repeat_prefix(buf, prefix_len);
    mandatory_prefix = peek_mandatory_prefix(buf, prefix_len);

    // HLT (F4) is escape-owned.
    if (opc == 0x00F4) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }
    // INT3 default escape behavior is a breakpoint exception when no user escape handles it.
    if (opc == 0x00CC) {
        raise_bp_ctx(ctx);
        result_code = CPU_STEP_EXCEPTION;
        goto cpu_step_finish;
    }
    // INT, IN, OUT, SYSCALL and SYSENTER are escape-owned and default to #UD here.
    if (opc == 0x00CD ||
        opc == 0x00E4 || opc == 0x00E5 || opc == 0x00E6 || opc == 0x00E7 ||
        opc == 0x00EC || opc == 0x00ED || opc == 0x00EE || opc == 0x00EF ||
        opc == 0x0F05 || opc == 0x0F34) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }

    branch = is_branch_opcode(opc);

    if (raw_opc == 0x80 || raw_opc == 0x81 || raw_opc == 0x83)
        group_reg = peek_modrm_reg_field(buf, fetched, prefix_len, 1);

    if (raw_opc == 0xF6 || raw_opc == 0xF7)
        group3_reg = peek_modrm_reg_field(buf, fetched, prefix_len, 1);

    if (raw_opc == 0xFE)
        fe_reg = peek_modrm_reg_field(buf, fetched, prefix_len, 1);

    if (raw_opc == 0xFF)
        ff_reg = peek_modrm_reg_field(buf, fetched, prefix_len, 1);

    if (raw_opc == 0xD0 || raw_opc == 0xD1 || raw_opc == 0xD2 || raw_opc == 0xD3 ||
        raw_opc == 0xC0 || raw_opc == 0xC1)
        group2_reg = peek_modrm_reg_field(buf, fetched, prefix_len, 1);

    if (raw_opc == 0xFF) {
        branch = (ff_reg == 2 || ff_reg == 3 || ff_reg == 4 || ff_reg == 5);
    }

    cpu_capture_scalar_snapshot(&saved_scalar, ctx);
    scalar_snapshot_valid = true;
    if (cpu_step_may_touch_vector_state(buf, fetched, prefix_len, opc, mandatory_prefix)) {
        cpu_capture_vector_snapshot(&saved_vector, ctx);
        vector_snapshot_valid = true;
    }

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------

    // RET near: C3 / C2 iw  (handled inline - branch)
    if (opc == 0x00C3) {
        ctx->last_inst_size = prefix_len + 1;
        ctx->rip = pop_value64(ctx);
        goto cpu_step_finish;
    }
    if (opc == 0x00C2) {
        if (fetched < prefix_len + 3) { raise_gp_ctx(ctx, 0); }
        if (cpu_has_exception(ctx)) {
            goto cpu_step_finish;
        }
        uint16_t imm16 = (uint16_t)buf[prefix_len + 1]
            | ((uint16_t)buf[prefix_len + 2] << 8);
        ctx->last_inst_size = prefix_len + 3;
        ctx->rip = pop_value64(ctx);
        ctx->regs[REG_RSP] += imm16;
        goto cpu_step_finish;
    }

    // RET far: CB / CA iw
    if (opc == 0x00CB || opc == 0x00CA) {
        execute_ret(ctx, buf, (size_t)fetched);
    }
    // IRET/IRETQ: CF
    else if (opc == 0x00CF) {
        execute_iret(ctx, buf, (size_t)fetched);
    }
    // Supported two-byte 0F xx opcodes (dispatch before single-byte low-byte aliases)
    else if (opc >= 0x0F40 && opc <= 0x0F4F) {
        execute_cmovcc(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F10 || opc == 0x0F11) && mandatory_prefix == 0xF3) {
        execute_movss(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F10 || opc == 0x0F11) && (mandatory_prefix == 0x66 || mandatory_prefix == 0xF2)) {
        execute_sse2_mov_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F10 || opc == 0x0F11) {
        execute_movups(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F28 || opc == 0x0F29) && mandatory_prefix == 0x66) {
        execute_sse2_mov_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F28 || opc == 0x0F29) {
        execute_movaps(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F60 || opc == 0x0F61 || opc == 0x0F62 || opc == 0x0F63 || opc == 0x0F67 ||
        opc == 0x0F68 || opc == 0x0F69 || opc == 0x0F6A || opc == 0x0F6B || opc == 0x0F6C ||
        opc == 0x0F6D) && mandatory_prefix == 0x66) {
        execute_sse2_pack(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F64 || opc == 0x0F65 || opc == 0x0F66 || opc == 0x0F74 || opc == 0x0F75 ||
        opc == 0x0F76) && mandatory_prefix == 0x66) {
        execute_sse2_int_cmp(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0FDB || opc == 0x0FDF || opc == 0x0FEB || opc == 0x0FEF) && mandatory_prefix == 0x66) {
        execute_sse2_int_logic(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F38 && is_pshufb_instruction(buf, fetched, prefix_len)) {
        execute_pshufb(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F38 && mandatory_prefix == 0x66 && is_aesenc_instruction(buf, fetched, prefix_len)) {
        execute_aesenc(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F38 && mandatory_prefix == 0x66 && is_aesenclast_instruction(buf, fetched, prefix_len)) {
        execute_aesenclast(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F71 || opc == 0x0F72 || opc == 0x0F73 ||
        opc == 0x0FD1 || opc == 0x0FD2 || opc == 0x0FD3 ||
        opc == 0x0FE1 || opc == 0x0FE2 ||
        opc == 0x0FF1 || opc == 0x0FF2 || opc == 0x0FF3) && mandatory_prefix == 0x66) {
        execute_sse2_shift(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0FD4 || opc == 0x0FD5 || opc == 0x0FD8 || opc == 0x0FD9 || opc == 0x0FDA ||
        opc == 0x0FDC || opc == 0x0FDD || opc == 0x0FDE || opc == 0x0FE0 || opc == 0x0FE3 ||
        opc == 0x0FE4 || opc == 0x0FE5 || opc == 0x0FE8 || opc == 0x0FE9 || opc == 0x0FEA ||
        opc == 0x0FEC || opc == 0x0FED || opc == 0x0FEE || opc == 0x0FF4 || opc == 0x0FF5 ||
        opc == 0x0FF6 || opc == 0x0FF8 || opc == 0x0FF9 || opc == 0x0FFA || opc == 0x0FFB ||
        opc == 0x0FFC || opc == 0x0FFD || opc == 0x0FFE) && mandatory_prefix == 0x66) {
        execute_sse2_int_arith(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F6E || opc == 0x0F7E || opc == 0x0FD6) {
        execute_movdq(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F6F || opc == 0x0F7F || opc == 0x0FD7) && (mandatory_prefix == 0x66 || mandatory_prefix == 0xF3)) {
        execute_sse2_mov_int(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FF0 || opc == 0x0FF7) {
        execute_sse2_misc(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F12 || opc == 0x0F13 || opc == 0x0F16 || opc == 0x0F17 || opc == 0x0F50) && mandatory_prefix == 0x66) {
        execute_sse2_mov_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F12 || opc == 0x0F13 || opc == 0x0F16 || opc == 0x0F17 || opc == 0x0F50) {
        execute_sse_mov_misc(ctx, buf, (size_t)fetched);
    }
    else if (((opc == 0x0F14 || opc == 0x0F15 || opc == 0x0FC5 || opc == 0x0FC6) && mandatory_prefix == 0x66) ||
        (opc == 0x0F70 && (mandatory_prefix == 0x66 || mandatory_prefix == 0xF2 || mandatory_prefix == 0xF3))) {
        execute_sse2_shuffle(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F14 || opc == 0x0F15 || opc == 0x0FC6) {
        execute_sse_shuffle(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F54 || opc == 0x0F55 || opc == 0x0F56) && mandatory_prefix == 0x66) {
        execute_sse2_logic_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F54 || opc == 0x0F55 || opc == 0x0F56 || opc == 0x0F57) {
        execute_sse_logic(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F58 || opc == 0x0F59 || opc == 0x0F5C || opc == 0x0F5E) && (mandatory_prefix == 0x66 || mandatory_prefix == 0xF2)) {
        execute_sse2_arith_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F58 || opc == 0x0F59 || opc == 0x0F5C || opc == 0x0F5E) {
        execute_sse_arith(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F51 || opc == 0x0F5D || opc == 0x0F5F) && (mandatory_prefix == 0x66 || mandatory_prefix == 0xF2)) {
        execute_sse2_math_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F51 || opc == 0x0F52 || opc == 0x0F53 || opc == 0x0F5D || opc == 0x0F5F) {
        execute_sse_math(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F5A && (mandatory_prefix == 0xF2 || mandatory_prefix == 0xF3)) {
        execute_sse2_scalar_convert(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F5A || opc == 0x0F5B || opc == 0x0FE6) {
        execute_sse2_convert(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F2A || opc == 0x0F2C || opc == 0x0F2D) && (mandatory_prefix == 0xF2 || mandatory_prefix == 0x66)) {
        execute_sse2_scalar_convert(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F2A || opc == 0x0F2C || opc == 0x0F2D) {
        execute_sse_convert(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0FC2 && (mandatory_prefix == 0x66 || mandatory_prefix == 0xF2)) ||
        ((opc == 0x0F2E || opc == 0x0F2F) && mandatory_prefix == 0x66)) {
        execute_sse2_cmp_pd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FC2 || opc == 0x0F2E || opc == 0x0F2F) {
        execute_sse_cmp(ctx, buf, (size_t)fetched);
    }
    else if (opc >= 0x0F80 && opc <= 0x0F8F) {
        execute_jcc(ctx, buf, (size_t)fetched);
    }
    else if (opc >= 0x0F90 && opc <= 0x0F9F) {
        execute_setcc(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FA0 || opc == 0x0FA8) {
        execute_push(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FA1 || opc == 0x0FA9) {
        execute_pop(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F0D || opc == 0x0F18 || (opc == 0x0F2B && mandatory_prefix == 0x00)) {
        execute_sse_misc(ctx, buf, (size_t)fetched);
    }
    else if ((opc == 0x0F2B && mandatory_prefix == 0x66) ||
        (opc == 0x0FE7 && mandatory_prefix == 0x66) ||
        opc == 0x0FC3) {
        execute_sse2_store_misc(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FAE) {
        execute_sse_state(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FAF) {
        execute_imul(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FBE || opc == 0x0FBF) {
        execute_movsx(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FB6 || opc == 0x0FB7) {
        execute_movzx(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FA3 || opc == 0x0FAB || opc == 0x0FB3 || opc == 0x0FBB || opc == 0x0FBA) {
        execute_bt(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FBC || opc == 0x0FBD) {
        execute_bsf(ctx, buf, (size_t)fetched);
    }
    else if (opc >= 0x0FC8 && opc <= 0x0FCF) {
        execute_bswap(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F31) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }
    else if (opc == 0x0FC0 || opc == 0x0FC1) {
        execute_xadd(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FB0 || opc == 0x0FB1) {
        execute_cmpxchg(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FC7 && is_rdpid_instruction(buf, (size_t)fetched, prefix_len)) {
        execute_rdpid(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FC7 && (prefix_len + 2) < fetched) {
        const uint8_t modrm = buf[prefix_len + 2];
        if ((((modrm >> 3) & 0x07) == 7) && (((modrm >> 6) & 0x03) == 3)) {
            raise_ud_ctx(ctx);
            result_code = CPU_STEP_UD;
            goto cpu_step_finish;
        }
        if (is_rdrand_instruction(buf, (size_t)fetched)) {
            raise_ud_ctx(ctx);
            result_code = CPU_STEP_UD;
            goto cpu_step_finish;
        }
        execute_cmpxchg8b16b(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0FC7) {
        if (is_rdrand_instruction(buf, (size_t)fetched)) {
            raise_ud_ctx(ctx);
            result_code = CPU_STEP_UD;
            goto cpu_step_finish;
        }
        else {
            execute_cmpxchg8b16b(ctx, buf, (size_t)fetched);
        }
    }
    else if (opc == 0x0F3A && mandatory_prefix == 0x66) {
        if (is_aeskeygenassist_instruction(buf, fetched, prefix_len)) {
            execute_aeskeygenassist(ctx, buf, (size_t)fetched);
        }
        else if (is_roundss_instruction(buf, fetched, prefix_len)) {
            execute_roundss(ctx, buf, (size_t)fetched);
        }
        else if (is_roundsd_instruction(buf, fetched, prefix_len)) {
            execute_roundsd(ctx, buf, (size_t)fetched);
        }
        else if (is_pcmpestrm_instruction(buf, fetched, prefix_len)) {
            execute_pcmpestrm(ctx, buf, (size_t)fetched);
        }
        else if (is_pcmpestri_instruction(buf, fetched, prefix_len)) {
            execute_pcmpestri(ctx, buf, (size_t)fetched);
        }
        else if (is_pcmpistrm_instruction(buf, fetched, prefix_len)) {
            execute_pcmpistrm(ctx, buf, (size_t)fetched);
        }
        else if (is_pcmpistri_instruction(buf, fetched, prefix_len)) {
            execute_pcmpistri(ctx, buf, (size_t)fetched);
        }
        else {
            raise_ud_ctx(ctx);
            result_code = CPU_STEP_UD;
            goto cpu_step_finish;
        }
    }
    else if (opc == 0x0FA2) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }
    else if (is_xgetbv) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }
    else if (is_rdtscp) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }
    else if (is_endbr_instruction(buf, (size_t)fetched)) {
        execute_endbr(ctx, buf, (size_t)fetched);
    }
    else if (is_rdssp_instruction(buf, (size_t)fetched)) {
        execute_rdssp(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F1E && (prefix_len + 2) < fetched) {
        const uint8_t opcode3 = buf[prefix_len + 2];
        const uint8_t reg = (uint8_t)((opcode3 >> 3) & 0x07);
        if (opcode3 == 0xFA || opcode3 == 0xFB || reg == 1) {
            raise_ud_ctx(ctx);
            result_code = CPU_STEP_UD;
            goto cpu_step_finish;
        }
    }
    // CDQ/CQO/CWD: 99
    else if (raw_opc == 0x99) {
        execute_cdq(ctx, buf, (size_t)fetched);
    }
    // CBW/CWDE/CDQE: 98
    else if (raw_opc == 0x98) {
        execute_cbw(ctx, buf, (size_t)fetched);
    }
    // ENTER: C8 iw ib
    else if (raw_opc == 0xC8) {
        execute_enter(ctx, buf, (size_t)fetched);
    }
    // LEAVE: C9
    else if (raw_opc == 0xC9) {
        execute_leave(ctx, buf, (size_t)fetched);
    }
    // PAUSE: F3 90
    else if (raw_opc == 0x90 && repeat_prefix == 0xF3 && !rex_b_prefix) {
        execute_pause(ctx, buf, (size_t)fetched);
    }
    else if (opc == 0x0F1F) {
        execute_nop(ctx, buf, (size_t)fetched);
    }
    // NOP: 90 (non-REX.B form, retaining XCHG short-register encodings for 90+r)
    else if (raw_opc == 0x90 && repeat_prefix == 0x00 && !rex_b_prefix) {
        execute_nop(ctx, buf, (size_t)fetched);
    }
    // XCHG: 86 /r, 87 /r, 90+r
    else if ((raw_opc >= 0x90 && raw_opc <= 0x97) || raw_opc == 0x86 || raw_opc == 0x87) {
        execute_xchg(ctx, buf, (size_t)fetched);
    }
    // MOVSXD: 63 /r (64-bit mode, 32->64 sign extension)
    else if (raw_opc == 0x63) {
        execute_movsxd(ctx, buf, (size_t)fetched);
    }
    // LEA: 8D /r
    else if (raw_opc == 0x8D) {
        execute_lea(ctx, buf, (size_t)fetched);
    }
    // MOV CRx is escape-owned and defaults to #UD when no user escape handles it.
    else if (opc == 0x0F20 || opc == 0x0F22) {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }
    // MOV: 88-8C, 8E, 0F20, 0F22, A0-A3, B0-BF, C6, C7
    else if (((raw_opc >= 0x88 && raw_opc <= 0x8C) || raw_opc == 0x8E) ||
        (raw_opc >= 0xA0 && raw_opc <= 0xA3) ||
        (raw_opc >= 0xB0 && raw_opc <= 0xBF) ||
        raw_opc == 0xC6 || raw_opc == 0xC7)
    {
        execute_mov(ctx, buf, (size_t)fetched);
    }
    // REP/REPNE string operations
    else if (repeat_prefix != 0 &&
        (raw_opc == 0xA4 || raw_opc == 0xA5 ||
            raw_opc == 0xA6 || raw_opc == 0xA7 ||
            raw_opc == 0xAA || raw_opc == 0xAB ||
            raw_opc == 0xAC || raw_opc == 0xAD ||
            raw_opc == 0xAE || raw_opc == 0xAF)) {
        execute_rep(ctx, buf, (size_t)fetched);
    }
    // MOVS: A4, A5
    else if (raw_opc == 0xA4 || raw_opc == 0xA5) {
        execute_movs(ctx, buf, (size_t)fetched);
    }
    // CMPS: A6, A7
    else if (raw_opc == 0xA6 || raw_opc == 0xA7) {
        execute_cmps(ctx, buf, (size_t)fetched);
    }
    // STOS: AA, AB
    else if (raw_opc == 0xAA || raw_opc == 0xAB) {
        execute_stos(ctx, buf, (size_t)fetched);
    }
    // LODS: AC, AD
    else if (raw_opc == 0xAC || raw_opc == 0xAD) {
        execute_lods(ctx, buf, (size_t)fetched);
    }
    // SCAS: AE, AF
    else if (raw_opc == 0xAE || raw_opc == 0xAF) {
        execute_scas(ctx, buf, (size_t)fetched);
    }
    // XLAT: D7
    else if (raw_opc == 0xD7) {
        execute_xlat(ctx, buf, (size_t)fetched);
    }
    // FNSTCW/FSTCW: D9 /7, 9B D9 /7
    else if (raw_opc == 0xD9 || (raw_opc == 0x9B && (prefix_len + 1) < fetched && buf[prefix_len + 1] == 0xD9)) {
        execute_x87_control(ctx, buf, (size_t)fetched);
    }
    // CLC/STC/CLD/STD: F8, F9, FC, FD
    else if (raw_opc == 0xF8 || raw_opc == 0xF9 || raw_opc == 0xFC || raw_opc == 0xFD) {
        execute_flags(ctx, buf, (size_t)fetched);
    }
    // PUSHF/POPF: 9C, 9D
    else if (raw_opc == 0x9C || raw_opc == 0x9D) {
        execute_pushf(ctx, buf, (size_t)fetched);
    }
    // LAHF/SAHF: 9E, 9F
    else if (raw_opc == 0x9E || raw_opc == 0x9F) {
        execute_lahf(ctx, buf, (size_t)fetched);
    }
    // PUSH r64 (50-57)
    else if (raw_opc >= 0x50 && raw_opc <= 0x57) {
        execute_push(ctx, buf, (size_t)fetched);
    }
    // POP r64 (58-5F)
    else if (raw_opc >= 0x58 && raw_opc <= 0x5F) {
        execute_pop(ctx, buf, (size_t)fetched);
    }
    // ADD r/m, r / r, r/m / imm (00-05)
    else if (raw_opc >= 0x00 && raw_opc <= 0x05) {
        execute_add(ctx, buf, (size_t)fetched);
    }
    // OR r/m, r / r, r/m / imm (08-0D)
    else if (raw_opc >= 0x08 && raw_opc <= 0x0D) {
        execute_or(ctx, buf, (size_t)fetched);
    }
    // ADC r/m, r / r, r/m / imm (10-15)
    else if (raw_opc >= 0x10 && raw_opc <= 0x15) {
        execute_adc(ctx, buf, (size_t)fetched);
    }
    // PUSH ES/CS/SS/DS (06 0E 16 1E)
    else if (raw_opc == 0x06 || raw_opc == 0x0E ||
        raw_opc == 0x16 || raw_opc == 0x1E) {
        execute_push(ctx, buf, (size_t)fetched);
    }
    // POP ES/SS/DS (07 17 1F)
    else if (raw_opc == 0x07 || raw_opc == 0x17 || raw_opc == 0x1F) {
        execute_pop(ctx, buf, (size_t)fetched);
    }
    // AND r/m, r / r, r/m (20-25)
    else if (raw_opc >= 0x20 && raw_opc <= 0x25) {
        execute_and(ctx, buf, (size_t)fetched);
    }
    // SBB r/m, r / r, r/m / imm (18-1D)
    else if (raw_opc >= 0x18 && raw_opc <= 0x1D) {
        execute_sbb(ctx, buf, (size_t)fetched);
    }
    // SUB r/m, r / r, r/m / imm (28-2D)
    else if (raw_opc >= 0x28 && raw_opc <= 0x2D) {
        execute_sub(ctx, buf, (size_t)fetched);
    }
    // XOR r/m, r / r, r/m / imm (30-35)
    else if (raw_opc >= 0x30 && raw_opc <= 0x35) {
        execute_xor(ctx, buf, (size_t)fetched);
    }
    // CMP r/m, r / r, r/m / imm (38-3D) and short-form 3C/3D
    else if (raw_opc >= 0x38 && raw_opc <= 0x3D) {
        execute_cmp(ctx, buf, (size_t)fetched);
    }
    // Short Jcc (70-7F)
    else if (raw_opc >= 0x70 && raw_opc <= 0x7F) {
        execute_jcc(ctx, buf, (size_t)fetched);
    }
    // Group 1 immediate: 80 / 81 / 83  - sub-dispatch by ModRM /reg
    else if ((raw_opc == 0x80 || raw_opc == 0x81 || raw_opc == 0x83) &&
        group_reg != 0xFF) {
        switch (group_reg) {
        case 0: execute_add(ctx, buf, (size_t)fetched); break; // ADD
        case 1: execute_or(ctx, buf, (size_t)fetched); break; // OR
        case 2: execute_adc(ctx, buf, (size_t)fetched); break; // ADC
        case 3: execute_sbb(ctx, buf, (size_t)fetched); break; // SBB
        case 4: execute_and(ctx, buf, (size_t)fetched); break; // AND
        case 5: execute_sub(ctx, buf, (size_t)fetched); break; // SUB
        case 6: execute_xor(ctx, buf, (size_t)fetched); break; // XOR
        case 7: execute_cmp(ctx, buf, (size_t)fetched); break; // CMP
        default: raise_ud_ctx(ctx); result_code = CPU_STEP_UD; goto cpu_step_finish;
        }
    }
    // ROL: D0-D3/0, C0/0, C1/0
    else if ((raw_opc == 0xD0 || raw_opc == 0xD1 || raw_opc == 0xD2 || raw_opc == 0xD3 ||
        raw_opc == 0xC0 || raw_opc == 0xC1) && group2_reg == 1) {
        execute_ror(ctx, buf, (size_t)fetched);
    }
    else if ((raw_opc == 0xD0 || raw_opc == 0xD1 || raw_opc == 0xD2 || raw_opc == 0xD3 ||
        raw_opc == 0xC0 || raw_opc == 0xC1) && group2_reg == 0) {
        execute_rol(ctx, buf, (size_t)fetched);
    }
    // SHL/SAL: D0-D3/4, C0/4, C1/4
    else if ((raw_opc == 0xD0 || raw_opc == 0xD1 || raw_opc == 0xD2 || raw_opc == 0xD3 ||
        raw_opc == 0xC0 || raw_opc == 0xC1) && group2_reg == 4) {
        execute_shl(ctx, buf, (size_t)fetched);
    }
    // SHR: D0-D3/5, C0/5, C1/5
    else if ((raw_opc == 0xD0 || raw_opc == 0xD1 || raw_opc == 0xD2 || raw_opc == 0xD3 ||
        raw_opc == 0xC0 || raw_opc == 0xC1) && group2_reg == 5) {
        execute_shr(ctx, buf, (size_t)fetched);
    }
    // SAR: D0-D3/7, C0/7, C1/7
    else if ((raw_opc == 0xD0 || raw_opc == 0xD1 || raw_opc == 0xD2 || raw_opc == 0xD3 ||
        raw_opc == 0xC0 || raw_opc == 0xC1) && group2_reg == 7) {
        execute_sar(ctx, buf, (size_t)fetched);
    }
    // TEST: A8, A9, 84, 85, F6/0, F7/0
    else if (raw_opc == 0xA8 || raw_opc == 0xA9 ||
        raw_opc == 0x84 || raw_opc == 0x85 ||
        ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 0)) {
        execute_test(ctx, buf, (size_t)fetched);
    }
    // NOT: F6/2, F7/2
    else if ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 2) {
        execute_not(ctx, buf, (size_t)fetched);
    }
    // NEG: F6/3, F7/3
    else if ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 3) {
        execute_neg(ctx, buf, (size_t)fetched);
    }
    // MUL: F6/4, F7/4
    else if ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 4) {
        execute_mul(ctx, buf, (size_t)fetched);
    }
    // IMUL: F6/5, F7/5
    else if ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 5) {
        execute_imul(ctx, buf, (size_t)fetched);
    }
    // DIV: F6/6, F7/6
    else if ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 6) {
        execute_div(ctx, buf, (size_t)fetched);
    }
    // IDIV: F6/7, F7/7
    else if ((raw_opc == 0xF6 || raw_opc == 0xF7) && group3_reg == 7) {
        execute_idiv(ctx, buf, (size_t)fetched);
    }
    // INC: FE/0
    else if (raw_opc == 0xFE && fe_reg == 0) {
        execute_inc(ctx, buf, (size_t)fetched);
    }
    // DEC: FE/1
    else if (raw_opc == 0xFE && fe_reg == 1) {
        execute_dec(ctx, buf, (size_t)fetched);
    }
    // AND short-form: 24, 25
    else if (raw_opc == 0x24 || raw_opc == 0x25) {
        execute_and(ctx, buf, (size_t)fetched);
    }
    // PUSH imm: 68, 6A
    else if (raw_opc == 0x68 || raw_opc == 0x6A) {
        execute_push(ctx, buf, (size_t)fetched);
    }
    // IMUL: 69 /r iw/id, 6B /r ib
    else if (raw_opc == 0x69 || raw_opc == 0x6B) {
        execute_imul(ctx, buf, (size_t)fetched);
    }
    // CALL near/far: E8, 9A
    else if (raw_opc == 0xE8 || raw_opc == 0x9A) {
        execute_call(ctx, buf, (size_t)fetched);
    }
    // JRCXZ / JECXZ / JCXZ: E3
    else if (raw_opc == 0xE3) {
        execute_jcc(ctx, buf, (size_t)fetched);
    }
    // JMP: EB (short), E9 (near), EA (far)
    else if (raw_opc == 0xEB || raw_opc == 0xE9 || raw_opc == 0xEA) {
        execute_jmp(ctx, buf, (size_t)fetched);
    }
    // POP r/m: 8F /0
    else if (raw_opc == 0x8F) {
        execute_pop(ctx, buf, (size_t)fetched);
    }
    // FF: PUSH/CALL/JMP r/m - sub-dispatch by ModRM /reg
    else if (raw_opc == 0xFF && ff_reg != 0xFF) {
        switch (ff_reg) {
        case 0:         execute_inc(ctx, buf, (size_t)fetched); break;
        case 1:         execute_dec(ctx, buf, (size_t)fetched); break;
        case 2: case 3: execute_call(ctx, buf, (size_t)fetched); break;
        case 4: case 5: execute_jmp(ctx, buf, (size_t)fetched); break;
        case 6:         execute_push(ctx, buf, (size_t)fetched); break;
        default: raise_ud_ctx(ctx); result_code = CPU_STEP_UD; goto cpu_step_finish;
        }
    }
    else {
        raise_ud_ctx(ctx);
        result_code = CPU_STEP_UD;
        goto cpu_step_finish;
    }

    // For non-branch instructions: advance RIP past the decoded instruction.
    // Branch handlers (Jcc, JMP, CALL, RET) already updated ctx->rip.
    if (!branch) {
        ctx->rip = rip_pre + (uint64_t)ctx->last_inst_size;
    }

cpu_step_finish:
    if (cpu_has_exception(ctx)) {
        CPU_EXCEPTION_STATE exception_state = ctx->exception;
        if (scalar_snapshot_valid) {
            cpu_restore_scalar_snapshot(ctx, &saved_scalar);
            if (vector_snapshot_valid) {
                cpu_restore_vector_snapshot(ctx, &saved_vector);
            }
        }
        ctx->exception = exception_state;
        return CPU_STEP_EXCEPTION;
    }

    return result_code;
}

int cpu_step(CPU_CONTEXT* ctx) {
    return cpu_step_with_prefetch(ctx, NULL, 0);
}

// ---------------------------------------------------------------------------
// cpu_run - run until HLT, exception, or max_steps
// ---------------------------------------------------------------------------

// max_steps == 0 means unlimited (run until HLT or exception propagates out).
// Returns the number of instructions successfully executed.
uint64_t cpu_run(CPU_CONTEXT* ctx, uint64_t max_steps) {
    uint64_t count = 0;
    for (;;) {
        if (max_steps != 0 && count >= max_steps) break;
        int result = cpu_step_with_prefetch(ctx, NULL, 0);
        if (result == CPU_STEP_HALT) break;
        if (result != CPU_STEP_OK)  break;
        count++;
    }
    return count;
}