/* disasm.c - render a Module as MLWSVM assembly. The output re-assembles to an
 * equivalent module (modulo comments and symbol-less data layout). */
#include "disasm.h"

#include "insn.h"

static const char *func_ref_name(const Module *m, uint64_t idx, char *buf, size_t n) {
    if (idx >= m->nfuncs) return "?func";
    const MFunc *f = &m->funcs[idx];
    if (f->flags & MF_IMPORT) {
        snprintf(buf, n, "%s.%s", m->imports[f->import_idx], f->name);
        return buf;
    }
    return f->name;
}

static void put_gref(StrBuf *b, const Module *m, uint64_t idx) {
    if (idx >= m->ngrefs) {
        sb_puts(b, "@?");
        return;
    }
    const MGref *g = &m->grefs[idx];
    if (g->kind == GREF_IMPORT) {
        sb_printf(b, "@%s.%s", m->imports[g->import_idx], g->sym);
        return;
    }
    for (uint32_t i = 0; i < m->nsyms; i++) {
        if (m->syms[i].offset == g->offset) {
            sb_printf(b, "@%s", m->syms[i].name);
            return;
        }
    }
    sb_printf(b, "@?offset_%u", g->offset);
}

static void put_struct(StrBuf *b, const Module *m, uint64_t idx) {
    if (idx >= m->nstructs) {
        sb_puts(b, "?struct");
        return;
    }
    const MStruct *st = &m->structs[idx];
    if (st->flags & MSF_BUILTIN) sb_puts(b, st->name);
    else if (st->flags & MSF_IMPORT) sb_printf(b, "%s.%s", m->imports[st->import_idx], st->name);
    else sb_puts(b, st->name);
}

static void put_index(StrBuf *b, const Operand *o, bool bytes, bool has_index) {
    char rb[8];
    sb_putc(b, bytes ? '{' : '[');
    if (has_index) {
        sb_puts(b, reg_name(o->ireg, rb));
        if (o->scale != 1) sb_printf(b, "*%llu", (unsigned long long)o->scale);
        if (o->imm > 0) sb_printf(b, "+%lld", (long long)o->imm);
        else if (o->imm < 0) sb_printf(b, "-%llu", (unsigned long long)(0 - (uint64_t)o->imm));
    } else {
        sb_printf(b, "%lld", (long long)o->imm);
    }
    sb_putc(b, bytes ? '}' : ']');
}

static void put_operand(StrBuf *b, const Module *m, const Operand *o) {
    char rb[8], fb[64];
    switch (o->mode) {
    case OM_REG: sb_puts(b, reg_name(o->reg, rb)); break;
    case OM_IMM: sb_printf(b, "%lld", (long long)o->imm); break;
    case OM_FIMM:
        fmt_double(o->f, fb, sizeof fb);
        sb_puts(b, fb);
        if (!strpbrk(fb, ".eni")) sb_puts(b, ".0");
        break;
    case OM_CONST:
        if (o->index >= m->nconsts) sb_puts(b, "?const");
        else if (m->consts[o->index].kind == CONST_STRING)
            sb_put_quoted(b, m->consts[o->index].data, m->consts[o->index].len);
        else sb_printf(b, "%s%s", m->consts[o->index].data, m->consts[o->index].kind == CONST_BIG ? "n" : "d");
        break;
    case OM_UPVAL: sb_printf(b, "UP[%llu]", (unsigned long long)o->index); break;
    case OM_IND: sb_printf(b, "[%s]", reg_name(o->reg, rb)); break;
    case OM_DISP: case OM_DISPB:
        sb_puts(b, reg_name(o->reg, rb));
        put_index(b, o, o->mode == OM_DISPB, false);
        break;
    case OM_IDX: case OM_IDXB:
        sb_puts(b, reg_name(o->reg, rb));
        put_index(b, o, o->mode == OM_IDXB, true);
        break;
    case OM_POSTINC: sb_printf(b, "[%s]+", reg_name(o->reg, rb)); break;
    case OM_PREDEC: sb_printf(b, "-[%s]", reg_name(o->reg, rb)); break;
    case OM_POSTINCB: sb_printf(b, "{%s}+", reg_name(o->reg, rb)); break;
    case OM_PREDECB: sb_printf(b, "-{%s}", reg_name(o->reg, rb)); break;
    case OM_GLOBAL: case OM_GLOBALB:
        put_gref(b, m, o->index);
        if (o->imm || o->mode == OM_GLOBALB) put_index(b, o, o->mode == OM_GLOBALB, false);
        break;
    case OM_GLOBALIDX: case OM_GLOBALIDXB:
        put_gref(b, m, o->index);
        put_index(b, o, o->mode == OM_GLOBALIDXB, true);
        break;
    case OM_MIND: case OM_MINDB:
        sb_putc(b, '[');
        put_operand(b, m, o->inner);
        sb_putc(b, ']');
        if (o->imm || o->mode == OM_MINDB) put_index(b, o, o->mode == OM_MINDB, false);
        break;
    case OM_ELEM: sb_printf(b, "%s[%s]", reg_name(o->reg, rb), reg_name(o->ireg, fb)); break;
    case OM_ELEMI: sb_printf(b, "%s[%llu]", reg_name(o->reg, rb), (unsigned long long)o->index); break;
    case OM_FIELD:
        sb_printf(b, "%s->", reg_name(o->reg, rb));
        if (o->index < m->nfrefs && m->frefs[o->index].struct_idx < m->nstructs) {
            put_struct(b, m, m->frefs[o->index].struct_idx);
            sb_printf(b, ".%s", m->frefs[o->index].field);
        } else {
            sb_puts(b, "?field");
        }
        break;
    case OM_STRUCT: put_struct(b, m, o->index); break;
    case OM_LABEL: sb_printf(b, ".L%llu", (unsigned long long)o->index); break;
    case OM_FUNC: sb_puts(b, func_ref_name(m, o->index, fb, sizeof fb)); break;
    case OM_NATIVE:
        if (o->index < m->nnatives) sb_put_quoted(b, m->natives[o->index], strlen(m->natives[o->index]));
        else sb_puts(b, "?native");
        break;
    default: sb_printf(b, "?mode%u", o->mode); break;
    }
}

