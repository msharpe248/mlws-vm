/* embed.c - build helper: turn a file into a C source file defining
 *   const char NAME[] (NUL-terminated) and const size_t NAME_LEN.
 * Used to compile web/dashboard.html into the mlwsvm binary.
 * Usage: embed INPUT NAME > OUTPUT.c */
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: embed INPUT NAME\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    printf("/* Generated from %s by build-tools/embed.c. Do not edit. */\n#include <stddef.h>\n", argv[1]);
    printf("const char %s[] = {", argv[2]);
    int c;
    unsigned long n = 0;
    while ((c = fgetc(f)) != EOF) {
        printf("%s%d,", n % 24 ? "" : "\n", c);
        n++;
    }
    printf("0};\nconst size_t %s_LEN = %lu;\n", argv[2], n);
    fclose(f);
    return 0;
}
