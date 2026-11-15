#include "certread.h"

#include "../src/sha256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ITEMS 300

/* ---- a growing string for the JSON ---- */
struct jb {
    char *s;
    size_t n, cap;
};

static void jb_add(struct jb *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        char *t = realloc(b->s, b->cap);
        if (!t)
            return;
        b->s = t;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void jb_puts(struct jb *b, const char *s)
{
    jb_add(b, s, strlen(s));
}

/* A JSON string: quotes, escapes; bytes that are no UTF-8 are taken as Latin-1. */
static void jb_str(struct jb *b, const char *s, size_t n)
{
    jb_add(b, "\"", 1);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = k > 0 && i + k <= n;
        for (size_t j = 1; ok && j < k; j++)
            ok = ((unsigned char)s[i + j] & 0xc0) == 0x80;
        if (!ok) {                                                      /* Latin-1 -> UTF-8 */
            char t[3] = { (char)(0xc0 | c >> 6), (char)(0x80 | (c & 0x3f)), 0 };
            jb_add(b, t, 2);
            i++;
            continue;
        }
        if (c == '"' || c == '\\') {
            char t[2] = { '\\', (char)c };
            jb_add(b, t, 2);
        } else if (c < 0x20 || c == 0x7f) {
            char t[8];
            snprintf(t, sizeof(t), "\\u%04x", c);
            jb_puts(b, t);
        } else {
            jb_add(b, s + i, k);
        }
        i += k;
    }
    jb_add(b, "\"", 1);
}

static void jb_key(struct jb *b, bool *first, const char *key)
{
    if (!*first)
        jb_add(b, ",", 1);
    *first = false;
    jb_str(b, key, strlen(key));
    jb_add(b, ":", 1);
}

static void jb_kv(struct jb *b, bool *first, const char *key, const char *val)
{
    jb_key(b, first, key);
    jb_str(b, val, strlen(val));
}

/* ---- ASN.1 (DER) ---- */
struct asn {
    unsigned tag;                       /* the tag number */
    int cls;                            /* 0 universal, 1 application, 2 context, 3 private */
    bool cons;
    const uint8_t *val;
    size_t vlen;
    const uint8_t *all;                 /* the whole element, header included */
    size_t alen;
};

/* The element at *p; false at the end of the data or when it does not parse. *p moves behind it. */
static bool asn_next(const uint8_t **p, const uint8_t *end, struct asn *a)
{
    const uint8_t *q = *p;
    if (q + 2 > end)
        return false;
    a->all = q;
    a->cls = q[0] >> 6;
    a->cons = (q[0] & 0x20) != 0;
    a->tag = q[0] & 0x1f;
    q++;
    if (a->tag == 0x1f)
        return false;                                                   /* tag numbers above 30: not used in what is read here */
    size_t len = *q++;
    if (len & 0x80) {
        size_t nb = len & 0x7f;
        if (nb == 0 || nb > 4 || q + nb > end)
            return false;
        len = 0;
        for (size_t i = 0; i < nb; i++)
            len = len << 8 | *q++;
    }
    if (len > (size_t)(end - q))
        return false;
    a->val = q;
    a->vlen = len;
    a->alen = (size_t)(q + len - a->all);
    *p = q + len;
    return true;
}

static bool is_tag(const struct asn *a, int cls, unsigned tag)
{
    return a->cls == cls && a->tag == tag;
}

static void oid_text(const uint8_t *v, size_t n, char *o, size_t cap)
{
    size_t w = 0;
    uint64_t x = 0;
    bool first = true;
    o[0] = 0;
    for (size_t i = 0; i < n; i++) {
        x = x << 7 | (v[i] & 0x7f);
        if (v[i] & 0x80) {
            if (x > (1ull << 56))
                return;
            continue;
        }
        if (first) {
            unsigned a = x < 40 ? 0 : x < 80 ? 1 : 2;
            w += (size_t)snprintf(o + w, cap - w, "%u.%llu", a, (unsigned long long)(x - a * 40));
            first = false;
        } else if (w < cap) {
            w += (size_t)snprintf(o + w, cap - w, ".%llu", (unsigned long long)x);
        }
        x = 0;
        if (w >= cap)
            return;
    }
}

struct oidname {
    const char *oid, *name;
};

static const struct oidname k_oids[] = {
    { "2.5.4.3", "CN" }, { "2.5.4.4", "SN" }, { "2.5.4.5", "serialNumber" }, { "2.5.4.6", "C" }, { "2.5.4.7", "L" }, { "2.5.4.8", "ST" }, { "2.5.4.9", "street" },
    { "2.5.4.10", "O" }, { "2.5.4.11", "OU" }, { "2.5.4.12", "title" }, { "2.5.4.17", "postalCode" }, { "2.5.4.42", "GN" }, { "2.5.4.97", "organizationIdentifier" },
    { "1.2.840.113549.1.9.1", "emailAddress" }, { "0.9.2342.19200300.100.1.25", "DC" }, { "0.9.2342.19200300.100.1.1", "UID" },
    { "1.3.6.1.4.1.311.60.2.1.3", "jurisdictionC" }, { "1.3.6.1.4.1.311.60.2.1.2", "jurisdictionST" }, { "1.3.6.1.4.1.311.60.2.1.1", "jurisdictionL" },
};

