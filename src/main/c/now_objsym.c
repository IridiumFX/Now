/*
 * now_objsym.c — read an object file's symbol table.
 *
 * Two formats, both only as far as "is this symbol defined here":
 *
 *   COFF  (MinGW .o and MSVC .obj)
 *   ELF64 (Linux .o)
 *
 * Everything is bounds-checked against the file length before it is
 * dereferenced. This parses attacker-adjacent input in the sense that
 * it parses whatever is on disk, and a truncated object from an
 * interrupted compile is the ordinary case rather than the exotic one.
 */
#include "now_objsym.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian readers, bounds-checked by the caller ---- */

static unsigned rd_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned long rd_u32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned long long rd_u64(const unsigned char *p) {
    return (unsigned long long)rd_u32(p) |
           ((unsigned long long)rd_u32(p + 4) << 32);
}

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (*len == 0) { free(buf); return NULL; }
    return buf;
}

/* ---- COFF ----
 *
 * Header is 20 bytes; symbols are 18 bytes each at PointerToSymbolTable;
 * the string table follows immediately after them. A symbol name is
 * either 8 inline bytes (NUL-padded, not NUL-terminated when exactly 8)
 * or, when the first four bytes are zero, an offset into that string
 * table. Defined-and-external means StorageClass 2 with a positive
 * SectionNumber — a section number of 0 is UNDEFINED, i.e. a reference
 * to a symbol defined elsewhere, which is precisely NOT what we are
 * looking for. */
#define COFF_SYM_SIZE      18
#define COFF_CLASS_EXTERNAL 2

static int coff_defines(const unsigned char *d, size_t len, const char *name,
                        int *other) {
    unsigned long symoff, nsyms, stroff;
    size_t namelen = strlen(name);
    int found = 0;

    if (len < 20) return -1;
    symoff = rd_u32(d + 8);
    nsyms  = rd_u32(d + 12);
    if (symoff == 0 || nsyms == 0) return 0;
    if (symoff > len) return -1;
    if (nsyms > (len - symoff) / COFF_SYM_SIZE) return -1;

    stroff = symoff + nsyms * COFF_SYM_SIZE;
    if (other) *other = 0;

    for (unsigned long i = 0; i < nsyms; i++) {
        const unsigned char *sym = d + symoff + i * COFF_SYM_SIZE;
        unsigned secnum = rd_u16(sym + 12);
        unsigned cls    = sym[16];
        unsigned naux   = sym[17];
        const char *sname = NULL;
        char inl[9];

        if (rd_u32(sym) == 0) {
            unsigned long off = rd_u32(sym + 4);
            if (stroff + off < len) sname = (const char *)(d + stroff + off);
        } else {
            memcpy(inl, sym, 8);
            inl[8] = '\0';
            sname = inl;
        }

        if (sname && cls == COFF_CLASS_EXTERNAL && secnum != 0 &&
            (short)secnum > 0) {
            /* MinGW x86_64 emits `main`; 32-bit targets emit `_main`.
             * Accept either rather than encoding an architecture. */
            int hit = (strncmp(sname, name, namelen) == 0 &&
                       sname[namelen] == '\0') ||
                      (sname[0] == '_' &&
                       strncmp(sname + 1, name, namelen) == 0 &&
                       sname[namelen + 1] == '\0');
            if (hit) {
                if (!other) return 1;
                found = 1;
            } else if (other) {
                (*other)++;
            }
        }
        i += naux;   /* auxiliary records are not symbols */
    }
    return found;
}

/* ---- ELF64 ----
 *
 * Walk the section headers for SHT_SYMTAB, whose sh_link names the
 * string table. A symbol is defined here when its st_shndx is not
 * SHN_UNDEF, and externally visible when its binding is GLOBAL or WEAK.
 */
#define SHT_SYMTAB   2
#define SHN_UNDEF    0
#define STB_GLOBAL   1
#define STB_WEAK     2

static int elf64_defines(const unsigned char *d, size_t len, const char *name,
                         int *other) {
    unsigned long long shoff;
    unsigned shentsize, shnum;
    int found = 0;

    if (other) *other = 0;

    if (len < 64) return -1;
    if (d[4] != 2) return -1;          /* not 64-bit; not handled */
    shoff     = rd_u64(d + 40);
    shentsize = rd_u16(d + 58);
    shnum     = rd_u16(d + 60);
    if (shentsize < 64 || shnum == 0) return -1;
    if (shoff > len || (len - shoff) / shentsize < shnum) return -1;

    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *sh = d + shoff + (size_t)i * shentsize;
        unsigned long type = rd_u32(sh + 4);
        unsigned long long off, size, entsize;
        unsigned long link;
        const unsigned char *strsh;
        unsigned long long stroff, strsize;

        if (type != SHT_SYMTAB) continue;

        off     = rd_u64(sh + 24);
        size    = rd_u64(sh + 32);
        link    = rd_u32(sh + 40);
        entsize = rd_u64(sh + 56);
        if (entsize < 24 || size == 0) return -1;
        if (off > len || len - off < size) return -1;
        if (link >= shnum) return -1;

        strsh   = d + shoff + (size_t)link * shentsize;
        stroff  = rd_u64(strsh + 24);
        strsize = rd_u64(strsh + 32);
        if (stroff > len || len - stroff < strsize) return -1;

        for (unsigned long long o = 0; o + entsize <= size; o += entsize) {
            const unsigned char *sym = d + off + o;
            unsigned long st_name = rd_u32(sym);
            unsigned info  = sym[4];
            unsigned shndx = rd_u16(sym + 6);
            unsigned bind  = info >> 4;

            if (shndx == SHN_UNDEF) continue;
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            if (st_name == 0 || st_name >= strsize) continue;
            if (strcmp((const char *)(d + stroff + st_name), name) == 0) {
                if (!other) return 1;
                found = 1;
            } else if (other) {
                (*other)++;
            }
        }
        return found;   /* symtab found and walked */
    }
    return 0;        /* no symtab: nothing is defined here */
}

static int obj_scan(const char *obj_path, const char *name, int *other) {
    size_t len = 0;
    unsigned char *d;
    int rc;

    if (!obj_path || !name || !*name) return -1;
    d = slurp(obj_path, &len);
    if (!d) return -1;

    if (len >= 4 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F')
        rc = elf64_defines(d, len, name, other);
    else
        rc = coff_defines(d, len, name, other);

    free(d);
    return rc;
}

NOW_API int now_obj_defines_symbol(const char *obj_path, const char *name) {
    return obj_scan(obj_path, name, NULL);
}

NOW_API int now_obj_other_global_count(const char *obj_path, const char *except) {
    int other = 0;
    if (obj_scan(obj_path, except, &other) < 0) return -1;
    return other;
}
