/* mdview - Markdown shown as a page (see mdview.h). */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "img.h"
#include "mdview.h"
#include "theme.h"
#include "uikit.h"
#include "wlwin.h"

#define MAXCOLS 12
#define MAX_DEPTH 8
#define PAD_X 8
#define LIST_STEP 24

enum { B_PARA, B_HEAD, B_ITEM, B_CODE, B_RULE, B_TABLE };
enum { IT_TEXT, IT_RECT, IT_RRECT, IT_IMAGE };
enum { F_BOLD = 1, F_ITAL = 2, F_CODE = 4, F_STRIKE = 8, F_LINK = 16 };
enum { T_WORD, T_SPACE, T_BRK, T_IMG };

struct block {
    int kind, level, quote, depth;
    bool inlist;                        /* a paragraph that continues a list item */
    char *text;                         /* the inline source (paragraph, heading, item) or the lines of code */
    char marker[16];                    /* an ordered item's number: "3." */
    bool bullet;
    int task;                           /* 0 no check box, 1 open, 2 done */
    int ncols, nrows;                   /* a table */
    char **cells;
    int align[MAXCOLS];                 /* 0 left, 1 centre, 2 right */
};

struct item {
    int type, x, y, w, h, scale, fl, link, off;
    uint32_t rgb;
};

struct imgc {
    char *path;
    struct lm_image im;
    bool ok;
};

struct md_doc {
    struct block *b;
    int nb, capb;
    struct item *it;
    int ni, capi;
    char *pool;
    size_t npool, cappool;
    char **urls;
    int nurl, capurl;
    struct imgc *img;
    int nimg;
    int width, height;
    char base[512];
    int sel_ai, sel_ao, sel_ci, sel_co;                                 /* the selection: anchor and caret as (text item, byte offset); item -1 = none */
};

struct str {
    char *s;
    size_t n, cap;
};

static void sapp(struct str *s, const char *t, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->n + n + 1) * 2 + 32;
        s->s = realloc(s->s, s->cap);
    }
    memcpy(s->s + s->n, t, n);
    s->n += n;
    s->s[s->n] = 0;
}

static char *sdup(const char *t, size_t n)
{
    char *r = malloc(n + 1);
    memcpy(r, t, n);
    r[n] = 0;
    return r;
}

/* ---- parsing -------------------------------------------------------------------------------------------------------------------- */

static int add_block(struct md_doc *d, int kind, int quote)
{
    if (d->nb == d->capb) {
        d->capb = d->capb ? d->capb * 2 : 32;
        d->b = realloc(d->b, (size_t)d->capb * sizeof(d->b[0]));
    }
    struct block *b = &d->b[d->nb];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->quote = quote;
    return d->nb++;
}

static bool blank_line(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t')
            return false;
    return true;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = 0;
    return s;
}

/* Cells of a table row: "| a | b |" -> a, b (backslash-pipe stays a pipe). Returns the count. */
static int split_row(const char *line, char **out, int max)
{
    char *copy = strdup(line);
    char *p = trim(copy);
    int n = 0;
    if (*p == '|')
        p++;
    char *start = p;
    for (;; p++) {
        if (*p == '\\' && p[1] == '|') {
            memmove(p, p + 1, strlen(p));
            continue;
        }
        if (*p == '|' || *p == 0) {
            bool last = *p == 0;
            char save = *p;
            *p = 0;
            char *c = trim(start);
            if (!(last && !*c))                                         /* (a closing pipe leaves an empty cell behind) */
                if (n < max)
                    out[n++] = strdup(c);
            *p = save;
            if (last)
                break;
            start = p + 1;
        }
    }
    free(copy);
    return n;
}

static bool separator_row(const char *line, int *align, int max, int *ncols)
{
    char *cells[MAXCOLS];
    int n = split_row(line, cells, max);
    bool ok = n > 0;
    for (int i = 0; i < n && ok; i++) {
        const char *c = cells[i];
        size_t len = strlen(c);
        bool left = len && c[0] == ':', right = len && c[len - 1] == ':';
        size_t from = left ? 1 : 0, to = right ? len - 1 : len;
        if (to <= from)
            ok = false;
        for (size_t k = from; k < to && ok; k++)
            if (c[k] != '-')
                ok = false;
        align[i] = left && right ? 1 : right ? 2 : 0;
    }
    for (int i = 0; i < n; i++)
        free(cells[i]);
    *ncols = n;
    return ok && (strchr(line, '|') || strchr(line, ':'));
}

static void table_add_row(struct block *b, const char *line)
{
    char *cells[MAXCOLS];
    int n = split_row(line, cells, MAXCOLS);
    b->cells = realloc(b->cells, (size_t)(b->nrows + 1) * (size_t)b->ncols * sizeof(char *));
    for (int c = 0; c < b->ncols; c++)
        b->cells[b->nrows * b->ncols + c] = c < n ? cells[c] : strdup("");
    for (int c = b->ncols; c < n; c++)
        free(cells[c]);
    b->nrows++;
}

static bool hr_line(const char *t)
{
    char ch = 0;
    int cnt = 0;
    for (; *t; t++) {
        if (*t == ' ' || *t == '\t')
            continue;
        if (*t != '-' && *t != '*' && *t != '_')
            return false;
        if (ch && *t != ch)
            return false;
        ch = *t;
        cnt++;
    }
    return cnt >= 3;
}

static void append_line(struct block *b, const char *t)
{
    size_t n = b->text ? strlen(b->text) : 0;
    bool hard = n >= 2 && b->text[n - 1] == ' ' && b->text[n - 2] == ' ';
    bool bs = n >= 1 && b->text[n - 1] == '\\';
    struct str s = { 0 };
    sapp(&s, b->text ? b->text : "", n);
    if (hard) {
        while (s.n && s.s[s.n - 1] == ' ')
            s.s[--s.n] = 0;
        sapp(&s, "\n", 1);
    } else if (bs) {
        s.s[--s.n] = 0;
        sapp(&s, "\n", 1);
    } else {
        sapp(&s, " ", 1);
    }
    sapp(&s, t, strlen(t));
    free(b->text);
    b->text = s.s;
}

