/*
 * মাতৃকা (Matrika) - v3.0
 * A Bengali-syntax programming language.
 *
 * Architecture:  Source → [Lexer] → Tokens → [Parser] → AST → [Evaluator]
 *
 * New in v3.0:
 *   - Integer and string variables
 *   - Arithmetic:   +  -  *  /  %
 *   - Comparison:   ==  !=  <  <=  >  >=
 *   - If / else:    যদি … { … } নাহলে { … }
 *   - While loop:   যতক্ষণ … { … }
 *   - Clean error messages:  file:line: error: <message>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Windows UTF-8 console ──────────────────────────────────────────── */
#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  define ENABLE_WIN_UTF8() \
       SetConsoleOutputCP(CP_UTF8); \
       SetConsoleCP(CP_UTF8); \
       _setmode(_fileno(stdout), _O_BINARY); \
       _setmode(_fileno(stderr), _O_BINARY); \
       _setmode(_fileno(stdin),  _O_BINARY);
#else
#  define ENABLE_WIN_UTF8()
#endif

/* ── Limits ─────────────────────────────────────────────────────────── */
#define MAX_SOURCE_LINES   65536
#define MAX_LINE_LEN        8192
#define MAX_TOKENS         65536   /* whole program token stream           */
#define MAX_AST_NODES      65536   /* whole program AST pool               */
#define MAX_VARS             512
#define MAX_VAR_NAME         256
#define MAX_STR              4096
#define MAX_LOOP_DEPTH        64   /* guard against infinite nesting       */


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 1 — ERROR REPORTING
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *g_filename   = "<stdin>";

/* Primary error — always exits. */
static void die(int line, const char *msg) {
    fprintf(stderr, "%s:%d: error: %s\n", g_filename, line, msg);
    exit(1);
}

/* Error with a quoted token for context. */
static void die_tok(int line, const char *msg, const char *tok) {
    fprintf(stderr, "%s:%d: error: %s: '%s'\n", g_filename, line, msg, tok);
    exit(1);
}

/* Type-error helper. */
static void die_type(int line, const char *op, const char *got) {
    fprintf(stderr, "%s:%d: error: operator '%s' cannot be applied to %s\n",
            g_filename, line, op, got);
    exit(1);
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 2 — LEXER
 *
 * Produces a flat Token array for the whole source file.
 * Each token records the source line it came from.
 *
 * Token kinds:
 *   TOK_KEYWORD   recognised Bengali keyword
 *   TOK_IDENT     identifier (Bengali or ASCII)
 *   TOK_STRING    "…"  (content already unescaped)
 *   TOK_NUMBER    numeric literal
 *   TOK_BOOL      সত্য / মিথ্যা
 *   TOK_OP        arithmetic operator:  +  -  *  /  %
 *   TOK_CMP       comparison operator:  ==  !=  <  <=  >  >=
 *   TOK_ASSIGN    =
 *   TOK_LPAREN    (
 *   TOK_RPAREN    )
 *   TOK_LBRACE    {
 *   TOK_RBRACE    }
 *   TOK_EOF       sentinel
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    TOK_KEYWORD,
    TOK_IDENT,
    TOK_STRING,
    TOK_NUMBER,
    TOK_BOOL,
    TOK_OP,
    TOK_CMP,
    TOK_ASSIGN,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    int       line;              /* 1-based source line number             */
    char      text[MAX_STR];     /* raw text (or unescaped string content) */
    double    num;               /* numeric value when kind==TOK_NUMBER    */
} Token;

/* Whole-program token stream (static — never on the stack) */
static Token  g_tokens[MAX_TOKENS];
static int    g_tok_count = 0;

/* ── Keyword table ──────────────────────────────────────────────────── */
static const char *KW_ASSIGN  = "ধরি";      /* let / assignment           */
static const char *KW_PRINT   = "বল";       /* print                      */
static const char *KW_IF      = "যদি";      /* if                         */
static const char *KW_ELSE    = "নাহলে";    /* else                       */
static const char *KW_WHILE   = "যতক্ষণ";  /* while                      */
static const char *KW_TRUE    = "সত্য";     /* true                       */
static const char *KW_FALSE   = "মিথ্যা";  /* false                      */
static const char *KW_COMMENT = "মন্তব্য"; /* comment (line comment kw)  */

static const char *KEYWORDS[] = {
    /* order matters: longer strings first when prefix-sharing */
    "ধরি", "বল", "যদি", "নাহলে", "যতক্ষণ", "মন্তব্য", NULL
};

