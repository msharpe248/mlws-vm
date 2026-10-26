/* ext.c - `mlwsvm ext`: manage a trusted extension directory and its TRUSTED
 * manifest (format: src/vm/extensions.c; rules: docs/EXTENDING.md).
 *
 *   mlwsvm ext list      [--dir D]
 *   mlwsvm ext install   [--dir D] FILE.mlwsx...
 *   mlwsvm ext remove    [--dir D] NAME...
 *   mlwsvm ext allow-ffi [--dir D] LIBRARY...     ("-" = the program's own symbols)
 *   mlwsvm ext deny-ffi  [--dir D] LIBRARY...
 *
 * Installing copies the file into the directory and records its SHA-256, so a
 * later change to it (or a different file put in its place) is refused at load
 * time. Installing never runs the extension's code. */
#include "../core/sha256.h"
#include "../platform/platform.h"
#include "tools.h"

#define HEADER "# MLWSVM trusted native code. Written by `mlwsvm ext`; see docs/EXTENDING.md.\n"

static int usage(void) {
    fprintf(stderr, "usage: mlwsvm ext list|install|remove|allow-ffi|deny-ffi [--dir DIR] [args...]\n"
                    "  list                    show what is trusted, and check it\n"
                    "  install FILE.mlwsx...   copy extensions in and record their SHA-256\n"
                    "  remove NAME...          remove installed extensions\n"
                    "  allow-ffi LIBRARY...    let .extern load a library (\"-\": the program itself)\n"
                    "  deny-ffi LIBRARY...     withdraw that\n"
                    "The directory defaults to lib/ext next to the mlwsvm executable.\n");
    return 2;
}

/* The manifest as lines, without the header. */
static void read_lines(const char *dir, StrVec *lines) {
    char *path = path_join(dir, "TRUSTED");
    char *text = read_file(path, NULL);
    free(path);
    for (char *s = text; s && *s;) {
        char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        if (n && s[0] != '#') VEC_PUSH(*lines, xstrndup(s, n));
        s = nl ? nl + 1 : s + n;
    }
    free(text);
}

static bool write_lines(const char *dir, const StrVec *lines) {
    char *path = path_join(dir, "TRUSTED");
    char *tmp = path_join(dir, "TRUSTED.tmp");
    FILE *f = fopen(tmp, "w");
    bool ok = f != NULL;
    if (f) {
        fputs(HEADER, f);
        for (size_t i = 0; i < lines->n; i++) fprintf(f, "%s\n", lines->v[i]);
        ok = fclose(f) == 0 && plat_restrict_writes(tmp) && rename(tmp, path) == 0;
    }
    if (!ok) fprintf(stderr, "mlwsvm ext: cannot write %s\n", path);
    free(path);
    free(tmp);
    return ok;
}

/* Drop lines "KIND NAME ..." */
static void drop(StrVec *lines, const char *kind, const char *name) {
    size_t kl = strlen(kind), nl = strlen(name);
    for (size_t i = 0; i < lines->n; i++) {
        const char *l = lines->v[i];
        if (!strncmp(l, kind, kl) && l[kl] == ' ' && !strncmp(l + kl + 1, name, nl) &&
            (l[kl + 1 + nl] == ' ' || !l[kl + 1 + nl])) {
            free(lines->v[i]);
            memmove(&lines->v[i], &lines->v[i + 1], (lines->n - i - 1) * sizeof *lines->v);
            lines->n--;
            i--;
        }
    }
}

static bool valid_name(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++)
        if (!(s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return false;
    return true;
}

static bool copy_file(const char *from, const char *to) {
    size_t n;
    char *data = read_file(from, &n);
    if (!data) return false;
    FILE *f = fopen(to, "wb");
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f) ok = fclose(f) == 0 && ok;
    free(data);
    return ok && plat_restrict_writes(to);
}

