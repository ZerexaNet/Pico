#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "error.h"
#include "thread.h"
#include "coroutine.h"
#include "stdlib/net.h"

#include "stdlib/qt_bind.h"

// stdlib module constructors
ObjMap *stdlib_str_module(void);
ObjMap *stdlib_file_module(void);
ObjMap *stdlib_maplib_module(void);
ObjMap *stdlib_mem_module(void);
ObjMap *stdlib_sys_module(void);
ObjMap *stdlib_ffi_module(void);

static Interpreter *current_vm = NULL;

void gc_mark_roots(void) {
    if (!current_vm) return;
    gc_mark_env(current_vm->globals);
}

Env *env_new(Env *parent) {
    Env *e = malloc(sizeof(Env));
    e->count = 0;
    e->parent = parent;
    return e;
}

void env_set(Env *e, ObjStr *key, Value val) {
    for (int i = 0; i < e->count; i++) {
        if (e->keys[i] == key) { e->vals[i] = val; return; }
    }
    if (e->count < ENV_SIZE) {
        e->keys[e->count] = key;
        e->vals[e->count] = val;
        e->count++;
    }
}

bool env_get(Env *e, ObjStr *key, Value *out) {
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (e->keys[i] == key) { *out = e->vals[i]; return true; }
        }
        e = e->parent;
    }
    return false;
}

bool env_assign(Env *e, ObjStr *key, Value val) {
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (e->keys[i] == key) { e->vals[i] = val; return true; }
        }
        e = e->parent;
    }
    return false;
}

void env_free(Env *e) { free(e); }

static void set_error(Interpreter *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);
    vm->has_error = true;
}

static double to_float(Value v) {
    if (v.type == VAL_INT) return (double)v.integer;
    return v.floating;
}

