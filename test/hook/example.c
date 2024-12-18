#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../include/tinyhook.h"

int (*orig_printf)(const char *format, ...);
int my_printf(const char *format, ...) {
    if (strcmp(format, "Hello, world!\n") == 0) {
        fprintf(stderr, "=== printf hooked!\n");
        return orig_printf("Hello, tinyhook!\n");
    }
    va_list args;
    va_start(args, format);
    return vprintf(format, args);
}

int fake_add(int a, int b) {
    fprintf(stderr, "=== calling fake_add with: %d, %d\n", a, b);
    return -1;
}

__attribute__((visibility("default"))) void exported_func() { return; }

__attribute__((constructor(0))) int load() {
    fprintf(stderr, "=== libexample loading...\n");

    // get an exported symbol address
    void *func_fake = symexp_solve(1, "_exported_func");
    fprintf(stderr, "=== exported_func() address: %p\n", func_fake);

    // hook a function by symbol
    void *func_add = symtbl_solve(0, "_add");
    fprintf(stderr, "=== add() address: %p\n", func_add);
    tiny_insert(func_add, fake_add, false);

    // hook system function
    tiny_hook(printf, my_printf, (void **)&orig_printf);
    return 0;
}
