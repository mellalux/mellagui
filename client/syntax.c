/* syntax.c: the lexer behind the editor's colours, see syntax.h. */
#include "syntax.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

enum { K_GENERIC, K_DIFF, K_MARKDOWN, K_HTML };
enum {
    SF_PREPROC = 1,         /* # directives at the start of a line (C) */
    SF_ICASE = 2,           /* keywords in any letter case (SQL) */
    SF_KEYCOLON = 4,        /* a word or string followed by ':' is a key (JSON, YAML, CSS) */
    SF_KEYEQ = 8,           /* a word at the start of a line followed by '=' is a key (INI, TOML) */
    SF_SECTION = 16,        /* [section] lines (INI) */
    SF_DOLLAR = 32,         /* $VAR, ${VAR}, $(VAR) (shell, make) */
    SF_DASHID = 64,         /* words may contain '-' (CSS) */
    SF_HEX = 128,           /* #rrggbb is a number (CSS) */
    SF_AT = 256,            /* @name: a decorator / annotation / at-rule */
    SF_RUSTCHAR = 512,      /* ' starts a character literal only when it is one ('a is a lifetime) */
    SF_BACKTICK = 1024,     /* `...` may run over several lines (JavaScript, Go) */
    SF_TRIPLE = 2048,       /* """...""" and '''...''' (Python) */
};
/* what a line leaves for the next one */
enum { ST_NORMAL = 0, ST_BLOCK, ST_TRIPLE_S, ST_TRIPLE_D, ST_BACKTICK, ST_FENCE, ST_TAG };

struct syn_lang {
    const char *name;
    const char *exts;               /* " c h " */
    const char *names;              /* exact file names, lower case: " makefile " */
    const char *shebang;            /* interpreters: " sh bash " */
    int kind, flags;
    const char *lc1, *lc2;          /* line comment starters */
    const char *bo, *bc;            /* block comment */
    const char *quotes;             /* characters that start a string (on one line) */
    const char *kw, *types, *consts;/* words wrapped in blanks: " if else " */
};

static const char C_KW[] = " auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch "
                           "typedef union volatile while _Alignas _Alignof _Atomic _Bool _Complex _Generic _Noreturn _Static_assert _Thread_local asm ";
static const char C_TYPES[] = " void char short int long float double signed unsigned bool size_t ssize_t off_t time_t uint8_t uint16_t uint32_t uint64_t "
                              "int8_t int16_t int32_t int64_t intptr_t uintptr_t ptrdiff_t FILE va_list ";