Value interp_exec(Interpreter *vm, AstNode *node, Env *env) {
    if (!node || vm->has_error) return VAL_NIL_V;

    switch (node->type) {
    case NODE_INT:    return VAL_INT_V(node->ival);
    case NODE_FLOAT:  return VAL_FLOAT_V(node->fval);
    case NODE_BOOL:   return VAL_BOOL_V(node->bval);
    case NODE_NIL:    return VAL_NIL_V;
    case NODE_STRING: return VAL_STR_V(str_intern(node->sval.s, node->sval.len));

    case NODE_FSTRING: {
        char buf[4096]; buf[0] = 0;
        for (int i = 0; i < node->fstr.parts.count; i++) {
            AstNode *part = node->fstr.parts.items[i];
            Value v = interp_exec(vm, part, env);
            if (vm->has_error) return VAL_NIL_V;
            char tmp[512];
            if (IS_STR(v)) snprintf(tmp, sizeof(tmp), "%.*s", v.string->len, v.string->data);
            else if (IS_INT(v)) snprintf(tmp, sizeof(tmp), "%lld", (long long)v.integer);
            else if (IS_FLOAT(v)) snprintf(tmp, sizeof(tmp), "%g", v.floating);
            else if (IS_BOOL(v)) snprintf(tmp, sizeof(tmp), "%s", v.boolean ? "true" : "false");
            else snprintf(tmp, sizeof(tmp), "nil");
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        }
        return VAL_STR_V(str_intern(buf, (int)strlen(buf)));
    }

    case NODE_IDENT: {
        ObjStr *key = str_intern(node->sval.s, node->sval.len);
        Value v;
        if (env_get(env, key, &v)) return v;
        set_error(vm, "undefined variable '%s'", node->sval.s);
        return VAL_NIL_V;
    }

    case NODE_LET: {
        Value v = node->let.value ? interp_exec(vm, node->let.value, env) : VAL_NIL_V;
        if (vm->has_error) return VAL_NIL_V;
        // 可选类型检查（--strict 模式或 type_ann 存在时）
        if (node->let.type_ann && vm->strict_types) {
            const char *t = node->let.type_ann;
            bool ok = true;
            if      (strcmp(t,"int")==0||strcmp(t,"整数")==0)   ok = IS_INT(v);
            else if (strcmp(t,"float")==0||strcmp(t,"浮点")==0) ok = IS_FLOAT(v)||IS_INT(v);
            else if (strcmp(t,"str")==0||strcmp(t,"字符串")==0) ok = IS_STR(v);
            else if (strcmp(t,"bool")==0||strcmp(t,"布尔")==0)  ok = IS_BOOL(v);
            else if (strcmp(t,"list")==0||strcmp(t,"列表")==0)  ok = IS_LIST(v);
            else if (strcmp(t,"map")==0||strcmp(t,"字典")==0)   ok = IS_MAP(v);
            if (!ok) { set_error(vm, "类型错误：'%s' 期望 %s", node->let.name, t); return VAL_NIL_V; }
        }
        ObjStr *key = str_intern(node->let.name, (int)strlen(node->let.name));
        env_set(env, key, v);
        return VAL_NIL_V;
    }

    case NODE_ASSIGN: {
        Value v = interp_exec(vm, node->assign.value, env);
        if (vm->has_error) return VAL_NIL_V;
        if (node->assign.name) {
            ObjStr *key = str_intern(node->assign.name, (int)strlen(node->assign.name));
            if (node->assign.op == TOK_PLUS_ASSIGN || node->assign.op == TOK_MINUS_ASSIGN) {
                Value cur;
                if (!env_get(env, key, &cur)) { set_error(vm, "undefined '%s'", node->assign.name); return VAL_NIL_V; }
                if (IS_INT(cur) && IS_INT(v))
                    v = VAL_INT_V(node->assign.op == TOK_PLUS_ASSIGN ? cur.integer + v.integer : cur.integer - v.integer);
                else
                    v = VAL_FLOAT_V(node->assign.op == TOK_PLUS_ASSIGN ? to_float(cur) + to_float(v) : to_float(cur) - to_float(v));
            }
            if (!env_assign(env, key, v)) env_set(env, key, v);
        } else {
            AstNode *left = node->binop.left;
            if (left->type == NODE_FIELD_ACCESS) {
                Value obj = interp_exec(vm, left->field.obj, env);
                if (obj.type != VAL_INSTANCE) { set_error(vm, "not an instance"); return VAL_NIL_V; }
                ObjInstance *inst = obj.instance;
                ObjStr *field = str_intern(left->field.field, (int)strlen(left->field.field));
                bool found = false;
                for (int i = 0; i < inst->def->field_count; i++) {
                    if (inst->def->fields[i].name == field) {
                        if (node->assign.op == TOK_PLUS_ASSIGN || node->assign.op == TOK_MINUS_ASSIGN) {
                            Value cur = inst->fields[i];
                            if (IS_INT(cur) && IS_INT(v))
                                v = VAL_INT_V(node->assign.op == TOK_PLUS_ASSIGN ? cur.integer + v.integer : cur.integer - v.integer);
                            else
                                v = VAL_FLOAT_V(node->assign.op == TOK_PLUS_ASSIGN ? to_float(cur) + to_float(v) : to_float(cur) - to_float(v));
                        }
                        inst->fields[i] = v;
                        found = true; break;
                    }
                }
                if (!found) set_error(vm, "no field '%s'", field->data);
            } else if (left->type == NODE_INDEX) {
                Value obj = interp_exec(vm, left->index.obj, env);
                Value idx = interp_exec(vm, left->index.idx, env);
                if (IS_LIST(obj)) {
                    if (!IS_INT(idx)) { set_error(vm, "index must be int"); return VAL_NIL_V; }
                    int i = (int)idx.integer;
                    if (i < 0 || i >= obj.list->len) { set_error(vm, "out of bounds"); return VAL_NIL_V; }
                    obj.list->items[i] = v;
                } else if (IS_MAP(obj)) {
                    if (!IS_STR(idx)) { set_error(vm, "map index must be string"); return VAL_NIL_V; }
                    map_set(obj.map, idx.string, v);
                }
            }
        }
        return VAL_NIL_V;
    }

    case NODE_BINOP: {
        if (node->binop.op == TOK_AND) {
            Value l = interp_exec(vm, node->binop.left, env);
            if (vm->has_error) return VAL_NIL_V;
            if (!IS_TRUTHY(l)) return l;
            return interp_exec(vm, node->binop.right, env);
        }
        if (node->binop.op == TOK_OR) {
            Value l = interp_exec(vm, node->binop.left, env);
            if (vm->has_error) return VAL_NIL_V;
            if (IS_TRUTHY(l)) return l;
            return interp_exec(vm, node->binop.right, env);
        }
        Value l = interp_exec(vm, node->binop.left, env);
        if (vm->has_error) return VAL_NIL_V;
        Value r = interp_exec(vm, node->binop.right, env);
        if (vm->has_error) return VAL_NIL_V;
        PicoTokenType op = node->binop.op;
        if (op == TOK_EQ)  return VAL_BOOL_V(value_equal(l, r));
        if (op == TOK_NEQ) return VAL_BOOL_V(!value_equal(l, r));
        if (op == TOK_DOTDOT) {
            if (!IS_INT(l) || !IS_INT(r)) { set_error(vm, ".. requires ints"); return VAL_NIL_V; }
            ObjList *res = list_new();
            for (int64_t i = l.integer; i <= r.integer; i++) list_push(res, VAL_INT_V(i));
            return VAL_LIST_V(res);
        }
        if (op == TOK_PLUS && IS_STR(l) && IS_STR(r))
            return VAL_STR_V(str_concat(l.string, r.string));
        if (IS_INT(l) && IS_INT(r)) {
            int64_t a = l.integer, b = r.integer;
            switch (op) {
            case TOK_PLUS:    return VAL_INT_V(a + b);
            case TOK_MINUS:   return VAL_INT_V(a - b);
            case TOK_STAR:    return VAL_INT_V(a * b);
            case TOK_SLASH:   if (!b) { set_error(vm, "division by zero"); return VAL_NIL_V; }
                              return VAL_INT_V(a / b);
            case TOK_PERCENT: if (!b) { set_error(vm, "division by zero"); return VAL_NIL_V; }
                              return VAL_INT_V(a % b);
            case TOK_LT:  return VAL_BOOL_V(a < b);
            case TOK_LE:  return VAL_BOOL_V(a <= b);
            case TOK_GT:  return VAL_BOOL_V(a > b);
            case TOK_GE:  return VAL_BOOL_V(a >= b);
            case TOK_AMP:    return VAL_INT_V(a & b);
            case TOK_PIPE:   return VAL_INT_V(a | b);
            case TOK_CARET:  return VAL_INT_V(a ^ b);
            case TOK_LSHIFT: return VAL_INT_V(a << b);
            case TOK_RSHIFT: return VAL_INT_V(a >> b);
            default: break;
            }
        }
        if (IS_NUM(l) && IS_NUM(r)) {
            double a = to_float(l), b = to_float(r);
            switch (op) {
            case TOK_PLUS:    return VAL_FLOAT_V(a + b);
            case TOK_MINUS:   return VAL_FLOAT_V(a - b);
            case TOK_STAR:    return VAL_FLOAT_V(a * b);
            case TOK_SLASH:   return VAL_FLOAT_V(a / b);
            case TOK_PERCENT: return VAL_FLOAT_V(fmod(a, b));
            case TOK_LT:  return VAL_BOOL_V(a < b);
            case TOK_LE:  return VAL_BOOL_V(a <= b);
            case TOK_GT:  return VAL_BOOL_V(a > b);
            case TOK_GE:  return VAL_BOOL_V(a >= b);
            default: break;
            }
        }
        set_error(vm, "invalid operands for binary op");
        return VAL_NIL_V;
    }

    case NODE_UNOP: {
        Value v = interp_exec(vm, node->unop.operand, env);
        if (vm->has_error) return VAL_NIL_V;
        if (node->unop.op == TOK_MINUS) {
            if (IS_INT(v))   return VAL_INT_V(-v.integer);
            if (IS_FLOAT(v)) return VAL_FLOAT_V(-v.floating);
        }
        if (node->unop.op == TOK_TILDE) {
            if (IS_INT(v)) return VAL_INT_V(~v.integer);
        }
        if (node->unop.op == TOK_NOT) return VAL_BOOL_V(!IS_TRUTHY(v));
        set_error(vm, "invalid unary op");
        return VAL_NIL_V;
    }

    case NODE_IF: {
        Value cond = interp_exec(vm, node->ifnode.cond, env);
        if (vm->has_error) return VAL_NIL_V;
        if (IS_TRUTHY(cond)) return interp_exec(vm, node->ifnode.then, env);
        if (node->ifnode.els) return interp_exec(vm, node->ifnode.els, env);
        return VAL_NIL_V;
    }

    case NODE_WHILE: {
        while (1) {
            Value cond = interp_exec(vm, node->whilenode.cond, env);
            if (vm->has_error || !IS_TRUTHY(cond)) break;
            interp_exec(vm, node->whilenode.body, env);
            if (vm->has_error || vm->returning) break;
            if (vm->breaking) { vm->breaking = false; break; }
            if (vm->continuing) vm->continuing = false;
        }
        return VAL_NIL_V;
    }

    case NODE_FOR: {
        Value iter = interp_exec(vm, node->fornode.iter, env);
        if (vm->has_error) return VAL_NIL_V;
        ObjStr *var = str_intern(node->fornode.var, (int)strlen(node->fornode.var));
        Env *loop_env = env_new(env);
        if (IS_LIST(iter)) {
            for (int i = 0; i < iter.list->len; i++) {
                env_set(loop_env, var, iter.list->items[i]);
                interp_exec(vm, node->fornode.body, loop_env);
                if (vm->has_error || vm->returning) break;
                if (vm->breaking) { vm->breaking = false; break; }
                if (vm->continuing) vm->continuing = false;
            }
        } else if (iter.type == VAL_COROUTINE) {
            ObjCoro *coro = iter.coro;
            while (!coro->is_done) {
                Value val = coro_resume(coro, VAL_NIL_V);
                if (coro->is_done) break;
                env_set(loop_env, var, val);
                interp_exec(vm, node->fornode.body, loop_env);
                if (vm->has_error || vm->returning) break;
                if (vm->breaking) { vm->breaking = false; break; }
                if (vm->continuing) vm->continuing = false;
            }
        } else {
            set_error(vm, "for: not iterable");
        }
        env_free(loop_env);
        return VAL_NIL_V;
    }

    case NODE_FN:
    case NODE_ASYNC_FN: {
        ObjFn *fn = gc_alloc(sizeof(ObjFn));
        fn->hdr.type = OBJ_FN;
        fn->hdr.marked = false;
        fn->hdr.next = gc_objects; gc_objects = (Obj*)fn;
        fn->name = node->fn.name ? str_intern(node->fn.name, (int)strlen(node->fn.name)) : NULL;
        fn->arity = node->fn.param_count;
        fn->body = node->fn.body;
        fn->closure = env;
        fn->is_generator = node->fn.is_generator;
        fn->is_async = (node->type == NODE_ASYNC_FN);
        fn->params = malloc(sizeof(char*) * fn->arity);
        for (int i = 0; i < fn->arity; i++) fn->params[i] = strdup(node->fn.params[i]);
        Value v = VAL_FN_V(fn);
        if (node->fn.name) {
            ObjStr *key = str_intern(node->fn.name, (int)strlen(node->fn.name));
            env_set(env, key, v);
        }
        return v;
    }

    case NODE_CALL: {
        Value callee = interp_exec(vm, node->call.callee, env);
        if (vm->has_error) return VAL_NIL_V;
        int argc = node->call.args.count;
        Value *argv = argc ? malloc(sizeof(Value) * argc) : NULL;
        for (int i = 0; i < argc; i++) {
            argv[i] = interp_exec(vm, node->call.args.items[i], env);
            if (vm->has_error) { free(argv); return VAL_NIL_V; }
        }
        Value result = VAL_NIL_V;
        if (callee.type == VAL_NATIVE) {
            result = callee.native(argc, argv);
        } else if (callee.type == VAL_FN) {
            ObjFn *fn = callee.fn;
            Env *call_env = env_new(fn->closure);
            for (int i = 0; i < fn->arity && i < argc; i++) {
                ObjStr *pname = str_intern(fn->params[i], (int)strlen(fn->params[i]));
                env_set(call_env, pname, argv[i]);
            }
            if (fn->is_generator) {
                result = coro_new(fn, call_env);
            } else {
                result = interp_exec(vm, fn->body, call_env);
                if (vm->returning) {
                    result = vm->return_val;
                    vm->returning = false;
                }
            }
            env_free(call_env);
        } else {
            set_error(vm, "not callable");
        }
        free(argv);
        return result;
    }

    case NODE_RETURN: {
        Value v = node->retnode.value ? interp_exec(vm, node->retnode.value, env) : VAL_NIL_V;
        vm->returning = true;
        vm->return_val = v;
        return v;
    }

    case NODE_YIELD: {
        Value v = node->yieldnode.value ? interp_exec(vm, node->yieldnode.value, env) : VAL_NIL_V;
        coro_yield(v);
        return v;
    }

    case NODE_BLOCK: {
        Env *new_env = env_new(env);
        Value last = VAL_NIL_V;
        for (int i = 0; i < node->block.stmts.count; i++) {
            last = interp_exec(vm, node->block.stmts.items[i], new_env);
            if (vm->has_error || vm->returning || vm->breaking || vm->continuing) break;
        }
        env_free(new_env);
        return last;
    }

    case NODE_PROGRAM: {
        Value last = VAL_NIL_V;
        for (int i = 0; i < node->program.stmts.count; i++) {
            last = interp_exec(vm, node->program.stmts.items[i], env);
            if (vm->has_error) break;
        }
        return last;
    }

    case NODE_EXPR_STMT:
        return interp_exec(vm, node->exprstmt.expr, env);

    case NODE_MATCH: {
        Value subject = interp_exec(vm, node->matchnode.subject, env);
        if (vm->has_error) return VAL_NIL_V;
        bool matched = false;
        for (int i = 0; i < node->matchnode.patterns.count; i++) {
            AstNode *pat_node = node->matchnode.patterns.items[i];
            // 特殊处理 "_" (通配符)
            if (pat_node->type == NODE_IDENT && strcmp(pat_node->sval.s, "_") == 0) {
                interp_exec(vm, node->matchnode.bodies.items[i], env);
                matched = true; break;
            }
            Value pat = interp_exec(vm, pat_node, env);
            if (value_equal(subject, pat)) {
                interp_exec(vm, node->matchnode.bodies.items[i], env);
                matched = true; break;
            }
        }
        if (!matched && node->matchnode.default_body) {
            interp_exec(vm, node->matchnode.default_body, env);
        }
        return VAL_NIL_V;
    }

    case NODE_LIST: {
        ObjList *l = list_new();
        for (int i = 0; i < node->list.elements.count; i++) {
            list_push(l, interp_exec(vm, node->list.elements.items[i], env));
            if (vm->has_error) return VAL_NIL_V;
        }
        return VAL_LIST_V(l);
    }

    case NODE_MAP: {
        ObjMap *m = map_new();
        for (int i = 0; i < node->map.keys.count; i++) {
            Value k = interp_exec(vm, node->map.keys.items[i], env);
            if (!IS_STR(k)) { set_error(vm, "map key must be string"); return VAL_NIL_V; }
            Value v = interp_exec(vm, node->map.vals.items[i], env);
            map_set(m, k.string, v);
            if (vm->has_error) return VAL_NIL_V;
        }
        return VAL_MAP_V(m);
    }

    case NODE_INDEX: {
        Value obj = interp_exec(vm, node->index.obj, env);
        Value idx = interp_exec(vm, node->index.idx, env);
        if (IS_LIST(obj)) {
            if (!IS_INT(idx)) { set_error(vm, "index must be int"); return VAL_NIL_V; }
            return list_get(obj.list, (int)idx.integer);
        }
        if (IS_MAP(obj)) {
            if (!IS_STR(idx)) { set_error(vm, "map index must be string"); return VAL_NIL_V; }
            Value res;
            if (map_get(obj.map, idx.string, &res)) return res;
            return VAL_NIL_V;
        }
        set_error(vm, "not indexable");
        return VAL_NIL_V;
    }

    case NODE_TRY: {
        Env *try_env = env_new(env);
        Value res = interp_exec(vm, node->trynode.body, try_env);
        if (vm->has_error) {
            vm->has_error = false;
            if (node->trynode.catch_body) {
                Env *catch_env = env_new(env);
                if (node->trynode.catch_var) {
                    ObjStr *msg = str_intern(vm->error_msg, (int)strlen(vm->error_msg));
                    env_set(catch_env, str_intern(node->trynode.catch_var->sval.s, (int)strlen(node->trynode.catch_var->sval.s)), VAL_STR_V(msg));
                }
                res = interp_exec(vm, node->trynode.catch_body, catch_env);
                env_free(catch_env);
            }
        }
        env_free(try_env);
        return res;
    }

    case NODE_BREAK:    vm->breaking = true; return VAL_NIL_V;
    case NODE_CONTINUE: vm->continuing = true; return VAL_NIL_V;

    case NODE_SPAWN: {
        Value v = interp_exec(vm, node->spawnnode.expr, env);
        if (v.type != VAL_FN) { set_error(vm, "spawn requires a function"); return VAL_NIL_V; }
        return thread_spawn(v.fn, 0, NULL);
    }

    case NODE_METHOD_CALL: {
        Value obj = interp_exec(vm, node->mcall.obj, env);
        if (vm->has_error) return VAL_NIL_V;
        // super.method() — obj 是 VAL_STRUCT_DEF，self 从当前 env 取
        if (obj.type == VAL_STRUCT_DEF) {
            ObjStr *mname = str_intern(node->mcall.method, (int)strlen(node->mcall.method));
            Value m;
            ObjStructDef *cls = obj.structdef;
            while (cls) { if (map_get(cls->methods, mname, &m)) break; cls = cls->parent; }
            if (cls) {
                Value self_val; env_get(env, str_intern("self", 4), &self_val);
                int argc = node->mcall.args.count;
                Value *argv = argc ? malloc(sizeof(Value) * argc) : NULL;
                for (int i = 0; i < argc; i++) argv[i] = interp_exec(vm, node->mcall.args.items[i], env);
                ObjFn *fn = m.fn;
                Env *call_env = env_new(fn->closure);
                env_set(call_env, str_intern("self", 4), self_val);
                env_set(call_env, str_intern("自身", 6), self_val);
                for (int i = 0; i < fn->arity && i < argc; i++)
                    env_set(call_env, str_intern(fn->params[i], (int)strlen(fn->params[i])), argv[i]);
                Value res = interp_exec(vm, fn->body, call_env);
                if (vm->returning) { res = vm->return_val; vm->returning = false; }
                env_free(call_env); free(argv);
                return res;
            }
            set_error(vm, "super has no method '%s'", node->mcall.method);
            return VAL_NIL_V;
        }
        if (IS_THREAD(obj)) {
            if (strcmp(node->mcall.method, "wait") == 0 || strcmp(node->mcall.method, "等待") == 0) {
                return thread_join(obj.thread);
            }
        }
        if (obj.type == VAL_MUTEX) {
            ObjMutex *m = (ObjMutex*)obj.mutex;
            if (strcmp(node->mcall.method, "lock") == 0 || strcmp(node->mcall.method, "锁定") == 0) {
                mutex_lock(m); return VAL_NIL_V;
            }
            if (strcmp(node->mcall.method, "unlock") == 0 || strcmp(node->mcall.method, "解锁") == 0) {
                mutex_unlock(m); return VAL_NIL_V;
            }
        }
        if (obj.type == VAL_CHANNEL) {
            ObjChannel *c = (ObjChannel*)obj.channel;
            if (strcmp(node->mcall.method, "send") == 0 || strcmp(node->mcall.method, "发送") == 0) {
                Value val = interp_exec(vm, node->mcall.args.items[0], env);
                channel_send(c, val); return VAL_NIL_V;
            }
            if (strcmp(node->mcall.method, "recv") == 0 || strcmp(node->mcall.method, "接收") == 0) {
                return channel_recv(c);
            }
        }
        if (obj.type == VAL_INSTANCE) {
            ObjInstance *inst = obj.instance;
            ObjStr *mname = str_intern(node->mcall.method, (int)strlen(node->mcall.method));
            Value m;
            // 沿继承链查找方法
            ObjStructDef *cls = inst->def;
            while (cls) {
                if (map_get(cls->methods, mname, &m)) break;
                cls = cls->parent;
            }
            if (cls) {
                int argc = node->mcall.args.count;
                Value *argv = argc ? malloc(sizeof(Value) * argc) : NULL;
                for (int i = 0; i < argc; i++) argv[i] = interp_exec(vm, node->mcall.args.items[i], env);
                ObjFn *fn = m.fn;
                Env *call_env = env_new(fn->closure);
                env_set(call_env, str_intern("self", 4), obj);
                env_set(call_env, str_intern("自身", 6), obj);
                // super: 绑定到父类定义，供方法内调用 super.method()
                if (inst->def->parent) {
                    env_set(call_env, str_intern("super", 5), VAL_STRUCT_DEF_V(inst->def->parent));
                    env_set(call_env, str_intern("父类", 6), VAL_STRUCT_DEF_V(inst->def->parent));
                }
                for (int i = 0; i < fn->arity && i < argc; i++) {
                    env_set(call_env, str_intern(fn->params[i], (int)strlen(fn->params[i])), argv[i]);
                }
                Value res = interp_exec(vm, fn->body, call_env);
                if (vm->returning) { res = vm->return_val; vm->returning = false; }
                env_free(call_env); free(argv);
                return res;
            }
        }
        set_error(vm, "unknown method '%s'", node->mcall.method);
        return VAL_NIL_V;
    }

    case NODE_STRUCT_DEF: {
        ObjStr *name = str_intern(node->structdef.name, (int)strlen(node->structdef.name));
        ObjStructDef *def = struct_def_new(name, node->structdef.field_count);
        for (int i = 0; i < node->structdef.field_count; i++) {
            def->fields[i].name = str_intern(node->structdef.field_names[i], (int)strlen(node->structdef.field_names[i]));
            def->fields[i].index = i;
        }
        for (int i = 0; i < node->structdef.methods.count; i++) {
            Value fn = interp_exec(vm, node->structdef.methods.items[i], env);
            map_set(def->methods, fn.fn->name, fn);
        }
        // 继承
        if (node->structdef.parent_name) {
            ObjStr *pname = str_intern(node->structdef.parent_name, (int)strlen(node->structdef.parent_name));
            Value pv;
            if (env_get(env, pname, &pv) && pv.type == VAL_STRUCT_DEF)
                def->parent = pv.structdef;
        }
        env_set(env, name, VAL_STRUCT_DEF_V(def));
        return VAL_NIL_V;
    }

    case NODE_STRUCT_LIT: {
        Value v;
        ObjStr *name = str_intern(node->structlit.name, (int)strlen(node->structlit.name));
        if (!env_get(env, name, &v) || v.type != VAL_STRUCT_DEF) { set_error(vm, "not a struct"); return VAL_NIL_V; }
        ObjStructDef *def = v.structdef;
        ObjInstance *inst = instance_new(def);
        for (int i = 0; i < node->structlit.keys.count; i++) {
            ObjStr *key = str_intern(node->structlit.keys.items[i]->sval.s, node->structlit.keys.items[i]->sval.len);
            Value val = interp_exec(vm, node->structlit.vals.items[i], env);
            for (int j = 0; j < def->field_count; j++) {
                if (def->fields[j].name == key) { inst->fields[j] = val; break; }
            }
        }
        return VAL_INST_V(inst);
    }

    case NODE_FIELD_ACCESS: {
        Value obj = interp_exec(vm, node->field.obj, env);
        if (vm->has_error) return VAL_NIL_V;
        if (obj.type != VAL_INSTANCE) { set_error(vm, "not an instance"); return VAL_NIL_V; }
        ObjInstance *inst = obj.instance;
        ObjStr *field = str_intern(node->field.field, (int)strlen(node->field.field));
        for (int i = 0; i < inst->def->field_count; i++) {
            if (inst->def->fields[i].name == field) return inst->fields[i];
        }
        Value m;
        if (map_get(inst->def->methods, field, &m)) return m;
        set_error(vm, "no field '%s'", field->data);
        return VAL_NIL_V;
    }

    default:
        return VAL_NIL_V;
    }
}