struct md_doc *md_parse(const char *text, size_t n)
{
    struct md_doc *d = calloc(1, sizeof(*d));
    md_sel_clear(d);
    int cur = -1, icode = -1, table = -1;
    char fence = 0;
    int fence_len = 0, fence_q = 0, fence_indent = 0, fence_blk = -1;
    int stack[MAX_DEPTH], nstack = 0;
    bool prev_blank = true;
    size_t pos = 0;
    while (pos < n) {
        size_t e = pos;
        while (e < n && text[e] != '\n')
            e++;
        size_t len = e - pos;
        char *line = sdup(text + pos, len);
        pos = e < n ? e + 1 : n;
        while (len && (line[len - 1] == '\r')) line[--len] = 0;

        /* a block quote's markers */
        const char *p = line;
        int q = 0;
        for (;;) {
            const char *s = p;
            int sp = 0;
            while (*s == ' ' && sp < 3) { s++; sp++; }
            if (*s != '>')
                break;
            q++;
            p = s + 1;
            if (*p == ' ')
                p++;
        }
        const char *rest = q ? p : line;

        if (fence) {
            const char *t = rest;
            int sp = 0;
            while (*t == ' ' && sp < 3) { t++; sp++; }
            int k = 0;
            while (t[k] == fence) k++;
            if (q == fence_q && k >= fence_len && blank_line(t + k)) {
                fence = 0;
                fence_blk = -1;
            } else if (q != fence_q) {
                fence = 0;
                fence_blk = -1;
                goto normal;
            } else {
                int strip = 0;
                while (strip < fence_indent && rest[strip] == ' ') strip++;
                struct block *b = &d->b[fence_blk];
                struct str s = { b->text, b->text ? strlen(b->text) : 0, b->text ? strlen(b->text) + 1 : 0 };
                if (b->level++)                                         /* (level counts the lines of a code block) */
                    sapp(&s, "\n", 1);
                sapp(&s, rest + strip, strlen(rest + strip));
                b->text = s.s;
            }
            free(line);
            continue;
        }
    normal:;
        if (cur >= 0 && d->b[cur].quote != q)
            cur = -1;
        if (blank_line(rest)) {
            cur = -1;
            table = -1;
            if (icode >= 0) {
                struct block *b = &d->b[icode];
                struct str s = { b->text, strlen(b->text), strlen(b->text) + 1 };
                sapp(&s, "\n", 1);
                b->text = s.s;
            }
            prev_blank = true;
            free(line);
            continue;
        }
        int indent = 0;
        const char *t = rest;
        while (*t == ' ' || *t == '\t') {
            indent += *t == '\t' ? 4 : 1;
            t++;
        }
        if (icode >= 0) {
            if (indent >= 4) {
                struct block *b = &d->b[icode];
                struct str s = { b->text, strlen(b->text), strlen(b->text) + 1 };
                const char *c = rest;
                int strip = 0;
                while (strip < 4 && (*c == ' ' || *c == '\t')) { c++; strip += *c == '\t' ? 4 : 1; }
                sapp(&s, c, strlen(c));
                b->text = s.s;
                free(line);
                continue;
            }
            struct block *b = &d->b[icode];                              /* the blank lines at its end go */
            size_t bl = strlen(b->text);
            while (bl && b->text[bl - 1] == '\n') b->text[--bl] = 0;
            icode = -1;
        }
        if (table >= 0) {
            if (strchr(t, '|')) {
                table_add_row(&d->b[table], t);
                prev_blank = false;
                free(line);
                continue;
            }
            table = -1;
        }
        /* a fenced code block */
        if ((t[0] == '`' || t[0] == '~') && indent < 4) {
            int k = 0;
            while (t[k] == t[0]) k++;
            if (k >= 3 && !(t[0] == '`' && strchr(t + k, '`'))) {
                cur = -1;
                fence = t[0];
                fence_len = k;
                fence_q = q;
                fence_indent = indent;
                int bi = add_block(d, B_CODE, q);
                d->b[bi].text = strdup("");
                fence_blk = bi;
                prev_blank = false;
                free(line);
                continue;
            }
        }
        /* an indented code block */
        if (indent >= 4 && cur < 0 && nstack == 0) {
            int bi = add_block(d, B_CODE, q);
            const char *c = rest;
            int strip = 0;
            while (strip < 4 && (*c == ' ' || *c == '\t')) { strip += *c == '\t' ? 4 : 1; c++; }
            d->b[bi].text = strdup(c);
            icode = bi;
            prev_blank = false;
            free(line);
            continue;
        }
        /* an ATX heading */
        if (t[0] == '#' && indent < 4) {
            int h = 0;
            while (t[h] == '#') h++;
            if (h <= 6 && (t[h] == ' ' || t[h] == 0 || t[h] == '\t')) {
                char *body = strdup(t + h);
                char *tb = trim(body);
                size_t bl = strlen(tb);
                while (bl && tb[bl - 1] == '#') tb[--bl] = 0;
                tb = trim(tb);
                int bi = add_block(d, B_HEAD, q);
                d->b[bi].level = h;
                d->b[bi].text = strdup(tb);
                free(body);
                cur = -1;
                nstack = 0;
                prev_blank = false;
                free(line);
                continue;
            }
        }
        /* a setext heading: the paragraph above, underlined */
        if (cur >= 0 && d->b[cur].kind == B_PARA && (t[0] == '=' || t[0] == '-') && indent < 4) {
            char ch = t[0];
            const char *u = t;
            while (*u == ch) u++;
            if (blank_line(u)) {
                d->b[cur].kind = B_HEAD;
                d->b[cur].level = ch == '=' ? 1 : 2;
                cur = -1;
                prev_blank = false;
                free(line);
                continue;
            }
        }
        if (hr_line(t) && indent < 4) {
            add_block(d, B_RULE, q);
            cur = -1;
            nstack = 0;
            prev_blank = false;
            free(line);
            continue;
        }
        /* a table: a line with pipes, then the row of dashes */
        if (cur >= 0 && d->b[cur].kind == B_PARA && !strchr(d->b[cur].text, '\n') && strchr(d->b[cur].text, '|')) {
            int al[MAXCOLS] = { 0 }, nc = 0;
            if (separator_row(t, al, MAXCOLS, &nc)) {
                char *hdr[MAXCOLS];
                int hn = split_row(d->b[cur].text, hdr, MAXCOLS);
                if (hn == nc) {
                    struct block *b = &d->b[cur];
                    char *src = b->text;
                    b->kind = B_TABLE;
                    b->ncols = nc;
                    b->nrows = 0;
                    b->cells = NULL;
                    b->text = NULL;
                    memcpy(b->align, al, sizeof(al));
                    table_add_row(b, src);
                    free(src);
                    table = cur;
                    cur = -1;
                    for (int i = 0; i < hn; i++) free(hdr[i]);
                    prev_blank = false;
                    free(line);
                    continue;
                }
                for (int i = 0; i < hn; i++) free(hdr[i]);
            }
        }
        /* a list item */
        {
            const char *m = t;
            bool ok = false, bullet = false;
            char marker[16] = "";
            if ((m[0] == '-' || m[0] == '*' || m[0] == '+') && (m[1] == ' ' || m[1] == '\t' || m[1] == 0)) {
                ok = true;
                bullet = true;
                m += 1;
            } else if (isdigit((unsigned char)m[0])) {
                int k = 0;
                while (isdigit((unsigned char)m[k]) && k < 9) k++;
                if ((m[k] == '.' || m[k] == ')') && (m[k + 1] == ' ' || m[k + 1] == '\t' || m[k + 1] == 0)) {
                    ok = true;
                    snprintf(marker, sizeof(marker), "%.*s.", k, m);
                    m += k + 1;
                }
            }
            if (ok && indent < 4 + (nstack ? stack[nstack - 1] : 0)) {
                while (*m == ' ' || *m == '\t') m++;
                while (nstack > 0 && indent < stack[nstack - 1]) nstack--;
                if (!(nstack > 0 && indent < stack[nstack - 1] + 2) && nstack < MAX_DEPTH)
                    stack[nstack++] = indent;
                int bi = add_block(d, B_ITEM, q);
                struct block *b = &d->b[bi];
                b->depth = nstack ? nstack - 1 : 0;
                b->bullet = bullet;
                snprintf(b->marker, sizeof(b->marker), "%s", marker);
                if (m[0] == '[' && (m[1] == ' ' || m[1] == 'x' || m[1] == 'X') && m[2] == ']' && (m[3] == ' ' || m[3] == 0)) {
                    b->task = m[1] == ' ' ? 1 : 2;
                    m += m[3] ? 4 : 3;
                }
                b->text = strdup(m);
                cur = bi;
                prev_blank = false;
                free(line);
                continue;
            }
        }
        /* text: it goes on with the paragraph / item above, or starts a paragraph */
        if (cur >= 0) {
            append_line(&d->b[cur], t);
        } else {
            if (indent == 0)
                nstack = 0;
            int bi = add_block(d, B_PARA, q);
            d->b[bi].text = strdup(t);
            d->b[bi].inlist = nstack > 0;
            d->b[bi].depth = nstack;
            cur = bi;
        }
        prev_blank = false;
        free(line);
    }
    (void)prev_blank;
    if (icode >= 0) {
        struct block *b = &d->b[icode];
        size_t bl = strlen(b->text);
        while (bl && b->text[bl - 1] == '\n') b->text[--bl] = 0;
    }
    return d;
}

