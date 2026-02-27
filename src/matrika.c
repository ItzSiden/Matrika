/*
 * মাতৃকা (Matrika) - v2.0
 * বাংলাভাষীদের জন্য প্রোগ্রামিং ভাষা
 * Refactored: Lexer → Parser → Evaluator (single C99 file, no dependencies)
 *
 * Architecture:
 *   ┌──────────────────────────────────────────────────────┐
 *   │  Source Text                                         │
 *   │      ↓  [LEXER]   tokenize()                        │
 *   │  TokenList                                           │
 *   │      ↓  [PARSER]  parse()                           │
 *   │  AST (ASTNode tree)                                  │
 *   │      ↓  [EVALUATOR] eval_node() / exec_stmt()       │
 *   │  Side effects (print, variable store, …)            │
 *   └──────────────────────────────────────────────────────┘
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Windows UTF-8 console ────────────────────────────────────────────── */
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

/* ── Compile-time limits ─────────────────────────────────────────────── */
#define MAX_VARS        256
#define MAX_VAR_NAME    256
#define MAX_VAR_VALUE   4096
#define MAX_LINE_LEN    8192
#define MAX_TOKENS      2048   /* tokens per line                          */
#define MAX_AST_NODES   4096   /* total AST nodes per program              */

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 1 – SHARED TYPES & ERROR HANDLING
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum { TYPE_STRING, TYPE_NUMBER, TYPE_BOOL } VarType;

/* Runtime value (used by both variable store and expression results) */
typedef struct {
    VarType type;
    double  num_val;
    int     bool_val;
    char    str_val[MAX_VAR_VALUE];
} Value;

/* Global error position (set by caller before lexing/parsing) */
static int         g_line_number = 0;
static const char *g_filename    = "";

static void die(const char *msg) {
    fprintf(stderr, "\n[!] মাতৃকা ত্রুটি [লাইন %d]: %s\n", g_line_number, msg);
    exit(1);
}
static void die2(const char *msg, const char *detail) {
    fprintf(stderr, "\n[!] মাতৃকা ত্রুটি [লাইন %d]: %s '%s'\n",
            g_line_number, msg, detail);
    exit(1);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 2 – LEXER
 *
 *  Input : raw UTF-8 source line (const char *)
 *  Output: TokenList  — a flat array of Token structs
 *
 *  Token types:
 *    TOK_KEYWORD   – a recognised Bengali keyword
 *    TOK_IDENT     – Bengali/ASCII identifier
 *    TOK_STRING    – "…" literal (content already unescaped)
 *    TOK_NUMBER    – numeric literal
 *    TOK_BOOL      – সত্য / মিথ্যা
 *    TOK_OP        – single-char operator  + - * /
 *    TOK_ASSIGN    – =
 *    TOK_LPAREN    – (
 *    TOK_RPAREN    – )
 *    TOK_COMMENT   – # … or মন্তব্য …  (consumed; not passed to parser)
 *    TOK_EOF       – sentinel
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    TOK_KEYWORD,
    TOK_IDENT,
    TOK_STRING,
    TOK_NUMBER,
    TOK_BOOL,
    TOK_OP,
    TOK_ASSIGN,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMENT,
    TOK_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    char      text[MAX_VAR_VALUE]; /* raw or processed text */
    double    num;                 /* parsed value if TOK_NUMBER */
} Token;

typedef struct {
    Token tokens[MAX_TOKENS];
    int   count;
} TokenList;

/* Single shared token list — lives in BSS, never on the call stack */
static TokenList g_token_list;

/* Bengali keywords recognised by the lexer */
static const char *KEYWORDS[] = {
    "ধরি",       /* assignment  */
    "বল",        /* print       */
    "মন্তব্য",   /* comment     */
    NULL
};

static const char *BOOL_TRUE  = "সত্য";
static const char *BOOL_FALSE = "মিথ্যা";

/* ── Helpers ─────────────────────────────────────────────────────────── */

static const char *lex_skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return s;
}

/* Returns length in bytes of the first UTF-8 code-point starting at s,
   or 1 for ASCII / invalid bytes. */
