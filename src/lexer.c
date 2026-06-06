#define _POSIX_C_SOURCE 200809L
#include "lexer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


// ── 关键字表 ─────────────────────────────────────────────────
typedef struct { const char *word; PicoTokenType type; } KW;
static const KW kw_table[] = {
    {"fn",TOK_FN},{"function",TOK_FN},{"函数",TOK_FN},{"方法",TOK_FN},
    {"let",TOK_LET},{"var",TOK_LET},{"定义",TOK_LET},{"令",TOK_LET},
    {"struct",TOK_STRUCT},{"结构体",TOK_STRUCT},
    {"if",TOK_IF},{"如果",TOK_IF},
    {"else",TOK_ELSE},{"否则",TOK_ELSE},
    {"for",TOK_FOR},{"对于",TOK_FOR},{"遍历",TOK_FOR},
    {"while",TOK_WHILE},{"当",TOK_WHILE},
    {"return",TOK_RETURN},{"返回",TOK_RETURN},
    {"yield",TOK_YIELD},{"产出",TOK_YIELD},
    {"true",TOK_TRUE},{"真",TOK_TRUE},
    {"false",TOK_FALSE},{"假",TOK_FALSE},
    {"nil",TOK_NIL},{"null",TOK_NIL},{"空",TOK_NIL},
    {"match",TOK_MATCH},{"匹配",TOK_MATCH},
    {"spawn",TOK_SPAWN},{"启动",TOK_SPAWN},
    {"import",TOK_IMPORT},{"导入",TOK_IMPORT},
    {"in",TOK_IN},{"在",TOK_IN},
    {"and",TOK_AND},{"且",TOK_AND},
    {"or",TOK_OR},{"或",TOK_OR},
    {"not",TOK_NOT},{"非",TOK_NOT},
    {"begin",TOK_BEGIN},{"开始",TOK_BEGIN},
    {"end",TOK_END},{"结束",TOK_END},
    {"try",TOK_TRY},{"尝试",TOK_TRY},
    {"catch",TOK_CATCH},{"捕获",TOK_CATCH},
    {"async",TOK_ASYNC},{"异步",TOK_ASYNC},
    {"await",TOK_AWAIT},{"等待",TOK_AWAIT},
    {"break",TOK_BREAK},{"跳出",TOK_BREAK},{"中止",TOK_BREAK},
    {"continue",TOK_CONTINUE},{"继续",TOK_CONTINUE},
    {NULL,0}
};

static PicoTokenType lookup_keyword(const char *s, int len) {
    for (int i = 0; kw_table[i].word; i++) {
        if ((int)strlen(kw_table[i].word) == len &&
            memcmp(kw_table[i].word, s, len) == 0)
            return kw_table[i].type;
    }
    return TOK_IDENT;
}

// ── UTF-8 工具 ───────────────────────────────────────────────
static int utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

static bool is_ident_start(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    return c >= 0x80;  // 任何多字节 UTF-8（含中文）
}

static bool is_ident_cont(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) return true;
    return c >= 0x80;
}

// ── 初始化 ───────────────────────────────────────────────────
void lexer_init(Lexer *l, const char *src) {
    l->src = src;
    l->pos = 0;
    l->line = 1;
    l->col = 1;
    l->indent_stack[0] = 0;
    l->indent_top = 0;
    l->pending_count = 0;
}

static char cur(Lexer *l)  { return l->src[l->pos]; }
static char peek1(Lexer *l){ return l->src[l->pos+1]; }

static void advance(Lexer *l) {
    if (l->src[l->pos] == '\n') { l->line++; l->col = 1; }
    else l->col++;
    l->pos++;
}

static Token make_tok(Lexer *l, PicoTokenType t, int start, int line, int col) {
    return (Token){t, l->src + start, l->pos - start, line, col};
}

static Token err_tok(Lexer *l, const char *msg) {
    (void)msg;
    return (Token){TOK_ERROR, l->src + l->pos, 1, l->line, l->col};
}

// ── 跳过空白（非换行）和注释 ─────────────────────────────────
static void skip_spaces(Lexer *l) {
    while (cur(l) == ' ' || cur(l) == '\t' || cur(l) == '\r') advance(l);
    if (cur(l) == '#') {
        while (cur(l) && cur(l) != '\n') advance(l);
    }
}