void md_free(struct md_doc *d)
{
    if (!d)
        return;
    for (int i = 0; i < d->nb; i++) {
        free(d->b[i].text);
        for (int c = 0; c < d->b[i].nrows * d->b[i].ncols && d->b[i].cells; c++)
            free(d->b[i].cells[c]);
        free(d->b[i].cells);
    }
    free(d->b);
    free(d->it);
    free(d->pool);
    for (int i = 0; i < d->nurl; i++) free(d->urls[i]);
    free(d->urls);
    for (int i = 0; i < d->nimg; i++) {
        free(d->img[i].path);
        if (d->img[i].ok) lm_img_free(&d->img[i].im);
    }
    free(d->img);
    free(d);
}

/* ---- inline text -------------------------------------------------------------------------------------------------------------------- */

struct tok {
    int kind;
    unsigned fl;
    int link;                           /* index into urls, -1 none */
    char *s;
    int w;
};

struct toks {
    struct tok *t;
    int n, cap;
};

static struct tok *tpush(struct toks *T, int kind, unsigned fl, int link, const char *s, size_t n)
{
    if (T->n == T->cap) {
        T->cap = T->cap ? T->cap * 2 : 32;
        T->t = realloc(T->t, (size_t)T->cap * sizeof(T->t[0]));
    }
    struct tok *k = &T->t[T->n++];
    k->kind = kind;
    k->fl = fl;
    k->link = link;
    k->s = s ? sdup(s, n) : NULL;
    k->w = 0;
    return k;
}

static void tfree(struct toks *T)
{
    for (int i = 0; i < T->n; i++)
        free(T->t[i].s);
    free(T->t);
    memset(T, 0, sizeof(*T));
}

static int url_add(struct md_doc *d, const char *u, size_t n)
{
    for (int i = 0; i < d->nurl; i++)
        if (strlen(d->urls[i]) == n && !strncmp(d->urls[i], u, n))
            return i;
    if (d->nurl == d->capurl) {
        d->capurl = d->capurl ? d->capurl * 2 : 8;
        d->urls = realloc(d->urls, (size_t)d->capurl * sizeof(char *));
    }
    d->urls[d->nurl] = sdup(u, n);
    return d->nurl++;
}

static bool is_punct(char c)
{
    return c && strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", c);
}

/* The index of the closing delimiter run (exactly r of ch) for an opening one that ended at `from`, or -1. */
static int find_close(const char *s, int n, int from, char ch, int r)
{
    for (int j = from; j < n; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (s[j] == '`') {
            int k = 0;
            while (j + k < n && s[j + k] == '`') k++;
            int m = j + k;
            for (; m < n; m++) {
                int c = 0;
                while (m + c < n && s[m + c] == '`') c++;
                if (c == k) break;
                m += c ? c - 1 : 0;
            }
            if (m < n) j = m + k - 1; else j += k - 1;
            continue;
        }
        if (s[j] == ch) {
            int k = 0;
            while (j + k < n && s[j + k] == ch) k++;
            if (k == r && j > from && s[j - 1] != ' ' && s[j - 1] != '\n' && (ch != '_' || j + k >= n || !isalnum((unsigned char)s[j + k])))
                return j;
            j += k - 1;
        }
    }
    return -1;
}

/* The ']' that closes the '[' at i, or -1. */
static int find_bracket(const char *s, int n, int i)
{
    int depth = 0;
    for (int j = i; j < n; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (s[j] == '`') {
            int k = 0;
            while (j + k < n && s[j + k] == '`') k++;
            const char *c = NULL;
            for (int m = j + k; m < n; m++)
                if (s[m] == '`') { int cnt = 0; while (m + cnt < n && s[m + cnt] == '`') cnt++; if (cnt == k) { c = s + m; m = n; } else m += cnt - 1; }
            if (c) { j = (int)(c - s) + k - 1; continue; }
            j += k - 1;
            continue;
        }
        if (s[j] == '[') depth++;
        else if (s[j] == ']' && --depth == 0) return j;
    }
    return -1;
}

static void scan_inline(struct md_doc *d, struct toks *T, const char *s, int n, unsigned fl, int link);

/* A link or image target "(url "title")" starting at s[i] == '(': the url's range and the index after the ')', or -1. */
static int parse_target(const char *s, int n, int i, int *u0, int *u1)
{
    int depth = 0, j = i;
    for (; j < n; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (s[j] == '(') depth++;
        else if (s[j] == ')' && --depth == 0) break;
    }
    if (j >= n)
        return -1;
    int a = i + 1, b = j;
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    if (a < b && s[a] == '<') {
        int c = a + 1;
        while (c < b && s[c] != '>') c++;
        *u0 = a + 1;
        *u1 = c;
        return j + 1;
    }
    int c = a;
    while (c < b && s[c] != ' ') c++;                                   /* (a title after a blank is dropped) */
    *u0 = a;
    *u1 = c;
    return j + 1;
}