static int cmd_list(const char *dir) {
    StrVec lines = {0};
    read_lines(dir, &lines);
    char why[1200];
    char *manifest = path_join(dir, "TRUSTED");
    if (!lines.n) printf("%s: nothing is trusted\n", dir);
    else printf("%s:\n", dir);
    if (lines.n && !plat_path_trusted(dir, why, sizeof why)) printf("  WARNING: %s\n", why);
    if (lines.n && !plat_path_trusted(manifest, why, sizeof why)) printf("  WARNING: %s\n", why);
    int bad = 0;
    for (size_t i = 0; i < lines.n; i++) {
        char kind[8], a[256], b[256], c[1024];
        int k = sscanf(lines.v[i], "%7s %255s %255s %1023s", kind, a, b, c);
        if (k == 2 && !strcmp(kind, "ffi")) {
            printf("  ffi  %s\n", a);
        } else if (k == 4 && !strcmp(kind, "ext")) {
            char *path = path_join(dir, c);
            char hex[65];
            const char *state = "ok";
            if (!sha256_file_hex(path, hex)) state = "MISSING";
            else if (strcmp(hex, b + 7) != 0) state = "MODIFIED (its SHA-256 no longer matches)";
            else if (!plat_path_trusted(path, why, sizeof why)) state = why;
            if (strcmp(state, "ok") != 0) bad++;
            printf("  ext  %-20s %s  %s\n", a, c, state);
            free(path);
        } else {
            printf("  ?    %s\n", lines.v[i]);
            bad++;
        }
    }
    for (size_t i = 0; i < lines.n; i++) free(lines.v[i]);
    VEC_FREE(lines);
    free(manifest);
    return bad ? 1 : 0;
}

int tool_ext(int argc, char **argv, const ToolEnv *env) {
    if (argc < 1) return usage();
    const char *cmd = argv[0], *dir = env->ext_dir;
    StrVec args = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else VEC_PUSH(args, argv[i]);
    }
    int rc = 0;
    if (!dir) {
        fprintf(stderr, "mlwsvm ext: no default extension directory (lib/ is not next to mlwsvm); use --dir\n");
        rc = 2;
    } else if (!strcmp(cmd, "list")) {
        rc = cmd_list(dir);
    } else if (!strcmp(cmd, "install") || !strcmp(cmd, "remove") || !strcmp(cmd, "allow-ffi") ||
               !strcmp(cmd, "deny-ffi")) {
        if (!args.n) {
            VEC_FREE(args);
            return usage();
        }
        if (!plat_mkdir(dir)) {
            fprintf(stderr, "mlwsvm ext: cannot create %s\n", dir);
            VEC_FREE(args);
            return 1;
        }
        plat_restrict_writes(dir);
        char why[1200];
        if (!plat_path_trusted(dir, why, sizeof why)) {
            fprintf(stderr, "mlwsvm ext: refusing to use an untrusted directory: %s\n", why);
            VEC_FREE(args);
            return 1;
        }
        StrVec lines = {0};
        read_lines(dir, &lines);
        for (size_t i = 0; i < args.n && !rc; i++) {
            const char *a = args.v[i];
            if (!strcmp(cmd, "install")) {
                const char *base = path_basename(a);
                size_t n = strlen(base);
                if (!str_ends_with(base, ".mlwsx") || !valid_name(base, n - 6)) {
                    fprintf(stderr, "mlwsvm ext: %s: expected NAME.mlwsx, NAME using a-z, 0-9 and _\n", a);
                    rc = 1;
                    break;
                }
                char *name = xstrndup(base, n - 6), *to = path_join(dir, base);
                char hex[65];
                if (!sha256_file_hex(a, hex) || !copy_file(a, to)) {
                    fprintf(stderr, "mlwsvm ext: cannot copy %s to %s\n", a, to);
                    rc = 1;
                } else {
                    drop(&lines, "ext", name);
                    StrBuf l = {0};
                    sb_printf(&l, "ext %s sha256:%s %s", name, hex, base);
                    VEC_PUSH(lines, l.s);
                    printf("installed %s (sha256 %.16s...) in %s\n", name, hex, dir);
                }
                free(name);
                free(to);
            } else if (!strcmp(cmd, "remove")) {
                char *file = xmalloc(strlen(a) + 8);
                sprintf(file, "%s.mlwsx", a);
                char *path = path_join(dir, file);
                drop(&lines, "ext", a);
                remove(path);
                printf("removed %s\n", a);
                free(file);
                free(path);
            } else {
                drop(&lines, "ffi", a);
                if (!strcmp(cmd, "allow-ffi")) {
                    StrBuf l = {0};
                    sb_printf(&l, "ffi %s", a);
                    VEC_PUSH(lines, l.s);
                    printf("allowed FFI library %s\n", a);
                } else {
                    printf("denied FFI library %s\n", a);
                }
            }
        }
        if (!rc && !write_lines(dir, &lines)) rc = 1;
        for (size_t i = 0; i < lines.n; i++) free(lines.v[i]);
        VEC_FREE(lines);
    } else {
        rc = usage();
    }
    VEC_FREE(args);
    return rc;
}
