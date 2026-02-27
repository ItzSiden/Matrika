/*
 * মাতৃকা (Matrika) - v1.0
 * বাংলাভাষীদের জন্য প্রোগ্রামিং ভাষা
 * Standalone interpreter — no dependencies
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Windows UTF-8 console support */
#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
  #define ENABLE_WIN_UTF8() \
      SetConsoleOutputCP(CP_UTF8); \
      SetConsoleCP(CP_UTF8); \
      _setmode(_fileno(stdout), _O_BINARY); \
      _setmode(_fileno(stderr), _O_BINARY); \
      _setmode(_fileno(stdin),  _O_BINARY);
#else
  #define ENABLE_WIN_UTF8() /* no-op on Linux/Mac */
#endif

/* ========== Configuration ========== */
#define MAX_VARS      256
#define MAX_VAR_NAME  256
#define MAX_VAR_VALUE 4096
#define MAX_LINE_LEN  8192
#define MAX_LINES     65536

/* ========== Types ========== */
typedef enum {
    TYPE_STRING,
    TYPE_NUMBER,
    TYPE_BOOL
} VarType;

typedef struct {
    char name[MAX_VAR_NAME];
    char str_val[MAX_VAR_VALUE];
    double num_val;
    int bool_val;
    VarType type;
    int used;
} Variable;

/* ========== Global State ========== */
static Variable vars[MAX_VARS];
static int var_count = 0;
static int line_number = 0;
static const char *filename = "";

/* ========== Error Handling ========== */
static void error(const char *msg) {
    fprintf(stderr, "\n[!] মাতৃকা ত্রুটি [লাইন %d]: %s\n", line_number, msg);
    exit(1);
}

static void error2(const char *msg, const char *detail) {
    fprintf(stderr, "\n[!] মাতৃকা ত্রুটি [লাইন %d]: %s '%s'\n", line_number, msg, detail);
    exit(1);
}

/* ========== UTF-8 Utilities ========== */

/* Skip leading whitespace (ASCII + UTF-8 safe) */
static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    return s;
}

/* Compare UTF-8 keyword at start of string, return pointer after keyword or NULL */
static const char *match_keyword(const char *src, const char *keyword) {
    size_t klen = strlen(keyword);
    if (strncmp(src, keyword, klen) == 0) {
        return src + klen;
    }
    return NULL;
}

/* ========== Variable Management ========== */
static Variable *find_var(const char *name) {
    for (int i = 0; i < var_count; i++) {
        if (vars[i].used && strcmp(vars[i].name, name) == 0)
            return &vars[i];
    }
    return NULL;
}

static Variable *create_var(const char *name) {
    if (var_count >= MAX_VARS) error("সর্বোচ্চ চলক সীমা অতিক্রম করা হয়েছে");
    Variable *v = &vars[var_count++];
    strncpy(v->name, name, MAX_VAR_NAME - 1);
    v->name[MAX_VAR_NAME - 1] = '\0';
    v->used = 1;
    v->str_val[0] = '\0';
    v->num_val = 0;
    v->bool_val = 0;
    return v;
}

static Variable *get_or_create_var(const char *name) {
    Variable *v = find_var(name);
    if (!v) v = create_var(name);
    return v;
}

/* ========== Expression Evaluation ========== */

/* Forward declare */
static void eval_expr(const char *expr, Variable *result);

/* Count UTF-8 chars in a string (for display) */
static int utf8_strlen(const char *s) {
    int count = 0;
    while (*s) {
        if ((*s & 0xC0) != 0x80) count++;
        s++;
    }
    return count;
}

/* Parse a string literal "..." and fill result */
static const char *parse_string_literal(const char *s, char *out, int out_size) {
    /* s should point to opening quote */
    if (*s != '"') return NULL;
    s++;
    int i = 0;
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            if (*s == 'n') { out[i++] = '\n'; s++; }
            else if (*s == 't') { out[i++] = '\t'; s++; }
            else if (*s == '"') { out[i++] = '"'; s++; }
            else if (*s == '\\') { out[i++] = '\\'; s++; }
            else { out[i++] = '\\'; out[i++] = *s++; }
        } else {
            /* Copy UTF-8 byte */
            out[i++] = *s++;
        }
        if (i >= out_size - 4) error("স্ট্রিং অতিরিক্ত দীর্ঘ");
    }
    out[i] = '\0';
    if (*s == '"') s++;
    return s;
}

/* Try parse number, return 1 on success */
static int try_parse_number(const char *s, double *out) {
    char *end;
    double val = strtod(s, &end);
    end = (char *)skip_spaces(end);
    if (end != s && *end == '\0') {
        *out = val;
        return 1;
    }
    return 0;
}

