#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include "error.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void parser_init(Parser *p, Lexer *l, Arena *a, const char *filename) {
    p->lexer = l;
    p->arena = a;
    p->filename = filename;
    p->error_count = 0;
    p->cur  = lexer_next(l);
    p->peek = lexer_next(l);
}

static Token advance(Parser *p) {
    Token t = p->cur;
    p->cur  = p->peek;
    p->peek = lexer_next(p->lexer);
    // 跳过 NEWLINE/SEMICOLON（语句间）
    while (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_SEMICOLON) {
        p->cur  = p->peek;
        p->peek = lexer_next(p->lexer);
    }
    return t;
}

static Token advance_raw(Parser *p) {
    Token t = p->cur;
    p->cur  = p->peek;
    p->peek = lexer_next(p->lexer);
    return t;
}

static bool check(Parser *p, PicoTokenType t) { return p->cur.type == t; }
static bool match(Parser *p, PicoTokenType t) {
    if (!check(p, t)) return false;
    advance(p); return true;
}

static Token expect(Parser *p, PicoTokenType t) {
    if (p->cur.type != t) {
        pico_error(p->filename, p->cur.line, p->cur.col,
            "期望 '%s'，得到 '%s'", token_type_name(t), token_type_name(p->cur.type));
        p->error_count++;
    }
    return advance(p);
}

static void skip_newlines(Parser *p) {
    while (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_SEMICOLON)
        advance_raw(p);
}

// ── 前向声明 ─────────────────────────────────────────────────
static AstNode *parse_expr(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);
static bool block_contains_yield(AstNode *node);

// ── 工具 ─────────────────────────────────────────────────────
static char *tok_str(Parser *p, Token t) {
    return arena_strdup(p->arena, t.start, t.len);
}

static AstNode *new_node(Parser *p, NodeType type) {
    return node_new(p->arena, type, p->cur.line, p->cur.col);
}

// ── 块解析（支持三种风格）────────────────────────────────────
// 风格1: 缩进块（INDENT...DEDENT）
// 风格2: { ... }
// 风格3: begin/开始 ... end/结束
static AstNode *parse_block(Parser *p) {
    AstNode *block = new_node(p, NODE_BLOCK);
    skip_newlines(p);

    if (check(p, TOK_LBRACE)) {
        // 风格2: { ... }
        advance(p);
        skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            nodelist_push(p->arena, &block->block.stmts, parse_stmt(p));
            skip_newlines(p);
        }
        expect(p, TOK_RBRACE);
    } else if (check(p, TOK_BEGIN)) {
        // 风格3: begin/开始 ... end/结束
        advance(p);
        skip_newlines(p);
        while (!check(p, TOK_END) && !check(p, TOK_EOF)) {
            nodelist_push(p->arena, &block->block.stmts, parse_stmt(p));
            skip_newlines(p);
        }
        expect(p, TOK_END);
    } else {
        // 风格1: 缩进块
        expect(p, TOK_INDENT);
        while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
            skip_newlines(p);
            if (check(p, TOK_DEDENT)) break;
            nodelist_push(p->arena, &block->block.stmts, parse_stmt(p));
        }
        match(p, TOK_DEDENT);
    }
    return block;
}

// ── 参数列表 ─────────────────────────────────────────────────
static void parse_params(Parser *p, char ***params, int *count) {
    *params = NULL; *count = 0;
    expect(p, TOK_LPAREN);
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Token t = expect(p, TOK_IDENT);
        char *name = tok_str(p, t);
        // 可选类型标注 : type（忽略，运行时不强制）
        if (check(p, TOK_COLON)) { advance(p); advance(p); }
        char **np = arena_alloc(p->arena, (*count + 1) * sizeof(char*));
        if (*params) memcpy(np, *params, *count * sizeof(char*));
        np[*count] = name;
        *params = np; (*count)++;
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);
}

// ── 表达式（Pratt 解析）──────────────────────────────────────
static int prefix_bp(PicoTokenType t) {
    switch (t) {
        case TOK_MINUS: case TOK_NOT: case TOK_TILDE: return 70;
        default: return -1;
    }
}