static int utf8_cp_len(unsigned char c) {
    if      (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) return 2;
    else if ((c & 0xF0) == 0xE0) return 3;
    else if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Is this the start of an ASCII operator / structural char? */
static int is_structural(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '=' || c == '(' || c == ')' || c == '#' || c == '"';
}

/* Read one UTF-8 word (identifier / keyword / bool) into buf.
   Returns pointer past the word. */
static const char *lex_read_word(const char *s, char *buf, int buf_size) {
    int i = 0;
    while (*s && !is_structural(*s) && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        int cplen = utf8_cp_len((unsigned char)*s);
        if (i + cplen >= buf_size - 1) die("টোকেন অতিরিক্ত দীর্ঘ");
        for (int k = 0; k < cplen && *s; k++) buf[i++] = *s++;
    }
    buf[i] = '\0';
    return s;
}

/* Unescape and copy string literal content into buf (without quotes).
   Returns pointer past closing quote. */
static const char *lex_read_string(const char *s, char *buf, int buf_size) {
    /* s points at opening " */
    s++; /* skip " */
    int i = 0;
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            switch (*s) {
                case 'n':  buf[i++] = '\n'; s++; break;
                case 't':  buf[i++] = '\t'; s++; break;
                case '"':  buf[i++] = '"';  s++; break;
                case '\\': buf[i++] = '\\'; s++; break;
                default:   buf[i++] = '\\'; buf[i++] = *s++; break;
            }
        } else {
            int cplen = utf8_cp_len((unsigned char)*s);
            if (i + cplen >= buf_size - 4) die("স্ট্রিং অতিরিক্ত দীর্ঘ");
            for (int k = 0; k < cplen && *s; k++) buf[i++] = *s++;
        }
    }
    buf[i] = '\0';
    if (*s == '"') s++;
    return s;
}

/* ── Main tokenize function ──────────────────────────────────────────── */

/* Tokenize a single source line into tlist.
   Returns 1 normally, 0 if the line is a comment / empty (caller may skip). */