/* Trim trailing whitespace in place */
static void trim_trailing(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

/* 
 * Evaluate expression into result Variable.
 * Supports:
 *   - String literals: "hello"
 *   - Numbers: 42, 3.14
 *   - Variable references: নাম
 *   - String concatenation with +: "hello" + নাম
 *   - Arithmetic: num + num, num - num, num * num, num / num
 *   - Boolean literals: সত্য, মিথ্যা
 */
static void eval_expr(const char *raw_expr, Variable *result) {
    char expr_buf[MAX_VAR_VALUE * 2];
    strncpy(expr_buf, raw_expr, sizeof(expr_buf) - 1);
    expr_buf[sizeof(expr_buf) - 1] = '\0';
    trim_trailing(expr_buf);

    const char *expr = skip_spaces(expr_buf);

    /* Boolean literals */
    if (strcmp(expr, "সত্য") == 0) {
        result->type = TYPE_BOOL;
        result->bool_val = 1;
        strcpy(result->str_val, "সত্য");
        result->num_val = 1;
        return;
    }
    if (strcmp(expr, "মিথ্যা") == 0) {
        result->type = TYPE_BOOL;
        result->bool_val = 0;
        strcpy(result->str_val, "মিথ্যা");
        result->num_val = 0;
        return;
    }

    /* We'll parse left-to-right for + - * / operators */
    /* Build a simple token-based evaluator */
    
    /* First, collect all tokens separated by operators */
    /* Strategy: scan for operators not inside quotes */
    
    /* Accumulate result by scanning tokens */
    char accumulated_str[MAX_VAR_VALUE] = "";
    double accumulated_num = 0;
    int is_string_mode = 0;  /* Once we see a string, we go string concat mode */
    int first_token = 1;
    char last_op = '+';
    
    const char *p = expr;
    
    while (*p) {
        p = skip_spaces(p);
        if (!*p) break;
        
        /* Read one value token */
        char token_str[MAX_VAR_VALUE] = "";
        double token_num = 0;
        int token_is_string = 0;
        
        if (*p == '"') {
            /* String literal */
            const char *after = parse_string_literal(p, token_str, sizeof(token_str));
            if (!after) error("স্ট্রিং লিটারেল সঠিক নয়");
            p = after;
            token_is_string = 1;
            is_string_mode = 1;
        } else {
            /* Number or variable - read until operator or end */
            char word[MAX_VAR_VALUE];
            int wi = 0;
            /* Read non-operator bytes (UTF-8 safe: operators are all ASCII) */
            while (*p && *p != '+' && *p != '-' && *p != '*' && *p != '/' && *p != ' ' && *p != '\t') {
                word[wi++] = *p++;
            }
            word[wi] = '\0';
            trim_trailing(word);
            
            if (wi == 0) {
                /* hit operator or space without reading anything */
                break;
            }
            
            /* Try number */
            if (try_parse_number(word, &token_num)) {
                token_is_string = 0;
                snprintf(token_str, sizeof(token_str), 
                    (token_num == (long long)token_num) ? "%.0f" : "%g", token_num);
            } else {
                /* Variable lookup */
                Variable *v = find_var(word);
                if (!v) error2("অপরিচিত চলক", word);
                if (v->type == TYPE_STRING || v->type == TYPE_BOOL) {
                    strcpy(token_str, v->str_val);
                    token_num = v->num_val;
                    token_is_string = 1;
                    is_string_mode = 1;
                } else {
                    token_num = v->num_val;
                    snprintf(token_str, sizeof(token_str),
                        (token_num == (long long)token_num) ? "%.0f" : "%g", token_num);
                    token_is_string = 0;
                }
            }
        }
        
        /* Apply token with last_op */
        if (first_token) {
            strcpy(accumulated_str, token_str);
            accumulated_num = token_num;
            first_token = 0;
        } else {
            if (is_string_mode || last_op == '+') {
                if (last_op == '+') {
                    /* String concat or number add */
                    if (is_string_mode || token_is_string) {
                        is_string_mode = 1;
                        strncat(accumulated_str, token_str, sizeof(accumulated_str) - strlen(accumulated_str) - 1);
                    } else {
                        accumulated_num += token_num;
                        snprintf(accumulated_str, sizeof(accumulated_str),
                            (accumulated_num == (long long)accumulated_num) ? "%.0f" : "%g", accumulated_num);
                    }
                } else {
                    error("স্ট্রিং এ শুধু + অপারেটর ব্যবহার করা যাবে");
                }
            } else {
                switch (last_op) {
                    case '-': accumulated_num -= token_num; break;
                    case '*': accumulated_num *= token_num; break;
                    case '/':
                        if (token_num == 0) error("শূন্য দিয়ে ভাগ করা যায় না");
                        accumulated_num /= token_num;
                        break;
                }
                snprintf(accumulated_str, sizeof(accumulated_str),
                    (accumulated_num == (long long)accumulated_num) ? "%.0f" : "%g", accumulated_num);
            }
        }
        
        /* Read next operator if any */
        p = skip_spaces(p);
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
            last_op = *p++;
        }
    }
    
    /* Set result */
    if (is_string_mode) {
        result->type = TYPE_STRING;
        strcpy(result->str_val, accumulated_str);
        result->num_val = 0;
    } else {
        result->type = TYPE_NUMBER;
        result->num_val = accumulated_num;
        snprintf(result->str_val, MAX_VAR_VALUE,
            (accumulated_num == (long long)accumulated_num) ? "%.0f" : "%g", accumulated_num);
    }
}