static int infix_bp(PicoTokenType t) {
    switch (t) {
        case TOK_OR:    return 10;
        case TOK_AND:   return 20;
        case TOK_EQ: case TOK_NEQ:
        case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: return 30;
        case TOK_DOTDOT: return 35;
        case TOK_PIPE:   return 36;  // bitwise |
        case TOK_CARET:  return 37;  // bitwise ^
        case TOK_AMP:    return 38;  // bitwise &
        case TOK_LSHIFT: case TOK_RSHIFT: return 39;
        case TOK_PLUS: case TOK_MINUS: return 40;
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 50;
        case TOK_DOT: case TOK_LBRACKET: case TOK_LPAREN: return 80;
        default: return -1;
    }
}

static AstNode *parse_primary(Parser *p) {
    Token t = p->cur;
    AstNode *n;

    switch (t.type) {
        case TOK_INT: {
            n = new_node(p, NODE_INT);
            n->ival = strtoll(t.start, NULL, 0);
            advance(p); return n;
        }
        case TOK_FLOAT: {
            n = new_node(p, NODE_FLOAT);
            n->fval = strtod(t.start, NULL);
            advance(p); return n;
        }
        case TOK_STRING: {
            n = new_node(p, NODE_STRING);
            n->sval.s = tok_str(p, t);
            n->sval.len = t.len;
            advance(p); return n;
        }
        case TOK_FSTRING: {
            advance(p);
            AstNode *n = node_new(p->arena, NODE_FSTRING, t.line, t.col);
            const char *s = t.start;
            int len = t.len;
            int start = 0;
            for (int i = 0; i < len; i++) {
                if (s[i] == '{') {
                    if (i > start) {
                        AstNode *lit = node_new(p->arena, NODE_STRING, t.line, t.col);
                        lit->sval.s = arena_strdup(p->arena, s + start, i - start);
                        lit->sval.len = i - start;
                        nodelist_push(p->arena, &n->fstr.parts, lit);
                    }
                    i++; int exp_start = i; int depth = 1;
                    while (i < len && depth > 0) {
                        if (s[i] == '{') depth++;
                        else if (s[i] == '}') depth--;
                        if (depth > 0) i++;
                    }
                    char *expr_src = arena_strdup(p->arena, s + exp_start, i - exp_start);
                    Lexer el; lexer_init(&el, expr_src);
                    Parser ep; parser_init(&ep, &el, p->arena, p->filename);
                    nodelist_push(p->arena, &n->fstr.parts, parse_expr(&ep));
                    start = i + 1;
                }
            }
            if (start < len) {
                AstNode *lit = node_new(p->arena, NODE_STRING, t.line, t.col);
                lit->sval.s = arena_strdup(p->arena, s + start, len - start);
                lit->sval.len = len - start;
                nodelist_push(p->arena, &n->fstr.parts, lit);
            }
            return n;
        }
        case TOK_TRUE: {
            n = new_node(p, NODE_BOOL); n->bval = true;
            advance(p); return n;
        }
        case TOK_FALSE: {
            n = new_node(p, NODE_BOOL); n->bval = false;
            advance(p); return n;
        }
        case TOK_NIL: {
            n = new_node(p, NODE_NIL);
            advance(p); return n;
        }
        case TOK_IDENT: {
            n = new_node(p, NODE_IDENT);
            n->sval.s = tok_str(p, t);
            advance(p); return n;
        }
        case TOK_LPAREN: {
            advance(p);
            n = parse_expr(p);
            expect(p, TOK_RPAREN);
            return n;
        }
        case TOK_LBRACKET: {
            // 列表字面量
            n = new_node(p, NODE_LIST);
            advance(p);
            while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                nodelist_push(p->arena, &n->list.elements, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
            }
            expect(p, TOK_RBRACKET);
            return n;
        }
        case TOK_LBRACE: {
            // 字典字面量
            n = new_node(p, NODE_MAP);
            advance(p);
            skip_newlines(p);
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                AstNode *key;
                if (check(p, TOK_IDENT)) {
                    key = new_node(p, NODE_STRING);
                    key->sval.s = tok_str(p, p->cur);
                    advance(p);
                } else {
                    key = parse_expr(p);
                }
                expect(p, TOK_COLON);
                AstNode *val = parse_expr(p);
                nodelist_push(p->arena, &n->map.keys, key);
                nodelist_push(p->arena, &n->map.vals, val);
                if (!match(p, TOK_COMMA)) break;
                skip_newlines(p);
            }
            expect(p, TOK_RBRACE);
            return n;
        }
        case TOK_FN: {
            // 匿名函数
            n = new_node(p, NODE_FN);
            advance(p);
            parse_params(p, &n->fn.params, &n->fn.param_count);
            if (check(p, TOK_COLON) || check(p, TOK_LBRACE) || check(p, TOK_BEGIN)) {
                n->fn.body = parse_block(p);
                n->fn.is_generator = block_contains_yield(n->fn.body);
            }
            return n;
        }
        default: {
            pico_error(p->filename, t.line, t.col,
                "意外的 token '%.*s'", t.len, t.start);
            p->error_count++;
            advance(p);
            n = new_node(p, NODE_NIL);
            return n;
        }
    }
}

