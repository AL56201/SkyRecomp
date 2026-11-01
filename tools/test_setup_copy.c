// Run with an empty scratch directory as argv[1].
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../runtime/paths.h"
#include "../runtime/romfs_build.h"

// Unused paths.c platform hooks: fail if the copy test unexpectedly calls them.
bool plat_exe_path(char *buf, size_t size) { (void)buf; (void)size; assert(0); return false; }
bool plat_remove_tree(const char *dir) { (void)dir; assert(0); return false; }

int main(int argc, char **argv) {
    assert(argc == 2);
    char file[1024], alias[1024], other[1024];
    snprintf(file, sizeof file, "%s/sentinel.bin", argv[1]);
    snprintf(alias, sizeof alias, "%s/./sentinel.bin", argv[1]);
    snprintf(other, sizeof other, "%s/other.bin", argv[1]);
    FILE *f = fopen(file, "wb");
    assert(f);
    assert(fwrite("preserve", 1, 8, f) == 8);
    fclose(f);
    assert(rt_same_path(file, alias));
    assert(!rt_same_path(file, other));
#ifdef _WIN32
    assert(CreateHardLinkA(other, file, NULL));
    assert(rt_same_path(file, other));
    assert(DeleteFileA(other));
#endif
    snprintf(alias, sizeof alias, "%s/.", argv[1]);
    assert(romfs_copy_tree(argv[1], alias, NULL));
    char contents[9] = {0};
    f = fopen(file, "rb");
    assert(f);
    assert(fread(contents, 1, 9, f) == 8);
    fclose(f);
    assert(!strcmp(contents, "preserve"));
    assert(remove(file) == 0);
    puts("setup copy protection: passed");
}