static int tokenize(const char *raw_line, TokenList *tlist) {
    tlist->count = 0;
    const char *s = lex_skip_spaces(raw_line);

    /* Empty line */
    if (!*s || *s == '\n' || *s == '\r') return 0;

    while (*s && *s != '\n' && *s != '\r') {
        s = lex_skip_spaces(s);
        if (!*s || *s == '\n' || *s == '\r') break;

        if (tlist->count >= MAX_TOKENS - 1) die("অতিরিক্ত টোকেন");
        Token *tok = &tlist->tokens[tlist->count];
        memset(tok, 0, sizeof(*tok));

        /* Comment (ASCII) */
        if (*s == '#') {
            tok->kind = TOK_COMMENT;
            strcpy(tok->text, "#");
            tlist->count++;
            return 0; /* rest of line is comment */
        }

        /* String literal */
        if (*s == '"') {
            tok->kind = TOK_STRING;
            s = lex_read_string(s, tok->text, sizeof(tok->text));
            tlist->count++;
            continue;
        }

        /* Single-char structural tokens */
        if (*s == '=') { tok->kind = TOK_ASSIGN;  tok->text[0] = '='; tok->text[1] = '\0'; tlist->count++; s++; continue; }
        if (*s == '(') { tok->kind = TOK_LPAREN;  tok->text[0] = '('; tok->text[1] = '\0'; tlist->count++; s++; continue; }
        if (*s == ')') { tok->kind = TOK_RPAREN;  tok->text[0] = ')'; tok->text[1] = '\0'; tlist->count++; s++; continue; }
        if (*s == '+' || *s == '-' || *s == '*' || *s == '/') {
            tok->kind = TOK_OP;
            tok->text[0] = *s; tok->text[1] = '\0';
            tlist->count++; s++;
            continue;
        }

        /* Word token: keyword / bool / number / identifier */
        char word[MAX_VAR_VALUE];
        s = lex_read_word(s, word, sizeof(word));

        if (word[0] == '\0') {
            /* Unrecognised byte — skip it */
            s++;
            continue;
        }

        /* Bengali comment keyword */
        if (strcmp(word, "মন্তব্য") == 0) {
            tok->kind = TOK_COMMENT;
            strcpy(tok->text, word);
            tlist->count++;
            return 0;
        }

        /* Boolean literals */
        if (strcmp(word, BOOL_TRUE) == 0) {
            tok->kind = TOK_BOOL; tok->num = 1;
            strcpy(tok->text, word);
            tlist->count++;
            continue;
        }
        if (strcmp(word, BOOL_FALSE) == 0) {
            tok->kind = TOK_BOOL; tok->num = 0;
            strcpy(tok->text, word);
            tlist->count++;
            continue;
        }

        /* Keywords */
        int is_kw = 0;
        for (int i = 0; KEYWORDS[i]; i++) {
            if (strcmp(word, KEYWORDS[i]) == 0) {
                tok->kind = TOK_KEYWORD;
                strcpy(tok->text, word);
                tlist->count++;
                is_kw = 1;
                break;
            }
        }
        if (is_kw) continue;

        /* Number? */
        char *endp;
        double num = strtod(word, &endp);
        if (endp != word && *endp == '\0') {
            tok->kind = TOK_NUMBER;
            tok->num  = num;
            strcpy(tok->text, word);
            tlist->count++;
            continue;
        }

        /* Identifier */
        tok->kind = TOK_IDENT;
        strcpy(tok->text, word);
        tlist->count++;
    }

    /* EOF sentinel */
    tlist->tokens[tlist->count].kind = TOK_EOF;
    tlist->tokens[tlist->count].text[0] = '\0';
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 3 – AST (Abstract Syntax Tree)
 *
 *  Node kinds:
 *    NODE_PROGRAM       – top-level list of statements
 *    NODE_ASSIGN        – ধরি <ident> = <expr>
 *    NODE_PRINT         – বল(<expr>)
 *    NODE_BINOP         – <expr> op <expr>
 *    NODE_STRING_LIT    – string value
 *    NODE_NUMBER_LIT    – numeric value
 *    NODE_BOOL_LIT      – boolean value
 *    NODE_IDENT         – variable reference
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    NODE_PROGRAM,
    NODE_ASSIGN,
    NODE_PRINT,
    NODE_BINOP,
    NODE_STRING_LIT,
    NODE_NUMBER_LIT,
    NODE_BOOL_LIT,
    NODE_IDENT
} NodeKind;

typedef struct ASTNode ASTNode;
struct ASTNode {
    NodeKind kind;

    /* For literals / identifiers */
    char   text[MAX_VAR_VALUE];
    double num;
    int    bool_val;

    /* For binary ops */
    char op; /* '+', '-', '*', '/' */

    /* Children (non-owning, point into a pool) */
    ASTNode *left;
    ASTNode *right;

    /* For program / statement lists (linked list) */
    ASTNode *next;
};

/* ── Node pool (avoids malloc per node) ─────────────────────────────── */
static ASTNode ast_pool[MAX_AST_NODES];
static int     ast_pool_used = 0;

static ASTNode *ast_alloc(NodeKind kind) {
    if (ast_pool_used >= MAX_AST_NODES) die("AST নোড সীমা অতিক্রম করা হয়েছে");
    ASTNode *n = &ast_pool[ast_pool_used++];
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}

static void ast_pool_reset(void) { ast_pool_used = 0; }

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 4 – PARSER
 *
 *  Input : TokenList (from lexer)
 *  Output: ASTNode * (expression tree or statement node)
 *
 *  Grammar (simplified):
 *    program   → stmt*
 *    stmt      → assign_stmt | print_stmt
 *    assign    → KW("ধরি") IDENT ASSIGN expr
 *    print     → KW("বল") LPAREN expr RPAREN
 *    expr      → term (('+' | '-') term)*
 *    term      → factor (('*' | '/') factor)*
 *    factor    → STRING_LIT | NUMBER_LIT | BOOL_LIT | IDENT
 * ═══════════════════════════════════════════════════════════════════════ */