static AstNode *parse_expr_bp(Parser *p, int min_bp) {
    AstNode *left;

    // 前缀
    int pbp = prefix_bp(p->cur.type);
    if (pbp >= 0) {
        Token op = p->cur; advance(p);
        AstNode *operand = parse_expr_bp(p, pbp);
        left = new_node(p, NODE_UNOP);
        left->unop.op = op.type;
        left->unop.operand = operand;
    } else {
        left = parse_primary(p);
    }

    while (true) {
        PicoTokenType op = p->cur.type;
        int bp = infix_bp(op);
        if (bp < min_bp) break;

        if (op == TOK_DOT) {
            advance(p);
            Token field = expect(p, TOK_IDENT);
            if (check(p, TOK_LPAREN)) {
                // 方法调用
                AstNode *mc = new_node(p, NODE_METHOD_CALL);
                mc->mcall.obj    = left;
                mc->mcall.method = tok_str(p, field);
                advance(p);  // (
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    nodelist_push(p->arena, &mc->mcall.args, parse_expr(p));
                    if (!match(p, TOK_COMMA)) break;
                }
                expect(p, TOK_RPAREN);
                left = mc;
            } else {
                AstNode *fa = new_node(p, NODE_FIELD_ACCESS);
                fa->field.obj   = left;
                fa->field.field = tok_str(p, field);
                left = fa;
            }
        } else if (op == TOK_LBRACKET) {
            advance(p);
            AstNode *idx = parse_expr(p);
            if (check(p, TOK_DOTDOT)) {
                advance(p);
                AstNode *hi = parse_expr(p);
                AstNode *sl = new_node(p, NODE_SLICE);
                sl->slice.obj = left; sl->slice.lo = idx; sl->slice.hi = hi;
                left = sl;
            } else {
                AstNode *in = new_node(p, NODE_INDEX);
                in->index.obj = left; in->index.idx = idx;
                left = in;
            }
            expect(p, TOK_RBRACKET);
        } else if (op == TOK_LPAREN) {
            // 函数调用
            advance(p);
            AstNode *call = new_node(p, NODE_CALL);
            call->call.callee = left;
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                nodelist_push(p->arena, &call->call.args, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
            }
            expect(p, TOK_RPAREN);
            left = call;
        } else {
            advance(p);
            AstNode *right = parse_expr_bp(p, bp + 1);
            AstNode *bin = new_node(p, NODE_BINOP);
            bin->binop.left  = left;
            bin->binop.right = right;
            bin->binop.op    = op;
            left = bin;
        }
    }
    return left;
}

static AstNode *parse_expr(Parser *p) { return parse_expr_bp(p, 0); }