static Value native_print(int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        value_print(argv[i]);
    }
    printf("\n");
    return VAL_NIL_V;
}

void interp_init(Interpreter *vm) {
    memset(vm, 0, sizeof(Interpreter));
    vm->globals      = env_new(NULL);
    vm->strict_types = getenv("PICO_STRICT") != NULL;
    current_vm = vm;
    interp_register_stdlib(vm);
}

Interpreter *interp_get_current() {
    return current_vm;
}

static Value native_range(int argc, Value *argv) {
    int64_t start = 0, end = 0;
    if (argc == 0) return VAL_NIL_V;
    if (argc == 1) end = (int64_t)to_float(argv[0]);
    else {
        start = (int64_t)to_float(argv[0]);
        end = (int64_t)to_float(argv[1]);
    }
    ObjList *l = list_new();
    for (int64_t i = start; i < end; i++) list_push(l, VAL_INT_V(i));
    return VAL_LIST_V(l);
}

static Value native_mutex(int argc, Value *argv) {
    (void)argc; (void)argv;
    return mutex_new();
}

static Value native_channel(int argc, Value *argv) {
    int cap = (argc > 0 && IS_INT(argv[0])) ? (int)argv[0].integer : 1;
    return channel_new(cap);
}

Value json_stringify(int argc, Value *argv);

