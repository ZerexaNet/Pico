#ifdef __EMSCRIPTEN__
/* Stub all platform-specific symbols for WASM build */
#include "value.h"
#include <stddef.h>

/* thread */
#include "thread.h"
void  scheduler_init(void) {}
Value thread_spawn(ObjFn *fn, int argc, Value *argv) { (void)fn;(void)argc;(void)argv; return VAL_NIL_V; }
Value thread_join(ObjThread *t)  { (void)t; return VAL_NIL_V; }
Value mutex_new(void)            { return VAL_NIL_V; }
void  mutex_lock(ObjMutex *m)    { (void)m; }
void  mutex_unlock(ObjMutex *m)  { (void)m; }
Value channel_new(int cap)       { (void)cap; return VAL_NIL_V; }
void  channel_send(ObjChannel *c, Value v) { (void)c;(void)v; }
Value channel_recv(ObjChannel *c){ (void)c; return VAL_NIL_V; }

/* coroutine */
Value coro_new(void *fn, void *env)    { (void)fn;(void)env; return VAL_NIL_V; }
Value coro_resume(void *c, Value arg)  { (void)c;(void)arg; return VAL_NIL_V; }
void  coro_yield(Value v)              { (void)v; }

/* net */
Value net_listen(int argc, Value *argv) { (void)argc;(void)argv; return VAL_NIL_V; }

/* qt */
Value qt_window_new(int argc, Value *argv)    { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_button_new(int argc, Value *argv)    { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_label_new(int argc, Value *argv)     { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_input_new(int argc, Value *argv)     { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_app_new(int argc, Value *argv)       { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_app_exec(int argc, Value *argv)      { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_widget_show(int argc, Value *argv)   { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_widget_set_title(int argc, Value *argv) { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_widget_add(int argc, Value *argv)    { (void)argc;(void)argv; return VAL_NIL_V; }
Value qt_widget_on_click(int argc, Value *argv){ (void)argc;(void)argv; return VAL_NIL_V; }

/* file module — filesystem not available in WASM */
#include "value.h"
ObjMap *stdlib_file_module(void) { return map_new(); }

/* mem / sys / ffi — not available in WASM */
ObjMap *stdlib_mem_module(void) { return map_new(); }
ObjMap *stdlib_sys_module(void) { return map_new(); }
ObjMap *stdlib_ffi_module(void) { return map_new(); }

#endif /* __EMSCRIPTEN__ */