static void flush_word(struct toks *T, char *w, int *wl, unsigned fl, int link)
{
    if (*wl) {
        tpush(T, T_WORD, fl, link, w, (size_t)*wl);
        *wl = 0;
    }
}

static void scan_inline(struct md_doc *d, struct toks *T, const char *s, int n, unsigned fl, int link)
{
    char w[1024];
    int wl = 0;
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (wl > 900)
            flush_word(T, w, &wl, fl, link);
        if (c == '\\' && i + 1 < n && is_punct(s[i + 1])) {
            w[wl++] = s[i + 1];
            i += 2;
            continue;
        }
        if (c == '\n') {
            flush_word(T, w, &wl, fl, link);
            tpush(T, T_BRK, fl, link, NULL, 0);
            i++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            flush_word(T, w, &wl, fl, link);
            if (T->n && T->t[T->n - 1].kind != T_SPACE && T->t[T->n - 1].kind != T_BRK)
                tpush(T, T_SPACE, fl, link, NULL, 0);
            while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
            continue;
        }
        if (c == '`') {
            int k = 0;
            while (i + k < n && s[i + k] == '`') k++;
            int m = i + k, found = -1;
            while (m < n) {
                int cnt = 0;
                while (m + cnt < n && s[m + cnt] == '`') cnt++;
                if (cnt == k) { found = m; break; }
                m += cnt ? cnt : 1;
            }
            if (found >= 0) {
                flush_word(T, w, &wl, fl, link);
                int a = i + k, b = found;
                if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') { a++; b--; }
                char cw[1024];
                int cl = 0;
                for (int j = a; j <= b; j++) {
                    if (j == b || s[j] == ' ' || s[j] == '\n') {
                        if (cl) { tpush(T, T_WORD, fl | F_CODE, link, cw, (size_t)cl); cl = 0; }
                        if (j < b && (T->n && T->t[T->n - 1].kind != T_SPACE))
                            tpush(T, T_SPACE, fl | F_CODE, link, NULL, 0);
                    } else if (cl < 1000) {
                        cw[cl++] = s[j];
                    }
                }
                i = found + k;
                continue;
            }
            for (int j = 0; j < k && wl < 1000; j++) w[wl++] = '`';
            i += k;
            continue;
        }
        if (c == '*' || c == '_' || c == '~') {
            int k = 0;
            while (i + k < n && s[i + k] == c) k++;
            int r = c == '~' ? (k >= 2 ? 2 : 0) : (k > 3 ? 3 : k);
            bool can = r > 0 && i + r < n && s[i + r] != ' ' && s[i + r] != '\n';
            if (c == '_' && i > 0 && isalnum((unsigned char)s[i - 1]))
                can = false;
            int close = can ? find_close(s, n, i + r, (char)c, r) : -1;
            if (close >= 0) {
                flush_word(T, w, &wl, fl, link);
                unsigned add = c == '~' ? F_STRIKE : r == 1 ? F_ITAL : r == 2 ? F_BOLD : (F_BOLD | F_ITAL);
                scan_inline(d, T, s + i + r, close - (i + r), fl | add, link);
                i = close + r;
                continue;
            }
            for (int j = 0; j < k && wl < 1000; j++) w[wl++] = (char)c;
            i += k;
            continue;
        }
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            int cb = find_bracket(s, n, i + 1);
            int u0, u1, after;
            if (cb > 0 && cb + 1 < n && s[cb + 1] == '(' && (after = parse_target(s, n, cb + 1, &u0, &u1)) > 0) {
                flush_word(T, w, &wl, fl, link);
                struct tok *k = tpush(T, T_IMG, fl, url_add(d, s + u0, (size_t)(u1 - u0)), s + i + 2, (size_t)(cb - i - 2));
                (void)k;
                i = after;
                continue;
            }
        }
        if (c == '[') {
            int cb = find_bracket(s, n, i);
            int u0, u1, after;
            if (cb > 0 && cb + 1 < n && s[cb + 1] == '(' && (after = parse_target(s, n, cb + 1, &u0, &u1)) > 0) {
                flush_word(T, w, &wl, fl, link);
                scan_inline(d, T, s + i + 1, cb - i - 1, fl | F_LINK, url_add(d, s + u0, (size_t)(u1 - u0)));
                i = after;
                continue;
            }
        }
        if (c == '<') {
            int j = i + 1;
            while (j < n && s[j] != '>' && s[j] != ' ') j++;
            if (j < n && s[j] == '>' && (!strncmp(s + i + 1, "http://", 7) || !strncmp(s + i + 1, "https://", 8) || !strncmp(s + i + 1, "mailto:", 7))) {
                flush_word(T, w, &wl, fl, link);
                int id = url_add(d, s + i + 1, (size_t)(j - i - 1));
                tpush(T, T_WORD, fl | F_LINK, id, s + i + 1, (size_t)(j - i - 1));
                i = j + 1;
                continue;
            }
        }
        if (c == 'h' && (i == 0 || !isalnum((unsigned char)s[i - 1])) && (!strncmp(s + i, "http://", 7) || !strncmp(s + i, "https://", 8))) {
            int j = i;
            while (j < n && s[j] != ' ' && s[j] != '\t' && s[j] != '\n' && s[j] != '<' && s[j] != '>') j++;
            while (j > i && strchr(".,;:!?)'\"]", s[j - 1])) j--;
            flush_word(T, w, &wl, fl, link);
            int id = url_add(d, s + i, (size_t)(j - i));
            tpush(T, T_WORD, fl | F_LINK, id, s + i, (size_t)(j - i));
            i = j;
            continue;
        }
        w[wl++] = (char)c;
        i++;
    }
    flush_word(T, w, &wl, fl, link);
}

/* ---- layout ---------------------------------------------------------------------------------------------------------------------- */

static void need_item(struct md_doc *d)
{
    if (d->ni == d->capi) {
        d->capi = d->capi ? d->capi * 2 : 256;
        d->it = realloc(d->it, (size_t)d->capi * sizeof(d->it[0]));
    }
}

static struct item *add_item(struct md_doc *d, int type, int x, int y, int w, int h, uint32_t rgb)
{
    need_item(d);
    struct item *it = &d->it[d->ni++];
    memset(it, 0, sizeof(*it));
    it->type = type;
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
    it->rgb = rgb;
    it->link = -1;
    it->scale = 1;
    return it;
}