/* ── UTF-8 helpers ──────────────────────────────────────────────────── */
static int utf8_cp_len(unsigned char c) {
    if      (c < 0x80)           return 1;
    else if ((c & 0xE0) == 0xC0) return 2;
    else if ((c & 0xF0) == 0xE0) return 3;
    else if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Is the byte an ASCII structural character (not part of a word)? */
static int lex_is_delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '+' || c == '-'  || c == '*'  || c == '/'  || c == '%' ||
           c == '=' || c == '!'  || c == '<'  || c == '>'  ||
           c == '(' || c == ')'  || c == '{'  || c == '}'  ||
           c == '#' || c == '"';
}

/* Copy one UTF-8 word (up to next delimiter) into buf. Returns ptr past word. */
static const char *lex_word(const char *s, char *buf, int bufsz, int line) {
    int i = 0;
    while (*s && !lex_is_delimiter(*s)) {
        int cplen = utf8_cp_len((unsigned char)*s);
        if (i + cplen >= bufsz - 1)
            die(line, "identifier or keyword is too long");
        for (int k = 0; k < cplen && *s; k++) buf[i++] = *s++;
    }
    buf[i] = '\0';
    return s;
}

/* Unescape a string literal. Returns ptr past closing quote. */
static const char *lex_string(const char *s, char *buf, int bufsz, int line) {
    s++; /* skip opening " */
    int i = 0;
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            switch (*s) {
                case 'n':  buf[i++] = '\n'; s++; break;
                case 't':  buf[i++] = '\t'; s++; break;
                case '"':  buf[i++] = '"';  s++; break;
                case '\\': buf[i++] = '\\'; s++; break;
                default:
                    buf[i++] = '\\';
                    if (i < bufsz - 1) buf[i++] = *s;
                    s++;
                    break;
            }
        } else {
            int cplen = utf8_cp_len((unsigned char)*s);
            if (i + cplen >= bufsz - 4)
                die(line, "string literal is too long");
            for (int k = 0; k < cplen && *s; k++) buf[i++] = *s++;
        }
    }
    buf[i] = '\0';
    if (*s == '"') s++;
    else           die(line, "unterminated string literal — missing closing '\"'");
    return s;
}

