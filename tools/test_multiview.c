// The multiview shader rewrite (runtime/nvn/vulkan/multiview.c) over dumped shaders (LARECOMP_SAVE_MSL=1: build/glsl):
// each output written next to an output directory, to compile with glslc (tools/test_multiview.sh).
// clang -std=c11 -Iruntime/nvn/vulkan tools/test_multiview.c runtime/nvn/vulkan/multiview.c -o build/test_multiview
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "multiview.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_multiview out_dir in.glsl...\n"); return 2; }
    int fails = 0;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); fails++; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)n + 1);
        src[fread(src, 1, (size_t)n, f)] = 0;
        fclose(f);
        char *out = vk_multiview_glsl(src);
        if (!out) { printf("REWRITE FAILED %s\n", argv[i]); fails++; free(src); continue; }
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[1], base);
        FILE *o = fopen(path, "wb");
        fputs(out, o);
        fclose(o);
        free(out);
        free(src);
    }
    printf("%d of %d rewrites failed\n", fails, argc - 2);
    return fails != 0;
}
