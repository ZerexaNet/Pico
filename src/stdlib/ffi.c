#include "ffi.h"
#include "../value.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifndef _WIN32
#include <dlfcn.h>

static Value ffi_load(int argc, Value *argv) {
    if (argc < 1 || !IS_STR(argv[0])) return VAL_NIL_V;
    void *h = dlopen(argv[0].string->data, RTLD_LAZY);
    if (!h) return VAL_NIL_V;
    return VAL_INT_V((int64_t)(uintptr_t)h);
}

static Value ffi_sym(int argc, Value *argv) {
    if (argc < 2 || !IS_INT(argv[0]) || !IS_STR(argv[1])) return VAL_NIL_V;
    void *h   = (void*)(uintptr_t)argv[0].integer;
    void *sym = dlsym(h, argv[1].string->data);
    if (!sym) return VAL_NIL_V;
    return VAL_INT_V((int64_t)(uintptr_t)sym);
}

/* call a function pointer with up to 6 int/float args, returns int */
typedef int64_t (*FFIFn6)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);

static Value ffi_call(int argc, Value *argv) {
    if (argc < 1 || !IS_INT(argv[0])) return VAL_NIL_V;
    FFIFn6 fn = (FFIFn6)(uintptr_t)argv[0].integer;
    int64_t args[6] = {0};
    for (int i = 1; i < argc && i <= 6; i++)
        args[i-1] = IS_INT(argv[i]) ? argv[i].integer :
                    IS_FLOAT(argv[i]) ? (int64_t)argv[i].floating : 0;
    int64_t ret = fn(args[0],args[1],args[2],args[3],args[4],args[5]);
    return VAL_INT_V(ret);
}

static Value ffi_close(int argc, Value *argv) {
    if (argc < 1 || !IS_INT(argv[0])) return VAL_NIL_V;
    dlclose((void*)(uintptr_t)argv[0].integer);
    return VAL_NIL_V;
}

#else
static Value ffi_load(int argc, Value *argv)  { (void)argc;(void)argv; return VAL_NIL_V; }
static Value ffi_sym(int argc, Value *argv)   { (void)argc;(void)argv; return VAL_NIL_V; }
static Value ffi_call(int argc, Value *argv)  { (void)argc;(void)argv; return VAL_NIL_V; }
static Value ffi_close(int argc, Value *argv) { (void)argc;(void)argv; return VAL_NIL_V; }
#endif

ObjMap *stdlib_ffi_module(void) {
    ObjMap *m = map_new();
#define REG(name, fn) map_set(m, str_intern(name, (int)strlen(name)), VAL_NATIVE_V(fn))
    REG("load",  ffi_load);
    REG("sym",   ffi_sym);
    REG("call",  ffi_call);
    REG("close", ffi_close);
#undef REG
    return m;
}