/* ── Main lexer ─────────────────────────────────────────────────────── */
static void lex(const char *source[], int num_lines) {
    g_tok_count = 0;

    for (int ln = 0; ln < num_lines; ln++) {
        int line = ln + 1;
        const char *s = source[ln];

        while (*s) {
            /* Skip whitespace */
            while (*s == ' ' || *s == '\t' || *s == '\r') s++;
            if (!*s || *s == '\n') break;

            /* Line comment — ASCII # */
            if (*s == '#') break;

            if (g_tok_count >= MAX_TOKENS - 1)
                die(line, "too many tokens in program");

            Token *tok = &g_tokens[g_tok_count];
            memset(tok, 0, sizeof(*tok));
            tok->line = line;

            /* String literal */
            if (*s == '"') {
                tok->kind = TOK_STRING;
                s = lex_string(s, tok->text, MAX_STR, line);
                g_tok_count++;
                continue;
            }

            /* Braces */
            if (*s == '{') { tok->kind = TOK_LBRACE; tok->text[0] = '{'; tok->text[1] = '\0'; g_tok_count++; s++; continue; }
            if (*s == '}') { tok->kind = TOK_RBRACE; tok->text[0] = '}'; tok->text[1] = '\0'; g_tok_count++; s++; continue; }

            /* Parens */
            if (*s == '(') { tok->kind = TOK_LPAREN; tok->text[0] = '('; tok->text[1] = '\0'; g_tok_count++; s++; continue; }
            if (*s == ')') { tok->kind = TOK_RPAREN; tok->text[0] = ')'; tok->text[1] = '\0'; g_tok_count++; s++; continue; }

            /* Two-char comparison operators */
            if ((*s == '=' && *(s+1) == '=') ||
                (*s == '!' && *(s+1) == '=') ||
                (*s == '<' && *(s+1) == '=') ||
                (*s == '>' && *(s+1) == '=')) {
                tok->kind = TOK_CMP;
                tok->text[0] = *s++; tok->text[1] = *s++; tok->text[2] = '\0';
                g_tok_count++;
                continue;
            }

            /* Single-char comparison */
            if (*s == '<' || *s == '>') {
                tok->kind = TOK_CMP;
                tok->text[0] = *s++; tok->text[1] = '\0';
                g_tok_count++;
                continue;
            }

            /* Assignment (must come after == check) */
            if (*s == '=') {
                tok->kind = TOK_ASSIGN;
                tok->text[0] = '='; tok->text[1] = '\0';
                g_tok_count++; s++;
                continue;
            }

            /* ! alone is not valid */
            if (*s == '!') {
                die(line, "unexpected '!' — did you mean '!='?");
            }

            /* Arithmetic operators */
            if (*s == '+' || *s == '-' || *s == '*' || *s == '/' || *s == '%') {
                tok->kind = TOK_OP;
                tok->text[0] = *s++; tok->text[1] = '\0';
                g_tok_count++;
                continue;
            }

            /* Word: keyword / bool / number / identifier */
            char word[MAX_STR];
            s = lex_word(s, word, MAX_STR, line);

            if (word[0] == '\0') {
                /* Single unrecognised ASCII byte — skip it with a warning */
                fprintf(stderr, "%s:%d: warning: skipping unexpected character (0x%02x)\n",
                        g_filename, line, (unsigned char)*s);
                s++;
                continue;
            }

            /* Bengali comment keyword — rest of line is comment */
            if (strcmp(word, KW_COMMENT) == 0) break;

            /* Boolean literals */
            if (strcmp(word, KW_TRUE) == 0) {
                tok->kind = TOK_BOOL; tok->num = 1;
                snprintf(tok->text, MAX_STR, "%s", word);
                g_tok_count++;
                continue;
            }
            if (strcmp(word, KW_FALSE) == 0) {
                tok->kind = TOK_BOOL; tok->num = 0;
                snprintf(tok->text, MAX_STR, "%s", word);
                g_tok_count++;
                continue;
            }

            /* Keywords */
            {
                int is_kw = 0;
                for (int i = 0; KEYWORDS[i]; i++) {
                    if (strcmp(word, KEYWORDS[i]) == 0) {
                        tok->kind = TOK_KEYWORD;
                        snprintf(tok->text, MAX_STR, "%s", word);
                        g_tok_count++;
                        is_kw = 1;
                        break;
                    }
                }
                if (is_kw) continue;
            }

            /* Numeric literal */
            {
                char *endp;
                double num = strtod(word, &endp);
                if (endp != word && *endp == '\0') {
                    tok->kind = TOK_NUMBER;
                    tok->num  = num;
                    snprintf(tok->text, MAX_STR, "%s", word);
                    g_tok_count++;
                    continue;
                }
            }

            /* Identifier */
            tok->kind = TOK_IDENT;
            snprintf(tok->text, MAX_STR, "%s", word);
            g_tok_count++;
        } /* while *s */
    } /* for ln */

    /* EOF sentinel */
    g_tokens[g_tok_count].kind     = TOK_EOF;
    g_tokens[g_tok_count].line     = num_lines;
    g_tokens[g_tok_count].text[0]  = '\0';
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 3 — AST
 *
 * Node kinds:
 *   NODE_BLOCK      list of statements (body)
 *   NODE_ASSIGN     ধরি <ident> = <expr>
 *   NODE_PRINT      বল(<expr>)
 *   NODE_IF         যদি <cond> { <body> } [নাহলে { <body> }]
 *   NODE_WHILE      যতক্ষণ <cond> { <body> }
 *   NODE_BINOP      <expr> <arith-op> <expr>
 *   NODE_CMP        <expr> <cmp-op>  <expr>
 *   NODE_NUMBER_LIT
 *   NODE_STRING_LIT
 *   NODE_BOOL_LIT
 *   NODE_IDENT
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    NODE_BLOCK,
    NODE_ASSIGN,
    NODE_PRINT,
    NODE_IF,
    NODE_WHILE,
    NODE_BINOP,
    NODE_CMP,
    NODE_NUMBER_LIT,
    NODE_STRING_LIT,
    NODE_BOOL_LIT,
    NODE_IDENT
} NodeKind;

typedef struct ASTNode ASTNode;
struct ASTNode {
    NodeKind kind;
    int      line;               /* source line for runtime errors         */

    char     text[MAX_STR];      /* identifier name / literal text / op    */
    double   num;                /* numeric literal value                  */
    int      bval;               /* boolean literal value                  */

    /* Generic child pointers */
    ASTNode *left;               /* condition / lhs / body                 */
    ASTNode *right;              /* rhs / else-body                        */