static void add_text(struct md_doc *d, int x, int y, int w, const char *s, int scale, unsigned fl, int link, uint32_t rgb)
{
    size_t n = strlen(s);
    if (d->npool + n + 1 > d->cappool) {
        d->cappool = (d->npool + n + 1) * 2 + 1024;
        d->pool = realloc(d->pool, d->cappool);
    }
    struct item *it = add_item(d, IT_TEXT, x, y, w, scale * LM_FONT_H, rgb);
    it->off = (int)d->npool;
    memcpy(d->pool + d->npool, s, n + 1);
    d->npool += n + 1;
    it->scale = scale;
    it->fl = (int)fl;
    it->link = link;
}

static int word_w(const struct tok *k, int scale)
{
    if (k->kind == T_SPACE)
        return (k->fl & F_CODE ? lm_font_cells_width(" ") : lm_font_text_width(" ")) * scale;
    if (k->fl & F_CODE)
        return lm_font_cells_width(k->s) * scale;
    if (k->fl & F_BOLD)
        return lm_font_text_width_bold(k->s) * scale;
    return lm_font_text_width(k->s) * scale;
}

static struct imgc *img_find(struct md_doc *d, const char *url)
{
    if (!d->base[0] || strstr(url, "://") || !*url)
        return NULL;
    char path[1024];
    if (url[0] == '/')
        snprintf(path, sizeof(path), "%s", url);
    else
        snprintf(path, sizeof(path), "%s/%s", d->base, url);
    for (int i = 0; i < d->nimg; i++)
        if (!strcmp(d->img[i].path, path))
            return d->img[i].ok ? &d->img[i] : NULL;
    d->img = realloc(d->img, (size_t)(d->nimg + 1) * sizeof(d->img[0]));
    struct imgc *c = &d->img[d->nimg++];
    memset(c, 0, sizeof(*c));
    c->path = strdup(path);
    char err[80];
    c->ok = lm_img_load(path, &c->im, err, sizeof(err)) == 0 && c->im.w > 0 && c->im.h > 0 && c->im.w <= 8192 && c->im.h <= 8192;
    return c->ok ? c : NULL;
}

struct placed {
    int tok, x, w, deco_from;           /* deco_from: how far left a code background / underline reaches over the space before it */
};

/* Sets the tokens in a box of width w at (x, y); returns the height. emit = false only measures (and `used` gets the widest line). */
static int flow(struct md_doc *d, struct toks *T, int x, int y, int w, int scale, unsigned base_fl, uint32_t color, int align, bool emit, int *used)
{
    int lh = scale * LM_FONT_H + (scale > 1 ? 8 : 6);
    int total = 0, widest = 0;
    struct placed *line = NULL;
    int ln = 0, lcap = 0, cx = 0;
    bool any = false, sp = false;                                       /* sp: a blank came before the next word (else it touches the last one) */
    int sp_w = lm_font_text_width(" ") * scale;
    /* pictures that cannot be loaded become words */
    for (int i = 0; i < T->n; i++)
        if (T->t[i].kind == T_IMG && !(emit && T->t[i].link >= 0 && img_find(d, d->urls[T->t[i].link]))) {
            char buf[300];
            snprintf(buf, sizeof(buf), "[image: %.200s]", T->t[i].s && *T->t[i].s ? T->t[i].s : d->urls[T->t[i].link]);
            free(T->t[i].s);
            T->t[i].s = strdup(buf);
            T->t[i].kind = T_WORD;
            T->t[i].fl |= F_LINK | F_ITAL;
        }
    for (int i = 0; i <= T->n; i++) {
        const struct tok *k = i < T->n ? &T->t[i] : NULL;
        bool flush = !k || k->kind == T_BRK || k->kind == T_IMG;
        if (k && k->kind == T_WORD) {
            int ww = word_w(k, scale);
            int gap = cx > 0 && sp ? sp_w : 0;
            int add = gap + ww;
            if (cx > 0 && cx + add > w) {
                flush = true;
                i--;                                                    /* (this word starts the next line) */
                k = NULL;
            } else {
                if (ln == lcap) { lcap = lcap ? lcap * 2 : 16; line = realloc(line, (size_t)lcap * sizeof(line[0])); }
                line[ln].tok = i;
                line[ln].x = cx + gap;
                line[ln].w = ww;
                line[ln].deco_from = gap;
                ln++;
                cx += add;
                any = true;
                sp = false;
                continue;
            }
        } else if (k && k->kind == T_SPACE) {
            if (cx > 0)
                sp = true;
            continue;
        }
        if (flush) {
            if (ln || (k && k->kind == T_BRK)) {
                int off = align == 1 ? (w - cx) / 2 : align == 2 ? w - cx : 0;
                if (off < 0) off = 0;
                if (emit)
                    for (int j = 0; j < ln; j++) {
                        const struct tok *tk = &T->t[line[j].tok];
                        unsigned fl = tk->fl | base_fl;
                        int tx = x + off + line[j].x, ty = y + total;
                        uint32_t col = color;
                        bool prev_same_code = j > 0 && (T->t[line[j - 1].tok].fl & F_CODE) && (fl & F_CODE);
                        bool prev_same_link = j > 0 && tk->link >= 0 && T->t[line[j - 1].tok].link == tk->link;
                        if (fl & F_CODE) {
                            int from = prev_same_code ? line[j].deco_from : 0;
                            add_item(d, IT_RRECT, tx - 2 - from, ty + 1, line[j].w + 4 + from, lh - 2, lm_theme.gutter)->off = 4;
                            col = lm_mix_rgb(color, lm_theme.accent, 0.25);
                        }
                        if (fl & F_ITAL)
                            col = lm_mix_rgb(col, lm_theme.accent, 0.4);
                        if (fl & F_LINK)
                            col = lm_theme.accent;
                        add_text(d, tx, ty + (lh - scale * LM_FONT_H) / 2, line[j].w, tk->s, scale, fl, tk->link, col);
                        if (fl & F_LINK) {
                            int from = prev_same_link ? line[j].deco_from : 0;
                            add_item(d, IT_RECT, tx - from, ty + lh - 3, line[j].w + from, 1, col);
                        }
                        if (fl & F_STRIKE) {
                            int from = j > 0 && (T->t[line[j - 1].tok].fl & F_STRIKE) ? line[j].deco_from : 0;
                            add_item(d, IT_RECT, tx - from, ty + lh / 2, line[j].w + from, 1, col);
                        }
                    }
                if (cx > widest) widest = cx;
                total += lh;
                ln = 0;
                cx = 0;
                sp = false;
            }
        }
        if (k && k->kind == T_IMG && emit) {
            struct imgc *c = img_find(d, d->urls[k->link]);
            if (c) {
                int iw = c->im.w, ih = c->im.h;
                if (iw > w) { ih = (int)((long long)ih * w / iw); iw = w; }
                if (ih < 1) ih = 1;
                struct item *it = add_item(d, IT_IMAGE, x, y + total + 2, iw, ih, 0);
                it->off = (int)(c - d->img);
                it->link = -1;
                total += ih + 8;
                any = true;
                if (iw > widest) widest = iw;
            }
        }
    }
    free(line);
    if (used)
        *used = widest;
    return any ? total : 0;
}