void interp_register_stdlib(Interpreter *vm) {
    env_set(vm->globals, str_intern("print", 5), VAL_NATIVE_V(native_print));
    env_set(vm->globals, str_intern("打印", 6), VAL_NATIVE_V(native_print));
    env_set(vm->globals, str_intern("range", 5), VAL_NATIVE_V(native_range));
    env_set(vm->globals, str_intern("Mutex", 5), VAL_NATIVE_V(native_mutex));
    env_set(vm->globals, str_intern("互斥锁", 9), VAL_NATIVE_V(native_mutex));
    env_set(vm->globals, str_intern("Channel", 7), VAL_NATIVE_V(native_channel));
    env_set(vm->globals, str_intern("通道", 6), VAL_NATIVE_V(native_channel));

    // 网络模块
    ObjMap *net = map_new();
    map_set(net, str_intern("listen", 6), VAL_NATIVE_V(net_listen));
    map_set(net, str_intern("监听", 6), VAL_NATIVE_V(net_listen));
    env_set(vm->globals, str_intern("net", 3), VAL_MAP_V(net));
    env_set(vm->globals, str_intern("网络", 6), VAL_MAP_V(net));

    // 数据模块
    ObjMap *data = map_new();
    map_set(data, str_intern("json", 4), VAL_NATIVE_V(json_stringify));
    env_set(vm->globals, str_intern("data", 4), VAL_MAP_V(data));
    env_set(vm->globals, str_intern("数据", 6), VAL_MAP_V(data));

    // 界面模块
    ObjMap *ui = map_new();
    map_set(ui, str_intern("window",   6),  VAL_NATIVE_V(qt_window_new));
    map_set(ui, str_intern("窗口",     6),  VAL_NATIVE_V(qt_window_new));
    map_set(ui, str_intern("button",   6),  VAL_NATIVE_V(qt_button_new));
    map_set(ui, str_intern("按钮",     6),  VAL_NATIVE_V(qt_button_new));
    map_set(ui, str_intern("label",    5),  VAL_NATIVE_V(qt_label_new));
    map_set(ui, str_intern("标签",     6),  VAL_NATIVE_V(qt_label_new));
    map_set(ui, str_intern("input",    5),  VAL_NATIVE_V(qt_input_new));
    map_set(ui, str_intern("输入框",   9),  VAL_NATIVE_V(qt_input_new));
    map_set(ui, str_intern("show",     4),  VAL_NATIVE_V(qt_widget_show));
    map_set(ui, str_intern("显示",     6),  VAL_NATIVE_V(qt_widget_show));
    map_set(ui, str_intern("add",      3),  VAL_NATIVE_V(qt_widget_add));
    map_set(ui, str_intern("添加",     6),  VAL_NATIVE_V(qt_widget_add));
    map_set(ui, str_intern("on_click", 8),  VAL_NATIVE_V(qt_widget_on_click));
    map_set(ui, str_intern("点击时",   9),  VAL_NATIVE_V(qt_widget_on_click));
    map_set(ui, str_intern("exec",     4),  VAL_NATIVE_V(qt_app_exec));
    map_set(ui, str_intern("运行",     6),  VAL_NATIVE_V(qt_app_exec));
    env_set(vm->globals, str_intern("ui", 2),   VAL_MAP_V(ui));
    env_set(vm->globals, str_intern("界面", 6), VAL_MAP_V(ui));

    // str module
    ObjMap *str_mod = stdlib_str_module();
    env_set(vm->globals, str_intern("str",    3), VAL_MAP_V(str_mod));
    env_set(vm->globals, str_intern("字符串模块", 15), VAL_MAP_V(str_mod));

    // file module
    ObjMap *file_mod = stdlib_file_module();
    env_set(vm->globals, str_intern("file", 4), VAL_MAP_V(file_mod));
    env_set(vm->globals, str_intern("文件",  6), VAL_MAP_V(file_mod));

    // maplib module
    ObjMap *map_mod = stdlib_maplib_module();
    env_set(vm->globals, str_intern("maplib",   6), VAL_MAP_V(map_mod));
    env_set(vm->globals, str_intern("字典模块", 12), VAL_MAP_V(map_mod));

    // mem module
    ObjMap *mem_mod = stdlib_mem_module();
    env_set(vm->globals, str_intern("mem", 3), VAL_MAP_V(mem_mod));

    // sys module
    ObjMap *sys_mod = stdlib_sys_module();
    env_set(vm->globals, str_intern("sys", 3), VAL_MAP_V(sys_mod));

    // ffi module
    ObjMap *ffi_mod = stdlib_ffi_module();
    env_set(vm->globals, str_intern("ffi", 3), VAL_MAP_V(ffi_mod));
}

Value interp_run_string(const char *src, const char *filename) {
    Interpreter vm;
    interp_init(&vm);
    vm.filename = filename;

    Lexer l;
    lexer_init(&l, src);
    Arena a;
    arena_init(&a);
    Parser p;
    parser_init(&p, &l, &a, filename);

    AstNode *prog = parse_program(&p);
    Value res = VAL_NIL_V;
    if (p.error_count == 0) {
        res = interp_exec(&vm, prog, vm.globals);
        if (vm.has_error) {
            fprintf(stderr, "运行时错误: %s\n", vm.error_msg);
        }
    }

    arena_free(&a);
    // env_free(vm.globals); // globals might contain functions pointing to AST... 
    // Wait, if we free Arena, the AST is gone. So we can't really run functions after this.
    // For a simple script, it's fine.
    return res;
}

Value interp_run_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "无法打开文件: %s\n", filename); return VAL_NIL_V; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc(size + 1);
    if (fread(buf, 1, size, f) != (size_t)size && size > 0) {}
    buf[size] = '\0';
    fclose(f);
    Value res = interp_run_string(buf, filename);
    free(buf);
    return res;
}