void disasm_insn(StrBuf *b, const Module *m, const Insn *in) {
    const OpInfo *oi = &op_info[in->op];
    sb_puts(b, oi->mnem);
    if (in->type) sb_printf(b, ".%s", type_name((MlwsType)in->type));
    if ((oi->flags & OPF_TYPE2) && in->type2) sb_printf(b, ".%s", type_name((MlwsType)in->type2));
    /* The disassembler also reads unverified modules, so check the call block's
     * shape before trusting its argument count; otherwise list the operands. */
    bool well_formed_call = (oi->flags & OPF_CALL) && in->nops >= 2 && in->ops[1].mode == OM_IMM &&
                            in->ops[1].imm >= 0 && in->ops[1].imm <= in->nops - 2;
    if (well_formed_call) {
        if (in->op != OP_COYIELD) { /* coyield's target is a placeholder */
            sb_putc(b, ' ');
            put_operand(b, m, &in->ops[0]);
        }
        int argc = in->nops > 1 ? (int)in->ops[1].imm : 0;
        sb_puts(b, " (");
        for (int k = 0; k < argc && 2 + k < in->nops; k++) {
            if (k) sb_puts(b, ", ");
            put_operand(b, m, &in->ops[2 + k]);
        }
        sb_putc(b, ')');
        if (in->nops > 2 + argc) {
            sb_puts(b, " -> (");
            for (int k = 2 + argc; k < in->nops; k++) {
                if (k > 2 + argc) sb_puts(b, ", ");
                put_operand(b, m, &in->ops[k]);
            }
            sb_putc(b, ')');
        }
        return;
    }
    for (int k = 0; k < in->nops; k++) {
        sb_puts(b, k ? ", " : " ");
        put_operand(b, m, &in->ops[k]);
    }
}

static void put_types(StrBuf *b, const uint8_t *t, int n) {
    for (int i = 0; i < n; i++) sb_printf(b, "%s%s", i ? ", " : "", type_name((MlwsType)t[i]));
}

static void disasm_data(StrBuf *b, const Module *m) {
    if (!m->gsize) return;
    sb_puts(b, "\n; ---- data ----\n");
    uint32_t off = 0;
    uint32_t si = 0;
    while (off < m->gsize) {
        while (si < m->nsyms && m->syms[si].offset < off) si++;
        if (si < m->nsyms && m->syms[si].offset == off) sb_printf(b, "%s:\n", m->syms[si].name);
        uint32_t next = si < m->nsyms && m->syms[si].offset > off ? m->syms[si].offset : m->gsize;
        if (si < m->nsyms && m->syms[si].offset == off)
            next = si + 1 < m->nsyms ? m->syms[si + 1].offset : m->gsize;
        uint8_t tag = m->gtags[off / 8];
        if (off % 8 == 0 && next - off >= 8 && tag != TAG_RAW && tag != T_NONE && type_size((MlwsType)tag) == 8) {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= (uint64_t)m->gdata[off + (uint32_t)i] << (8 * i);
            if (tag == T_F64) {
                double d;
                char fb[64];
                memcpy(&d, &v, 8);
                fmt_double(d, fb, sizeof fb);
                sb_printf(b, "    .f64 %s%s\n", fb, strpbrk(fb, ".eni") ? "" : ".0");
            } else {
                sb_printf(b, "    .%s %lld\n", tag == T_U64 ? "u64" : "i64", (long long)v);
            }
            off += 8;
            continue;
        }
        uint32_t end = next < off + 16 ? next : off + 16;
        sb_puts(b, "    .u8 ");
        for (uint32_t i = off; i < end; i++) sb_printf(b, "%s%u", i > off ? ", " : "", m->gdata[i]);
        sb_putc(b, '\n');
        off = end;
    }
}