static const struct oidname k_algs[] = {
    { "1.2.840.113549.1.1.1", "rsaEncryption" }, { "1.2.840.113549.1.1.4", "md5WithRSAEncryption" }, { "1.2.840.113549.1.1.5", "sha1WithRSAEncryption" },
    { "1.2.840.113549.1.1.10", "RSASSA-PSS" }, { "1.2.840.113549.1.1.11", "sha256WithRSAEncryption" }, { "1.2.840.113549.1.1.12", "sha384WithRSAEncryption" },
    { "1.2.840.113549.1.1.13", "sha512WithRSAEncryption" }, { "1.2.840.113549.1.1.14", "sha224WithRSAEncryption" }, { "1.2.840.10045.2.1", "id-ecPublicKey" },
    { "1.2.840.10045.4.1", "ecdsa-with-SHA1" }, { "1.2.840.10045.4.3.1", "ecdsa-with-SHA224" }, { "1.2.840.10045.4.3.2", "ecdsa-with-SHA256" },
    { "1.2.840.10045.4.3.3", "ecdsa-with-SHA384" }, { "1.2.840.10045.4.3.4", "ecdsa-with-SHA512" }, { "1.2.840.10040.4.1", "dsaEncryption" },
    { "1.2.840.10040.4.3", "dsa-with-SHA1" }, { "2.16.840.1.101.3.4.3.2", "dsa-with-SHA256" }, { "1.3.101.110", "X25519" }, { "1.3.101.111", "X448" },
    { "1.3.101.112", "Ed25519" }, { "1.3.101.113", "Ed448" }, { "1.2.840.113549.1.3.1", "dhKeyAgreement" },
    { "1.2.840.10045.3.1.7", "prime256v1 (P-256)" }, { "1.3.132.0.34", "secp384r1 (P-384)" }, { "1.3.132.0.35", "secp521r1 (P-521)" },
    { "1.3.132.0.10", "secp256k1" }, { "1.2.840.10045.3.1.1", "prime192v1 (P-192)" }, { "1.3.132.0.33", "secp224r1 (P-224)" },
    { "1.3.36.3.3.2.8.1.1.7", "brainpoolP256r1" }, { "1.3.36.3.3.2.8.1.1.11", "brainpoolP384r1" }, { "1.3.36.3.3.2.8.1.1.13", "brainpoolP512r1" },
};

static const struct oidname k_eku[] = {
    { "1.3.6.1.5.5.7.3.1", "serverAuth" }, { "1.3.6.1.5.5.7.3.2", "clientAuth" }, { "1.3.6.1.5.5.7.3.3", "codeSigning" }, { "1.3.6.1.5.5.7.3.4", "emailProtection" },
    { "1.3.6.1.5.5.7.3.8", "timeStamping" }, { "1.3.6.1.5.5.7.3.9", "OCSPSigning" }, { "2.5.29.37.0", "anyExtendedKeyUsage" }, { "1.3.6.1.4.1.311.10.3.4", "EFS" },
    { "1.3.6.1.5.5.7.3.5", "ipsecEndSystem" }, { "1.3.6.1.5.5.7.3.6", "ipsecTunnel" }, { "1.3.6.1.5.5.7.3.7", "ipsecUser" }, { "1.3.6.1.4.1.311.20.2.2", "smartcardLogon" },
};

static const struct oidname k_ext[] = {
    { "2.5.29.14", "subjectKeyIdentifier" }, { "2.5.29.15", "keyUsage" }, { "2.5.29.17", "subjectAltName" }, { "2.5.29.18", "issuerAltName" },
    { "2.5.29.19", "basicConstraints" }, { "2.5.29.30", "nameConstraints" }, { "2.5.29.31", "crlDistributionPoints" }, { "2.5.29.32", "certificatePolicies" },
    { "2.5.29.35", "authorityKeyIdentifier" }, { "2.5.29.36", "policyConstraints" }, { "2.5.29.37", "extendedKeyUsage" }, { "1.3.6.1.5.5.7.1.1", "authorityInfoAccess" },
    { "1.3.6.1.4.1.11129.2.4.2", "signedCertificateTimestamps" }, { "2.5.29.20", "cRLNumber" }, { "2.5.29.21", "reasonCode" },
};

#define LOOKUP(tab, oid) lookup(tab, sizeof(tab) / sizeof(tab[0]), oid)
static const char *lookup(const struct oidname *tab, size_t n, const char *oid)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(tab[i].oid, oid))
            return tab[i].name;
    return NULL;
}

static const char *oid_name(const struct asn *a, const struct oidname *tab, size_t n, char *buf, size_t cap)
{
    oid_text(a->val, a->vlen, buf, cap);
    const char *nm = lookup(tab, n, buf);
    return nm ? nm : buf;
}

