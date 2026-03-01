/*
 * মাতৃকা (Matrika) - v4.0
 * A Bengali-syntax programming language.
 *
 * Architecture:  Source → [Lexer] → Tokens → [Parser] → AST → [Evaluator]
 *
 * New in v4.0:
 *   - User-defined functions:  কাজ name(a, b) { … }
 *   - Return statement:        ফিরাও <expr>
 *   - Function call:           name(arg1, arg2)  (as an expression)
 *   - Proper call stack:       call barrier isolates caller/callee scopes
 *   - Recursion supported
 *
 * From v3.0:
 *   - Integer and string variables
 *   - Arithmetic:   +  -  *  /  %
 *   - Comparison:   ==  !=  <  <=  >  >=
 *   - If / else:    যদি … { … } নাহলে { … }
 *   - While loop:   যতক্ষণ … { … }
 *   - Lexical scope stack
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
    TOK_COMMA,    /* , — argument/parameter separator                       */
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
static const char *KW_FUNC    = "কাজ";      /* function declaration        */
static const char *KW_RETURN  = "ফিরাও";   /* return statement            */

static const char *KEYWORDS[] = {
    /* order matters: longer strings first when prefix-sharing */
    "ধরি", "বল", "যদি", "নাহলে", "যতক্ষণ", "মন্তব্য", "কাজ", "ফিরাও", NULL
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
           c == '#' || c == '"'  || c == ',';
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

            /* Comma */
            if (*s == ',') { tok->kind = TOK_COMMA;  tok->text[0] = ','; tok->text[1] = '\0'; g_tok_count++; s++; continue; }

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
 *   NODE_BLOCK        list of statements (body)
 *   NODE_ASSIGN       ধরি <ident> = <expr>
 *   NODE_PRINT        বল(<expr>)
 *   NODE_IF           যদি <cond> { <body> } [নাহলে { <body> }]
 *   NODE_WHILE        যতক্ষণ <cond> { <body> }
 *   NODE_FUNC_DECL    কাজ name(p1,p2) { body }
 *   NODE_FUNC_CALL    name(arg1, arg2)  — expression
 *   NODE_RETURN       ফিরাও <expr>
 *   NODE_BINOP        <expr> <arith-op> <expr>
 *   NODE_CMP          <expr> <cmp-op>  <expr>
 *   NODE_NUMBER_LIT
 *   NODE_STRING_LIT
 *   NODE_BOOL_LIT
 *   NODE_IDENT
 * ═══════════════════════════════════════════════════════════════════════ */

#define MAX_PARAMS  32   /* max parameters per function                    */

typedef enum {
    NODE_BLOCK,
    NODE_ASSIGN,
    NODE_PRINT,
    NODE_IF,
    NODE_WHILE,
    NODE_FUNC_DECL,
    NODE_FUNC_CALL,
    NODE_RETURN,
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

    /* Generic child pointers (binary ops, assign, print, while) */
    ASTNode *left;               /* condition / lhs / body                 */
    ASTNode *right;              /* rhs / then-body (while)                */

    /* Dedicated NODE_IF branches — avoids abusing next as a side-channel */
    ASTNode *cond;               /* if: condition expression               */
    ASTNode *then_branch;        /* if: body executed when cond is truthy  */
    ASTNode *else_branch;        /* if: body executed otherwise (may be NULL) */

    /*
     * Function support — stored as indices into separate flat pools so
     * the node struct stays small (avoids BSS overflow with 64 K nodes).
     *
     *   NODE_FUNC_DECL: text = name, param_count = arity,
     *                   param_base = first index into g_param_pool[],
     *                   left = body (NODE_BLOCK)
     *   NODE_FUNC_CALL: text = name, param_count = argc,
     *                   arg_base = first index into g_arg_pool[]
     *   NODE_RETURN:    left = return-value expression (NULL → return 0)
     */
    int      param_count;   /* arity (decl) or arg count (call)           */
    int      param_base;    /* index into g_param_pool[] for decl params  */
    int      arg_base;      /* index into g_arg_pool[]   for call args    */

    /* Linked list of statements inside a block */
    ASTNode *next;
};

/* ── Separate pools for function params and call args ───────────────── */
#define MAX_TOTAL_PARAMS  1024   /* sum over all declared functions       */
#define MAX_TOTAL_ARGS    4096   /* sum over all call sites in the AST    */

static char     g_param_pool[MAX_TOTAL_PARAMS][MAX_VAR_NAME];
static int      g_param_pool_top = 0;

static ASTNode *g_arg_pool[MAX_TOTAL_ARGS];
static int      g_arg_pool_top = 0;

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
 *              | func_decl | return_stmt
 *   assign     → KW("ধরি") IDENT '=' expr
 *   print      → KW("বল") '(' expr ')'
 *   if_stmt    → KW("যদি") expr '{' block '}' [KW("নাহলে") '{' block '}']
 *   while_stmt → KW("যতক্ষণ") expr '{' block '}'
 *   func_decl  → KW("কাজ") IDENT '(' [IDENT (',' IDENT)*] ')' '{' block '}'
 *   return_stmt→ KW("ফিরাও") expr
 *   expr       → cmp_expr
 *   cmp_expr   → add_expr (CMP add_expr)*
 *   add_expr   → mul_expr (('+' | '-') mul_expr)*
 *   mul_expr   → unary  (('*' | '/' | '%') unary)*
 *   unary      → '-' unary | factor
 *   factor     → NUMBER | STRING | BOOL | IDENT ['(' arg_list ')'] | '(' expr ')'
 *   arg_list   → [expr (',' expr)*]
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
        /* Check for function call: name( ... ) */
        if (par_peek(p)->kind == TOK_LPAREN) {
            par_advance(p); /* consume '(' */
            ASTNode *call = ast_new(NODE_FUNC_CALL, t->line);
            snprintf(call->text, MAX_STR, "%s", t->text);
            call->param_count = 0;
            call->arg_base    = g_arg_pool_top;
            /* Parse argument list */
            while (par_peek(p)->kind != TOK_RPAREN) {
                if (call->param_count >= MAX_PARAMS)
                    die(t->line, "too many arguments in function call");
                if (g_arg_pool_top >= MAX_TOTAL_ARGS)
                    die(t->line, "argument pool exhausted");
                if (call->param_count > 0)
                    par_expect(p, TOK_COMMA, "expected ',' between arguments");
                g_arg_pool[g_arg_pool_top++] = parse_expr(p);
                call->param_count++;
            }
            par_expect(p, TOK_RPAREN, "expected ')' to close argument list");
            return call;
        }
        /* Plain identifier */
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
            stmt->cond        = cond;
            stmt->then_branch = then_body;
            stmt->else_branch = else_body;
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

        /* ── কাজ function declaration ─────────────────────────────────── */
        else if (t->kind == TOK_KEYWORD && strcmp(t->text, KW_FUNC) == 0) {
            par_advance(p);
            Token *name = par_expect(p, TOK_IDENT,
                "expected function name after 'কাজ'");
            par_expect(p, TOK_LPAREN,
                "expected '(' after function name");

            stmt = ast_new(NODE_FUNC_DECL, t->line);
            snprintf(stmt->text, MAX_STR, "%s", name->text);
            stmt->param_count = 0;
            stmt->param_base  = g_param_pool_top;

            /* Parse parameter list: name, name, ... */
            while (par_peek(p)->kind != TOK_RPAREN) {
                if (par_peek(p)->kind == TOK_EOF)
                    die(t->line, "unexpected end of file in parameter list");
                if (stmt->param_count >= MAX_PARAMS)
                    die(t->line, "too many parameters in function declaration");
                if (g_param_pool_top >= MAX_TOTAL_PARAMS)
                    die(t->line, "parameter pool exhausted");
                if (stmt->param_count > 0)
                    par_expect(p, TOK_COMMA,
                        "expected ',' between parameter names");
                Token *pname = par_expect(p, TOK_IDENT,
                    "expected parameter name");
                snprintf(g_param_pool[g_param_pool_top], MAX_VAR_NAME,
                         "%.*s", MAX_VAR_NAME - 1, pname->text);
                g_param_pool_top++;
                stmt->param_count++;
            }
            par_expect(p, TOK_RPAREN,
                "expected ')' to close parameter list");
            par_expect(p, TOK_LBRACE,
                "expected '{' to open function body");
            stmt->left = parse_block(p);   /* body */
            par_expect(p, TOK_RBRACE,
                "expected '}' to close function body");
        }

        /* ── ফিরাও return ────────────────────────────────────────────── */
        else if (t->kind == TOK_KEYWORD && strcmp(t->text, KW_RETURN) == 0) {
            par_advance(p);
            stmt = ast_new(NODE_RETURN, t->line);
            /* Return value is optional — if next token starts a statement
               boundary ('}', keyword, EOF) treat as "return 0"            */
            Token *nx = par_peek(p);
            int at_boundary = (nx->kind == TOK_RBRACE ||
                               nx->kind == TOK_EOF    ||
                               nx->kind == TOK_KEYWORD);
            stmt->left = at_boundary ? NULL : parse_expr(p);
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

/* exec_program — defined after the evaluator (needs exec and g_return) */
static void exec_program(const ASTNode *block);


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

/* ── Scope stack ────────────────────────────────────────────────────── */
/*
 * All variables live in one flat pool (g_var_pool[]).  A scope "frame"
 * records the pool index at which that scope starts; pop_scope() simply
 * restores the pool top to the saved value, discarding the inner vars.
 *
 * Lookup walks frames from innermost (top) to outermost (0), searching
 * each slice of the pool so inner bindings shadow outer ones.
 * Assignment updates an existing binding wherever it lives, or creates a
 * new one in the current (innermost) scope.
 */
#define MAX_SCOPE_DEPTH  256
#define MAX_VARS_TOTAL   4096   /* total vars across ALL scopes at once     */

typedef struct {
    char name[MAX_VAR_NAME];
    Val  val;
    int  used;
} Var;

static Var g_var_pool[MAX_VARS_TOTAL];
static int g_var_top = 0;             /* next free slot in the pool          */

/* Frame stack: each entry is the pool index where that scope begins. */
static int g_frames[MAX_SCOPE_DEPTH + 1];
static int g_frame_top = 0;          /* current frame index (0 = global)    */

static void scope_init(void) {
    g_var_top   = 0;
    g_frame_top = 0;
    g_frames[0] = 0;
}

static void push_scope(void) {
    if (g_frame_top >= MAX_SCOPE_DEPTH - 1)
        die(0, "scope nesting limit exceeded");
    g_frame_top++;
    g_frames[g_frame_top] = g_var_top;  /* new scope starts here in pool    */
}

static void pop_scope(void) {
    if (g_frame_top == 0)
        die(0, "internal error: cannot pop global scope");
    g_var_top = g_frames[g_frame_top];  /* discard vars of the popped scope  */
    g_frame_top--;
}

/* Forward declaration — defined after the call-stack section below.      */
static int call_barrier(void);

/* Search innermost scope first, then outward — but stop at call barrier.
 * Frames ABOVE the barrier (local to the current call) are always searched.
 * Frames AT OR BELOW the barrier are caller frames and are skipped —
 * EXCEPT frame 0, which is the program top-level (global) scope and is
 * always visible.                                                         */
static Var *var_find(const char *name) {
    int barrier = call_barrier();
    for (int f = g_frame_top; f >= 0; f--) {
        /* Skip caller's frames (between barrier and the call's own frames),
           but always search the very bottom frame (true global scope).    */
        if (f > 0 && f <= barrier) continue;
        int start = g_frames[f];
        int end   = (f == g_frame_top) ? g_var_top : g_frames[f + 1];
        for (int i = start; i < end; i++)
            if (g_var_pool[i].used && strcmp(g_var_pool[i].name, name) == 0)
                return &g_var_pool[i];
    }
    return NULL;
}

/*
 * If the variable exists in any visible scope, update it there.
 * Otherwise create a new binding in the current (innermost) scope.
 */
static Var *var_set(const char *name, Val v, int line) {
    Var *var = var_find(name);
    if (var) {
        var->val = v;
        return var;
    }
    if (g_var_top >= MAX_VARS_TOTAL)
        die(line, "variable limit exceeded");
    var = &g_var_pool[g_var_top++];
    snprintf(var->name, MAX_VAR_NAME, "%.*s", MAX_VAR_NAME - 1, name);
    var->used = 1;
    var->val  = v;
    return var;
}

/*
 * var_set_local — always creates a new binding in the CURRENT scope,
 * even if the same name exists in an outer scope.  Used to bind function
 * parameters so they shadow any outer variable of the same name without
 * mutating it.
 */
static Var *var_set_local(const char *name, Val v, int line) {
    if (g_var_top >= MAX_VARS_TOTAL)
        die(line, "variable limit exceeded");
    Var *var = &g_var_pool[g_var_top++];
    snprintf(var->name, MAX_VAR_NAME, "%.*s", MAX_VAR_NAME - 1, name);
    var->used = 1;
    var->val  = v;
    return var;
}

/* ── Scope barrier (call frames) ────────────────────────────────────── */
/*
 * When a function is called we push a "call frame" — a scope that is
 * opaque to var_find so that the function body cannot accidentally read
 * or mutate the caller's locals.  We implement this by recording a
 * "barrier index": var_find stops searching when it reaches a frame whose
 * index is at or below the barrier.
 *
 *  g_call_barrier_stack[] stores the g_frame_top value at the point each
 *  function call started.  var_find only searches frames [barrier+1 ..
 *  g_frame_top], which are exactly the frames local to the current call.
 *  The global scope (frame 0) is always visible — it sits below every
 *  barrier, so we special-case it in var_find.
 */
#define MAX_CALL_DEPTH  256

static int g_call_barriers[MAX_CALL_DEPTH + 1];
static int g_call_depth = 0;

static void call_push_barrier(void) {
    if (g_call_depth >= MAX_CALL_DEPTH)
        die(0, "maximum call depth exceeded (recursion too deep?)");
    g_call_barriers[g_call_depth++] = g_frame_top;
}

static void call_pop_barrier(void) {
    if (g_call_depth == 0)
        die(0, "internal error: call barrier underflow");
    g_call_depth--;
}

/* The frame index that var_find must not cross (exclusive lower bound).
   We always allow the global scope (frame 0). */
static int call_barrier(void) {
    return (g_call_depth > 0) ? g_call_barriers[g_call_depth - 1] : -1;
}

/* ── Function table ─────────────────────────────────────────────────── */
#define MAX_FUNCS  256

typedef struct {
    char            name[MAX_VAR_NAME];
    int             arity;
    char            params[MAX_PARAMS][MAX_VAR_NAME];
    const ASTNode  *body;   /* NODE_BLOCK — not owned, points into AST pool */
    int             defined;
} FuncDef;

static FuncDef g_funcs[MAX_FUNCS];
static int     g_func_count = 0;

static FuncDef *func_find(const char *name) {
    for (int i = 0; i < g_func_count; i++)
        if (g_funcs[i].defined && strcmp(g_funcs[i].name, name) == 0)
            return &g_funcs[i];
    return NULL;
}

static void func_register(const ASTNode *decl) {
    if (func_find(decl->text))
        die(decl->line, "function already defined");
    if (g_func_count >= MAX_FUNCS)
        die(decl->line, "function limit exceeded");
    FuncDef *f = &g_funcs[g_func_count++];
    snprintf(f->name, MAX_VAR_NAME, "%.*s", MAX_VAR_NAME - 1, decl->text);
    f->arity = decl->param_count;
    for (int i = 0; i < decl->param_count; i++)
        snprintf(f->params[i], MAX_VAR_NAME, "%s",
                 g_param_pool[decl->param_base + i]);
    f->body    = decl->left;   /* NODE_BLOCK */
    f->defined = 1;
}

/* ── Return signal ──────────────────────────────────────────────────── */
/*
 * Rather than using setjmp/longjmp or exceptions, we use a simple flag.
 * exec() checks g_return.active after every statement and bails out early
 * if it is set.  call_function() clears the flag after the call returns.
 */
typedef struct {
    int active;   /* 1 = ফিরাও has been executed, unwind in progress       */
    Val value;    /* the value being returned                                */
} ReturnSignal;

static ReturnSignal g_return = {0, {0, 0, 0, ""}};

static void return_signal_clear(void) {
    g_return.active = 0;
}


/* ═══════════════════════════════════════════════════════════════════════
 * SECTION 6 — EVALUATOR
 * ═══════════════════════════════════════════════════════════════════════ */

static Val  eval(const ASTNode *node);       /* forward decl               */
static void exec(const ASTNode *node);       /* forward decl               */

/* ── Function call dispatch ─────────────────────────────────────────── */
static Val call_function(const FuncDef *f, const ASTNode *call_node,
                         Val *argv, int argc) {
    if (argc != f->arity)
        die(call_node->line, "wrong number of arguments");

    /* Save scope state, push a call barrier so the callee cannot see
       the caller's local variables (only globals in frame 0 stay visible) */
    call_push_barrier();
    push_scope();   /* the function's own local scope                       */

    /* Bind parameters as locals in the new scope */
    for (int i = 0; i < f->arity; i++)
        var_set_local(f->params[i], argv[i], call_node->line);

    /* Execute the function body */
    return_signal_clear();
    exec(f->body);

    /* Capture return value (default to 0 if no ফিরাও was hit) */
    Val result = g_return.active ? g_return.value : make_num(0.0);
    return_signal_clear();

    /* Restore scope */
    pop_scope();
    call_pop_barrier();

    return result;
}

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

        /* ── Function call ────────────────────────────────────────────── */
        case NODE_FUNC_CALL: {
            FuncDef *f = func_find(node->text);
            if (!f) die_tok(node->line, "undefined function", node->text);
            if (node->param_count != f->arity)
                die(node->line, "wrong number of arguments");
            /* Evaluate all arguments before pushing scope */
            Val argv[MAX_PARAMS];
            for (int i = 0; i < node->param_count; i++)
                argv[i] = eval(g_arg_pool[node->arg_base + i]);
            return call_function(f, node, argv, node->param_count);
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
            push_scope();
            ASTNode *s = node->left;
            while (s) {
                exec(s);
                /* Propagate return signal: stop executing further stmts   */
                if (g_return.active) break;
                s = s->next;
            }
            pop_scope();
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
            Val cond = eval(node->cond);
            if (val_truthy(&cond)) {
                exec(node->then_branch);
            } else if (node->else_branch) {
                exec(node->else_branch);
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
                if (g_return.active) break;   /* ফিরাও inside loop         */
                if (++guard > 10000000)
                    die(node->line,
                        "loop exceeded 10,000,000 iterations — "
                        "possible infinite loop");
            }
            break;
        }

        /* ── Function declaration ─────────────────────────────────────── */
        case NODE_FUNC_DECL: {
            func_register(node);
            break;
        }

        /* ── Return statement ─────────────────────────────────────────── */
        case NODE_RETURN: {
            g_return.value  = node->left ? eval(node->left) : make_num(0.0);
            g_return.active = 1;
            break;
        }

        default:
            die(node->line,
                "internal error: exec() called on non-statement node");
    }
}

/*
 * exec_program — runs the top-level block WITHOUT pushing a new scope,
 * so all top-level variable declarations land in frame 0 (the true global
 * scope) and remain visible inside function calls across call barriers.
 */
static void exec_program(const ASTNode *block) {
    ASTNode *s = block->left;
    while (s) {
        exec(s);
        if (g_return.active) break;
        s = s->next;
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
    scope_init();
    g_func_count      = 0;
    g_param_pool_top  = 0;
    g_arg_pool_top    = 0;
    return_signal_clear();
    ASTNode *program = parse_program();
    exec_program(program);
}

/* REPL — parse and run one logical statement at a time.
   Multi-line constructs (if / while) are accumulated until '}' is seen. */
static void run_repl(void) {
    printf("Matrika v4.0  —  type 'বিদায়' to exit\n");
    printf("------------------------------------------\n");

    char line[MAX_LINE_LEN];
    /* Accumulate lines for a single top-level construct */
    static char repl_buf[MAX_SOURCE_LINES][MAX_LINE_LEN];
    static const char *repl_lines[MAX_SOURCE_LINES];
    int    repl_count = 0;
    int    brace_depth = 0;
    int    global_line = 0;
    scope_init();

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
                exec_program(program);
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