void md_layout(struct md_doc *d, int width, const char *base_dir)
{
    bool was_cells = lm_font_ui_cells_get();
    lm_font_ui_cells(false);                                            /* the page is set in labels */
    d->ni = 0;
    d->npool = 0;
    md_sel_clear(d);                                                    /* (the items are made again: the selection cannot be kept) */
    for (int i = 0; i < d->nurl; i++) free(d->urls[i]);
    d->nurl = 0;
    snprintf(d->base, sizeof(d->base), "%s", base_dir ? base_dir : "");
    if (width < 120)
        width = 120;
    d->width = width;
    int y = 8;
    for (int bi = 0; bi < d->nb; bi++) {
        struct block *b = &d->b[bi];
        int y0 = y;
        int qx = b->quote * 16;
        int x = qx + (b->inlist ? b->depth * LIST_STEP : 0);
        int w = width - x;
        uint32_t body = lm_theme.text_soft;
        if (b->quote)
            body = lm_mix_rgb(lm_theme.text_soft, lm_theme.text_dim, 0.35);
        struct toks T = { 0 };
        switch (b->kind) {
        case B_PARA:
            scan_inline(d, &T, b->text, (int)strlen(b->text), 0, -1);
            y += flow(d, &T, x, y, w, 1, 0, body, 0, true, NULL) + 10;
            break;
        case B_HEAD: {
            int sc = b->level == 1 ? 2 : 1;
            if (bi > 0)
                y += b->level <= 2 ? 10 : 4;
            scan_inline(d, &T, b->text, (int)strlen(b->text), F_BOLD, -1);
            y += flow(d, &T, x, y, w, sc, F_BOLD, b->level <= 2 ? lm_theme.text_bright : lm_theme.text_soft, 0, true, NULL);
            if (b->level <= 2) {
                add_item(d, IT_RECT, x, y + 1, w, 1, lm_theme.divider);
                y += 6;
            }
            y += 8;
            break;
        }
        case B_ITEM: {
            int ix = qx + b->depth * LIST_STEP;
            int lh = LM_FONT_H + 6;
            if (b->task) {
                int bx = ix + 2, by = y + (lh - 12) / 2;
                add_item(d, IT_RECT, bx, by, 12, 1, lm_theme.text_dim);
                add_item(d, IT_RECT, bx, by + 11, 12, 1, lm_theme.text_dim);
                add_item(d, IT_RECT, bx, by, 1, 12, lm_theme.text_dim);
                add_item(d, IT_RECT, bx + 11, by, 1, 12, lm_theme.text_dim);
                if (b->task == 2)
                    add_item(d, IT_RECT, bx + 3, by + 3, 6, 6, lm_theme.accent);
            } else if (b->bullet) {
                int sz = b->depth == 0 ? 6 : 5;
                struct item *it = add_item(d, b->depth >= 2 ? IT_RECT : IT_RRECT, ix + 7, y + (lh - sz) / 2, sz, sz, body);
                it->off = sz / 2;
            } else {
                int mw = lm_font_text_width(b->marker);
                add_text(d, ix + LIST_STEP - 6 - mw, y + 3, mw, b->marker, 1, 0, -1, lm_theme.text_dim);
            }
            scan_inline(d, &T, b->text, (int)strlen(b->text), 0, -1);
            int h = flow(d, &T, ix + LIST_STEP, y, width - ix - LIST_STEP, 1, 0, body, 0, true, NULL);
            y += (h ? h : lh) + 2;
            if (bi + 1 >= d->nb || d->b[bi + 1].kind != B_ITEM)
                y += 8;
            break;
        }
        case B_CODE: {
            int nl = 1;
            for (const char *c = b->text; *c; c++) if (*c == '\n') nl++;
            int lh = LM_FONT_H + 2, h = nl * lh + 12;
            add_item(d, IT_RRECT, x, y, w, h, lm_theme.gutter)->off = 6;
            const char *c = b->text;
            for (int i = 0; i < nl; i++) {
                const char *e = strchr(c, '\n');
                size_t len = e ? (size_t)(e - c) : strlen(c);
                char *ln = sdup(c, len);
                if (len)
                    add_text(d, x + 10, y + 6 + i * lh, w - 20, ln, 1, F_CODE, -1, lm_theme.text_soft);
                free(ln);
                c = e ? e + 1 : c + len;
            }
            y += h + 10;
            break;
        }
        case B_RULE:
            add_item(d, IT_RECT, x, y + 8, w, 1, lm_theme.divider);
            y += 18;
            break;
        case B_TABLE: {
            int nc = b->ncols, nr = b->nrows;
            int colw[MAXCOLS];
            long sum = 0;
            for (int c = 0; c < nc; c++) {
                int mx = 24;
                for (int r = 0; r < nr; r++) {
                    struct toks CT = { 0 };
                    const char *cs = b->cells[r * nc + c];
                    scan_inline(d, &CT, cs, (int)strlen(cs), r == 0 ? F_BOLD : 0, -1);
                    int used = 0;
                    flow(d, &CT, 0, 0, 100000, 1, r == 0 ? F_BOLD : 0, body, 0, false, &used);
                    tfree(&CT);
                    if (used > mx) mx = used;
                }
                colw[c] = mx + 2 * PAD_X;
                sum += colw[c];
            }
            if (sum > w)
                for (int c = 0; c < nc; c++) {
                    colw[c] = (int)(colw[c] * (long long)w / sum);
                    if (colw[c] < 48) colw[c] = 48;
                }
            int tw = 0;
            for (int c = 0; c < nc; c++) tw += colw[c];
            int ry = y;
            int rowh[256] = { 0 };
            for (int r = 0; r < nr && r < 256; r++) {
                int hh = LM_FONT_H + 6;
                for (int c = 0; c < nc; c++) {
                    struct toks CT = { 0 };
                    const char *cs = b->cells[r * nc + c];
                    scan_inline(d, &CT, cs, (int)strlen(cs), r == 0 ? F_BOLD : 0, -1);
                    int ch = flow(d, &CT, 0, 0, colw[c] - 2 * PAD_X, 1, r == 0 ? F_BOLD : 0, body, 0, false, NULL);
                    tfree(&CT);
                    if (ch > hh) hh = ch;
                }
                rowh[r] = hh + 8;
            }
            int th = 0;
            for (int r = 0; r < nr && r < 256; r++) th += rowh[r];
            add_item(d, IT_RECT, x, ry, tw, rowh[0], lm_theme.gutter);
            int cy = ry;
            for (int r = 0; r < nr && r < 256; r++) {
                int cx = x;
                for (int c = 0; c < nc; c++) {
                    struct toks CT = { 0 };
                    const char *cs = b->cells[r * nc + c];
                    scan_inline(d, &CT, cs, (int)strlen(cs), r == 0 ? F_BOLD : 0, -1);
                    flow(d, &CT, cx + PAD_X, cy + 4, colw[c] - 2 * PAD_X, 1, r == 0 ? F_BOLD : 0, r == 0 ? lm_theme.text_bright : body, b->align[c], true, NULL);
                    tfree(&CT);
                    cx += colw[c];
                }
                add_item(d, IT_RECT, x, cy, tw, 1, lm_theme.divider);
                cy += rowh[r];
            }
            add_item(d, IT_RECT, x, cy, tw, 1, lm_theme.divider);
            int cx = x;
            for (int c = 0; c <= nc; c++) {
                add_item(d, IT_RECT, cx, ry, 1, th, lm_theme.divider);
                if (c < nc) cx += colw[c];
            }
            y += th + 14;
            break;
        }
        }
        tfree(&T);
        for (int k = 0; k < b->quote; k++)                              /* the bars of a block quote, joined from block to block */
            add_item(d, IT_RECT, k * 16 + 2, y0 - 2, 3, y - y0 + (bi + 1 < d->nb && d->b[bi + 1].quote > k ? 2 : -6), lm_mix_rgb(lm_theme.accent, lm_theme.background, 0.4));
    }
    d->height = y + 12;
    lm_font_ui_cells(was_cells);
}