static const struct syn_lang langs[] = {
    { "C", " c h ", "", "", K_GENERIC, SF_PREPROC, "//", NULL, "/*", "*/", "\"'", C_KW, C_TYPES, " NULL true false EOF stdin stdout stderr " },
    { "C++", " cpp cc cxx hpp hh hxx ino ", "", "", K_GENERIC, SF_PREPROC, "//", NULL, "/*", "*/", "\"'",
      " auto break case const continue default do else enum extern for goto if inline register return sizeof static struct switch typedef union volatile while "
      "alignas alignof and asm bitand bitor catch class compl concept consteval constexpr constinit const_cast co_await co_return co_yield decltype delete "
      "dynamic_cast explicit export friend mutable namespace new noexcept not operator or private protected public reinterpret_cast requires static_assert "
      "static_cast template this thread_local throw try typeid typename using virtual xor override final ",
      " void char short int long float double signed unsigned bool size_t string vector map set unique_ptr shared_ptr wchar_t char8_t char16_t char32_t "
      "uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t ",
      " nullptr NULL true false " },
    { "Java", " java ", "", "", K_GENERIC, SF_AT, "//", NULL, "/*", "*/", "\"'",
      " abstract assert break case catch class const continue default do else enum extends final finally for goto if implements import instanceof interface "
      "native new package private protected public return static strictfp super switch synchronized this throw throws transient try volatile while var record ",
      " boolean byte char double float int long short void String Integer Long Double Object List Map Set ", " true false null " },
    { "C#", " cs ", "", "", K_GENERIC, SF_PREPROC, "//", NULL, "/*", "*/", "\"'",
      " abstract as base break case catch checked class const continue default delegate do else enum event explicit extern finally fixed for foreach goto if "
      "implicit in interface internal is lock namespace new operator out override params private protected public readonly ref return sealed sizeof "
      "stackalloc static struct switch this throw try typeof unchecked unsafe using virtual volatile while async await var ",
      " bool byte char decimal double float int long object sbyte short string uint ulong ushort void String List Dictionary ", " true false null " },
    { "Python", " py pyw ", " sconstruct ", " python python2 python3 ", K_GENERIC, SF_TRIPLE | SF_AT, "#", NULL, NULL, NULL, "\"'",
      " and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise "
      "return try while with yield match case ",
      " int str float bool list dict set tuple bytes object type self cls complex frozenset range ", " True False None " },
    { "JavaScript", " js mjs cjs jsx ", "", " node nodejs deno bun ", K_GENERIC, SF_BACKTICK, "//", NULL, "/*", "*/", "\"'",
      " async await break case catch class const continue debugger default delete do else export extends finally for from function if import in instanceof "
      "let new of return static super switch this throw try typeof var void while with yield get set ",
      " Array Map Set Promise Date Error Object String Number Boolean Symbol RegExp JSON Math console window document ",
      " true false null undefined NaN Infinity " },
    { "TypeScript", " ts tsx ", "", "", K_GENERIC, SF_BACKTICK | SF_AT, "//", NULL, "/*", "*/", "\"'",
      " abstract as async await break case catch class const continue debugger declare default delete do else enum export extends finally for from function "
      "if implements import in instanceof interface is keyof let namespace new of private protected public readonly return static super switch this throw try "
      "type typeof var void while with yield ",
      " string number boolean any unknown never object symbol bigint void Array Map Set Promise Date Error Record Partial ", " true false null undefined NaN " },
    { "Rust", " rs ", "", "", K_GENERIC, SF_RUSTCHAR, "//", NULL, "/*", "*/", "\"",
      " as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move mut pub ref return self Self static struct "
      "super trait type unsafe use where while macro_rules ",
      " i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box Rc Arc HashMap ", " true false None Some Ok Err " },
    { "Go", " go ", "", "", K_GENERIC, SF_BACKTICK, "//", NULL, "/*", "*/", "\"'",
      " break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var ",
      " bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr ", " true false nil iota " },
    { "Shell", " sh bash zsh ksh bashrc zshrc profile ", " .bashrc .zshrc .profile .bash_profile .bash_aliases pkgbuild ", " sh bash zsh dash ksh ash ", K_GENERIC, SF_DOLLAR,
      "#", NULL, NULL, NULL, "\"'`",
      " if then else elif fi case esac for while until do done in function select time return break continue exit local export readonly declare typeset "
      "unset shift source alias eval exec set trap ",
      "", " true false " },
    { "Makefile", " mk ", " makefile gnumakefile ", "", K_GENERIC, SF_DOLLAR, "#", NULL, NULL, NULL, "\"'",
      " ifeq ifneq ifdef ifndef else endif include sinclude define endef export override vpath ", "", "" },
    { "JSON", " json jsonc json5 ", "", "", K_GENERIC, SF_KEYCOLON, "//", NULL, "/*", "*/", "\"", "", "", " true false null " },
    { "HTML", " html htm xhtml xml svg xsl xslt ui plist rss ", "", "", K_HTML, 0, NULL, NULL, NULL, NULL, "", "", "", "" },
    { "CSS", " css scss less ", "", "", K_GENERIC, SF_KEYCOLON | SF_DASHID | SF_HEX | SF_AT, "//", NULL, "/*", "*/", "\"'", "", "", " none auto inherit initial important " },
    { "INI", " ini conf cfg toml desktop service properties editorconfig gitconfig ", " .gitconfig .editorconfig ", "", K_GENERIC, SF_SECTION | SF_KEYEQ, "#", ";", NULL, NULL,
      "\"'", "", "", " true false yes no on off " },
    { "YAML", " yml yaml ", "", "", K_GENERIC, SF_KEYCOLON, "#", NULL, NULL, NULL, "\"'", "", "", " true false null yes no on off " },
    { "SQL", " sql ", "", "", K_GENERIC, SF_ICASE, "--", NULL, "/*", "*/", "\"'`",
      " select from where insert into values update set delete create table alter drop index join inner left right outer on group by order having limit offset "
      "union all distinct as and or not in is like between exists case when then else end primary key foreign references default unique check constraint begin "
      "commit rollback view trigger ",
      " int integer bigint smallint text varchar char date time timestamp boolean float double decimal numeric blob serial ", " null true false " },
    { "Lua", " lua ", "", " lua luajit ", K_GENERIC, 0, "--", NULL, "--[[", "]]", "\"'",
      " and break do else elseif end for function goto if in local not or repeat return then until while ", " self ", " true false nil " },
    { "Diff", " diff patch ", "", "", K_DIFF, 0, NULL, NULL, NULL, NULL, "", "", "", "" },
    { "Markdown", " md markdown mdown mkd ", "", "", K_MARKDOWN, 0, NULL, NULL, NULL, NULL, "", "", "", "" },
};
#define NLANGS ((int)(sizeof(langs) / sizeof(langs[0])))

