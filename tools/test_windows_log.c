// Link with runtime/platform/windows.c and --gc-sections to exercise the actual Windows log writer.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../runtime/platform.h"

void rt_log(const char *format, ...);
// Unrelated platform exception handlers are linked but never installed or exercised by this log test.
bool rt_fault_watch(uint64_t address, uint64_t pc) { (void)address; (void)pc; abort(); }
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { (void)pc; (void)offset; abort(); }
void rt_fault_report(const char *what, uint64_t address, uint64_t pc, uint64_t lr) {
    (void)what; (void)address; (void)pc; (void)lr; abort();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(freopen(argv[1], "w+b", stderr));
    setvbuf(stderr, NULL, _IONBF, 0);
    char long_line[5001];
    memset(long_line, 'x', sizeof long_line - 1);
    long_line[5000] = 0;
    rt_log("small %d %s\n", 42, "message");
    rt_log("%s\n", long_line);
    assert(ftell(stderr) == 17 + 5001);
    rewind(stderr);
    char actual[5019] = {0};
    assert(fread(actual, 1, 5018, stderr) == 5018);
    assert(!memcmp(actual, "small 42 message\n", 17));
    assert(!memcmp(actual + 17, long_line, 5000));
    assert(actual[5017] == '\n');
    puts("Windows logging: formatted and oversized messages preserved");
}