/* An FFI type as .extern and .cstruct write it. */
static void put_ffi_type(StrBuf *b, uint8_t kind, const char *tag) {
    static const char *const K[] = {"void", "i8",  "i16", "i32",  "i64", "u8",  "u16",
                                    "u32",  "u64", "f32", "f64", "cstr", "buf", "ptr"};
    uint8_t k = kind & (uint8_t)~FFI_OUT;
    if (kind & FFI_OUT) sb_puts(b, "out ");
    if (k == FFI_CALLBACK) {
        sb_printf(b, "callback %s", tag ? tag : "?");
        return;
    }
    if (k == FFI_STRUCT || k == FFI_STRUCTP) {
        sb_printf(b, "struct%s %s", k == FFI_STRUCTP ? "*" : "", tag ? tag : "?");
        return;
    }
    sb_puts(b, k < FFI_STRUCT ? K[k] : "?");
    if (tag) sb_printf(b, "<%s>", tag);
}

static bool is_cstruct(const Module *m, const char *name) {
    for (uint32_t i = 0; i < m->ncstructs; i++)
        if (!strcmp(m->cstructs[i].name, name)) return true;
    return false;
}

bool disasm_module(StrBuf *b, const Module *m, MlwsError *err) {
    op_init();
    sb_printf(b, "; disassembly of module %s (source: %s)\n", m->name, m->source && *m->source ? m->source : "?");
    sb_printf(b, ".module %s\n", m->name);
    for (uint32_t i = 0; i < m->nimports; i++) sb_printf(b, ".import %s\n", m->imports[i]);
    for (uint32_t i = 0; i < m->nextensions; i++) sb_printf(b, ".extension \"%s\"\n", m->extensions[i]);
    for (uint32_t i = 0; i < m->nffrees; i++) {
        sb_printf(b, ".ffi_free %s, ", m->ffrees[i].tag);
        sb_put_quoted(b, m->ffrees[i].lib, strlen(m->ffrees[i].lib));
        sb_puts(b, ", ");
        sb_put_quoted(b, m->ffrees[i].sym, strlen(m->ffrees[i].sym));
        sb_putc(b, '\n');
    }
    /* C structs come first: the externs after them name them. */
    for (uint32_t i = 0; i < m->ncstructs; i++) {
        const MCStruct *c = &m->cstructs[i];
        for (uint32_t s = 0; s < m->nstructs; s++)
            if (!strcmp(m->structs[s].name, c->name) && m->structs[s].flags & MSF_EXPORT)
                sb_printf(b, ".export %s\n", c->name);
        sb_printf(b, ".cstruct %s\n", c->name);
        for (uint32_t k = 0; k < c->nfields; k++) {
            sb_printf(b, "    %s: ", c->fields[k]);
            put_ffi_type(b, c->kinds[k], c->tags[k]);
            if (c->counts[k]) sb_printf(b, "[%u]", c->counts[k]);
            sb_putc(b, '\n');
        }
        sb_puts(b, ".ends\n");
    }
    for (uint32_t i = 0; i < m->ncallbacks; i++) {
        const MCallback *cb = &m->callbacks[i];
        sb_printf(b, ".callback %s (", cb->name);
        for (int k = 0; k < cb->nargs; k++) {
            if (k) sb_puts(b, ", ");
            put_ffi_type(b, cb->args[k], cb->tags[k]);
        }
        sb_puts(b, ")");
        if (cb->ret != FFI_VOID) {
            sb_puts(b, " -> (");
            put_ffi_type(b, cb->ret, cb->ret_tag);
            sb_puts(b, ")");
        }
        if (cb->flags & MCB_ASYNC) sb_puts(b, ", async");
        sb_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->nexterns; i++) {
        const MExtern *x = &m->externs[i];
        sb_printf(b, ".extern %s, ", x->name);
        sb_put_quoted(b, x->lib, strlen(x->lib));
        sb_puts(b, ", ");
        sb_put_quoted(b, x->sym, strlen(x->sym));
        sb_puts(b, " (");
        for (int k = 0; k < x->nargs; k++) {
            if (k) sb_puts(b, ", ");
            if (k == x->nfixed) sb_puts(b, "..., ");
            put_ffi_type(b, x->args[k], x->tags[k]);
        }
        sb_puts(b, ")");
        if (x->ret != FFI_VOID) {
            sb_puts(b, " -> (");
            put_ffi_type(b, x->ret, x->ret_tag);
            sb_puts(b, ")");
        }
        if (x->flags & MX_BLOCKING) sb_puts(b, ", blocking");
        if (x->flags & MX_SERIAL) sb_puts(b, ", serial");
        sb_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->nfuncs; i++)
        if ((m->funcs[i].flags & (MF_EXPORT | MF_IMPORT)) == MF_EXPORT) sb_printf(b, ".export %s\n", m->funcs[i].name);
    for (uint32_t i = 0; i < m->nsyms; i++)
        if (m->syms[i].flags & MS_EXPORT) sb_printf(b, ".export %s\n", m->syms[i].name);
    for (uint32_t i = 0; i < m->nstructs; i++) {
        const MStruct *st = &m->structs[i];
        if (st->flags & (MSF_IMPORT | MSF_BUILTIN) || is_cstruct(m, st->name)) continue;
        if (st->flags & MSF_EXPORT) sb_printf(b, ".export %s\n", st->name);
        sb_printf(b, "\n.struct %s\n", st->name);
        for (uint32_t k = 0; k < st->nfields; k++)
            sb_printf(b, "    %s: %s\n", st->fields[k], type_name((MlwsType)st->types[k]));
        sb_puts(b, ".ends\n");
    }
    disasm_data(b, m);
    for (uint32_t fi = 0; fi < m->nfuncs; fi++) {
        const MFunc *f = &m->funcs[fi];
        if (f->flags & MF_IMPORT) continue;
        ByteReader r = {m->code + f->code_off, f->code_len, 0, false};
        VEC(Insn) code = {0};
        while (r.pos < r.n) {
            Insn in;
            if (!decode_insn(&r, &in, err)) {
                for (size_t i = 0; i < code.n; i++) insn_free(&code.v[i]);
                VEC_FREE(code);
                return false;
            }
            VEC_PUSH(code, in);
        }
        bool *is_target = xcalloc(code.n + 1, 1);
        for (size_t i = 0; i < code.n; i++)
            for (int k = 0; k < code.v[i].nops; k++)
                if (code.v[i].ops[k].mode == OM_LABEL && code.v[i].ops[k].index < code.n)
                    is_target[code.v[i].ops[k].index] = true;
        sb_printf(b, "\n.func %s(", f->name);
        put_types(b, f->params, f->nparams);
        sb_putc(b, ')');
        if (f->flags & MF_RETS_DECLARED) {
            sb_puts(b, " -> (");
            put_types(b, f->rets, f->nrets);
            sb_putc(b, ')');
        }
        sb_printf(b, "\n    .regs I=%u, F=%u, R=%u, P=%u\n", f->nregs[0], f->nregs[1], f->nregs[2], f->nregs[3]);
        for (uint32_t h = 0; h < f->nhandlers; h++) {
            const MHandler *mh = &f->handlers[h];
            sb_printf(b, "    .try .L%u, .L%u, .L%u\n", mh->start, mh->end, mh->target);
            if (mh->start <= code.n) is_target[mh->start] = true;
            if (mh->end <= code.n) is_target[mh->end] = true;
            if (mh->target < code.n) is_target[mh->target] = true;
        }
        for (size_t i = 0; i < code.n; i++) {
            if (is_target[i]) sb_printf(b, ".L%zu:\n", i);
            sb_puts(b, "    ");
            size_t start = b->n;
            disasm_insn(b, m, &code.v[i]);
            size_t width = b->n - start;
            uint32_t line = f->lines && i < f->ninsns ? f->lines[i] : 0;
            if (line) sb_printf(b, "%*s; line %u", width < 40 ? (int)(40 - width) : 1, "", line);
            sb_putc(b, '\n');
            insn_free(&code.v[i]);
        }
        sb_puts(b, ".endf\n");
        free(is_target);
        VEC_FREE(code);
    }
    return true;
}