/* Parser cursor */
typedef struct {
    const TokenList *tl;
    int              pos;
} Parser;

static Token *p_peek(Parser *p) {
    return (Token *)&p->tl->tokens[p->pos];
}
static Token *p_advance(Parser *p) {
    Token *t = (Token *)&p->tl->tokens[p->pos];
    if (t->kind != TOK_EOF) p->pos++;
    return t;
}
static Token *p_expect(Parser *p, TokenKind k, const char *errmsg) {
    Token *t = p_peek(p);
    if (t->kind != k) die(errmsg);
    return p_advance(p);
}

/* Forward declaration */
static ASTNode *parse_expr(Parser *p);

static ASTNode *parse_factor(Parser *p) {
    Token *t = p_peek(p);

    if (t->kind == TOK_STRING) {
        p_advance(p);
        ASTNode *n = ast_alloc(NODE_STRING_LIT);
        snprintf(n->text, MAX_VAR_VALUE, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_NUMBER) {
        p_advance(p);
        ASTNode *n = ast_alloc(NODE_NUMBER_LIT);
        n->num = t->num;
        snprintf(n->text, MAX_VAR_VALUE, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_BOOL) {
        p_advance(p);
        ASTNode *n = ast_alloc(NODE_BOOL_LIT);
        n->bool_val = (int)t->num;
        snprintf(n->text, MAX_VAR_VALUE, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_IDENT) {
        p_advance(p);
        ASTNode *n = ast_alloc(NODE_IDENT);
        snprintf(n->text, MAX_VAR_VALUE, "%s", t->text);
        return n;
    }
    if (t->kind == TOK_LPAREN) {
        p_advance(p);
        ASTNode *inner = parse_expr(p);
        p_expect(p, TOK_RPAREN, "')' প্রত্যাশিত");
        return inner;
    }

    die2("অপ্রত্যাশিত টোকেন", t->text);
    return NULL; /* unreachable */
}

static ASTNode *parse_term(Parser *p) {
    ASTNode *left = parse_factor(p);
    while (p_peek(p)->kind == TOK_OP &&
           (p_peek(p)->text[0] == '*' || p_peek(p)->text[0] == '/')) {
        Token  *op  = p_advance(p);
        ASTNode *right = parse_factor(p);
        ASTNode *node  = ast_alloc(NODE_BINOP);
        node->op    = op->text[0];
        node->left  = left;
        node->right = right;
        left = node;
    }
    return left;
}

static ASTNode *parse_expr(Parser *p) {
    ASTNode *left = parse_term(p);
    while (p_peek(p)->kind == TOK_OP &&
           (p_peek(p)->text[0] == '+' || p_peek(p)->text[0] == '-')) {
        Token  *op    = p_advance(p);
        ASTNode *right = parse_term(p);
        ASTNode *node  = ast_alloc(NODE_BINOP);
        node->op    = op->text[0];
        node->left  = left;
        node->right = right;
        left = node;
    }
    return left;
}

/* Parse a statement from token list; returns NODE_ASSIGN or NODE_PRINT */
static ASTNode *parse_stmt(const TokenList *tl) {
    Parser p;
    p.tl  = tl;
    p.pos = 0;

    Token *first = p_peek(&p);
    if (first->kind == TOK_EOF) return NULL;

    /* Assignment: ধরি <ident> = <expr> */
    if (first->kind == TOK_KEYWORD && strcmp(first->text, "ধরি") == 0) {
        p_advance(&p);
        Token *name_tok = p_expect(&p, TOK_IDENT, "চলকের নাম প্রত্যাশিত (ধরি এর পরে)");
        p_expect(&p, TOK_ASSIGN, "'=' প্রত্যাশিত");
        ASTNode *expr = parse_expr(&p);
        if (p_peek(&p)->kind != TOK_EOF)
            die("অ্যাসাইনমেন্টের পরে অতিরিক্ত টোকেন");

        ASTNode *node = ast_alloc(NODE_ASSIGN);
        memcpy(node->text, name_tok->text, MAX_VAR_VALUE);
        node->text[MAX_VAR_VALUE - 1] = '\0';
        node->left = expr;  /* expr to assign */
        return node;
    }

    /* Print: বল(<expr>) */
    if (first->kind == TOK_KEYWORD && strcmp(first->text, "বল") == 0) {
        p_advance(&p);
        p_expect(&p, TOK_LPAREN, "'(' প্রত্যাশিত (বল এর পরে)");
        ASTNode *expr = parse_expr(&p);
        p_expect(&p, TOK_RPAREN, "')' প্রত্যাশিত (বল বন্ধ করতে)");
        if (p_peek(&p)->kind != TOK_EOF)
            die("বল() এর পরে অতিরিক্ত টোকেন");

        ASTNode *node = ast_alloc(NODE_PRINT);
        node->left = expr;
        return node;
    }

    /* Unknown */
    die2("অজানা নির্দেশনা", first->text);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 5 – RUNTIME / EVALUATOR
 *
 *  Variable store + recursive eval_node().
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Variable store ──────────────────────────────────────────────────── */
typedef struct {
    char  name[MAX_VAR_NAME];
    Value val;
    int   used;
} Variable;

static Variable var_store[MAX_VARS];
static int      var_count = 0;

static Variable *rt_find_var(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (var_store[i].used && strcmp(var_store[i].name, name) == 0)
            return &var_store[i];
    return NULL;
}

static Variable *rt_get_or_create(const char *name) {
    Variable *v = rt_find_var(name);
    if (v) return v;
    if (var_count >= MAX_VARS) die("সর্বোচ্চ চলক সীমা অতিক্রম করা হয়েছে");
    v = &var_store[var_count++];
    snprintf(v->name, MAX_VAR_NAME, "%.*s", MAX_VAR_NAME - 1, name);
    v->used = 1;
    memset(&v->val, 0, sizeof(v->val));
    return v;
}

/* ── Value helpers ───────────────────────────────────────────────────── */
static void val_set_number(Value *v, double n) {
    v->type     = TYPE_NUMBER;
    v->num_val  = n;
    v->bool_val = (n != 0);
    snprintf(v->str_val, MAX_VAR_VALUE,
             (n == (long long)n) ? "%.0f" : "%g", n);
}

static void val_set_string(Value *v, const char *s) {
    v->type    = TYPE_STRING;
    v->num_val = 0;
    v->bool_val = (s && s[0] != '\0');
    snprintf(v->str_val, MAX_VAR_VALUE, "%s", s);
}

static void val_set_bool(Value *v, int b, const char *repr) {
    v->type     = TYPE_BOOL;
    v->num_val  = b ? 1 : 0;
    v->bool_val = b;
    snprintf(v->str_val, MAX_VAR_VALUE, "%s", repr);
}

/* ── Recursive expression evaluator ─────────────────────────────────── */
static Value eval_node(const ASTNode *node) {
    Value result;
    memset(&result, 0, sizeof(result));

    switch (node->kind) {

        case NODE_NUMBER_LIT:
            val_set_number(&result, node->num);
            break;

        case NODE_STRING_LIT:
            val_set_string(&result, node->text);
            break;

        case NODE_BOOL_LIT:
            val_set_bool(&result, node->bool_val, node->text);
            break;

        case NODE_IDENT: {
            Variable *v = rt_find_var(node->text);
            if (!v) die2("অপরিচিত চলক", node->text);
            result = v->val;
            break;
        }

        case NODE_BINOP: {
            Value lv = eval_node(node->left);
            Value rv = eval_node(node->right);

            if (node->op == '+') {
                /* If either operand is a string → concatenation */
                if (lv.type == TYPE_STRING || rv.type == TYPE_STRING ||
                    lv.type == TYPE_BOOL   || rv.type == TYPE_BOOL) {
                    char buf[MAX_VAR_VALUE];
                    int llen = (int)strlen(lv.str_val);
                    int rlen = (int)strlen(rv.str_val);
                    if (llen >= MAX_VAR_VALUE - 1) llen = MAX_VAR_VALUE - 1;
                    memcpy(buf, lv.str_val, (size_t)llen);
                    int space = MAX_VAR_VALUE - 1 - llen;
                    if (rlen > space) rlen = space;
                    memcpy(buf + llen, rv.str_val, (size_t)rlen);
                    buf[llen + rlen] = '\0';
                    val_set_string(&result, buf);
                } else {
                    val_set_number(&result, lv.num_val + rv.num_val);
                }
            } else if (node->op == '-') {
                if (lv.type == TYPE_STRING || rv.type == TYPE_STRING)
                    die("স্ট্রিং থেকে বিয়োগ করা যায় না");
                val_set_number(&result, lv.num_val - rv.num_val);
            } else if (node->op == '*') {
                if (lv.type == TYPE_STRING || rv.type == TYPE_STRING)
                    die("স্ট্রিং গুণ করা যায় না");
                val_set_number(&result, lv.num_val * rv.num_val);
            } else if (node->op == '/') {
                if (lv.type == TYPE_STRING || rv.type == TYPE_STRING)
                    die("স্ট্রিং ভাগ করা যায় না");
                if (rv.num_val == 0) die("শূন্য দিয়ে ভাগ করা যায় না");
                val_set_number(&result, lv.num_val / rv.num_val);
            } else {
                die("অজানা অপারেটর");
            }
            break;
        }

        default:
            die("eval_node: অপ্রত্যাশিত নোড টাইপ");
    }

    return result;
}

/* ── Statement executor ──────────────────────────────────────────────── */
static void exec_stmt(const ASTNode *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case NODE_ASSIGN: {
            Value v = eval_node(stmt->left);
            Variable *var = rt_get_or_create(stmt->text);
            var->val = v;
            break;
        }
        case NODE_PRINT: {
            Value v = eval_node(stmt->left);
            printf("%s\n", v.str_val);
            break;
        }
        default:
            die("exec_stmt: অপ্রত্যাশিত বিবৃতি টাইপ");
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 6 – DRIVER  (process one source line end-to-end)
 * ═══════════════════════════════════════════════════════════════════════ */

static void process_line(const char *raw_line) {
    /* Reset AST pool for each line (single-line statements) */
    ast_pool_reset();

    int has_tokens = tokenize(raw_line, &g_token_list);
    if (!has_tokens) return;           /* empty or comment */
    if (g_token_list.count == 0) return;
    if (g_token_list.tokens[0].kind == TOK_EOF) return;

    ASTNode *stmt = parse_stmt(&g_token_list);
    if (!stmt) return;

    exec_stmt(stmt);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 7 – FILE RUNNER & REPL
 * ═══════════════════════════════════════════════════════════════════════ */

static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "❌ ফাইল খোলা যায়নি: %s\n", path);
        exit(1);
    }

    char line[MAX_LINE_LEN];
    g_line_number = 0;

    while (fgets(line, sizeof(line), f)) {
        g_line_number++;
        process_line(line);
    }

    fclose(f);
}

static void run_repl(void) {
    printf("** মাতৃকা v2.0 -- ইন্টারেক্টিভ মোড **\n");
    printf("   বের হতে টাইপ করুন: বিদায়\n");
    printf("-------------------------------------------\n");

    char line[MAX_LINE_LEN];
    g_line_number = 0;

    while (1) {
        printf("মাতৃকা> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        g_line_number++;

        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Trim and check for exit keyword */
        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (strcmp(trimmed, "বিদায়") == 0) { printf("বিদায়!\n"); break; }

        process_line(line);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SECTION 8 – ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    ENABLE_WIN_UTF8();

    if (argc == 1) {
        run_repl();
    } else if (argc == 2) {
        g_filename = argv[1];
        const char *ext = strrchr(argv[1], '.');
        if (!ext || strcmp(ext, ".matrika") != 0)
            fprintf(stderr, "[!] সতর্কতা: ফাইলের নাম .matrika দিয়ে শেষ হওয়া উচিত\n");
        run_file(argv[1]);
    } else {
        fprintf(stderr, "ব্যবহার: matrika [ফাইল.matrika]\n");
        return 1;
    }
    return 0;
}