int md_height(const struct md_doc *d) { return d->height; }
int md_width(const struct md_doc *d) { return d->width; }

/* ---- painting and asking ---------------------------------------------------------------------------------------------------------- */

/* ---- selection ------------------------------------------------------------------------------------------------------------------ */

/* Width of the first n bytes of a text item (the page is set in labels, code in cells; the caller has the label mode on). */
static int prefix_w(const struct item *it, const char *s, size_t n)
{
    char buf[1024];
    char *t = n < sizeof(buf) ? buf : malloc(n + 1);
    memcpy(t, s, n);
    t[n] = 0;
    int w = (it->fl & F_CODE ? lm_font_cells_width(t) : it->fl & F_BOLD ? lm_font_text_width_bold(t) : lm_font_text_width(t)) * it->scale;
    if (t != buf)
        free(t);
    return w;
}

static size_t next_cp(const char *s, size_t o)
{
    if (s[o])
        do
            o++;
        while (((unsigned char)s[o] & 0xc0) == 0x80);
    return o;
}

/* The byte offset in a text item that is nearest to the pixel `rel` right of its left edge. */
static size_t off_at(const struct item *it, const char *s, int rel)
{
    size_t len = strlen(s), o = 0;
    int prev = 0;
    if (rel <= 0)
        return 0;
    while (o < len) {
        size_t n = next_cp(s, o);
        int w = prefix_w(it, s, n);
        if (rel < (prev + w + 1) / 2)
            return o;
        prev = w;
        o = n;
    }
    return len;
}

/* The text item nearest to a page position (the one the pointer is on, else the closest line, then the closest in its line). */
static int item_near(const struct md_doc *d, int px, int py)
{
    int best = -1;
    long bk = 0;
    for (int i = 0; i < d->ni; i++) {
        const struct item *it = &d->it[i];
        if (it->type != IT_TEXT)
            continue;
        int dy = py < it->y - 3 ? it->y - 3 - py : py >= it->y + it->h + 3 ? py - (it->y + it->h + 3) + 1 : 0;
        int dx = px < it->x ? it->x - px : px >= it->x + it->w ? px - (it->x + it->w) + 1 : 0;
        long k = (long)dy * 100000 + dx;
        if (best < 0 || k < bk) {
            best = i;
            bk = k;
        }
    }
    return best;
}

static bool pos_at(const struct md_doc *d, int px, int py, int *item, int *off)
{
    bool was_cells = lm_font_ui_cells_get();
    lm_font_ui_cells(false);
    int i = item_near(d, px, py);
    if (i >= 0) {
        *item = i;
        *off = (int)off_at(&d->it[i], d->pool + d->it[i].off, px - d->it[i].x);
    }
    lm_font_ui_cells(was_cells);
    return i >= 0;
}

void md_sel_clear(struct md_doc *d)
{
    d->sel_ai = d->sel_ci = -1;
    d->sel_ao = d->sel_co = 0;
}

void md_sel_begin(struct md_doc *d, int px, int py)
{
    int i, o;
    if (!pos_at(d, px, py, &i, &o)) {
        md_sel_clear(d);
        return;
    }
    d->sel_ai = d->sel_ci = i;
    d->sel_ao = d->sel_co = o;
}

void md_sel_drag(struct md_doc *d, int px, int py)
{
    int i, o;
    if (d->sel_ai >= 0 && pos_at(d, px, py, &i, &o)) {
        d->sel_ci = i;
        d->sel_co = o;
    }
}

/* A double click: the word (letters, digits, '_', anything non-ASCII) at the position. */
void md_sel_word(struct md_doc *d, int px, int py)
{
    int i, o;
    if (!pos_at(d, px, py, &i, &o)) {
        md_sel_clear(d);
        return;
    }
    const char *s = d->pool + d->it[i].off;
    size_t len = strlen(s), a = (size_t)o, b = (size_t)o;
#define WORDC(c) (isalnum((unsigned char)(c)) || (c) == '_' || ((unsigned char)(c)) >= 0x80)
    while (a > 0 && WORDC(s[a - 1]))
        a--;
    while (b < len && WORDC(s[b]))
        b++;
    if (a == b && b < len)                                              /* on a mark: that character */
        b = next_cp(s, b);
#undef WORDC
    d->sel_ai = d->sel_ci = i;
    d->sel_ao = (int)a;
    d->sel_co = (int)b;
}

/* A triple click: the whole line (every text item that sits in the same line). */
void md_sel_line(struct md_doc *d, int px, int py)
{
    int i, o;
    if (!pos_at(d, px, py, &i, &o)) {
        md_sel_clear(d);
        return;
    }
    int y = d->it[i].y, a = i, b = i;
    for (int k = 0; k < d->ni; k++)
        if (d->it[k].type == IT_TEXT && d->it[k].y == y) {
            if (k < a) a = k;
            if (k > b) b = k;
        }
    d->sel_ai = a;
    d->sel_ao = 0;
    d->sel_ci = b;
    d->sel_co = (int)strlen(d->pool + d->it[b].off);
}

void md_sel_all(struct md_doc *d)
{
    int a = -1, b = -1;
    for (int i = 0; i < d->ni; i++)
        if (d->it[i].type == IT_TEXT) {
            if (a < 0)
                a = i;
            b = i;
        }
    if (a < 0) {
        md_sel_clear(d);
        return;
    }
    d->sel_ai = a;
    d->sel_ao = 0;
    d->sel_ci = b;
    d->sel_co = (int)strlen(d->pool + d->it[b].off);
}