/* ---- strings and times ---- */
/* The text of a string element as UTF-8 into `o` (at most cap - 1 bytes); BMPString is UTF-16BE, T61 / Printable / IA5 are taken as they are. */
static size_t asn_string(const struct asn *a, char *o, size_t cap)
{
    size_t w = 0;
    if (a->tag == 30 && a->cls == 0) {                                  /* BMPString */
        for (size_t i = 0; i + 1 < a->vlen && w + 4 < cap; i += 2) {
            unsigned cp = (unsigned)a->val[i] << 8 | a->val[i + 1];
            if (cp < 0x80) {
                o[w++] = (char)cp;
            } else if (cp < 0x800) {
                o[w++] = (char)(0xc0 | cp >> 6);
                o[w++] = (char)(0x80 | (cp & 0x3f));
            } else {
                o[w++] = (char)(0xe0 | cp >> 12);
                o[w++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                o[w++] = (char)(0x80 | (cp & 0x3f));
            }
        }
    } else {
        w = a->vlen < cap - 1 ? a->vlen : cap - 1;
        memcpy(o, a->val, w);
    }
    o[w] = 0;
    return w;
}

static bool asn_time(const struct asn *a, struct tm *tm)
{
    char t[20];
    if (a->cls != 0 || (a->tag != 23 && a->tag != 24) || a->vlen < 11 || a->vlen > 19)
        return false;
    memcpy(t, a->val, a->vlen);
    t[a->vlen] = 0;
    int y, mo, d, h, mi, s = 0, n = 0;
    if (a->tag == 23) {
        int yy;
        if (sscanf(t, "%2d%2d%2d%2d%2d%n", &yy, &mo, &d, &h, &mi, &n) != 5)
            return false;
        y = yy >= 50 ? 1900 + yy : 2000 + yy;
    } else {
        if (sscanf(t, "%4d%2d%2d%2d%2d%n", &y, &mo, &d, &h, &mi, &n) != 5)
            return false;
    }
    if (t[n] >= '0' && t[n] <= '9')
        sscanf(t + n, "%2d", &s);
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = y - 1900;
    tm->tm_mon = mo - 1;
    tm->tm_mday = d;
    tm->tm_hour = h;
    tm->tm_min = mi;
    tm->tm_sec = s;
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

static void time_text(const struct tm *tm, char *o, size_t cap)
{
    snprintf(o, cap, "%04d-%02d-%02d %02d:%02d:%02d UTC", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void hex_colon(const uint8_t *p, size_t n, char *o, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; i < n && w + 4 < cap; i++)
        w += (size_t)snprintf(o + w, cap - w, "%s%02X", i ? ":" : "", p[i]);
    if (!n)
        o[0] = 0;
}

/* ---- names, keys, extensions ---- */
/* A Name as "CN=a, O=b" (in the order of the file) into `o`; when `obj` is set also as a JSON object with the parts. */
static void name_text(const struct asn *name, char *o, size_t cap)
{
    size_t w = 0;
    o[0] = 0;
    const uint8_t *p = name->val, *end = name->val + name->vlen;
    struct asn rdn;
    while (asn_next(&p, end, &rdn)) {
        const uint8_t *q = rdn.val, *qe = rdn.val + rdn.vlen;
        struct asn atv;
        while (asn_next(&q, qe, &atv)) {
            const uint8_t *r = atv.val, *re = atv.val + atv.vlen;
            struct asn oid, val;
            if (!asn_next(&r, re, &oid) || !is_tag(&oid, 0, 6) || !asn_next(&r, re, &val))
                continue;
            char buf[64], s[300];
            const char *nm = oid_name(&oid, k_oids, sizeof(k_oids) / sizeof(k_oids[0]), buf, sizeof(buf));
            asn_string(&val, s, sizeof(s));
            w += (size_t)snprintf(o + w, w < cap ? cap - w : 0, "%s%s=%s", w ? ", " : "", nm, s);
            if (w >= cap) {
                o[cap - 1] = 0;
                return;
            }
        }
    }
}

static int bit_length(const uint8_t *v, size_t n)
{
    while (n > 0 && v[0] == 0) {
        v++;
        n--;
    }
    if (!n)
        return 0;
    int bits = (int)n * 8;
    for (uint8_t m = 0x80; m && !(v[0] & m); m >>= 1)
        bits--;
    return bits;
}

/* "RSA, 2048 bit" / "EC, prime256v1 (P-256)" ... from an AlgorithmIdentifier and, for RSA, the key bits. */
static void key_text(const struct asn *alg, const uint8_t *keybits, size_t keylen, char *o, size_t cap)
{
    const uint8_t *p = alg->val, *end = alg->val + alg->vlen;
    struct asn oid, par;
    char buf[80];
    o[0] = 0;
    if (!asn_next(&p, end, &oid) || !is_tag(&oid, 0, 6))
        return;
    const char *nm = oid_name(&oid, k_algs, sizeof(k_algs) / sizeof(k_algs[0]), buf, sizeof(buf));
    if (!strcmp(nm, "rsaEncryption") || !strcmp(nm, "RSASSA-PSS")) {
        const uint8_t *q = keybits, *qe = keybits + keylen;
        struct asn seq, n;
        if (keylen && asn_next(&q, qe, &seq) && is_tag(&seq, 0, 16)) {
            const uint8_t *r = seq.val, *re = seq.val + seq.vlen;
            if (asn_next(&r, re, &n) && is_tag(&n, 0, 2)) {
                snprintf(o, cap, "RSA, %d bit", bit_length(n.val, n.vlen));
                return;
            }
        }
        snprintf(o, cap, "RSA");
    } else if (!strcmp(nm, "id-ecPublicKey")) {
        char b2[80];
        if (asn_next(&p, end, &par) && is_tag(&par, 0, 6))
            snprintf(o, cap, "EC, %s", oid_name(&par, k_algs, sizeof(k_algs) / sizeof(k_algs[0]), b2, sizeof(b2)));
        else
            snprintf(o, cap, "EC");
    } else {
        snprintf(o, cap, "%s", nm);
    }
}

static const char *const k_keyusage[] = { "digitalSignature", "nonRepudiation", "keyEncipherment", "dataEncipherment", "keyAgreement", "keyCertSign", "cRLSign",
                                          "encipherOnly", "decipherOnly" };

static void san_text(const struct asn *seq, struct jb *b, size_t limit)
{
    const uint8_t *p = seq->val, *end = seq->val + seq->vlen;
    struct asn gn;
    bool first = true;
    while (asn_next(&p, end, &gn) && b->n < limit) {
        char t[300];
        if (gn.cls != 2)
            continue;
        const char *kind = gn.tag == 2 ? "DNS" : gn.tag == 1 ? "email" : gn.tag == 6 ? "URI" : gn.tag == 7 ? "IP" : NULL;
        if (!kind)
            continue;
        if (gn.tag == 7 && (gn.vlen == 4 || gn.vlen == 16)) {
            if (gn.vlen == 4) {
                snprintf(t, sizeof(t), "%u.%u.%u.%u", gn.val[0], gn.val[1], gn.val[2], gn.val[3]);
            } else {
                size_t w = 0;
                for (int i = 0; i < 16; i += 2)
                    w += (size_t)snprintf(t + w, sizeof(t) - w, "%s%x", i ? ":" : "", gn.val[i] << 8 | gn.val[i + 1]);
            }
        } else {
            size_t w = gn.vlen < sizeof(t) - 1 ? gn.vlen : sizeof(t) - 1;
            memcpy(t, gn.val, w);
            t[w] = 0;
        }
        char line[340];
        snprintf(line, sizeof(line), "%s%s:%s", first ? "" : ", ", kind, t);
        jb_puts(b, line);
        first = false;
    }
}

/* The extensions as the pairs of a JSON object; the subject alternative names are also returned as plain text for the top of the item. */
static void extensions_json(const struct asn *exts, struct jb *b, char *san, size_t sancap)
{
    jb_add(b, "{", 1);
    bool first = true;
    const uint8_t *p = exts->val, *end = exts->val + exts->vlen;
    struct asn ext;
    san[0] = 0;
    while (asn_next(&p, end, &ext)) {
        const uint8_t *q = ext.val, *qe = ext.val + ext.vlen;
        struct asn oid, a, val;
        bool critical = false;
        if (!asn_next(&q, qe, &oid) || !is_tag(&oid, 0, 6) || !asn_next(&q, qe, &a))
            continue;
        if (is_tag(&a, 0, 1)) {
            critical = a.vlen && a.val[0] != 0;
            if (!asn_next(&q, qe, &val))
                continue;
        } else {
            val = a;
        }
        if (!is_tag(&val, 0, 4))
            continue;
        char obuf[80];
        const char *nm = oid_name(&oid, k_ext, sizeof(k_ext) / sizeof(k_ext[0]), obuf, sizeof(obuf));
        char key[120];
        snprintf(key, sizeof(key), "%s%s", nm, critical ? " (critical)" : "");
        jb_key(b, &first, key);
        const uint8_t *vp = val.val, *ve = val.val + val.vlen;
        struct asn in;
        struct jb v = { 0 };
        if (!strcmp(nm, "subjectAltName") && asn_next(&vp, ve, &in) && is_tag(&in, 0, 16)) {
            san_text(&in, &v, 4000);
            if (v.s) {
                snprintf(san, sancap, "%s", v.s);
                jb_str(b, v.s, v.n);
            } else {
                jb_puts(b, "\"\"");
            }
        } else if (!strcmp(nm, "basicConstraints") && asn_next(&vp, ve, &in) && is_tag(&in, 0, 16)) {
            const uint8_t *r = in.val, *re = in.val + in.vlen;
            struct asn x;
            bool ca = false;
            long path = -1;
            while (asn_next(&r, re, &x)) {
                if (is_tag(&x, 0, 1))
                    ca = x.vlen && x.val[0] != 0;
                else if (is_tag(&x, 0, 2) && x.vlen && x.vlen <= 4) {
                    path = 0;
                    for (size_t i = 0; i < x.vlen; i++)
                        path = path << 8 | x.val[i];
                }
            }
            char t[80];
            if (path >= 0)
                snprintf(t, sizeof(t), "CA:%s, pathlen:%ld", ca ? "TRUE" : "FALSE", path);
            else
                snprintf(t, sizeof(t), "CA:%s", ca ? "TRUE" : "FALSE");
            jb_str(b, t, strlen(t));
        } else if (!strcmp(nm, "keyUsage") && asn_next(&vp, ve, &in) && is_tag(&in, 0, 3) && in.vlen >= 1) {
            jb_add(b, "[", 1);
            bool f2 = true;
            for (size_t bit = 0; bit < 9; bit++) {
                size_t byte = 1 + bit / 8;
                if (byte < in.vlen && (in.val[byte] & (0x80 >> (bit % 8)))) {
                    if (!f2)
                        jb_add(b, ",", 1);
                    f2 = false;
                    jb_str(b, k_keyusage[bit], strlen(k_keyusage[bit]));
                }
            }
            jb_add(b, "]", 1);
        } else if (!strcmp(nm, "extendedKeyUsage") && asn_next(&vp, ve, &in) && is_tag(&in, 0, 16)) {
            jb_add(b, "[", 1);
            const uint8_t *r = in.val, *re = in.val + in.vlen;
            struct asn x;
            bool f2 = true;
            while (asn_next(&r, re, &x)) {
                if (!is_tag(&x, 0, 6))
                    continue;
                char t[80];
                const char *en = oid_name(&x, k_eku, sizeof(k_eku) / sizeof(k_eku[0]), t, sizeof(t));
                if (!f2)
                    jb_add(b, ",", 1);
                f2 = false;
                jb_str(b, en, strlen(en));
            }
            jb_add(b, "]", 1);
        } else if (!strcmp(nm, "subjectKeyIdentifier") && asn_next(&vp, ve, &in) && is_tag(&in, 0, 4)) {
            char t[200];
            hex_colon(in.val, in.vlen < 32 ? in.vlen : 32, t, sizeof(t));
            jb_str(b, t, strlen(t));
        } else if (!strcmp(nm, "authorityKeyIdentifier") && asn_next(&vp, ve, &in) && is_tag(&in, 0, 16)) {
            const uint8_t *r = in.val, *re = in.val + in.vlen;
            struct asn x;
            char t[200] = "";
            while (asn_next(&r, re, &x))
                if (x.cls == 2 && x.tag == 0)
                    hex_colon(x.val, x.vlen < 32 ? x.vlen : 32, t, sizeof(t));
            jb_str(b, t, strlen(t));
        } else {
            char t[40];
            snprintf(t, sizeof(t), "(%zu bytes, not decoded)", val.vlen);
            jb_str(b, t, strlen(t));
        }
        free(v.s);
    }
    jb_add(b, "}", 1);
}

/* ---- the items ---- */
static void describe_validity(const struct tm *nb, const struct tm *na, char *status, size_t cap)
{
    struct tm a = *na, f = *nb;
    time_t exp = timegm(&a), from = timegm(&f), now = time(NULL);
    if (now > exp)
        snprintf(status, cap, "EXPIRED %ld days ago", (long)((now - exp) / 86400));
    else if (now < from)
        snprintf(status, cap, "not valid yet (%ld days to go)", (long)((from - now) / 86400));
    else
        snprintf(status, cap, "valid, %ld days left", (long)((exp - now) / 86400));
}

/* An X.509 certificate (DER) as a JSON object. */
static bool cert_item(const uint8_t *der, size_t len, struct jb *b, char *info, size_t infon, const char *type)
{
    const uint8_t *p = der, *end = der + len;
    struct asn cert, tbs;
    if (!asn_next(&p, end, &cert) || !is_tag(&cert, 0, 16) || !cert.cons)
        return false;
    p = cert.val;
    end = cert.val + cert.vlen;
    if (!asn_next(&p, end, &tbs) || !is_tag(&tbs, 0, 16))
        return false;
    struct asn sigalg;
    if (!asn_next(&p, end, &sigalg) || !is_tag(&sigalg, 0, 16))
        return false;
    const uint8_t *q = tbs.val, *qe = tbs.val + tbs.vlen;
    struct asn a;
    if (!asn_next(&q, qe, &a))
        return false;
    int version = 1;
    if (is_tag(&a, 2, 0)) {                                             /* [0] version */
        const uint8_t *vp = a.val;
        struct asn v;
        if (asn_next(&vp, a.val + a.vlen, &v) && is_tag(&v, 0, 2) && v.vlen == 1)
            version = v.val[0] + 1;
        if (!asn_next(&q, qe, &a))
            return false;
    }
    struct asn serial = a, tbsalg, issuer, validity, subject, spki;
    if (!is_tag(&serial, 0, 2) || !asn_next(&q, qe, &tbsalg) || !asn_next(&q, qe, &issuer) || !asn_next(&q, qe, &validity) || !asn_next(&q, qe, &subject) ||
        !asn_next(&q, qe, &spki) || !is_tag(&spki, 0, 16))
        return false;
    struct tm nb, na;
    const uint8_t *vp = validity.val;
    struct asn t1, t2;
    if (!asn_next(&vp, validity.val + validity.vlen, &t1) || !asn_next(&vp, validity.val + validity.vlen, &t2) || !asn_time(&t1, &nb) || !asn_time(&t2, &na))
        return false;
    char subj[600], iss[600], tb[40], ta[40], status[80], hex[400], ks[120], salg[100], obuf[80];
    name_text(&subject, subj, sizeof(subj));
    name_text(&issuer, iss, sizeof(iss));
    time_text(&nb, tb, sizeof(tb));
    time_text(&na, ta, sizeof(ta));
    describe_validity(&nb, &na, status, sizeof(status));
    hex_colon(serial.val, serial.vlen < 40 ? serial.vlen : 40, hex, sizeof(hex));
    const uint8_t *sp = spki.val;
    struct asn kalg, kbits;
    ks[0] = 0;
    if (asn_next(&sp, spki.val + spki.vlen, &kalg) && asn_next(&sp, spki.val + spki.vlen, &kbits) && is_tag(&kbits, 0, 3) && kbits.vlen >= 1)
        key_text(&kalg, kbits.val + 1, kbits.vlen - 1, ks, sizeof(ks));
    {
        const uint8_t *ap = sigalg.val;
        struct asn o;
        snprintf(salg, sizeof(salg), "?");
        if (asn_next(&ap, sigalg.val + sigalg.vlen, &o) && is_tag(&o, 0, 6))
            snprintf(salg, sizeof(salg), "%s", oid_name(&o, k_algs, sizeof(k_algs) / sizeof(k_algs[0]), obuf, sizeof(obuf)));
    }
    uint8_t fp[LM_SHA256_LEN];
    lm_sha256(cert.all, cert.alen, fp);
    char fps[200];
    hex_colon(fp, sizeof(fp), fps, sizeof(fps));
    struct jb ext = { 0 };
    char san[1200] = "";
    while (asn_next(&q, qe, &a)) {
        if (is_tag(&a, 2, 3)) {                                         /* [3] extensions */
            const uint8_t *ep = a.val;
            struct asn seq;
            if (asn_next(&ep, a.val + a.vlen, &seq) && is_tag(&seq, 0, 16))
                extensions_json(&seq, &ext, san, sizeof(san));
        }
    }
    bool first = true;
    jb_add(b, "{", 1);
    jb_kv(b, &first, "type", type);
    jb_kv(b, &first, "subject", subj);
    jb_kv(b, &first, "issuer", iss);
    jb_kv(b, &first, "validFrom", tb);
    jb_kv(b, &first, "validUntil", ta);
    jb_kv(b, &first, "status", status);
    if (san[0])
        jb_kv(b, &first, "subjectAltName", san);
    jb_kv(b, &first, "publicKey", ks);
    jb_kv(b, &first, "signatureAlgorithm", salg);
    jb_kv(b, &first, "serial", hex);
    jb_kv(b, &first, "fingerprintSHA256", fps);
    char vs[16];
    snprintf(vs, sizeof(vs), "%d", version);
    jb_kv(b, &first, "version", vs);
    jb_kv(b, &first, "selfSigned", !strcmp(subj, iss) ? "yes (subject = issuer)" : "no");
    if (ext.s) {
        jb_key(b, &first, "extensions");
        jb_add(b, ext.s, ext.n);
    }
    jb_add(b, "}", 1);
    free(ext.s);
    if (!info[0]) {
        const char *cn = strstr(subj, "CN=");
        snprintf(info, infon, "%s, %.60s, %s", type, cn ? cn + 3 : subj, status);
    }
    return true;
}

/* A certificate revocation list. */
static bool crl_item(const uint8_t *der, size_t len, struct jb *b, char *info, size_t infon)
{
    const uint8_t *p = der, *end = der + len;
    struct asn crl, tbs, sigalg;
    if (!asn_next(&p, end, &crl) || !is_tag(&crl, 0, 16))
        return false;
    const uint8_t *q = crl.val, *qe = crl.val + crl.vlen;
    if (!asn_next(&q, qe, &tbs) || !is_tag(&tbs, 0, 16) || !asn_next(&q, qe, &sigalg) || !is_tag(&sigalg, 0, 16))
        return false;
    const uint8_t *r = tbs.val, *re = tbs.val + tbs.vlen;
    struct asn a, issuer, thisu, nextu, list;
    if (!asn_next(&r, re, &a))
        return false;
    if (is_tag(&a, 0, 2) && !asn_next(&r, re, &a))                      /* the version, then the signature algorithm */
        return false;
    if (!is_tag(&a, 0, 16) || !asn_next(&r, re, &issuer) || !is_tag(&issuer, 0, 16) || !asn_next(&r, re, &thisu))
        return false;
    char iss[600], t1[40] = "", t2[40] = "", salg[100] = "?", obuf[80];
    name_text(&issuer, iss, sizeof(iss));
    struct tm tm1, tm2;
    if (asn_time(&thisu, &tm1))
        time_text(&tm1, t1, sizeof(t1));
    const uint8_t *save = r;
    if (asn_next(&r, re, &nextu) && asn_time(&nextu, &tm2))
        time_text(&tm2, t2, sizeof(t2));
    else
        r = save;
    long revoked = 0;
    if (asn_next(&r, re, &list) && is_tag(&list, 0, 16)) {
        const uint8_t *lp = list.val;
        struct asn e;
        while (asn_next(&lp, list.val + list.vlen, &e))
            revoked++;
    }
    const uint8_t *ap = sigalg.val;
    struct asn o;
    if (asn_next(&ap, sigalg.val + sigalg.vlen, &o) && is_tag(&o, 0, 6))
        snprintf(salg, sizeof(salg), "%s", oid_name(&o, k_algs, sizeof(k_algs) / sizeof(k_algs[0]), obuf, sizeof(obuf)));
    bool first = true;
    char n[24];
    jb_add(b, "{", 1);
    jb_kv(b, &first, "type", "Certificate revocation list");
    jb_kv(b, &first, "issuer", iss);
    jb_kv(b, &first, "thisUpdate", t1);
    jb_kv(b, &first, "nextUpdate", t2);
    snprintf(n, sizeof(n), "%ld", revoked);
    jb_kv(b, &first, "revokedCertificates", n);
    jb_kv(b, &first, "signatureAlgorithm", salg);
    jb_add(b, "}", 1);
    if (!info[0])
        snprintf(info, infon, "Certificate revocation list, %.60s, %ld revoked", iss, revoked);
    return true;
}

/* A certificate request (PKCS#10). */
static bool csr_item(const uint8_t *der, size_t len, struct jb *b, char *info, size_t infon)
{
    const uint8_t *p = der, *end = der + len;
    struct asn csr, info_seq, sigalg;
    if (!asn_next(&p, end, &csr) || !is_tag(&csr, 0, 16))
        return false;
    const uint8_t *q = csr.val, *qe = csr.val + csr.vlen;
    if (!asn_next(&q, qe, &info_seq) || !is_tag(&info_seq, 0, 16) || !asn_next(&q, qe, &sigalg))
        return false;
    const uint8_t *r = info_seq.val, *re = info_seq.val + info_seq.vlen;
    struct asn ver, subject, spki;
    if (!asn_next(&r, re, &ver) || !is_tag(&ver, 0, 2) || !asn_next(&r, re, &subject) || !asn_next(&r, re, &spki) || !is_tag(&spki, 0, 16))
        return false;
    char subj[600], ks[120] = "", salg[100] = "?", obuf[80];
    name_text(&subject, subj, sizeof(subj));
    const uint8_t *sp = spki.val;
    struct asn kalg, kbits;
    if (asn_next(&sp, spki.val + spki.vlen, &kalg) && asn_next(&sp, spki.val + spki.vlen, &kbits) && is_tag(&kbits, 0, 3) && kbits.vlen >= 1)
        key_text(&kalg, kbits.val + 1, kbits.vlen - 1, ks, sizeof(ks));
    const uint8_t *ap = sigalg.val;
    struct asn o;
    if (asn_next(&ap, sigalg.val + sigalg.vlen, &o) && is_tag(&o, 0, 6))
        snprintf(salg, sizeof(salg), "%s", oid_name(&o, k_algs, sizeof(k_algs) / sizeof(k_algs[0]), obuf, sizeof(obuf)));
    bool first = true;
    jb_add(b, "{", 1);
    jb_kv(b, &first, "type", "Certificate request");
    jb_kv(b, &first, "subject", subj);
    jb_kv(b, &first, "publicKey", ks);
    jb_kv(b, &first, "signatureAlgorithm", salg);
    jb_add(b, "}", 1);
    if (!info[0])
        snprintf(info, infon, "Certificate request, %.80s", subj);
    return true;
}

/* A key: PRIVATE keys are described by kind and size only (the numbers of the key are looked at for the size and not copied anywhere). `label` is the PEM
 * label, or NULL for bare DER. */
static bool key_item(const uint8_t *der, size_t len, const char *label, struct jb *b, char *info, size_t infon)
{
    const uint8_t *p = der, *end = der + len;
    struct asn seq, a, c;
    char ks[120] = "";
    const char *type = "Public key";
    if (!asn_next(&p, end, &seq) || !is_tag(&seq, 0, 16))
        return false;
    const uint8_t *q = seq.val, *qe = seq.val + seq.vlen;
    if (!asn_next(&q, qe, &a))
        return false;
    if (is_tag(&a, 0, 16)) {                                            /* SubjectPublicKeyInfo: { AlgorithmIdentifier, BIT STRING } */
        if (!asn_next(&q, qe, &c) || !is_tag(&c, 0, 3) || c.vlen < 1)
            return false;
        key_text(&a, c.val + 1, c.vlen - 1, ks, sizeof(ks));
    } else if (is_tag(&a, 0, 2)) {                                      /* { INTEGER, ... }: PKCS#8, RSA private / public, EC private */
        struct asn b2;
        if (!asn_next(&q, qe, &b2))
            return false;
        if (is_tag(&b2, 0, 16)) {                                       /* PKCS#8: { version, AlgorithmIdentifier, OCTET STRING } */
            type = "Private key";
            key_text(&b2, NULL, 0, ks, sizeof(ks));
            if (!strncmp(ks, "RSA", 3) && asn_next(&q, qe, &c) && is_tag(&c, 0, 4)) {
                const uint8_t *ip = c.val;
                struct asn inner;
                if (asn_next(&ip, c.val + c.vlen, &inner) && is_tag(&inner, 0, 16)) {
                    const uint8_t *r = inner.val, *re = inner.val + inner.vlen;
                    struct asn ver, n;
                    if (asn_next(&r, re, &ver) && asn_next(&r, re, &n) && is_tag(&n, 0, 2))
                        snprintf(ks, sizeof(ks), "RSA, %d bit", bit_length(n.val, n.vlen));
                }
            }
        } else if (is_tag(&b2, 0, 2)) {                                 /* RSA: { version, n, ... } private or { n, e } public */
            bool priv = (label && strstr(label, "PRIVATE")) || (!label && a.vlen <= 1);
            type = priv ? "Private key" : "Public key";
            snprintf(ks, sizeof(ks), "RSA, %d bit", priv ? bit_length(b2.val, b2.vlen) : bit_length(a.val, a.vlen));
        } else if (is_tag(&b2, 0, 4)) {                                 /* ECPrivateKey: { 1, OCTET STRING, [0] curve, ... } */
            type = "Private key";
            snprintf(ks, sizeof(ks), "EC");
            while (asn_next(&q, qe, &c))
                if (is_tag(&c, 2, 0)) {
                    const uint8_t *cp = c.val;
                    struct asn o;
                    char t[80];
                    if (asn_next(&cp, c.val + c.vlen, &o) && is_tag(&o, 0, 6))
                        snprintf(ks, sizeof(ks), "EC, %s", oid_name(&o, k_algs, sizeof(k_algs) / sizeof(k_algs[0]), t, sizeof(t)));
                }
        } else {
            return false;
        }
    } else {
        return false;
    }
    bool first = true;
    jb_add(b, "{", 1);
    jb_kv(b, &first, "type", type);
    jb_kv(b, &first, "key", ks);
    if (!strcmp(type, "Private key"))
        jb_kv(b, &first, "note", "the key itself is not shown");
    jb_add(b, "}", 1);
    if (!info[0])
        snprintf(info, infon, "%s, %s", type, ks);
    return true;
}

/* ---- PEM ---- */
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static uint8_t *b64decode(const char *s, size_t n, size_t *outn)
{
    uint8_t *o = malloc(n / 4 * 3 + 4);
    size_t w = 0;
    unsigned acc = 0;
    int bits = 0;
    if (!o)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=' || s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
            continue;
        int v = b64val((unsigned char)s[i]);
        if (v < 0) {
            free(o);
            return NULL;
        }
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o[w++] = (uint8_t)(acc >> bits);
        }
    }
    *outn = w;
    return o;
}

static bool item_from_der(const uint8_t *der, size_t len, const char *label, struct jb *b, char *info, size_t infon)
{
    if (label) {
        if (strstr(label, "CERTIFICATE REQUEST"))
            return csr_item(der, len, b, info, infon);
        if (strstr(label, "X509 CRL"))
            return crl_item(der, len, b, info, infon);
        if (!strcmp(label, "CERTIFICATE") || !strcmp(label, "TRUSTED CERTIFICATE") || !strcmp(label, "X509 CERTIFICATE"))
            return cert_item(der, len, b, info, infon, "Certificate");
        if (strstr(label, "ENCRYPTED PRIVATE KEY") || strstr(label, "OPENSSH PRIVATE KEY") || strstr(label, "DH PARAMETERS")) {
            bool first = true;
            jb_add(b, "{", 1);
            jb_kv(b, &first, "type", strstr(label, "ENCRYPTED") ? "Private key (encrypted)" : strstr(label, "OPENSSH") ? "Private key (OpenSSH format)" : "DH parameters");
            if (strstr(label, "PRIVATE"))
                jb_kv(b, &first, "note", "the key itself is not shown");
            jb_add(b, "}", 1);
            if (!info[0])
                snprintf(info, infon, "%s", strstr(label, "ENCRYPTED") ? "Private key (encrypted)" : strstr(label, "OPENSSH") ? "Private key (OpenSSH format)" : "DH parameters");
            return true;
        }
        return key_item(der, len, label, b, info, infon);
    }
    return cert_item(der, len, b, info, infon, "Certificate") || crl_item(der, len, b, info, infon) || csr_item(der, len, b, info, infon) ||
           key_item(der, len, NULL, b, info, infon);
}

char *cert_json(const unsigned char *buf, size_t n, char *info, size_t infon)
{
    struct jb items[1] = { { 0 } };
    struct jb *out = &items[0];
    int count = 0;
    info[0] = 0;
    const char *s = (const char *)buf;
    const char *end = s + n;
    struct jb one = { 0 };
    bool pem = false;
    for (const char *p = s; p < end && count < MAX_ITEMS;) {
        const char *b0 = memmem(p, (size_t)(end - p), "-----BEGIN ", 11);
        if (!b0)
            break;
        const char *lab = b0 + 11, *lab_end = memmem(lab, (size_t)(end - lab), "-----", 5);
        if (!lab_end || lab_end - lab > 60) {
            p = b0 + 11;
            continue;
        }
        char label[64];
        memcpy(label, lab, (size_t)(lab_end - lab));
        label[lab_end - lab] = 0;
        const char *body = lab_end + 5;
        char endmark[80];
        snprintf(endmark, sizeof(endmark), "-----END %s-----", label);
        const char *e0 = memmem(body, (size_t)(end - body), endmark, strlen(endmark));
        if (!e0) {
            p = body;
            continue;
        }
        pem = true;
        p = e0 + strlen(endmark);
        const char *hdr = memmem(body, (size_t)(e0 - body), "\n\n", 2);       /* old encrypted keys have headers (Proc-Type, DEK-Info) before the data */
        bool encrypted = memmem(body, (size_t)(e0 - body), "Proc-Type:", 10) != NULL && memmem(body, (size_t)(e0 - body), "ENCRYPTED", 9);
        if (hdr && encrypted)
            body = hdr + 2;
        size_t dn = 0;
        uint8_t *der = encrypted ? NULL : b64decode(body, (size_t)(e0 - body), &dn);
        one.n = 0;
        if (one.s)
            one.s[0] = 0;
        bool ok = false;
        if (encrypted) {
            bool first = true;
            jb_add(&one, "{", 1);
            jb_kv(&one, &first, "type", "Private key (encrypted)");
            jb_kv(&one, &first, "note", "the key itself is not shown");
            jb_add(&one, "}", 1);
            if (!info[0])
                snprintf(info, infon, "Private key (encrypted)");
            ok = true;
        } else if (der) {
            ok = item_from_der(der, dn, label, &one, info, infon);
        }
        free(der);
        if (ok && one.s) {
            jb_add(out, count ? "," : "", count ? 1 : 0);
            jb_add(out, one.s, one.n);
            count++;
        }
    }
    free(one.s);
    if (!pem && !count && n > 4 && buf[0] == 0x30) {                    /* bare DER */
        struct jb d = { 0 };
        if (item_from_der(buf, n, NULL, &d, info, infon) && d.s) {
            jb_add(out, d.s, d.n);
            count = 1;
        }
        free(d.s);
    }
    if (!count) {
        free(out->s);
        return NULL;
    }
    struct jb res = { 0 };
    if (count > 1) {
        jb_add(&res, "[", 1);
        jb_add(&res, out->s, out->n);
        jb_add(&res, "]", 1);
        char more[120];
        snprintf(more, sizeof(more), "%s (first of %d)", info, count);
        snprintf(info, infon, "%s", more);
        free(out->s);
        return res.s;
    }
    return out->s;
}