    /* Linked list of statements inside a block */
    ASTNode *next;
};

static ASTNode  g_ast_pool[MAX_AST_NODES];
static int      g_ast_used = 0;

static ASTNode *ast_new(NodeKind kind, int line) {
    if (g_ast_used >= MAX_AST_NODES)
        die(line, "internal error: AST node limit exceeded");
    ASTNode *n = &g_ast_pool[g_ast_used++];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->line = line;
    return n;
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 4 — PARSER
 *
 * Grammar:
 *   program    → block EOF
 *   block      → stmt*
 *   stmt       → assign | print | if_stmt | while_stmt
 *   assign     → KW("ধরি") IDENT '=' expr
 *   print      → KW("বল") '(' expr ')'
 *   if_stmt    → KW("যদি") expr '{' block '}' [KW("নাহলে") '{' block '}']
 *   while_stmt → KW("যতক্ষণ") expr '{' block '}'
 *   expr       → cmp_expr
 *   cmp_expr   → add_expr (CMP add_expr)*
 *   add_expr   → mul_expr (('+' | '-') mul_expr)*
 *   mul_expr   → unary  (('*' | '/' | '%') unary)*
 *   unary      → '-' unary | factor
 *   factor     → NUMBER | STRING | BOOL | IDENT | '(' expr ')'
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int pos;   /* index into g_tokens[] */
} Parser;

static Token *par_peek(Parser *p) {
    return &g_tokens[p->pos];
}
static Token *par_advance(Parser *p) {
    Token *t = &g_tokens[p->pos];
    if (t->kind != TOK_EOF) p->pos++;
    return t;
}
static Token *par_expect(Parser *p, TokenKind k, const char *msg) {
    Token *t = par_peek(p);
    if (t->kind != k) die(t->line, msg);
    return par_advance(p);
}
static int par_match_kw(Parser *p, const char *kw) {
    Token *t = par_peek(p);
    return t->kind == TOK_KEYWORD && strcmp(t->text, kw) == 0;
}

/* Forward declarations */
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_block(Parser *p);