/* ---- finding the language ----------------------------------------------------------------------------------------------------------- */

static bool has_word(const char *list, const char *w, size_t n, bool icase)      /* is the word one of the blank separated ones in `list`? */
{
    if (!list || !*list || n == 0 || n > 40)
        return false;
    for (const char *p = list; *p;) {
        while (*p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ' ')
            e++;
        if ((size_t)(e - p) == n) {
            bool same = true;
            for (size_t i = 0; i < n && same; i++)
                same = icase ? tolower((unsigned char)p[i]) == tolower((unsigned char)w[i]) : p[i] == w[i];
            if (same)
                return true;
        }
        p = e;
    }
    return false;
}

const struct syn_lang *syn_find(const char *path, const char *first_line)
{
    const char *base = path && strrchr(path, '/') ? strrchr(path, '/') + 1 : path ? path : "";
    char low[128];
    size_t n = strlen(base);
    if (n >= sizeof(low))
        n = sizeof(low) - 1;
    for (size_t i = 0; i < n; i++)
        low[i] = (char)tolower((unsigned char)base[i]);
    low[n] = 0;
    for (int i = 0; i < NLANGS; i++)
        if (n && has_word(langs[i].names, low, n, false))
            return &langs[i];
    const char *dot = strrchr(low, '.');
    if (dot && dot[1]) {
        for (int i = 0; i < NLANGS; i++)
            if (has_word(langs[i].exts, dot + 1, strlen(dot + 1), false))
                return &langs[i];
    }
    if (first_line && first_line[0] == '#' && first_line[1] == '!') {                 /* #!/usr/bin/env python3 */
        const char *p = first_line + 2, *end = p + strcspn(p, "\r\n");
        const char *last = p;
        for (const char *q = p; q < end; q++)
            if (*q == '/' || *q == ' ')
                last = q + 1;
        size_t wl = 0;
        while (last + wl < end && !isspace((unsigned char)last[wl]))
            wl++;
        for (int i = 0; i < NLANGS; i++)
            if (wl && has_word(langs[i].shebang, last, wl, false))
                return &langs[i];
    }
    return NULL;
}

const char *syn_name(const struct syn_lang *l)
{
    return l ? l->name : "";
}

/* ---- the lexer ---------------------------------------------------------------------------------------------------------------------- */

#define MARK(a, b, c) do { if (cls) for (size_t m_ = (a); m_ < (b) && m_ < len; m_++) cls[m_] = (uint8_t)(c); } while (0)

static bool idstart(unsigned char c, int flags)
{
    return isalpha(c) || c == '_' || c >= 0x80 || ((flags & SF_DOLLAR) && c == '$');
}

static bool idchar(unsigned char c, int flags)
{
    return isalnum(c) || c == '_' || c >= 0x80 || ((flags & SF_DASHID) && c == '-');
}

static size_t skip_blank(const char *s, size_t i, size_t len)
{
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return i;
}

static bool starts(const char *s, size_t i, size_t len, const char *what)
{
    size_t n = strlen(what);
    return n && i + n <= len && !memcmp(s + i, what, n);
}

/* Where the next `q` at or after i is (len when there is none); with `escapes` a backslash hides the character after it. */
static size_t find_quote(const char *s, size_t i, size_t len, char q, bool escapes)
{
    for (; i < len; i++) {
        if (escapes && s[i] == '\\')
            i++;
        else if (s[i] == q)
            return i;
    }
    return len;
}

static size_t find_str(const char *s, size_t i, size_t len, const char *what)
{
    size_t n = strlen(what);
    for (; i + n <= len; i++)
        if (!memcmp(s + i, what, n))
            return i;
    return len;
}

