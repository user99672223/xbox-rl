/*
 * dlopen_host.c - stands in for Wine's unix side: a musl program (built with the xbox-rl
 * toolchain) that dlopen()s a native module built against glibc and calls one function.
 *   dlopen_host MODULE.so SYMBOL        exit code = the function's return value
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dlopen_host MODULE SYMBOL\n"); return 2; }
    /* what Wine's ntdll does before its first native unix library (patches/wine/0017) */
    const char *compat = getenv("XRL_GLIBC_COMPAT");
    if (!compat) compat = "/lib/libgcompat.so.0";
    if (*compat && !access(compat, R_OK) && !dlopen(compat, RTLD_NOW | RTLD_GLOBAL)) printf("cannot load %s: %s\n", compat, dlerror());
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("dlopen(%s) failed: %s\n", argv[1], dlerror()); return 3; }
    printf("dlopen(%s) ok\n", argv[1]);
    int (*fn)(void) = (int (*)(void))dlsym(h, argv[2]);
    if (!fn) { printf("dlsym(%s) failed: %s\n", argv[2], dlerror()); return 4; }
    int rc = fn();
    printf("%s returned %d\n", argv[2], rc);
    return rc;
}