// ── 语句 ─────────────────────────────────────────────────────
static bool block_contains_yield(AstNode *node) {
    if (!node) return false;
    if (node->type == NODE_YIELD) return true;
    if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmts.count; i++)
            if (block_contains_yield(node->block.stmts.items[i])) return true;
    }
    if (node->type == NODE_IF) return block_contains_yield(node->ifnode.then) || block_contains_yield(node->ifnode.els);
    if (node->type == NODE_WHILE) return block_contains_yield(node->whilenode.body);
    if (node->type == NODE_FOR) return block_contains_yield(node->fornode.body);
    if (node->type == NODE_TRY) return block_contains_yield(node->trynode.body) || block_contains_yield(node->trynode.catch_body);
    if (node->type == NODE_MATCH) {
        for (int i = 0; i < node->matchnode.bodies.count; i++)
            if (block_contains_yield(node->matchnode.bodies.items[i])) return true;
        if (block_contains_yield(node->matchnode.default_body)) return true;
    }
    return false;
}

static AstNode *parse_fn(Parser *p, bool is_async) {
    AstNode *n = new_node(p, NODE_FN);
    advance(p);
    if (check(p, TOK_IDENT)) { n->fn.name = tok_str(p, p->cur); advance(p); }
    // parse params with optional type annotations: (x: int, y: str)
    expect(p, TOK_LPAREN);
    char **params = NULL; char **ptypes = NULL; int pc = 0;
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        Token pname = expect(p, TOK_IDENT);
        params = arena_alloc(p->arena, (pc+1)*sizeof(char*));
        ptypes = arena_alloc(p->arena, (pc+1)*sizeof(char*));
        // copy previous
        if (pc > 0) {
            memcpy(params, n->fn.params,       pc*sizeof(char*));
            memcpy(ptypes, n->fn.param_types,  pc*sizeof(char*));
        }
        params[pc] = tok_str(p, pname);
        ptypes[pc] = NULL;
        if (check(p, TOK_COLON)) { advance(p); ptypes[pc] = tok_str(p, p->cur); advance(p); }
        n->fn.params       = params;
        n->fn.param_types  = ptypes;
        pc++;
        if (!match(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);
    n->fn.param_count = pc;
    n->fn.is_async    = is_async;
    n->fn.return_type = NULL;
    // optional return type: -> type
    if (check(p, TOK_THIN_ARROW)) { advance(p); n->fn.return_type = tok_str(p, p->cur); advance(p); }
    if (check(p, TOK_COLON) || check(p, TOK_LBRACE) || check(p, TOK_BEGIN)) {
        n->fn.body = parse_block(p);
        n->fn.is_generator = block_contains_yield(n->fn.body);
    }
    return n;
}

static AstNode *parse_struct(Parser *p) {
    AstNode *n = new_node(p, NODE_STRUCT_DEF);
    advance(p);
    Token name = expect(p, TOK_IDENT);
    n->structdef.name = tok_str(p, name);
    n->structdef.parent_name = NULL;

    // 继承：struct 子类(父类):
    if (check(p, TOK_LPAREN)) {
        advance(p);
        Token pname = expect(p, TOK_IDENT);
        n->structdef.parent_name = tok_str(p, pname);
        expect(p, TOK_RPAREN);
    }

    // 字段和方法在块中
    skip_newlines(p);
    if (check(p, TOK_COLON) || check(p, TOK_LBRACE) || check(p, TOK_BEGIN)) {
        AstNode *body = parse_block(p);
        // 分离字段声明和方法
        char **fnames = NULL; int fcount = 0;
        for (int i = 0; i < body->block.stmts.count; i++) {
            AstNode *s = body->block.stmts.items[i];
            if (s->type == NODE_FN) {
                nodelist_push(p->arena, &n->structdef.methods, s);
            } else if (s->type == NODE_EXPR_STMT &&
                       s->exprstmt.expr->type == NODE_IDENT) {
                // 字段声明: name 或 name: type
                char **nf = arena_alloc(p->arena, (fcount+1)*sizeof(char*));
                if (fnames) memcpy(nf, fnames, fcount*sizeof(char*));
                nf[fcount] = s->exprstmt.expr->sval.s;
                fnames = nf; fcount++;
            } else if (s->type == NODE_LET) {
                char **nf = arena_alloc(p->arena, (fcount+1)*sizeof(char*));
                if (fnames) memcpy(nf, fnames, fcount*sizeof(char*));
                nf[fcount] = s->let.name;
                fnames = nf; fcount++;
            }
        }
        n->structdef.field_names  = fnames;
        n->structdef.field_count  = fcount;
    }
    return n;
}

static AstNode *parse_if(Parser *p) {
    AstNode *n = new_node(p, NODE_IF);
    advance(p);
    n->ifnode.cond = parse_expr(p);
    if (check(p, TOK_COLON)) advance(p);
    n->ifnode.then = parse_block(p);
    skip_newlines(p);
    if (check(p, TOK_ELSE)) {
        advance(p);
        if (check(p, TOK_IF)) {
            n->ifnode.els = parse_if(p);
        } else {
            if (check(p, TOK_COLON)) advance(p);
            n->ifnode.els = parse_block(p);
        }
    }
    return n;
}

static AstNode *parse_while(Parser *p) {
    AstNode *n = new_node(p, NODE_WHILE);
    advance(p);
    n->whilenode.cond = parse_expr(p);
    if (check(p, TOK_COLON)) advance(p);
    n->whilenode.body = parse_block(p);
    return n;
}

static AstNode *parse_for(Parser *p) {
    AstNode *n = new_node(p, NODE_FOR);
    advance(p);
    Token var = expect(p, TOK_IDENT);
    n->fornode.var = tok_str(p, var);
    expect(p, TOK_IN);
    n->fornode.iter = parse_expr(p);
    if (check(p, TOK_COLON)) advance(p);
    n->fornode.body = parse_block(p);
    return n;
}

static AstNode *parse_match(Parser *p) {
    AstNode *n = new_node(p, NODE_MATCH);
    advance(p);
    n->matchnode.subject = parse_expr(p);
    if (check(p, TOK_COLON)) advance(p);
    // 块
    skip_newlines(p);
    bool brace = check(p, TOK_LBRACE);
    if (brace) advance(p);
    else match(p, TOK_INDENT);
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_DEDENT) &&
           !check(p, TOK_END) && !check(p, TOK_EOF)) {
        // pattern => body
        if (check(p, TOK_IDENT) && p->cur.start[0] == '_' && p->cur.len == 1) {
            advance(p); expect(p, TOK_ARROW);
            n->matchnode.default_body = parse_stmt(p);
        } else {
            AstNode *pat = parse_expr(p);
            nodelist_push(p->arena, &n->matchnode.patterns, pat);
            expect(p, TOK_ARROW);
            nodelist_push(p->arena, &n->matchnode.bodies, parse_stmt(p));
        }
        skip_newlines(p);
    }
    if (brace) match(p, TOK_RBRACE);
    else match(p, TOK_DEDENT);
    return n;
}