// ── 字符串 ───────────────────────────────────────────────────
static Token read_string(Lexer *l, bool is_fstring) {
    int line = l->line, col = l->col;
    advance(l);  // skip opening "
    int start = l->pos;
    while (cur(l) && cur(l) != '"' && cur(l) != '\n') {
        if (cur(l) == '\\') advance(l);
        advance(l);
    }
    Token t = {is_fstring ? TOK_FSTRING : TOK_STRING,
               l->src + start, l->pos - start, line, col};
    if (cur(l) == '"') advance(l);
    return t;
}

// ── 数字 ─────────────────────────────────────────────────────
static Token read_number(Lexer *l) {
    int start = l->pos, line = l->line, col = l->col;
    bool is_float = false;
    while (cur(l) >= '0' && cur(l) <= '9') advance(l);
    if (cur(l) == '.' && peek1(l) != '.') {
        is_float = true;
        advance(l);
        while (cur(l) >= '0' && cur(l) <= '9') advance(l);
    }
    return make_tok(l, is_float ? TOK_FLOAT : TOK_INT, start, line, col);
}

// ── 标识符/关键字 ────────────────────────────────────────────
static Token read_ident(Lexer *l) {
    int start = l->pos, line = l->line, col = l->col;
    while (cur(l) && is_ident_cont(l->src + l->pos)) {
        int clen = utf8_char_len((unsigned char)cur(l));
        for (int i = 0; i < clen; i++) advance(l);
    }
    int len = l->pos - start;
    PicoTokenType t = lookup_keyword(l->src + start, len);
    return (Token){t, l->src + start, len, line, col};
}

// ── 缩进处理 ─────────────────────────────────────────────────
// 行首调用，返回本行缩进量
static int measure_indent(Lexer *l) {
    int n = 0;
    int p = l->pos;
    while (l->src[p] == ' ') { n++; p++; }
    while (l->src[p] == '\t') { n += 4; p++; }
    return n;
}

static void emit_indent_tokens(Lexer *l, int new_indent) {
    int cur_indent = l->indent_stack[l->indent_top];
    if (new_indent > cur_indent) {
        l->indent_top++;
        l->indent_stack[l->indent_top] = new_indent;
        l->pending[l->pending_count++] =
            (Token){TOK_INDENT, l->src + l->pos, 0, l->line, l->col};
    } else {
        while (l->indent_top > 0 &&
               l->indent_stack[l->indent_top] > new_indent) {
            l->indent_top--;
            l->pending[l->pending_count++] =
                (Token){TOK_DEDENT, l->src + l->pos, 0, l->line, l->col};
        }
    }
}

