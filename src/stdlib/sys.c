#include "sys.h"
#include "../value.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/syscall.h>

static Value sys_syscall(int argc, Value *argv) {
    if (argc < 1 || !IS_INT(argv[0])) return VAL_NIL_V;
    long num = (long)argv[0].integer;
    long a1 = argc > 1 && IS_INT(argv[1]) ? (long)argv[1].integer : 0;
    long a2 = argc > 2 && IS_INT(argv[2]) ? (long)argv[2].integer : 0;
    long a3 = argc > 3 && IS_INT(argv[3]) ? (long)argv[3].integer : 0;
    long a4 = argc > 4 && IS_INT(argv[4]) ? (long)argv[4].integer : 0;
    long a5 = argc > 5 && IS_INT(argv[5]) ? (long)argv[5].integer : 0;
    long ret = syscall(num, a1, a2, a3, a4, a5);
    return VAL_INT_V((int64_t)ret);
}
#else
static Value sys_syscall(int argc, Value *argv) {
    (void)argc; (void)argv;
    return VAL_INT_V(-1); /* not supported on Windows */
}
#endif

static Value sys_exit(int argc, Value *argv) {
    int code = (argc > 0 && IS_INT(argv[0])) ? (int)argv[0].integer : 0;
    _exit(code);
    return VAL_NIL_V;
}

static Value sys_getenv_fn(int argc, Value *argv) {
    if (argc < 1 || !IS_STR(argv[0])) return VAL_NIL_V;
    const char *v = getenv(argv[0].string->data);
    if (!v) return VAL_NIL_V;
    return VAL_STR_V(str_intern(v, (int)strlen(v)));
}

ObjMap *stdlib_sys_module(void) {
    ObjMap *m = map_new();
#define REG(name, fn) map_set(m, str_intern(name, (int)strlen(name)), VAL_NATIVE_V(fn))
    REG("syscall", sys_syscall);
    REG("exit",    sys_exit);
    REG("getenv",  sys_getenv_fn);
#undef REG
    return m;
}