static int diff_line(const char *s, size_t len, uint8_t *cls)
{
    int c = SYN_PLAIN;
    if (starts(s, 0, len, "+++") || starts(s, 0, len, "---") || starts(s, 0, len, "diff ") || starts(s, 0, len, "index "))
        c = SYN_KEYWORD;
    else if (starts(s, 0, len, "@@"))
        c = SYN_PREPROC;
    else if (len && s[0] == '+')
        c = SYN_ADDED;
    else if (len && s[0] == '-')
        c = SYN_REMOVED;
    MARK(0, len, c);
    return ST_NORMAL;
}

static int md_line(const char *s, size_t len, int state, uint8_t *cls)
{
    size_t i = skip_blank(s, 0, len);
    if (starts(s, i, len, "```") || starts(s, i, len, "~~~")) {                      /* a code fence opens or closes */
        MARK(0, len, SYN_PREPROC);
        return state == ST_FENCE ? ST_NORMAL : ST_FENCE;
    }
    if (state == ST_FENCE) {
        MARK(0, len, SYN_STRING);
        return ST_FENCE;
    }
    if (i < len && s[i] == '#') {                                                     /* a heading */
        size_t k = i;
        while (k < len && s[k] == '#')
            k++;
        if (k - i <= 6 && (k == len || s[k] == ' ')) {
            MARK(0, len, SYN_KEYWORD);
            return ST_NORMAL;
        }
    }
    if (i < len && s[i] == '>') {
        MARK(0, len, SYN_COMMENT);
        return ST_NORMAL;
    }
    size_t k = i;                                                                     /* a list marker: - * + or 1. */
    if (k + 1 < len && (s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ') {
        MARK(k, k + 1, SYN_NUMBER);
    } else {
        while (k < len && isdigit((unsigned char)s[k]))
            k++;
        if (k > i && k + 1 < len && s[k] == '.' && s[k + 1] == ' ')
            MARK(i, k + 1, SYN_NUMBER);
    }
    for (size_t j = 0; j < len; j++) {                                                /* `code` spans and the address of a [link](address) */
        if (s[j] == '`') {
            size_t e = find_quote(s, j + 1, len, '`', false);
            MARK(j, e < len ? e + 1 : len, SYN_STRING);
            j = e;
        } else if (s[j] == ']' && j + 1 < len && s[j + 1] == '(') {
            size_t e = find_quote(s, j + 2, len, ')', false);
            MARK(j + 2, e, SYN_FUNC);
            j = e;
        }
    }
    return ST_NORMAL;
}

static int html_line(const char *s, size_t len, int state, uint8_t *cls)
{
    size_t i = 0;
    while (i < len) {
        if (state == ST_BLOCK) {                                                      /* a comment: <!-- ... --> */
            size_t e = find_str(s, i, len, "-->");
            MARK(i, e < len ? e + 3 : len, SYN_COMMENT);
            i = e < len ? e + 3 : len;
            state = e < len ? ST_NORMAL : ST_BLOCK;
        } else if (state == ST_TAG) {                                                 /* inside <tag ...> */
            unsigned char c = (unsigned char)s[i];
            if (c == '>' || (c == '/' && i + 1 < len && s[i + 1] == '>')) {
                size_t e = c == '/' ? i + 2 : i + 1;
                MARK(i, e, SYN_KEYWORD);
                i = e;
                state = ST_NORMAL;
            } else if (c == '"' || c == '\'') {
                size_t e = find_quote(s, i + 1, len, (char)c, false);
                MARK(i, e < len ? e + 1 : len, SYN_STRING);
                i = e < len ? e + 1 : len;
            } else if (isalpha(c) || c == '_' || c == ':') {
                size_t e = i;
                while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == ':' || s[e] == '-' || s[e] == '.'))
                    e++;
                MARK(i, e, SYN_TYPE);
                i = e;
            } else {
                i++;
            }
        } else if (s[i] == '<' && starts(s, i, len, "<!--")) {
            state = ST_BLOCK;
            MARK(i, i + 4, SYN_COMMENT);
            i += 4;
        } else if (s[i] == '<' && i + 1 < len && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '?' || s[i + 1] == '!')) {
            size_t e = i + 2;
            while (e < len && (isalnum((unsigned char)s[e]) || s[e] == ':' || s[e] == '-' || s[e] == '_' || s[e] == '.'))
                e++;
            MARK(i, e, SYN_KEYWORD);
            i = e;
            state = ST_TAG;
        } else if (s[i] == '&') {                                                     /* &amp; */
            size_t e = i + 1;
            while (e < len && e < i + 10 && (isalnum((unsigned char)s[e]) || s[e] == '#'))
                e++;
            if (e < len && s[e] == ';') {
                MARK(i, e + 1, SYN_NUMBER);
                i = e + 1;
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    return state;
}

static bool escapes_in_backticks(const struct syn_lang *l)                           /* a JavaScript template string has \` ; a Go raw string has no escapes */
{
    return l->name[0] == 'J' || l->name[0] == 'T';
}

int syn_line(const struct syn_lang *l, const char *s, size_t len, int state, uint8_t *cls)
{
    if (cls)
        memset(cls, SYN_PLAIN, len);
    if (!l)
        return ST_NORMAL;
    if (l->kind == K_DIFF)
        return diff_line(s, len, cls);
    if (l->kind == K_MARKDOWN)
        return md_line(s, len, state, cls);
    if (l->kind == K_HTML)
        return html_line(s, len, state, cls);

    const int fl = l->flags;
    size_t i = 0;
    /* a construct that began on an earlier line goes on */
    if (state == ST_BLOCK && l->bc) {
        size_t e = find_str(s, 0, len, l->bc);
        i = e < len ? e + strlen(l->bc) : len;
        MARK(0, i, SYN_COMMENT);
        if (e >= len)
            return ST_BLOCK;
    } else if (state == ST_TRIPLE_S || state == ST_TRIPLE_D) {
        size_t e = find_str(s, 0, len, state == ST_TRIPLE_S ? "'''" : "\"\"\"");
        i = e < len ? e + 3 : len;
        MARK(0, i, SYN_STRING);
        if (e >= len)
            return state;
    } else if (state == ST_BACKTICK) {
        size_t e = find_quote(s, 0, len, '`', escapes_in_backticks(l));
        i = e < len ? e + 1 : len;
        MARK(0, i, SYN_STRING);
        if (e >= len)
            return ST_BACKTICK;
    }

    size_t first = skip_blank(s, i, len);                                             /* the first token of the line */
    if ((fl & SF_SECTION) && first < len && s[first] == '[') {                        /* [section] */
        size_t e = find_quote(s, first + 1, len, ']', false);
        if (e < len) {
            MARK(first, e + 1, SYN_KEYWORD);
            i = e + 1;
        }
    } else if ((fl & SF_KEYEQ) && first < len && s[first] != '#' && s[first] != ';') {    /* key = value */
        size_t e = first;
        while (e < len && s[e] != '=' && s[e] != '#' && s[e] != ';' && s[e] != '"' && s[e] != '\'')
            e++;
        if (e < len && s[e] == '=') {
            size_t k = e;
            while (k > first && (s[k - 1] == ' ' || s[k - 1] == '\t'))
                k--;
            MARK(first, k, SYN_TYPE);
            i = e;
        }
    }
    if ((fl & SF_PREPROC) && first < len && s[first] == '#' && i <= first) {          /* #include <x.h> */
        size_t e = skip_blank(s, first + 1, len), w = e;
        while (w < len && isalpha((unsigned char)s[w]))
            w++;
        MARK(first, w, SYN_PREPROC);
        i = w;
        if (w - e == 7 && !memcmp(s + e, "include", 7)) {
            size_t a = skip_blank(s, w, len);
            if (a < len && s[a] == '<') {
                size_t z = find_quote(s, a + 1, len, '>', false);
                MARK(a, z < len ? z + 1 : len, SYN_STRING);
                i = z < len ? z + 1 : len;
            }
        }
    }

    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        /* comments */
        if (l->bo && starts(s, i, len, l->bo)) {
            size_t e = find_str(s, i + strlen(l->bo), len, l->bc);
            size_t stop = e < len ? e + strlen(l->bc) : len;
            MARK(i, stop, SYN_COMMENT);
            i = stop;
            if (e >= len)
                return ST_BLOCK;
            continue;
        }
        if ((l->lc1 && starts(s, i, len, l->lc1)) || (l->lc2 && starts(s, i, len, l->lc2))) {
            bool hash = s[i] == '#';
            if (!hash || i == 0 || isspace((unsigned char)s[i - 1]) || strchr(";({&|`", s[i - 1])) {     /* (a # inside a word is not a comment: $#, a#b) */
                MARK(i, len, SYN_COMMENT);
                return ST_NORMAL;
            }
        }
        /* strings */
        if (c == '`' && (fl & SF_BACKTICK)) {
            size_t e = find_quote(s, i + 1, len, '`', escapes_in_backticks(l));
            MARK(i, e < len ? e + 1 : len, SYN_STRING);
            if (e >= len)
                return ST_BACKTICK;
            i = e + 1;
            continue;
        }
        if ((c == '"' || c == '\'') && (fl & SF_TRIPLE) && i + 2 < len && s[i + 1] == (char)c && s[i + 2] == (char)c) {
            const char q[4] = { (char)c, (char)c, (char)c, 0 };
            size_t e = find_str(s, i + 3, len, q);
            MARK(i, e < len ? e + 3 : len, SYN_STRING);
            if (e >= len)
                return c == '\'' ? ST_TRIPLE_S : ST_TRIPLE_D;
            i = e + 3;
            continue;
        }
        if (c == '\'' && (fl & SF_RUSTCHAR)) {                                        /* 'x' and '\n' are characters, 'a is a lifetime */
            size_t e = 0;
            if (i + 1 < len && s[i + 1] == '\\')
                e = find_quote(s, i + 2, len, '\'', false);
            else if (i + 2 < len && s[i + 2] == '\'')
                e = i + 2;
            if (e && e < len) {
                MARK(i, e + 1, SYN_STRING);
                i = e + 1;
            } else {
                MARK(i, i + 1, SYN_TYPE);
                i++;
            }
            continue;
        }
        if (c && l->quotes[0] && strchr(l->quotes, c)) {                              /* a string on one line (` too: a shell command, an SQL name) */
            size_t e = find_quote(s, i + 1, len, (char)c, true);
            size_t stop = e < len ? e + 1 : len;
            int cl = SYN_STRING;
            if (fl & SF_KEYCOLON) {                                                   /* "key": */
                size_t k = skip_blank(s, stop, len);
                if (k < len && s[k] == ':')
                    cl = SYN_TYPE;
            }
            MARK(i, stop, cl);
            i = stop;
            continue;
        }
        /* shell and make variables */
        if (c == '$' && (fl & SF_DOLLAR)) {
            size_t e = i + 1;
            if (e < len && (s[e] == '{' || s[e] == '(')) {
                size_t z = find_quote(s, e + 1, len, s[e] == '{' ? '}' : ')', false);
                e = z < len ? z + 1 : len;
            } else if (e < len && isalpha((unsigned char)s[e])) {
                while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_'))
                    e++;
            } else if (e < len) {
                e++;                                                                  /* $? $@ $1 */
            }
            MARK(i, e, SYN_TYPE);
            i = e;
            continue;
        }
        /* @decorator, @media */
        if (c == '@' && (fl & SF_AT) && i + 1 < len && isalpha((unsigned char)s[i + 1])) {
            size_t e = i + 1;
            while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '.' || s[e] == '-'))
                e++;
            MARK(i, e, SYN_PREPROC);
            i = e;
            continue;
        }
        /* numbers */
        bool prev_id = i > 0 && idchar((unsigned char)s[i - 1], fl);
        if (!prev_id && (isdigit(c) || (c == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1])) ||
                         (c == '#' && (fl & SF_HEX) && i + 1 < len && isxdigit((unsigned char)s[i + 1])))) {
            size_t e = i + 1;
            while (e < len && (isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '.' || s[e] == '%'))
                e++;
            MARK(i, e, SYN_NUMBER);
            i = e;
            continue;
        }
        /* words */
        if (idstart(c, fl) && c != '$') {
            size_t e = i + 1;
            while (e < len && idchar((unsigned char)s[e], fl))
                e++;
            bool ic = (fl & SF_ICASE) != 0;
            int cl = SYN_PLAIN;
            if (has_word(l->kw, s + i, e - i, ic))
                cl = SYN_KEYWORD;
            else if (has_word(l->types, s + i, e - i, ic))
                cl = SYN_TYPE;
            else if (has_word(l->consts, s + i, e - i, ic))
                cl = SYN_NUMBER;
            else {
                size_t k = skip_blank(s, e, len);
                if (e < len && s[e] == '(' && !(fl & SF_KEYCOLON))
                    cl = SYN_FUNC;
                else if ((fl & SF_KEYCOLON) && k < len && s[k] == ':' && !(k + 1 < len && s[k + 1] == ':'))
                    cl = SYN_TYPE;
            }
            MARK(i, e, cl);
            i = e;
            continue;
        }
        i++;
    }
    return ST_NORMAL;
}