static AstNode *parse_try(Parser *p) {
    AstNode *n = new_node(p, NODE_TRY);
    advance(p);
    if (check(p, TOK_COLON)) advance(p);
    n->trynode.body = parse_block(p);
    skip_newlines(p);
    if (check(p, TOK_CATCH)) {
        advance(p);
        if (check(p, TOK_IDENT)) {
            n->trynode.catch_var = new_node(p, NODE_IDENT);
            n->trynode.catch_var->sval.s = tok_str(p, p->cur);
            advance(p);
        }
        if (check(p, TOK_COLON)) advance(p);
        n->trynode.catch_body = parse_block(p);
    }
    return n;
}

static AstNode *parse_import(Parser *p) {
    AstNode *n = new_node(p, NODE_IMPORT);
    advance(p);
    char **names = NULL; int count = 0;
    do {
        Token t = expect(p, TOK_IDENT);
        char **nn = arena_alloc(p->arena, (count+1)*sizeof(char*));
        if (names) memcpy(nn, names, count*sizeof(char*));
        nn[count] = tok_str(p, t);
        names = nn; count++;
    } while (match(p, TOK_COMMA));
    n->import.names = names;
    n->import.count = count;
    return n;
}

static AstNode *parse_stmt(Parser *p) {
    skip_newlines(p);
    Token t = p->cur;

    switch (t.type) {
        case TOK_LET: {
            AstNode *n = new_node(p, NODE_LET);
            advance(p);
            Token name = expect(p, TOK_IDENT);
            n->let.name     = tok_str(p, name);
            n->let.type_ann = NULL;
            if (check(p, TOK_COLON)) {
                advance(p);
                n->let.type_ann = tok_str(p, p->cur);
                advance(p);
            }
            expect(p, TOK_ASSIGN);
            n->let.value = parse_expr(p);
            return n;
        }
        case TOK_FN: return parse_fn(p, false);
        case TOK_ASYNC: {
            advance(p);
            if (check(p, TOK_FN)) return parse_fn(p, true);
            // async expr
            AstNode *n = new_node(p, NODE_AWAIT);
            n->awaitnode.value = parse_expr(p);
            return n;
        }
        case TOK_STRUCT: return parse_struct(p);
        case TOK_IF:     return parse_if(p);
        case TOK_WHILE:  return parse_while(p);
        case TOK_FOR:    return parse_for(p);
        case TOK_MATCH:  return parse_match(p);
        case TOK_TRY:    return parse_try(p);
        case TOK_IMPORT: return parse_import(p);
        case TOK_RETURN: {
            AstNode *n = new_node(p, NODE_RETURN);
            advance(p);
            if (!check(p, TOK_NEWLINE) && !check(p, TOK_SEMICOLON) &&
                !check(p, TOK_DEDENT) && !check(p, TOK_EOF))
                n->retnode.value = parse_expr(p);
            return n;
        }
        case TOK_YIELD: {
            AstNode *n = new_node(p, NODE_YIELD);
            advance(p);
            if (!check(p, TOK_NEWLINE) && !check(p, TOK_SEMICOLON) &&
                !check(p, TOK_DEDENT) && !check(p, TOK_EOF))
                n->yieldnode.value = parse_expr(p);
            return n;
        }
        case TOK_AWAIT: {
            AstNode *n = new_node(p, NODE_AWAIT);
            advance(p);
            n->awaitnode.value = parse_expr(p);
            return n;
        }
        case TOK_SPAWN: {
            AstNode *n = new_node(p, NODE_SPAWN);
            advance(p);
            n->spawnnode.expr = parse_expr(p);
            return n;
        }
        case TOK_BREAK: {
            AstNode *n = new_node(p, NODE_BREAK);
            advance(p); return n;
        }
        case TOK_CONTINUE: {
            AstNode *n = new_node(p, NODE_CONTINUE);
            advance(p); return n;
        }
        default: {
            // 赋值或表达式语句
            AstNode *expr = parse_expr(p);
            if (check(p, TOK_ASSIGN) || check(p, TOK_PLUS_ASSIGN) ||
                check(p, TOK_MINUS_ASSIGN)) {
                PicoTokenType op = p->cur.type;
                advance(p);
                AstNode *val = parse_expr(p);
                AstNode *n = new_node(p, NODE_ASSIGN);
                // 从 expr 提取名字
                if (expr->type == NODE_IDENT) {
                    n->assign.name  = expr->sval.s;
                    n->assign.value = val;
                    n->assign.op    = op;
                } else {
                    // 复杂左值（索引/字段）— 包装成 ASSIGN
                    n->assign.name  = NULL;
                    n->assign.value = val;
                    n->assign.op    = op;
                    // 存 expr 在 left 字段（复用 binop.left）
                    n->binop.left  = expr;
                    n->binop.right = val;
                    n->binop.op    = op;
                    n->type = NODE_ASSIGN;
                }
                return n;
            }
            AstNode *s = new_node(p, NODE_EXPR_STMT);
            s->exprstmt.expr = expr;
            return s;
        }
    }
}

AstNode *parse_program(Parser *p) {
    AstNode *prog = node_new(p->arena, NODE_PROGRAM, 1, 1);
    skip_newlines(p);
    while (!check(p, TOK_EOF)) {
        nodelist_push(p->arena, &prog->program.stmts, parse_stmt(p));
        skip_newlines(p);
    }
    return prog;
}
