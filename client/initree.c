#include "initree.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_NODES 60000

static struct json *mk(enum json_type t, const char *key, size_t klen)
{
    struct json *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->type = t;
    if (key)
        j->key = strndup(key, klen);
    return j;
}

static void trim(const char **s, const char **e)
{
    while (*s < *e && isspace((unsigned char)**s)) (*s)++;
    while (*e > *s && isspace((unsigned char)(*e)[-1])) (*e)--;
}

struct json *ini_parse(const char *text)
{
    struct json *root = mk(JSON_OBJ, NULL, 0), *cur = root, *rtail = NULL, *ctail = NULL;
    if (!root)
        return NULL;
    int nodes = 0;
    const char *p = text;
    if (!strncmp(p, "\xef\xbb\xbf", 3)) p += 3;
    while (*p) {
        const char *ls = p, *le = strchr(p, '\n');
        p = le ? le + 1 : p + strlen(p);
        if (!le) le = ls + strlen(ls);
        trim(&ls, &le);
        if (ls == le || *ls == '#' || *ls == ';')
            continue;
        if (++nodes > MAX_NODES)
            goto bad;
        if (*ls == '[') {                                               /* [section] */
            if (le[-1] != ']' || le - ls < 3)
                goto bad;
            const char *ns = ls + 1, *ne = le - 1;
            trim(&ns, &ne);
            struct json *sec = mk(JSON_OBJ, ns, (size_t)(ne - ns));
            if (!sec)
                goto bad;
            if (rtail) rtail->next = sec; else root->child = sec;
            rtail = sec;
            cur = sec;
            ctail = NULL;
            continue;
        }
        const char *eq = ls;                                            /* key = value, key: value */
        while (eq < le && *eq != '=' && !(*eq == ':' && (eq + 1 == le || isspace((unsigned char)eq[1])))) eq++;
        if (eq == le)
            goto bad;
        const char *ke = eq;
        trim(&ls, &ke);
        if (ls == ke)
            goto bad;
        const char *vs = eq + 1, *ve = le;
        trim(&vs, &ve);
        bool quoted = ve - vs >= 2 && (*vs == '"' || *vs == '\'') && ve[-1] == *vs;
        if (quoted) {
            vs++;
            ve--;
        } else {                                                        /* a comment at the end of the line */
            for (const char *c = vs; c + 1 < ve; c++)
                if ((c[1] == '#' || c[1] == ';') && isspace((unsigned char)c[0])) {
                    ve = c;
                    trim(&vs, &ve);
                    break;
                }
        }
        char *val = strndup(vs, (size_t)(ve - vs));
        if (!val)
            goto bad;
        struct json *m;
        if (!quoted && (!strcasecmp(val, "true") || !strcasecmp(val, "false"))) {
            m = mk(JSON_BOOL, ls, (size_t)(ke - ls));
            if (m) m->b = tolower((unsigned char)val[0]) == 't';
            free(val);
        } else if (!quoted && *val && (isdigit((unsigned char)*val) || ((*val == '-' || *val == '+') && isdigit((unsigned char)val[1])))
                   && ({ char *end; strtod(val, &end); !*end; })) {
            m = mk(JSON_NUM, ls, (size_t)(ke - ls));
            if (m) { m->str = val; m->num = strtod(val, NULL); } else free(val);
        } else {
            m = mk(JSON_STR, ls, (size_t)(ke - ls));
            if (m) m->str = val; else free(val);
        }
        if (!m)
            goto bad;
        if (cur == root) {
            if (rtail) rtail->next = m; else root->child = m;
            rtail = m;
        } else {
            if (ctail) ctail->next = m; else cur->child = m;
            ctail = m;
        }
    }
    if (!root->child)
        goto bad;
    return root;
bad:
    json_free(root);
    return NULL;
}