/* ========== Statement Handlers ========== */

/*
 * ধরি <নাম> = <expr>
 * Assignment statement
 */
static void handle_assignment(const char *line) {
    /* line starts after "ধরি " */
    const char *p = skip_spaces(line);

    /* Read variable name (until '=' or space) */
    char varname[MAX_VAR_NAME];
    int ni = 0;
    while (*p && *p != '=' && *p != ' ' && *p != '\t') {
        varname[ni++] = *p++;
        if (ni >= MAX_VAR_NAME - 4) error("চলকের নাম অতিরিক্ত দীর্ঘ");
    }
    varname[ni] = '\0';
    trim_trailing(varname);

    if (ni == 0) error("চলকের নাম দেওয়া হয়নি");

    p = skip_spaces(p);
    if (*p != '=') error("'=' চিহ্ন পাওয়া যায়নি");
    p++; /* skip '=' */
    p = skip_spaces(p);

    /* Evaluate expression */
    Variable result;
    memset(&result, 0, sizeof(result));
    eval_expr(p, &result);

    /* Store */
    Variable *v = get_or_create_var(varname);
    v->type = result.type;
    strcpy(v->str_val, result.str_val);
    v->num_val = result.num_val;
    v->bool_val = result.bool_val;
}

/*
 * বল(<expr>)
 * Print statement
 */
static void handle_print(const char *args) {
    /* args is content inside বল(...) */
    Variable result;
    memset(&result, 0, sizeof(result));
    eval_expr(args, &result);
    printf("%s\n", result.str_val);
}

/* ========== Line Parser ========== */

/*
 * Parse a single line of Ananda code.
 * Handles:
 *   - Empty lines / comments (#)
 *   - ধরি assignments
 *   - বল() print calls
 */
static void parse_line(const char *raw_line) {
    const char *line = skip_spaces(raw_line);

    /* Empty line */
    if (!*line || *line == '\n' || *line == '\r') return;

    /* Comment: line starts with # */
    if (*line == '#') return;

    /* Bengali comment: লাইন starts with "মন্তব্য" */
    if (match_keyword(line, "মন্তব্য")) return;

    /* Assignment: ধরি */
    const char *after;
    if ((after = match_keyword(line, "ধরি")) != NULL) {
        handle_assignment(after);
        return;
    }

    /* Print: বল(...) */
    if ((after = match_keyword(line, "বল(")) != NULL) {
        /* Find closing ) */
        /* Find last ) in line */
        const char *end = after + strlen(after) - 1;
        while (end >= after && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) end--;
        if (*end != ')') error("বল() বন্ধ করা হয়নি — ')' দেওয়া হয়নি");
        
        /* Copy inner expression */
        int inner_len = end - after;
        if (inner_len < 0) inner_len = 0;
        char inner[MAX_VAR_VALUE];
        strncpy(inner, after, inner_len);
        inner[inner_len] = '\0';
        handle_print(inner);
        return;
    }

    /* Unknown statement */
    char msg_buf[512];
    /* Show first 40 bytes of line for context */
    char preview[64];
    strncpy(preview, line, 60);
    preview[60] = '\0';
    trim_trailing(preview);
    snprintf(msg_buf, sizeof(msg_buf), "অজানা নির্দেশনা: %s", preview);
    error(msg_buf);
}

/* ========== File Runner ========== */
static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "❌ ফাইল খোলা যায়নি: %s\n", path);
        exit(1);
    }

    char line[MAX_LINE_LEN];
    line_number = 0;

    while (fgets(line, sizeof(line), f)) {
        line_number++;
        parse_line(line);
    }

    fclose(f);
}

/* ========== REPL ========== */
static void run_repl(void) {
    printf("** মাতৃকা v1.0 -- ইন্টারেক্টিভ মোড **\n");
    printf("   বের হতে টাইপ করুন: বিদায়\n");
    printf("----------------------------------------\n");

    char line[MAX_LINE_LEN];
    line_number = 0;

    while (1) {
        printf("মাতৃকা> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line_number++;

        /* Trim newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strcmp(skip_spaces(line), "বিদায়") == 0) {
            printf("বিদায়!\n");
            break;
        }

        parse_line(line);
    }
}

/* ========== Entry Point ========== */
int main(int argc, char *argv[]) {
    ENABLE_WIN_UTF8();

    if (argc == 1) {
        run_repl();
    } else if (argc == 2) {
        filename = argv[1];
        /* Check extension */
        const char *ext = strrchr(argv[1], '.');
        if (!ext || strcmp(ext, ".matrika") != 0) {
            fprintf(stderr, "[!] সতর্কতা: ফাইলের নাম .matrika দিয়ে শেষ হওয়া উচিত\n");
        }
        run_file(argv[1]);
    } else {
        fprintf(stderr, "ব্যবহার: matrika [ফাইল.matrika]\n");
        return 1;
    }
    return 0;
}