/* The selection in order: lo = (item, offset) first, hi = the end. False when nothing (or an empty range) is selected. */
static bool sel_range(const struct md_doc *d, int *li, int *lo, int *hi, int *ho)
{
    if (d->sel_ai < 0 || d->sel_ci < 0)
        return false;
    bool fwd = d->sel_ai < d->sel_ci || (d->sel_ai == d->sel_ci && d->sel_ao <= d->sel_co);
    *li = fwd ? d->sel_ai : d->sel_ci;
    *lo = fwd ? d->sel_ao : d->sel_co;
    *hi = fwd ? d->sel_ci : d->sel_ai;
    *ho = fwd ? d->sel_co : d->sel_ao;
    return !(*li == *hi && *lo == *ho);
}

bool md_sel_active(const struct md_doc *d)
{
    int a, b, c, e;
    return sel_range(d, &a, &b, &c, &e);
}

/* The selected text: words in a line are separated by a blank (a wide gap, as between table cells, by a tab), lines by a line break and a gap
 * between blocks by an empty line. */
char *md_sel_text(const struct md_doc *d)
{
    int li, lo, hi, ho;
    if (!sel_range(d, &li, &lo, &hi, &ho))
        return NULL;
    struct str out = { 0 };
    const struct item *prev = NULL;
    for (int i = li; i <= hi; i++) {
        const struct item *it = &d->it[i];
        if (it->type != IT_TEXT)
            continue;
        const char *s = d->pool + it->off;
        size_t len = strlen(s), a = i == li ? (size_t)lo : 0, b = i == hi ? (size_t)ho : len;
        if (prev) {
            int dy = it->y - prev->y;
            if (dy >= prev->h) {
                sapp(&out, "\n", 1);
                if (dy > prev->h + 12)
                    sapp(&out, "\n", 1);
            } else if (dy < 0 || it->x - (prev->x + prev->w) >= 14) {
                sapp(&out, "\t", 1);
            } else {
                sapp(&out, " ", 1);
            }
        }
        if (b > a)
            sapp(&out, s + a, b - a);
        prev = it;
    }
    if (!out.s)
        out.s = strdup("");
    return out.s;
}

void md_paint(struct wlwin *w, const struct md_doc *d, int x, int y, int w_box, int h_box, int scroll)
{
    (void)w_box;
    bool was_cells = lm_font_ui_cells_get();
    lm_font_ui_cells(false);
    int sli, slo, shi, sho;
    bool has_sel = sel_range(d, &sli, &slo, &shi, &sho);
    for (int i = 0; i < d->ni; i++) {
        const struct item *it = &d->it[i];
        if (it->y + it->h < scroll || it->y > scroll + h_box)
            continue;
        int px = x + it->x, py = y + it->y - scroll;
        if (has_sel && it->type == IT_TEXT && i >= sli && i <= shi) {
            const char *s = d->pool + it->off;
            size_t len = strlen(s), a = i == sli ? (size_t)slo : 0, b = i == shi ? (size_t)sho : len;
            int x1 = a ? prefix_w(it, s, a) : 0, x2 = b >= len ? it->w : prefix_w(it, s, b);
            if (b >= len && i < shi)                                    /* the blank up to the next word of the line is selected too */
                for (int j = i + 1; j < d->ni; j++)
                    if (d->it[j].type == IT_TEXT) {
                        if (d->it[j].y == it->y && d->it[j].x > it->x + it->w && d->it[j].x - it->x - it->w < 24)
                            x2 = d->it[j].x - it->x;
                        break;
                    }
            if (x2 > x1)
                wlwin_fill(w, px + x1, py - 2, x2 - x1, it->h + 4, lm_theme.button_hover);
        }
        switch (it->type) {
        case IT_RECT:
            wlwin_fill(w, px, py, it->w, it->h, it->rgb);
            break;
        case IT_RRECT:
            wlwin_round_fill(w, px, py, it->w, it->h, it->off ? it->off : 4, it->rgb);
            break;
        case IT_IMAGE: {
            const struct imgc *c = &d->img[it->off];
            wlwin_blit(w, px, py, it->w, it->h, c->im.px, c->im.w, c->im.h, c->im.w);
            break;
        }
        case IT_TEXT: {
            const char *s = d->pool + it->off;
            if (it->fl & F_CODE)
                wlwin_text_cells(w, px, py, s, it->scale, it->rgb, it->w + 2);
            else if (it->fl & F_BOLD)
                wlwin_text_bold(w, px, py, s, it->scale, it->rgb, 0);
            else
                wlwin_text(w, px, py, s, it->scale, it->rgb, 0);
            break;
        }
        }
    }
    lm_font_ui_cells(was_cells);
}

const char *md_link_at(const struct md_doc *d, int px, int py)
{
    for (int i = 0; i < d->ni; i++) {
        const struct item *it = &d->it[i];
        if (it->type == IT_TEXT && it->link >= 0 && px >= it->x && px < it->x + it->w && py >= it->y && py < it->y + it->h + 3)
            return d->urls[it->link];
    }
    return NULL;
}

int md_count(const struct md_doc *d, enum md_stat what)
{
    int n = 0;
    switch (what) {
    case MD_N_BLOCKS: return d->nb;
    case MD_N_HEADINGS: for (int i = 0; i < d->nb; i++) n += d->b[i].kind == B_HEAD; return n;
    case MD_N_LINKS: return d->nurl;
    case MD_N_ITEMS: return d->ni;
    case MD_N_CODE: for (int i = 0; i < d->nb; i++) n += d->b[i].kind == B_CODE; return n;
    case MD_N_TABLES: for (int i = 0; i < d->nb; i++) n += d->b[i].kind == B_TABLE; return n;
    case MD_N_IMAGES: for (int i = 0; i < d->ni; i++) n += d->it[i].type == IT_IMAGE; return n;
    }
    return 0;
}

void md_dump(const struct md_doc *d, FILE *f)
{
    for (int i = 0; i < d->ni; i++) {
        const struct item *it = &d->it[i];
        if (it->type == IT_TEXT)
            fprintf(f, "text %d %d %d fl=%d scale=%d link=%s \"%s\"\n", it->x, it->y, it->w, it->fl, it->scale, it->link >= 0 ? d->urls[it->link] : "-", d->pool + it->off);
        else if (it->type == IT_IMAGE)
            fprintf(f, "image %d %d %d %d\n", it->x, it->y, it->w, it->h);
        else
            fprintf(f, "%s %d %d %d %d\n", it->type == IT_RECT ? "rect" : "round", it->x, it->y, it->w, it->h);
    }
    fprintf(f, "ok height=%d width=%d blocks=%d headings=%d links=%d\n", d->height, d->width, d->nb, md_count(d, MD_N_HEADINGS), d->nurl);
}
