#include "mem.h"
#include "../value.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static Value mem_alloc(int argc, Value *argv) {
    if (argc < 1 || !IS_INT(argv[0])) return VAL_NIL_V;
    void *p = malloc((size_t)argv[0].integer);
    if (!p) return VAL_NIL_V;
    return VAL_INT_V((int64_t)(uintptr_t)p);
}

static Value mem_free(int argc, Value *argv) {
    if (argc < 1 || !IS_INT(argv[0])) return VAL_NIL_V;
    free((void*)(uintptr_t)argv[0].integer);
    return VAL_NIL_V;
}

#define MEM_READ(bits) \
static Value mem_read_u##bits(int argc, Value *argv) { \
    if (argc < 1 || !IS_INT(argv[0])) return VAL_NIL_V; \
    uint##bits##_t v; \
    memcpy(&v, (void*)(uintptr_t)argv[0].integer, sizeof(v)); \
    return VAL_INT_V((int64_t)v); \
}

#define MEM_WRITE(bits) \
static Value mem_write_u##bits(int argc, Value *argv) { \
    if (argc < 2 || !IS_INT(argv[0]) || !IS_INT(argv[1])) return VAL_NIL_V; \
    uint##bits##_t v = (uint##bits##_t)argv[1].integer; \
    memcpy((void*)(uintptr_t)argv[0].integer, &v, sizeof(v)); \
    return VAL_NIL_V; \
}

MEM_READ(8)  MEM_READ(16)  MEM_READ(32)  MEM_READ(64)
MEM_WRITE(8) MEM_WRITE(16) MEM_WRITE(32) MEM_WRITE(64)

static Value mem_copy(int argc, Value *argv) {
    if (argc < 3 || !IS_INT(argv[0]) || !IS_INT(argv[1]) || !IS_INT(argv[2])) return VAL_NIL_V;
    memcpy((void*)(uintptr_t)argv[0].integer,
           (void*)(uintptr_t)argv[1].integer,
           (size_t)argv[2].integer);
    return VAL_NIL_V;
}

static Value mem_set(int argc, Value *argv) {
    if (argc < 3 || !IS_INT(argv[0]) || !IS_INT(argv[1]) || !IS_INT(argv[2])) return VAL_NIL_V;
    memset((void*)(uintptr_t)argv[0].integer, (int)argv[1].integer, (size_t)argv[2].integer);
    return VAL_NIL_V;
}

static Value mem_addr_of_str(int argc, Value *argv) {
    if (argc < 1 || !IS_STR(argv[0])) return VAL_NIL_V;
    return VAL_INT_V((int64_t)(uintptr_t)argv[0].string->data);
}

ObjMap *stdlib_mem_module(void) {
    ObjMap *m = map_new();
#define REG(name, fn) map_set(m, str_intern(name, (int)strlen(name)), VAL_NATIVE_V(fn))
    REG("alloc",      mem_alloc);
    REG("free",       mem_free);
    REG("read_u8",    mem_read_u8);
    REG("read_u16",   mem_read_u16);
    REG("read_u32",   mem_read_u32);
    REG("read_u64",   mem_read_u64);
    REG("write_u8",   mem_write_u8);
    REG("write_u16",  mem_write_u16);
    REG("write_u32",  mem_write_u32);
    REG("write_u64",  mem_write_u64);
    REG("copy",       mem_copy);
    REG("set",        mem_set);
    REG("addr_of_str",mem_addr_of_str);
#undef REG
    return m;
}