// ── 主词法函数 ───────────────────────────────────────────────
Token lexer_next(Lexer *l) {
    if (l->pending_count > 0) {
        Token t = l->pending[0];
        memmove(l->pending, l->pending+1, (--l->pending_count)*sizeof(Token));
        return t;
    }

retry:
    // 行首处理缩进
    if (l->col == 1) {
        int ind = measure_indent(l);
        // 跳过空行
        int p = l->pos;
        while (l->src[p] == ' ' || l->src[p] == '\t') p++;
        if (l->src[p] == '\n' || l->src[p] == '#' || l->src[p] == '\0') {
            skip_spaces(l);
            if (cur(l) == '#') while (cur(l) && cur(l) != '\n') advance(l);
            if (cur(l) == '\n') { advance(l); goto retry; }
            if (!cur(l)) goto eof;
        }
        // 跳过缩进空白
        while (cur(l) == ' ' || cur(l) == '\t') advance(l);
        emit_indent_tokens(l, ind);
        if (l->pending_count > 0) {
            Token t = l->pending[0];
            memmove(l->pending, l->pending+1, (--l->pending_count)*sizeof(Token));
            return t;
        }
    }

    skip_spaces(l);

    if (!cur(l)) goto eof;

    int line = l->line, col = l->col;
    int start = l->pos;
    unsigned char c = (unsigned char)cur(l);


    // 多字节标识符（中文等）
    if (c >= 0x80) return read_ident(l);

    // f-string
    if (c == 'f' && (peek1(l) == '"' || peek1(l) == '\'')) {
        advance(l);
        return read_string(l, true);
    }

    if (c == '"' || c == '\'') return read_string(l, false);
    if (c >= '0' && c <= '9') return read_number(l);
    if (is_ident_start(l->src + l->pos)) return read_ident(l);

    advance(l);
    switch (c) {
        case '\n': {
            Token t = {TOK_NEWLINE, l->src+start, 1, line, col};
            return t;
        }
        case '+': if(cur(l)=='='){advance(l);return(Token){TOK_PLUS_ASSIGN, l->src+start,2,line,col};}
                  return (Token){TOK_PLUS,    l->src+start,1,line,col};
        case '-': if(cur(l)=='='){advance(l);return(Token){TOK_MINUS_ASSIGN,l->src+start,2,line,col};}
                  if(cur(l)=='>'){advance(l);return(Token){TOK_THIN_ARROW,  l->src+start,2,line,col};}
                  return (Token){TOK_MINUS,   l->src+start,1,line,col};
        case '*': return (Token){TOK_STAR,    l->src+start,1,line,col};
        case '/': return (Token){TOK_SLASH,   l->src+start,1,line,col};
        case '%': return (Token){TOK_PERCENT, l->src+start,1,line,col};
        case '=': if(cur(l)=='='){advance(l);return(Token){TOK_EQ,  l->src+start,2,line,col};}
                  if(cur(l)=='>'){advance(l);return(Token){TOK_ARROW,l->src+start,2,line,col};}
                  return (Token){TOK_ASSIGN,  l->src+start,1,line,col};
        case '!': if(cur(l)=='='){advance(l);return(Token){TOK_NEQ, l->src+start,2,line,col};}
                  return err_tok(l,"expected !=");
        case '<': if(cur(l)=='='){advance(l);return(Token){TOK_LE,  l->src+start,2,line,col};}
                  return (Token){TOK_LT,      l->src+start,1,line,col};
        case '>': if(cur(l)=='='){advance(l);return(Token){TOK_GE,  l->src+start,2,line,col};}
                  return (Token){TOK_GT,      l->src+start,1,line,col};
        case '.': if(cur(l)=='.'){advance(l);return(Token){TOK_DOTDOT,l->src+start,2,line,col};}
                  return (Token){TOK_DOT,     l->src+start,1,line,col};
        case ',': return (Token){TOK_COMMA,   l->src+start,1,line,col};
        case ':': return (Token){TOK_COLON,   l->src+start,1,line,col};
        case ';': return (Token){TOK_SEMICOLON,l->src+start,1,line,col};
        case '|': return (Token){TOK_PIPE,    l->src+start,1,line,col};
        case '(': return (Token){TOK_LPAREN,  l->src+start,1,line,col};
        case ')': return (Token){TOK_RPAREN,  l->src+start,1,line,col};
        case '{': return (Token){TOK_LBRACE,  l->src+start,1,line,col};
        case '}': return (Token){TOK_RBRACE,  l->src+start,1,line,col};
        case '[': return (Token){TOK_LBRACKET,l->src+start,1,line,col};
        case ']': return (Token){TOK_RBRACKET,l->src+start,1,line,col};
        default:  return err_tok(l,"unexpected char");
    }

eof:
    // 发出所有待处理的 DEDENT
    while (l->indent_top > 0) {
        l->indent_top--;
        l->pending[l->pending_count++] =
            (Token){TOK_DEDENT, l->src+l->pos, 0, l->line, l->col};
    }
    if (l->pending_count > 0) {
        Token t = l->pending[0];
        memmove(l->pending, l->pending+1, (--l->pending_count)*sizeof(Token));
        return t;
    }
    return (Token){TOK_EOF, l->src+l->pos, 0, l->line, l->col};
}

// ── peek（不消耗）────────────────────────────────────────────
Token lexer_peek(Lexer *l) {
    Lexer save = *l;
    Token t = lexer_next(l);
    *l = save;
    return t;
}

const char *token_type_name(PicoTokenType t) {
    static const char *names[] = {
        "INT","FLOAT","STRING","IDENT","FSTRING",
        "fn","let","struct","if","else","for","while","return","yield",
        "import","true","false","nil","match","spawn","try","catch",
        "in","and","or","not","begin","end","async","await",
        "break","continue",
        "+","-","*","/","%","==","!=","<","<=",">",">=","=","+=","-=",
        "..","=>",".","," ,":",";"," |","(",")","{","}","[","]",
        "NEWLINE","INDENT","DEDENT","EOF","ERROR"
    };
    if (t < (int)(sizeof(names)/sizeof(names[0]))) return names[t];
    return "?";
}
