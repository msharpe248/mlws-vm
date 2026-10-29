/* opttool.c - `mlwsvm opt`: optimize one module and write it, with a report
 * of what each pass did. See docs/OPTIMIZER.md. */
#include "../asm/asm.h"
#include "../core/disasm.h"
#include "../platform/platform.h"
#include "tools.h"

static int usage_opt(void) {
    fprintf(stderr, "usage: mlwsvm opt [-L dir] [-O0|-O1|-O2|-O3] [--passes LIST] [-o out.mlwso] [-S] [--report FILE|-] "
                    "[-v] [-q] file.(masm|mlwso)\n"
                    "       mlwsvm opt --list\n");
    return 2;
}

static void list_passes(void) {
    printf("pass          level  what one change is\n");
    for (int p = 0; p < PASS__COUNT; p++) printf("%-12s  -O%d    %s\n", opt_pass_name(p), opt_pass_level(p), opt_pass_doc(p));
    printf("\n-O1 changes nothing a program can see but how many instructions it runs; -O2 also\n"
           "what only the tools see (the debugger shows fewer registers and functions); -O3 also\n"
           "removes frames, which a program can see in a backtrace (Error.trace).\n"
           "--passes takes a comma-separated list, applied in order after -O: levels (0-3), all,\n"
           "none, pass names, and -name to turn one off (e.g. --passes 3,-inline).\n");
}

int tool_opt(int argc, char **argv, const ToolEnv *env) {
    const char *file = NULL, *out = NULL, *report = "-", *passes = NULL;
    bool asm_out = false, verbose = false, quiet = false;
    int level = 1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            list_passes();
            return 0;
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-S")) asm_out = true;
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "-q")) quiet = true;
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = argv[++i];
        else if (!strncmp(argv[i], "-O", 2) && argv[i][2] >= '0' && argv[i][2] <= '3' && !argv[i][3]) level = argv[i][2] - '0';
        else if (argv[i][0] == '-' || file) return usage_opt();
        else file = argv[i];
    }
    if (!file) return usage_opt();
    OptConfig cfg;
    char lv[2] = {(char)('0' + level), 0}, msg[512];
    opt_parse(lv, &cfg, msg, sizeof msg);
    if (passes) {
        OptConfig more = cfg;
        StrBuf spec = {0};
        sb_printf(&spec, "%s,%s", lv, passes);
        if (!opt_parse(spec.s, &more, msg, sizeof msg)) {
            fprintf(stderr, "mlwsvm opt: --passes: %s\n", msg);
            sb_free(&spec);
            return 2;
        }
        sb_free(&spec);
        cfg = more;
    }
    bool binary_in = str_ends_with(file, ".mlwso");
    if (!out && !asm_out && binary_in) {
        fprintf(stderr, "mlwsvm opt: say where the optimized module goes (-o FILE, or -S for assembly)\n");
        return 2;
    }
    MlwsError err = {{0}, {0}};
    Module *m = NULL;
    if (binary_in) {
        size_t len;
        char *data = read_file(file, &len);
        if (!data) {
            fprintf(stderr, "mlwsvm opt: cannot read %s\n", file);
            return 1;
        }
        m = module_read((const uint8_t *)data, len, &err);
        free(data);
    } else {
        VEC(char *) lp = {0};
        VEC_PUSH(lp, path_dirname(file));
        for (size_t i = 0; i < env->libpaths.n; i++) VEC_PUSH(lp, xstrdup(env->libpaths.v[i]));
        const char *base = path_basename(file);
        const char *dot = strrchr(base, '.');
        char *name = dot ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
        m = assemble_file(file, name, (const char *const *)lp.v, lp.n, &err);
        free(name);
        for (size_t i = 0; i < lp.n; i++) free(lp.v[i]);
        VEC_FREE(lp);
    }
    if (!m) {
        fprintf(stderr, "error[%s]: %s\n", err.code, err.msg);
        return 65;
    }
    OptReport rep;
    memset(&rep, 0, sizeof rep);
    rep.log = verbose;
    opt_module(m, &cfg, &rep);
    int rc = 0;
    if (asm_out) {
        StrBuf b = {0};
        if (!disasm_module(&b, m, &err)) {
            fprintf(stderr, "error[%s]: %s\n", err.code, err.msg);
            rc = 65;
        } else if (out) {
            if (!write_file(out, b.s, b.n)) fprintf(stderr, "mlwsvm opt: cannot write %s\n", out), rc = 1;
        } else {
            fwrite(b.s, 1, b.n, stdout);
        }
        sb_free(&b);
    } else {
        StrBuf o = {0};
        if (out) sb_puts(&o, out);
        else sb_putn(&o, file, strlen(file) - (str_ends_with(file, ".masm") ? 5 : 0)), sb_puts(&o, ".mlwso");
        ByteBuf bb = {0};
        module_write(m, &bb);
        if (!write_file(o.s, bb.b, bb.n)) fprintf(stderr, "mlwsvm opt: cannot write %s\n", o.s), rc = 1;
        bb_free(&bb);
        sb_free(&o);
    }
    if (!quiet) {
        bool to_err = !strcmp(report, "-");
        FILE *f = to_err ? stderr : fopen(report, "w");
        if (f) {
            fflush(stdout);
            opt_report_print(&rep, f);
            if (!to_err) fclose(f);
        } else {
            fprintf(stderr, "mlwsvm opt: cannot write %s\n", report);
            rc = 1;
        }
    }
    opt_report_free(&rep);
    module_free(m);
    return rc;
}
