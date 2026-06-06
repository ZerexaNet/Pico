#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    // 字面量
    TOK_INT, TOK_FLOAT, TOK_STRING, TOK_IDENT, TOK_FSTRING,
    // 关键字（中英共用）
    TOK_FN, TOK_LET, TOK_STRUCT, TOK_IF, TOK_ELSE,
    TOK_FOR, TOK_WHILE, TOK_RETURN, TOK_YIELD,
    TOK_IMPORT, TOK_TRUE, TOK_FALSE, TOK_NIL,
    TOK_MATCH, TOK_SPAWN, TOK_TRY, TOK_CATCH,
    TOK_IN, TOK_AND, TOK_OR, TOK_NOT,
    TOK_BEGIN, TOK_END, TOK_ASYNC, TOK_AWAIT,
    TOK_BREAK, TOK_CONTINUE,
    // 运算符
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN, TOK_SLASH_ASSIGN, TOK_AMP_ASSIGN, TOK_PIPE_ASSIGN, TOK_CARET_ASSIGN,
    TOK_DOTDOT,       // ..  范围
    TOK_ARROW,        // =>  match 分支
    TOK_THIN_ARROW,   // ->  返回类型标注
    TOK_DOT, TOK_COMMA, TOK_COLON, TOK_SEMICOLON, TOK_PIPE,
    TOK_AMP,          // &  位与
    TOK_CARET,        // ^  位异或
    TOK_TILDE,        // ~  位非
    TOK_LSHIFT,       // << 左移
    TOK_RSHIFT,       // >> 右移
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_NEWLINE, TOK_INDENT, TOK_DEDENT,
    TOK_EOF, TOK_ERROR
} PicoTokenType;

typedef struct {
    PicoTokenType   type;
    const char *start;   // 指向源码（不拥有）
    int         len;
    int         line;
    int         col;
} Token;

typedef struct {
    const char *src;
    int         pos;
    int         line;
    int         col;
    int         indent_stack[64];
    int         indent_top;
    Token       pending[4];  // 待发出的 INDENT/DEDENT
    int         pending_count;
} Lexer;

void  lexer_init(Lexer *l, const char *src);
Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);
const char *token_type_name(PicoTokenType t);