/* ── factor ──────────────────────────────────────────────────────────── */
static ASTNode *parse_factor(Parser *p) {
    Token *t = par_peek(p);

    if (t->kind == TOK_NUMBER) {
        par_advance(p);
        ASTNode *n = ast_new(NODE_NUMBER_LIT, t->line);
        n->num = t->num;
        snprintf(n->text, MAX_STR, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_STRING) {
        par_advance(p);
        ASTNode *n = ast_new(NODE_STRING_LIT, t->line);
        snprintf(n->text, MAX_STR, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_BOOL) {
        par_advance(p);
        ASTNode *n = ast_new(NODE_BOOL_LIT, t->line);
        n->bval = (int)t->num;
        snprintf(n->text, MAX_STR, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_IDENT) {
        par_advance(p);
        ASTNode *n = ast_new(NODE_IDENT, t->line);
        snprintf(n->text, MAX_STR, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_LPAREN) {
        par_advance(p);
        ASTNode *inner = parse_expr(p);
        par_expect(p, TOK_RPAREN, "expected ')' to close parenthesised expression");
        return inner;
    }

    /* Reached something that cannot start an expression */
    if (t->kind == TOK_EOF)
        die(t->line, "unexpected end of file — expected an expression");

    die_tok(t->line, "unexpected token in expression", t->text);
    return NULL; /* unreachable */
}

/* ── unary ───────────────────────────────────────────────────────────── */
static ASTNode *parse_unary(Parser *p) {
    Token *t = par_peek(p);
    if (t->kind == TOK_OP && t->text[0] == '-') {
        par_advance(p);
        /* Represent unary minus as (0 - operand) */
        ASTNode *zero = ast_new(NODE_NUMBER_LIT, t->line);
        zero->num = 0.0;
        snprintf(zero->text, MAX_STR, "0");
        ASTNode *operand = parse_unary(p);
        ASTNode *node = ast_new(NODE_BINOP, t->line);
        node->text[0] = '-'; node->text[1] = '\0';
        node->left  = zero;
        node->right = operand;
        return node;
    }
    return parse_factor(p);
}

/* ── mul_expr ────────────────────────────────────────────────────────── */
static ASTNode *parse_mul(Parser *p) {
    ASTNode *left = parse_unary(p);
    for (;;) {
        Token *t = par_peek(p);
        if (t->kind != TOK_OP) break;
        char op = t->text[0];
        if (op != '*' && op != '/' && op != '%') break;
        par_advance(p);
        ASTNode *right = parse_unary(p);
        ASTNode *node  = ast_new(NODE_BINOP, t->line);
        node->text[0] = op; node->text[1] = '\0';
        node->left  = left;
        node->right = right;
        left = node;
    }
    return left;
}

/* ── add_expr ────────────────────────────────────────────────────────── */
static ASTNode *parse_add(Parser *p) {
    ASTNode *left = parse_mul(p);
    for (;;) {
        Token *t = par_peek(p);
        if (t->kind != TOK_OP) break;
        char op = t->text[0];
        if (op != '+' && op != '-') break;
        par_advance(p);
        ASTNode *right = parse_mul(p);
        ASTNode *node  = ast_new(NODE_BINOP, t->line);
        node->text[0] = op; node->text[1] = '\0';
        node->left  = left;
        node->right = right;
        left = node;
    }
    return left;
}

/* ── cmp_expr ────────────────────────────────────────────────────────── */
static ASTNode *parse_cmp(Parser *p) {
    ASTNode *left = parse_add(p);
    for (;;) {
        Token *t = par_peek(p);
        if (t->kind != TOK_CMP) break;
        par_advance(p);
        ASTNode *right = parse_add(p);
        ASTNode *node  = ast_new(NODE_CMP, t->line);
        snprintf(node->text, MAX_STR, "%s", t->text);
        node->left  = left;
        node->right = right;
        left = node;
    }
    return left;
}

/* ── expr (top-level expression parser) ─────────────────────────────── */
static ASTNode *parse_expr(Parser *p) {
    return parse_cmp(p);
}

/* ── block ───────────────────────────────────────────────────────────── */
/*
 * Parse a sequence of statements until '}' or EOF.
 * Called after a '{' has already been consumed.
 */
static ASTNode *parse_block(Parser *p) {
    ASTNode *block = ast_new(NODE_BLOCK, par_peek(p)->line);
    ASTNode *tail  = NULL;

    while (par_peek(p)->kind != TOK_RBRACE &&
           par_peek(p)->kind != TOK_EOF) {

        Token  *t    = par_peek(p);
        ASTNode *stmt = NULL;

        /* ── ধরি assignment ──────────────────────────────────────────── */
        if (t->kind == TOK_KEYWORD && strcmp(t->text, KW_ASSIGN) == 0) {
            par_advance(p);
            Token *name = par_expect(p, TOK_IDENT,
                "expected variable name after 'ধরি'");
            par_expect(p, TOK_ASSIGN,
                "expected '=' after variable name in assignment");
            ASTNode *expr = parse_expr(p);
            stmt = ast_new(NODE_ASSIGN, t->line);
            snprintf(stmt->text, MAX_STR, "%s", name->text);
            stmt->left = expr;
        }

        /* ── বল print ───────────────────────────────────────────────── */
        else if (t->kind == TOK_KEYWORD && strcmp(t->text, KW_PRINT) == 0) {
            par_advance(p);
            par_expect(p, TOK_LPAREN, "expected '(' after 'বল'");
            ASTNode *expr = parse_expr(p);
            par_expect(p, TOK_RPAREN,
                "expected ')' to close 'বল(…)'");
            stmt = ast_new(NODE_PRINT, t->line);
            stmt->left = expr;
        }

        /* ── যদি if ─────────────────────────────────────────────────── */
        else if (t->kind == TOK_KEYWORD && strcmp(t->text, KW_IF) == 0) {
            par_advance(p);
            ASTNode *cond = parse_expr(p);
            par_expect(p, TOK_LBRACE,
                "expected '{' after condition in 'যদি'");
            ASTNode *then_body = parse_block(p);
            par_expect(p, TOK_RBRACE,
                "expected '}' to close 'যদি' block");

            ASTNode *else_body = NULL;
            if (par_match_kw(p, KW_ELSE)) {
                par_advance(p);
                par_expect(p, TOK_LBRACE,
                    "expected '{' after 'নাহলে'");
                else_body = parse_block(p);
                par_expect(p, TOK_RBRACE,
                    "expected '}' to close 'নাহলে' block");
            }

            stmt = ast_new(NODE_IF, t->line);
            stmt->left  = cond;
            stmt->right = then_body;
            /* Store else body in then_body->next as a side-channel */
            then_body->next = else_body;
        }

        /* ── যতক্ষণ while ───────────────────────────────────────────── */
        else if (t->kind == TOK_KEYWORD && strcmp(t->text, KW_WHILE) == 0) {
            par_advance(p);
            ASTNode *cond = parse_expr(p);
            par_expect(p, TOK_LBRACE,
                "expected '{' after condition in 'যতক্ষণ'");
            ASTNode *body = parse_block(p);
            par_expect(p, TOK_RBRACE,
                "expected '}' to close 'যতক্ষণ' block");

            stmt = ast_new(NODE_WHILE, t->line);
            stmt->left  = cond;
            stmt->right = body;
        }

        /* ── Unknown ─────────────────────────────────────────────────── */
        else {
            if (t->kind == TOK_EOF)
                die(t->line, "unexpected end of file inside block — missing '}'");
            die_tok(t->line, "unknown statement", t->text);
        }

        /* Append stmt to block's statement list */
        if (!block->left)   block->left = stmt;
        else                tail->next  = stmt;
        tail = stmt;
    }

    return block;
}

/* ── Top-level parse ─────────────────────────────────────────────────── */
static ASTNode *parse_program(void) {
    Parser p;
    p.pos = 0;

    ASTNode *block = parse_block(&p);

    if (par_peek(&p)->kind != TOK_EOF) {
        Token *t = par_peek(&p);
        die_tok(t->line, "unexpected token at top level", t->text);
    }
    return block;
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 5 — RUNTIME VALUES & VARIABLE STORE
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum { MT_NUMBER, MT_STRING, MT_BOOL } ValType;

typedef struct {
    ValType type;
    double  num;
    int     bval;
    char    str[MAX_STR];
} Val;

/* Canonical string representation (for print & string concat) */
static void val_to_str(const Val *v, char *out, int outsz) {
    switch (v->type) {
        case MT_NUMBER:
            snprintf(out, outsz,
                     (v->num == (long long)v->num) ? "%.0f" : "%g",
                     v->num);
            break;
        case MT_BOOL:
            snprintf(out, outsz, "%s", v->bval ? KW_TRUE : KW_FALSE);
            break;
        case MT_STRING:
            snprintf(out, outsz, "%s", v->str);
            break;
    }
}

static Val make_num(double n) {
    Val v;
    memset(&v, 0, sizeof(v));
    v.type = MT_NUMBER;
    v.num  = n;
    v.bval = (n != 0.0);
    val_to_str(&v, v.str, MAX_STR);
    return v;
}

static Val make_str(const char *s) {
    Val v;
    memset(&v, 0, sizeof(v));
    v.type = MT_STRING;
    snprintf(v.str, MAX_STR, "%s", s);
    v.num  = 0.0;
    v.bval = (s[0] != '\0');
    return v;
}

static Val make_bool(int b) {
    Val v;
    memset(&v, 0, sizeof(v));
    v.type = MT_BOOL;
    v.bval = b ? 1 : 0;
    v.num  = b ? 1.0 : 0.0;
    snprintf(v.str, MAX_STR, "%s", b ? KW_TRUE : KW_FALSE);
    return v;
}

/* Truthy test: numbers ≠ 0, non-empty strings, booleans */
static int val_truthy(const Val *v) {
    switch (v->type) {
        case MT_NUMBER: return v->num != 0.0;
        case MT_STRING: return v->str[0] != '\0';
        case MT_BOOL:   return v->bval;
    }
    return 0;
}

/* ── Variable store ─────────────────────────────────────────────────── */
typedef struct {
    char name[MAX_VAR_NAME];
    Val  val;
    int  used;
} Var;

static Var  g_vars[MAX_VARS];
static int  g_var_count = 0;

static Var *var_find(const char *name) {
    for (int i = 0; i < g_var_count; i++)
        if (g_vars[i].used && strcmp(g_vars[i].name, name) == 0)
            return &g_vars[i];
    return NULL;
}

static Var *var_set(const char *name, Val v, int line) {
    Var *var = var_find(name);
    if (!var) {
        if (g_var_count >= MAX_VARS)
            die(line, "variable limit exceeded");
        var = &g_vars[g_var_count++];
        snprintf(var->name, MAX_VAR_NAME, "%.*s", MAX_VAR_NAME - 1, name);
        var->used = 1;
    }
    var->val = v;
    return var;
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 6 — EVALUATOR
 * ═══════════════════════════════════════════════════════════════════════ */

static Val  eval(const ASTNode *node);       /* forward decl               */
static void exec(const ASTNode *node);       /* forward decl               */

/* ── Expression evaluator ───────────────────────────────────────────── */
static Val eval(const ASTNode *node) {
    switch (node->kind) {

        /* ── Literals ─────────────────────────────────────────────────── */
        case NODE_NUMBER_LIT: return make_num(node->num);
        case NODE_STRING_LIT: return make_str(node->text);
        case NODE_BOOL_LIT:   return make_bool(node->bval);

        /* ── Variable lookup ──────────────────────────────────────────── */
        case NODE_IDENT: {
            Var *v = var_find(node->text);
            if (!v) die_tok(node->line, "undefined variable", node->text);
            return v->val;
        }

        /* ── Arithmetic binary op ─────────────────────────────────────── */
        case NODE_BINOP: {
            Val  lv  = eval(node->left);
            Val  rv  = eval(node->right);
            char op  = node->text[0];

            /* String concatenation with '+' */
            if (op == '+' &&
                (lv.type == MT_STRING || rv.type == MT_STRING)) {
                char lbuf[MAX_STR], rbuf[MAX_STR], out[MAX_STR];
                val_to_str(&lv, lbuf, MAX_STR);
                val_to_str(&rv, rbuf, MAX_STR);
                int llen  = (int)strlen(lbuf);
                int rlen  = (int)strlen(rbuf);
                int avail = MAX_STR - 1;
                if (llen > avail)   llen = avail;
                memcpy(out, lbuf, (size_t)llen);
                int space = avail - llen;
                if (rlen > space) rlen = space;
                memcpy(out + llen, rbuf, (size_t)rlen);
                out[llen + rlen] = '\0';
                return make_str(out);
            }

            /* All other arithmetic requires numbers */
            if (lv.type == MT_STRING)
                die_type(node->line, node->text, "a string (left side)");
            if (rv.type == MT_STRING)
                die_type(node->line, node->text, "a string (right side)");

            double l = lv.num;
            double r = rv.num;
            switch (op) {
                case '+': return make_num(l + r);
                case '-': return make_num(l - r);
                case '*': return make_num(l * r);
                case '/':
                    if (r == 0.0)
                        die(node->line, "division by zero");
                    return make_num(l / r);
                case '%':
                    if (r == 0.0)
                        die(node->line, "modulo by zero");
                    {
                        long long li = (long long)l;
                        long long ri = (long long)r;
                        return make_num((double)(li % ri));
                    }
                default:
                    die_tok(node->line, "unknown arithmetic operator", node->text);
            }
            break;
        }

        /* ── Comparison op ────────────────────────────────────────────── */
        case NODE_CMP: {
            Val lv = eval(node->left);
            Val rv = eval(node->right);
            const char *op = node->text;
            int result;

            /* String comparisons */
            if (lv.type == MT_STRING || rv.type == MT_STRING) {
                char lbuf[MAX_STR], rbuf[MAX_STR];
                val_to_str(&lv, lbuf, MAX_STR);
                val_to_str(&rv, rbuf, MAX_STR);
                int cmp = strcmp(lbuf, rbuf);
                if      (strcmp(op, "==") == 0) result = (cmp == 0);
                else if (strcmp(op, "!=") == 0) result = (cmp != 0);
                else
                    die_type(node->line, op,
                             "strings (only == and != are valid for strings)");
                return make_bool(result);
            }

            /* Numeric comparisons */
            double l = lv.num;
            double r = rv.num;
            if      (strcmp(op, "==") == 0) result = (l == r);
            else if (strcmp(op, "!=") == 0) result = (l != r);
            else if (strcmp(op, "<")  == 0) result = (l <  r);
            else if (strcmp(op, "<=") == 0) result = (l <= r);
            else if (strcmp(op, ">")  == 0) result = (l >  r);
            else if (strcmp(op, ">=") == 0) result = (l >= r);
            else die_tok(node->line, "unknown comparison operator", op);
            return make_bool(result);
        }

        default:
            die(node->line, "internal error: eval() called on non-expression node");
    }

    /* unreachable */
    return make_num(0);
}

/* ── Statement executor ─────────────────────────────────────────────── */
static void exec(const ASTNode *node) {
    if (!node) return;

    switch (node->kind) {

        /* ── Block (sequence of statements) ──────────────────────────── */
        case NODE_BLOCK: {
            ASTNode *s = node->left;
            while (s) {
                exec(s);
                s = s->next;
            }
            break;
        }

        /* ── Assignment ───────────────────────────────────────────────── */
        case NODE_ASSIGN: {
            Val v = eval(node->left);
            var_set(node->text, v, node->line);
            break;
        }

        /* ── Print ────────────────────────────────────────────────────── */
        case NODE_PRINT: {
            Val v = eval(node->left);
            char buf[MAX_STR];
            val_to_str(&v, buf, MAX_STR);
            printf("%s\n", buf);
            break;
        }

        /* ── If / else ────────────────────────────────────────────────── */
        case NODE_IF: {
            Val cond = eval(node->left);
            if (val_truthy(&cond)) {
                exec(node->right);           /* then body  */
            } else {
                ASTNode *else_body = node->right->next;
                if (else_body) exec(else_body);
            }
            break;
        }

        /* ── While ────────────────────────────────────────────────────── */
        case NODE_WHILE: {
            int guard = 0;
            while (1) {
                Val cond = eval(node->left);
                if (!val_truthy(&cond)) break;
                exec(node->right);
                if (++guard > 10000000)
                    die(node->line,
                        "loop exceeded 10,000,000 iterations — "
                        "possible infinite loop");
            }
            break;
        }

        default:
            die(node->line,
                "internal error: exec() called on non-statement node");
    }
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 7 — FILE RUNNER & REPL
 * ═══════════════════════════════════════════════════════════════════════ */

/* Source line storage — whole file loaded before parsing */
static char  g_source_buf[MAX_SOURCE_LINES][MAX_LINE_LEN];
static const char *g_source_lines[MAX_SOURCE_LINES];
static int   g_source_count = 0;

static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: error: cannot open file\n", path);
        exit(1);
    }

    g_source_count = 0;
    while (fgets(g_source_buf[g_source_count], MAX_LINE_LEN, f)) {
        g_source_lines[g_source_count] = g_source_buf[g_source_count];
        g_source_count++;
        if (g_source_count >= MAX_SOURCE_LINES) {
            fprintf(stderr, "%s: error: file exceeds %d line limit\n",
                    path, MAX_SOURCE_LINES);
            fclose(f);
            exit(1);
        }
    }
    fclose(f);

    lex(g_source_lines, g_source_count);
    ASTNode *program = parse_program();
    exec(program);
}

/* REPL — parse and run one logical statement at a time.
   Multi-line constructs (if / while) are accumulated until '}' is seen. */
static void run_repl(void) {
    printf("Matrika v3.0  —  type 'বিদায়' to exit\n");
    printf("------------------------------------------\n");

    char line[MAX_LINE_LEN];
    /* Accumulate lines for a single top-level construct */
    static char repl_buf[MAX_SOURCE_LINES][MAX_LINE_LEN];
    static const char *repl_lines[MAX_SOURCE_LINES];
    int    repl_count = 0;
    int    brace_depth = 0;
    int    global_line = 0;

    while (1) {
        printf(brace_depth > 0 ? "...       " : "matrika>  ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        global_line++;

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Check exit */
        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (brace_depth == 0 && strcmp(trimmed, "বিদায়") == 0) {
            printf("বিদায়!\n");
            break;
        }

        /* Count braces to detect block boundaries */
        for (const char *c = line; *c; c++) {
            if (*c == '{') brace_depth++;
            if (*c == '}') brace_depth--;
        }

        snprintf(repl_buf[repl_count], MAX_LINE_LEN, "%s", line);
        repl_lines[repl_count] = repl_buf[repl_count];
        repl_count++;

        /* Execute when we have a complete statement (brace_depth back to 0) */
        if (brace_depth <= 0) {
            brace_depth = 0;
            g_tok_count = 0;
            g_ast_used  = 0;
            lex(repl_lines, repl_count);
            if (g_tok_count > 0) {
                ASTNode *program = parse_program();
                exec(program);
            }
            repl_count = 0;
        }
    }
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 8 — ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    ENABLE_WIN_UTF8();

    if (argc == 1) {
        g_filename = "<stdin>";
        run_repl();
    } else if (argc == 2) {
        g_filename = argv[1];
        const char *ext = strrchr(argv[1], '.');
        if (!ext || strcmp(ext, ".matrika") != 0)
            fprintf(stderr, "%s: warning: file does not have a .matrika extension\n",
                    argv[1]);
        run_file(argv[1]);
    } else {
        fprintf(stderr, "usage: matrika [file.matrika]\n");
        return 1;
    }
    return 0;
}