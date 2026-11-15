#define _FILE_OFFSET_BITS 64
#include "elfread.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- JSON text ---- */
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

static void jb_str(struct jb *b, const char *s, size_t n)
{
    jb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char t[2] = { '\\', (char)c };
            jb_add(b, t, 2);
        } else if (c < 0x20 || c >= 0x7f) {
            if (c >= 0x80 && n && (c >> 6) >= 2) {                       /* a UTF-8 byte of a name: kept (invalid bytes become ?) */
                jb_add(b, s + i, 1);
            } else {
                jb_add(b, "?", 1);
            }
        } else {
            jb_add(b, s + i, 1);
        }
    }
    jb_add(b, "\"", 1);
}

static void jb_key(struct jb *b, bool *first, const char *k)
{
    if (!*first)
        jb_add(b, ",", 1);
    *first = false;
    jb_str(b, k, strlen(k));
    jb_add(b, ":", 1);
}

static void jb_kv(struct jb *b, bool *first, const char *k, const char *v)
{
    jb_key(b, first, k);
    jb_str(b, v, strlen(v));
}

static void jb_kvf(struct jb *b, bool *first, const char *k, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void jb_kvf(struct jb *b, bool *first, const char *k, const char *fmt, ...)
{
    char t[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    __builtin_va_end(ap);
    jb_kv(b, first, k, t);
}

/* ---- reading the file ---- */
struct file {
    int fd;
    uint64_t size;
    bool big;                           /* big endian */
    bool is64;
};

static bool rd(const struct file *f, uint64_t off, void *buf, size_t n)
{
    if (off > f->size || n > f->size - off)
        return false;
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(f->fd, p, n, (off_t)off);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return true;
}

static uint16_t g16(const struct file *f, const uint8_t *p)
{
    return f->big ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t g32(const struct file *f, const uint8_t *p)
{
    return f->big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t g64(const struct file *f, const uint8_t *p)
{
    uint64_t a = g32(f, p), b = g32(f, p + 4);
    return f->big ? a << 32 | b : b << 32 | a;
}

/* A NUL ended string at `off` into out (at most cap - 1 bytes). */
static void rd_str(const struct file *f, uint64_t off, char *out, size_t cap)
{
    out[0] = 0;
    size_t n = cap - 1;
    if (off >= f->size)
        return;
    if (n > f->size - off)
        n = (size_t)(f->size - off);
    ssize_t r = pread(f->fd, out, n, (off_t)off);
    if (r <= 0) {
        out[0] = 0;
        return;
    }
    out[r] = 0;
    out[strnlen(out, (size_t)r)] = 0;
}

static const char *machine_name(unsigned m)
{
    switch (m) {
    case 0: return "none";
    case 2: return "SPARC";
    case 3: return "x86";
    case 4: return "Motorola 68000";
    case 8: return "MIPS";
    case 15: return "PA-RISC";
    case 18: return "SPARC32+";
    case 20: return "PowerPC";
    case 21: return "PowerPC 64";
    case 22: return "S/390";
    case 40: return "ARM";
    case 42: return "SuperH";
    case 43: return "SPARC V9";
    case 50: return "IA-64";
    case 62: return "x86-64";
    case 87: return "V850";
    case 92: return "OpenRISC";
    case 94: return "Xtensa";
    case 105: return "MSP430";
    case 106: return "Blackfin";
    case 183: return "AArch64";
    case 187: return "TILE-Gx";
    case 189: return "MicroBlaze";
    case 195: return "ARC";
    case 243: return "RISC-V";
    case 247: return "eBPF";
    case 258: return "LoongArch";
    default: return "unknown";
    }
}

static const char *osabi_name(unsigned a)
{
    switch (a) {
    case 0: return "UNIX System V";
    case 1: return "HP-UX";
    case 2: return "NetBSD";
    case 3: return "Linux (GNU)";
    case 6: return "Solaris";
    case 9: return "FreeBSD";
    case 12: return "OpenBSD";
    case 97: return "ARM";
    case 255: return "standalone";
    default: return "other";
    }
}

static const char *sh_type_name(unsigned t)
{
    switch (t) {
    case 0: return "NULL";
    case 1: return "PROGBITS";
    case 2: return "SYMTAB";
    case 3: return "STRTAB";
    case 4: return "RELA";
    case 5: return "HASH";
    case 6: return "DYNAMIC";
    case 7: return "NOTE";
    case 8: return "NOBITS";
    case 9: return "REL";
    case 11: return "DYNSYM";
    case 14: return "INIT_ARRAY";
    case 15: return "FINI_ARRAY";
    case 0x6ffffff6: return "GNU_HASH";
    case 0x6ffffffd: return "VERDEF";
    case 0x6ffffffe: return "VERNEED";
    case 0x6fffffff: return "VERSYM";
    default: return "other";
    }
}

static const char *pt_name(unsigned t)
{
    switch (t) {
    case 0: return "NULL";
    case 1: return "LOAD";
    case 2: return "DYNAMIC";
    case 3: return "INTERP";
    case 4: return "NOTE";
    case 5: return "SHLIB";
    case 6: return "PHDR";
    case 7: return "TLS";
    case 0x6474e550: return "GNU_EH_FRAME";
    case 0x6474e551: return "GNU_STACK";
    case 0x6474e552: return "GNU_RELRO";
    case 0x6474e553: return "GNU_PROPERTY";
    default: return "other";
    }
}

static void flags_text(unsigned f, char *o)
{
    o[0] = f & 4 ? 'R' : '-';
    o[1] = f & 2 ? 'W' : '-';
    o[2] = f & 1 ? 'X' : '-';
    o[3] = 0;
}

struct sec {
    uint32_t name, type;
    uint64_t flags, addr, off, size, link, info;
};

#define MAX_SEC 4096
#define MAX_PH 1024
#define STR_MAX (8u << 20)

static char *elf_json(struct file *f, const uint8_t *eh, char *info, size_t infon)
{
    unsigned type, machine;
    uint64_t entry, phoff, shoff;
    unsigned phentsize, phnum, shentsize, shnum, shstrndx;
    uint32_t eflags;
    const uint8_t *p = eh + 16;
    type = g16(f, p);
    machine = g16(f, p + 2);
    if (f->is64) {
        entry = g64(f, p + 8);
        phoff = g64(f, p + 16);
        shoff = g64(f, p + 24);
        eflags = g32(f, p + 32);
        phentsize = g16(f, p + 38);
        phnum = g16(f, p + 40);
        shentsize = g16(f, p + 42);
        shnum = g16(f, p + 44);
        shstrndx = g16(f, p + 46);
    } else {
        entry = g32(f, p + 8);
        phoff = g32(f, p + 12);
        shoff = g32(f, p + 16);
        eflags = g32(f, p + 20);
        phentsize = g16(f, p + 26);
        phnum = g16(f, p + 28);
        shentsize = g16(f, p + 30);
        shnum = g16(f, p + 32);
        shstrndx = g16(f, p + 34);
    }
    size_t ehsize = f->is64 ? 64 : 52;
    uint8_t full[64];
    if (!rd(f, 0, full, ehsize))
        return NULL;
    if (f->is64) {
        shoff = g64(f, full + 40);
        phoff = g64(f, full + 32);
    }
    /* program headers */
    struct { uint32_t type, flags; uint64_t off, vaddr, filesz, memsz; } ph[MAX_PH];
    unsigned nph = 0;
    size_t pes = f->is64 ? 56 : 32;
    if (phoff && phentsize >= pes && phnum <= MAX_PH)
        for (unsigned i = 0; i < phnum; i++) {
            uint8_t b[64];
            if (!rd(f, phoff + (uint64_t)i * phentsize, b, pes))
                break;
            ph[nph].type = g32(f, b);
            if (f->is64) {
                ph[nph].flags = g32(f, b + 4);
                ph[nph].off = g64(f, b + 8);
                ph[nph].vaddr = g64(f, b + 16);
                ph[nph].filesz = g64(f, b + 32);
                ph[nph].memsz = g64(f, b + 40);
            } else {
                ph[nph].off = g32(f, b + 4);
                ph[nph].vaddr = g32(f, b + 8);
                ph[nph].filesz = g32(f, b + 16);
                ph[nph].memsz = g32(f, b + 20);
                ph[nph].flags = g32(f, b + 24);
            }
            nph++;
        }
    /* section headers */
    struct sec *sec = calloc(MAX_SEC, sizeof(struct sec));
    unsigned nsec = 0;
    size_t ses = f->is64 ? 64 : 40;
    if (shoff && shentsize >= ses) {
        if (shnum == 0 && shoff) {                                      /* the real count is in the first header's size field */
            uint8_t b[64];
            if (rd(f, shoff, b, ses))
                shnum = (unsigned)(f->is64 ? g64(f, b + 32) : g32(f, b + 20));
        }
        for (unsigned i = 0; i < shnum && i < MAX_SEC; i++) {
            uint8_t b[64];
            if (!rd(f, shoff + (uint64_t)i * shentsize, b, ses))
                break;
            struct sec *s = &sec[nsec++];
            s->name = g32(f, b);
            s->type = g32(f, b + 4);
            if (f->is64) {
                s->flags = g64(f, b + 8);
                s->addr = g64(f, b + 16);
                s->off = g64(f, b + 24);
                s->size = g64(f, b + 32);
                s->link = g32(f, b + 40);
                s->info = g32(f, b + 44);
            } else {
                s->flags = g32(f, b + 8);
                s->addr = g32(f, b + 12);
                s->off = g32(f, b + 16);
                s->size = g32(f, b + 20);
                s->link = g32(f, b + 24);
                s->info = g32(f, b + 28);
            }
        }
    }
    char *shstr = NULL;
    size_t shstrn = 0;
    if (shstrndx < nsec && sec[shstrndx].size && sec[shstrndx].size <= STR_MAX && sec[shstrndx].type != 8) {
        shstrn = (size_t)sec[shstrndx].size;
        shstr = malloc(shstrn + 1);
        if (shstr && !rd(f, sec[shstrndx].off, shstr, shstrn)) {
            free(shstr);
            shstr = NULL;
            shstrn = 0;
        } else if (shstr) {
            shstr[shstrn] = 0;
        }
    }
    #define SEC_NAME(s) ((s)->name < shstrn ? shstr + (s)->name : "")
    struct jb b = { 0 };
    bool first = true;
    jb_add(&b, "{", 1);
    const char *tname = type == 1 ? "relocatable object" : type == 2 ? "executable" : type == 3 ? "shared object / position independent executable" : type == 4 ? "core dump" : "other";
    char interp[256] = "";
    for (unsigned i = 0; i < nph; i++)
        if (ph[i].type == 3 && ph[i].filesz < sizeof(interp) && ph[i].filesz > 0)
            rd_str(f, ph[i].off, interp, sizeof(interp));
    bool is_dyn_exe = type == 3 && interp[0];
    jb_kvf(&b, &first, "format", "ELF %d-bit %s endian", f->is64 ? 64 : 32, f->big ? "big" : "little");
    jb_kvf(&b, &first, "type", "%s", type == 3 && !interp[0] ? "shared library" : type == 3 ? "position independent executable (PIE)" : tname);
    jb_kvf(&b, &first, "machine", "%s", machine_name(machine));
    jb_kvf(&b, &first, "abi", "%s (version %u)", osabi_name(eh[7]), eh[8]);
    if (entry)
        jb_kvf(&b, &first, "entry point", "0x%llx", (unsigned long long)entry);
    if (interp[0])
        jb_kv(&b, &first, "interpreter", interp);
    /* the dynamic section */
    struct { char names[64][160]; int n; } *needed = calloc(1, sizeof(*needed));
    char soname[200] = "", rpath[400] = "", runpath[400] = "";
    uint64_t dflags = 0, dflags1 = 0;
    bool bind_now = false;
    for (unsigned i = 0; i < nsec; i++) {
        if (sec[i].type != 6 || sec[i].link >= nsec || sec[i].size > STR_MAX)
            continue;
        const struct sec *ds = &sec[sec[i].link];
        size_t esz = f->is64 ? 16 : 8;
        char *dstr = ds->size && ds->size <= STR_MAX ? malloc((size_t)ds->size + 1) : NULL;
        if (!dstr || !rd(f, ds->off, dstr, (size_t)ds->size)) {
            free(dstr);
            continue;
        }
        dstr[ds->size] = 0;
        for (uint64_t o = 0; o + esz <= sec[i].size; o += esz) {
            uint8_t e[16];
            if (!rd(f, sec[i].off + o, e, esz))
                break;
            int64_t tag = f->is64 ? (int64_t)g64(f, e) : (int64_t)(int32_t)g32(f, e);
            uint64_t val = f->is64 ? g64(f, e + 8) : g32(f, e + 4);
            if (tag == 0)
                break;
            const char *str = val < ds->size ? dstr + val : "";
            if (tag == 1 && needed->n < 64)
                snprintf(needed->names[needed->n++], sizeof(needed->names[0]), "%s", str);
            else if (tag == 14)
                snprintf(soname, sizeof(soname), "%s", str);
            else if (tag == 15)
                snprintf(rpath, sizeof(rpath), "%s", str);
            else if (tag == 29)
                snprintf(runpath, sizeof(runpath), "%s", str);
            else if (tag == 30)
                dflags = val;
            else if (tag == 0x6ffffffb)
                dflags1 = val;
            else if (tag == 24)
                bind_now = true;
        }
        free(dstr);
    }
    if (dflags & 8)
        bind_now = true;
    if (dflags1 & 1)
        bind_now = true;
    bool pie = (dflags1 & 0x08000000u) || is_dyn_exe;
    if (soname[0])
        jb_kv(&b, &first, "soname", soname);
    if (needed->n) {
        jb_key(&b, &first, "needs");
        jb_add(&b, "[", 1);
        for (int i = 0; i < needed->n; i++) {
            if (i)
                jb_add(&b, ",", 1);
            jb_str(&b, needed->names[i], strlen(needed->names[i]));
        }
        jb_add(&b, "]", 1);
    }
    if (rpath[0])
        jb_kv(&b, &first, "rpath", rpath);
    if (runpath[0])
        jb_kv(&b, &first, "runpath", runpath);
    /* notes: build id, the kernel the program asks for */
    char buildid[160] = "", abitag[80] = "";
    for (unsigned i = 0; i < nph; i++) {
        if (ph[i].type != 4 || ph[i].filesz > 65536 || ph[i].filesz < 12)
            continue;
        uint8_t *nb = malloc((size_t)ph[i].filesz);
        if (!nb || !rd(f, ph[i].off, nb, (size_t)ph[i].filesz)) {
            free(nb);
            continue;
        }
        for (size_t o = 0; o + 12 <= ph[i].filesz;) {
            uint32_t nsz = g32(f, nb + o), dsz = g32(f, nb + o + 4), nt = g32(f, nb + o + 8);
            size_t nameo = o + 12, desco = nameo + ((nsz + 3u) & ~3u);
            if (desco + dsz > ph[i].filesz)
                break;
            if (nsz == 4 && !memcmp(nb + nameo, "GNU", 4)) {
                if (nt == 3 && dsz <= 64) {
                    for (uint32_t k = 0; k < dsz; k++)
                        snprintf(buildid + 2 * k, 3, "%02x", nb[desco + k]);
                } else if (nt == 1 && dsz >= 16 && g32(f, nb + desco) == 0) {
                    snprintf(abitag, sizeof(abitag), "Linux %u.%u.%u", g32(f, nb + desco + 4), g32(f, nb + desco + 8), g32(f, nb + desco + 12));
                }
            }
            o = desco + ((dsz + 3u) & ~3u);
        }
        free(nb);
    }
    if (buildid[0])
        jb_kv(&b, &first, "build id", buildid);
    if (abitag[0])
        jb_kv(&b, &first, "minimum kernel", abitag);
    /* symbols: stripped?, the protections that show as symbols */
    bool has_symtab = false, has_debug = false, canary = false, fortify = false;
    unsigned ndynsym = 0, nsymtab = 0;
    for (unsigned i = 0; i < nsec; i++) {
        const char *nm = SEC_NAME(&sec[i]);
        if (sec[i].type == 2) {
            has_symtab = true;
            nsymtab = (unsigned)(sec[i].size / (f->is64 ? 24 : 16));
        }
        if (!strncmp(nm, ".debug_", 7) || !strcmp(nm, ".zdebug_info"))
            has_debug = true;
        if (sec[i].type == 11 && sec[i].link < nsec && sec[i].size <= (64u << 20) && sec[i].link < nsec) {
            ndynsym = (unsigned)(sec[i].size / (f->is64 ? 24 : 16));
            const struct sec *ss = &sec[sec[i].link];
            if (ss->size && ss->size <= STR_MAX) {
                char *str = malloc((size_t)ss->size + 1);
                if (str && rd(f, ss->off, str, (size_t)ss->size)) {
                    str[ss->size] = 0;
                    size_t es = f->is64 ? 24 : 16;
                    for (uint64_t o = es; o + es <= sec[i].size; o += es) {
                        uint8_t sb[24];
                        if (!rd(f, sec[i].off + o, sb, es))
                            break;
                        uint32_t no = g32(f, sb);
                        const char *nm2 = no < ss->size ? str + no : "";
                        if (!strncmp(nm2, "__stack_chk_fail", 16) || !strncmp(nm2, "__stack_chk_guard", 17))
                            canary = true;
                        else if (strlen(nm2) > 6 && !strncmp(nm2, "__", 2) && !strcmp(nm2 + strlen(nm2) - 4, "_chk") && strncmp(nm2, "__stack", 7))
                            fortify = true;
                    }
                }
                free(str);
            }
        }
    }
    bool relro = false, nx = true, have_stack = false;
    for (unsigned i = 0; i < nph; i++) {
        if (ph[i].type == 0x6474e552)
            relro = true;
        if (ph[i].type == 0x6474e551) {
            have_stack = true;
            nx = !(ph[i].flags & 1);
        }
    }
    jb_kv(&b, &first, "stripped", has_symtab ? "no (has a symbol table)" : "yes (no symbol table)");
    if (has_symtab)
        jb_kvf(&b, &first, "symbols", "%u in the symbol table, %u dynamic", nsymtab, ndynsym);
    else if (ndynsym)
        jb_kvf(&b, &first, "symbols", "%u dynamic", ndynsym);
    jb_kv(&b, &first, "debug information", has_debug ? "yes" : "no");
    /* the compiler's comment */
    for (unsigned i = 0; i < nsec; i++)
        if (!strcmp(SEC_NAME(&sec[i]), ".comment") && sec[i].size && sec[i].size < 4096) {
            char cm[4096];
            if (rd(f, sec[i].off, cm, (size_t)sec[i].size)) {
                cm[sec[i].size] = 0;
                jb_key(&b, &first, "compiler");
                jb_add(&b, "[", 1);
                bool f2 = true;
                for (size_t o = 0; o < sec[i].size;) {
                    size_t l = strnlen(cm + o, (size_t)sec[i].size - o);
                    if (l) {
                        if (!f2)
                            jb_add(&b, ",", 1);
                        f2 = false;
                        jb_str(&b, cm + o, l);
                    }
                    o += l + 1;
                }
                jb_add(&b, "]", 1);
            }
        }
    if (type == 2 || type == 3) {
        jb_key(&b, &first, "hardening");
        jb_add(&b, "{", 1);
        bool f2 = true;
        jb_kv(&b, &f2, "PIE", pie ? "yes" : "no");
        jb_kv(&b, &f2, "RELRO", relro ? (bind_now ? "full" : "partial") : "no");
        jb_kv(&b, &f2, "BIND_NOW", bind_now ? "yes" : "no");
        jb_kv(&b, &f2, "non-executable stack", !have_stack ? "unknown (no PT_GNU_STACK)" : nx ? "yes" : "no");
        jb_kv(&b, &f2, "stack protector", canary ? "yes" : "no");
        jb_kv(&b, &f2, "FORTIFY", fortify ? "yes" : "no");
        jb_add(&b, "}", 1);
    }
    if (nph) {
        jb_key(&b, &first, "program headers");
        jb_add(&b, "[", 1);
        for (unsigned i = 0; i < nph; i++) {
            char fl[4], t[200];
            flags_text(ph[i].flags, fl);
            snprintf(t, sizeof(t), "%s %s offset 0x%llx vaddr 0x%llx filesz 0x%llx memsz 0x%llx", pt_name(ph[i].type), fl, (unsigned long long)ph[i].off, (unsigned long long)ph[i].vaddr,
                     (unsigned long long)ph[i].filesz, (unsigned long long)ph[i].memsz);
            if (i)
                jb_add(&b, ",", 1);
            jb_str(&b, t, strlen(t));
        }
        jb_add(&b, "]", 1);
    }
    if (nsec) {
        jb_key(&b, &first, "sections");
        jb_add(&b, "[", 1);
        for (unsigned i = 0; i < nsec; i++) {
            char t[300];
            snprintf(t, sizeof(t), "%s %s size %llu addr 0x%llx offset 0x%llx%s%s%s", SEC_NAME(&sec[i])[0] ? SEC_NAME(&sec[i]) : "(none)", sh_type_name(sec[i].type), (unsigned long long)sec[i].size,
                     (unsigned long long)sec[i].addr, (unsigned long long)sec[i].off, sec[i].flags & 2 ? " alloc" : "", sec[i].flags & 1 ? " write" : "", sec[i].flags & 4 ? " exec" : "");
            if (i)
                jb_add(&b, ",", 1);
            jb_str(&b, t, strlen(t));
        }
        jb_add(&b, "]", 1);
    }
    jb_add(&b, "}", 1);
    snprintf(info, infon, "ELF %d-bit %s %s%s%s", f->is64 ? 64 : 32, machine_name(machine),
             type == 3 && !interp[0] ? "shared library" : type == 3 ? "PIE executable" : tname, interp[0] || needed->n ? ", dynamically linked" : type == 2 ? ", static" : "",
             has_symtab ? "" : ", stripped");
    (void)eflags;
    free(needed);
    free(shstr);
    free(sec);
    return b.s;
}

/* ---- PE (.exe, .dll) ---- */
static const char *pe_machine(unsigned m)
{
    switch (m) {
    case 0x14c: return "x86";
    case 0x8664: return "x86-64";
    case 0x1c0: return "ARM";
    case 0x1c4: return "ARM Thumb-2";
    case 0xaa64: return "ARM64";
    case 0x200: return "IA-64";
    case 0x5064: return "RISC-V 64";
    default: return "unknown";
    }
}

static const char *pe_subsystem(unsigned s)
{
    switch (s) {
    case 1: return "native";
    case 2: return "Windows GUI";
    case 3: return "Windows console";
    case 5: return "OS/2 console";
    case 7: return "POSIX console";
    case 9: return "Windows CE";
    case 10: return "EFI application";
    case 11: return "EFI boot service driver";
    case 12: return "EFI runtime driver";
    case 14: return "Xbox";
    default: return "other";
    }
}

static char *pe_json(struct file *f, char *info, size_t infon)
{
    f->big = false;
    uint8_t dos[64];
    if (!rd(f, 0, dos, 64))
        return NULL;
    uint32_t peoff = g32(f, dos + 60);
    uint8_t h[24];
    if (!rd(f, peoff, h, 24) || memcmp(h, "PE\0\0", 4))
        return NULL;
    unsigned machine = g16(f, h + 4), nsec = g16(f, h + 6), optsize = g16(f, h + 20), chars = g16(f, h + 22);
    uint32_t stamp = g32(f, h + 8);
    uint8_t opt[240];
    size_t on = optsize < sizeof(opt) ? optsize : sizeof(opt);
    if (on < 96 || !rd(f, peoff + 24, opt, on))
        return NULL;
    unsigned magic = g16(f, opt);
    bool pe32plus = magic == 0x20b;
    unsigned subsystem = g16(f, opt + 68), dllchars = g16(f, opt + 70);
    uint64_t entry = g32(f, opt + 16), image_base = pe32plus ? g64(f, opt + 24) : g32(f, opt + 28);
    size_t dd_off = pe32plus ? 112 : 96;
    uint32_t imp_rva = 0;
    if (on >= dd_off + 16)
        imp_rva = g32(f, opt + dd_off + 8);
    struct jb b = { 0 };
    bool first = true;
    jb_add(&b, "{", 1);
    bool dll = chars & 0x2000;
    jb_kvf(&b, &first, "format", "PE (%s)", pe32plus ? "PE32+" : "PE32");
    jb_kvf(&b, &first, "type", "%s", dll ? "dynamic link library" : (chars & 2) ? "executable" : "object");
    jb_kvf(&b, &first, "machine", "%s", pe_machine(machine));
    jb_kvf(&b, &first, "subsystem", "%s", pe_subsystem(subsystem));
    jb_kvf(&b, &first, "entry point (RVA)", "0x%llx", (unsigned long long)entry);
    jb_kvf(&b, &first, "image base", "0x%llx", (unsigned long long)image_base);
    if (stamp) {
        time_t t = (time_t)stamp;
        struct tm tm;
        gmtime_r(&t, &tm);
        jb_kvf(&b, &first, "linked", "%04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    jb_kvf(&b, &first, "ASLR (dynamic base)", "%s", (dllchars & 0x40) ? "yes" : "no");
    jb_kvf(&b, &first, "DEP (NX compatible)", "%s", (dllchars & 0x100) ? "yes" : "no");
    jb_kvf(&b, &first, "control flow guard", "%s", (dllchars & 0x4000) ? "yes" : "no");
    /* sections */
    struct { char name[9]; uint32_t vsize, vaddr, rawsize, rawptr, chars; } secs[96];
    unsigned ns = 0;
    for (unsigned i = 0; i < nsec && i < 96; i++) {
        uint8_t s[40];
        if (!rd(f, (uint64_t)peoff + 24 + optsize + (uint64_t)i * 40, s, 40))
            break;
        memcpy(secs[ns].name, s, 8);
        secs[ns].name[8] = 0;
        secs[ns].vsize = g32(f, s + 8);
        secs[ns].vaddr = g32(f, s + 12);
        secs[ns].rawsize = g32(f, s + 16);
        secs[ns].rawptr = g32(f, s + 20);
        secs[ns].chars = g32(f, s + 36);
        ns++;
    }
    /* imported DLLs */
    if (imp_rva) {
        uint64_t off = 0;
        bool ok = false;
        for (unsigned i = 0; i < ns; i++)
            if (imp_rva >= secs[i].vaddr && imp_rva < secs[i].vaddr + (secs[i].rawsize > secs[i].vsize ? secs[i].rawsize : secs[i].vsize)) {
                off = secs[i].rawptr + (imp_rva - secs[i].vaddr);
                ok = true;
            }
        if (ok) {
            jb_key(&b, &first, "imports");
            jb_add(&b, "[", 1);
            bool f2 = true;
            for (unsigned k = 0; k < 400; k++) {
                uint8_t d[20];
                if (!rd(f, off + (uint64_t)k * 20, d, 20))
                    break;
                uint32_t name_rva = g32(f, d + 12);
                if (!name_rva && !g32(f, d))
                    break;
                for (unsigned i = 0; i < ns; i++)
                    if (name_rva >= secs[i].vaddr && name_rva < secs[i].vaddr + (secs[i].rawsize > secs[i].vsize ? secs[i].rawsize : secs[i].vsize)) {
                        char nm[128];
                        rd_str(f, secs[i].rawptr + (name_rva - secs[i].vaddr), nm, sizeof(nm));
                        if (!f2)
                            jb_add(&b, ",", 1);
                        f2 = false;
                        jb_str(&b, nm, strlen(nm));
                        break;
                    }
            }
            jb_add(&b, "]", 1);
        }
    }
    if (ns) {
        jb_key(&b, &first, "sections");
        jb_add(&b, "[", 1);
        for (unsigned i = 0; i < ns; i++) {
            char t[256];
            snprintf(t, sizeof(t), "%.8s virtual size %u at 0x%x, file size %u%s%s%s", secs[i].name, secs[i].vsize, secs[i].vaddr, secs[i].rawsize, secs[i].chars & 0x20000000 ? " exec" : "",
                     secs[i].chars & 0x40000000 ? " read" : "", secs[i].chars & 0x80000000u ? " write" : "");
            if (i)
                jb_add(&b, ",", 1);
            jb_str(&b, t, strlen(t));
        }
        jb_add(&b, "]", 1);
    }
    jb_add(&b, "}", 1);
    snprintf(info, infon, "PE %s %s, %s", pe32plus ? "32+" : "32", dll ? "DLL" : "executable", pe_machine(machine));
    return b.s;
}

char *exe_json(const char *path, char *info, size_t infon)
{
    info[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    uint8_t eh[64];
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 64) {
        close(fd);
        return NULL;
    }
    struct file f = { .fd = fd, .size = (uint64_t)st.st_size };
    char *out = NULL;
    if (rd(&f, 0, eh, 64) && !memcmp(eh, "\x7f" "ELF", 4) && (eh[4] == 1 || eh[4] == 2) && (eh[5] == 1 || eh[5] == 2)) {
        f.is64 = eh[4] == 2;
        f.big = eh[5] == 2;
        out = elf_json(&f, eh, info, infon);
    } else if (eh[0] == 'M' && eh[1] == 'Z') {
        out = pe_json(&f, info, infon);
    }
    close(fd);
    return out;
}
