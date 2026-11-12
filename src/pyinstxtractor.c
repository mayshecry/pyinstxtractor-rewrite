/*
 * pyinstxtractor.c - PyInstaller archive extractor
 *
 * C rewrite by mayshecry, based on the original pyinstxtractor.py by
 * Extreme Coders <extremecoders(at)hotmail(dot)com>
 * https://github.com/extremecoders-re/pyinstxtractor
 *
 * Locates the PyInstaller cookie appended to an executable, parses the
 * CArchive table of contents, extracts members (handling zlib compression),
 * fixes Python bytecode headers, and can unpack nested PYZ archives.
 *
 * Supported cookie formats: PyInstaller 2.0 and 2.1+ (which covers every
 * 3.x through 6.x release, since the cookie structure never changed).
 *
 * Build:
 *     cc -std=c99 -Wall -Wextra -O2 -o pyinstxtractor pyinstxtractor.c main.c -lz
 *
 * Usage:
 *     ./pyinstxtractor <executable> [output_directory]
 *
 * Licensed under the GNU General Public License v3, as is the original work.
 */

#include "pyinstxtractor.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Forward declarations for functions defined later in this file */
static char *xstrdup(const char *s);
static int extract_pyz(pyinst_archive_t *arch, const char *pyz_path);
static int fix_bare_pycs(pyinst_archive_t *arch);

/* strdup is POSIX; provide a portable fallback */
static char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (!d) return NULL;
    return memcpy(d, s, len);
}

/* -------------------------------------------------------------------------- */
/* Small utilities                                                            */
/* -------------------------------------------------------------------------- */

static bool read_exact(FILE *fp, void *buf, size_t len) {
    return fread(buf, 1, len, fp) == len;
}

/* Read a big-endian uint32 (PyInstaller stores all integers in network order) */
static bool read_be32(FILE *fp, uint32_t *out) {
    unsigned char b[4];
    if (!read_exact(fp, b, 4)) return false;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
    return true;
}

static uint32_t be32_ptr(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Recursively create directories. Safe to call on an existing directory. */
static int make_dirs(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/*
 * Sanitize a member name so it can never escape the extraction directory.
 *
 * Works on whole path components rather than substrings, because substring
 * tricks are easy to get wrong: a name like "../evil" contains no "/.."
 * sequence at all, yet it is the classic traversal attempt.
 *
 *   - backslashes are treated as separators (Windows-built archives)
 *   - empty and "." components are dropped
 *   - ".." components are replaced by "__"
 *   - components passing through unchanged keep their original spelling
 */
static void sanitize_path(const char *input, char *output, size_t outsz) {
    size_t out_len = 0;
    const char *p = input;

    if (outsz == 0) return;
    output[0] = '\0';

    while (*p != '\0') {
        while (*p == '/' || *p == '\\') p++;
        if (*p == '\0') break;

        const char *start = p;
        while (*p != '\0' && *p != '/' && *p != '\\') p++;
        size_t comp_len = (size_t)(p - start);

        if (comp_len == 1 && start[0] == '.') continue;

        bool is_dotdot = (comp_len == 2 && start[0] == '.' && start[1] == '.');
        const char *emit = is_dotdot ? "__" : start;
        size_t emit_len = is_dotdot ? 2 : comp_len;

        size_t need = (out_len ? 1 : 0) + emit_len;
        if (out_len + need + 1 > outsz) break; /* truncate rather than overflow */

        if (out_len) output[out_len++] = '/';
        memcpy(output + out_len, emit, emit_len);
        out_len += emit_len;
    }

    output[out_len] = '\0';

    /* An entirely empty name would silently collide; give it a stable name. */
    if (out_len == 0) {
        snprintf(output, outsz, "_unnamed");
    }
}

static int path_join(const char *dir, const char *name, char *out, size_t outsz) {
    int n = snprintf(out, outsz, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= outsz) return -1;
    return 0;
}

/* If path already exists, append _N until a free name is found. */
static char *ensure_unique(const char *path) {
    if (!file_exists(path)) return xstrdup(path);
    size_t len = strlen(path);
    char *buf = malloc(len + 64);
    if (!buf) return NULL;
    for (size_t i = 1; i < 100000; i++) {
        int n = snprintf(buf, len + 64, "%s_%zu", path, i);
        if (n < 0) { free(buf); return NULL; }
        if (!file_exists(buf)) return buf;
    }
    free(buf);
    return NULL;
}

/* zlib decompression with automatic output-buffer growth */
static int decompress_zlib(const unsigned char *src, size_t src_len,
                           unsigned char **dst, size_t *dst_len, size_t hint) {
    size_t dlen = hint ? hint : src_len * 2;
    if (dlen < 4096) dlen = 4096;
    unsigned char *out = malloc(dlen);
    if (!out) return Z_MEM_ERROR;

    int ret;
    uLongf dest_len = 0;
    for (;;) {
        dest_len = (uLongf)dlen;
        ret = uncompress(out, &dest_len, src, (uLong)src_len);
        if (ret != Z_BUF_ERROR) break;
        size_t ndlen = dlen * 2;
        unsigned char *tmp = realloc(out, ndlen);
        if (!tmp) { free(out); return Z_MEM_ERROR; }
        out = tmp;
        dlen = ndlen;
    }

    if (ret != Z_OK) { free(out); return ret; }
    *dst = out;
    *dst_len = (size_t)dest_len;
    return Z_OK;
}

/* -------------------------------------------------------------------------- */
/* .pyc header writing                                                        */
/* -------------------------------------------------------------------------- */

/* Write a whole buffer to a file, atomically enough for our purposes. */
static int write_file(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(path);
        return -1;
    }
    if (fclose(f) != 0) {
        remove(path);
        return -1;
    }
    return 0;
}

/*
 * Write a deterministic Python bytecode header followed by raw code data.
 * Mirrors PEP 552 layout for 3.7+, legacy layout for older 3.x.
 */
static int write_pyc(const char *filename, const unsigned char *data, size_t data_len,
                     const uint8_t *magic, int py_maj, int py_min) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;

    if (fwrite(magic, 1, 4, f) != 4) goto fail;

    if (py_maj >= 3 && py_min >= 7) {
        unsigned char zeros[12] = {0}; /* bitfield(4) + hash/timestamp(8) */
        if (fwrite(zeros, 1, 12, f) != 12) goto fail;
    } else {
        unsigned char zeros[4] = {0}; /* timestamp */
        if (fwrite(zeros, 1, 4, f) != 4) goto fail;
        if (py_maj >= 3 && py_min >= 3) { /* size field added in 3.3 */
            if (fwrite(zeros, 1, 4, f) != 4) goto fail;
        }
    }

    if (data_len > 0 && fwrite(data, 1, data_len, f) != data_len) goto fail;
    fclose(f);
    return 0;

fail:
    fclose(f);
    remove(filename);
    return -1;
}

/* Overwrite the first four bytes of an existing .pyc with the real magic */
static int fix_pyc_magic(const char *filename, const uint8_t *magic) {
    FILE *f = fopen(filename, "r+b");
    if (!f) return -1;
    if (fwrite(magic, 1, 4, f) != 4) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Archive low-level I/O                                                      */
/* -------------------------------------------------------------------------- */

static bool archive_open(pyinst_archive_t *arch) {
    arch->fp = fopen(arch->file_path, "rb");
    if (!arch->fp) {
        fprintf(stderr, "[!] Error: Could not open '%s'\n", arch->file_path);
        return false;
    }
    if (fseek(arch->fp, 0, SEEK_END) != 0) { fclose(arch->fp); return false; }
    long sz = ftell(arch->fp);
    if (sz < 0) { fclose(arch->fp); return false; }
    arch->file_size = (size_t)sz;
    rewind(arch->fp);
    return true;
}

static void archive_close(pyinst_archive_t *arch) {
    if (arch->fp) fclose(arch->fp);
    arch->fp = NULL;
}

/*
 * Scan backwards from the end of the file in 8 KiB chunks looking for the
 * PyInstaller magic cookie.
 */
static bool find_cookie(pyinst_archive_t *arch) {
    const size_t chunk = 8192;
    size_t end = arch->file_size;
    arch->cookie_pos = -1;

    if (end < PYINST_MAGIC_LEN) {
        fprintf(stderr, "[!] Error: File is too short or truncated\n");
        return false;
    }

    while (true) {
        size_t start = (end >= chunk) ? end - chunk : 0;
        size_t csize = end - start;
        if (csize < PYINST_MAGIC_LEN) break;

        if (fseek(arch->fp, (long)start, SEEK_SET) != 0) break;

        unsigned char *buf = malloc(csize);
        if (!buf) break;
        if (fread(buf, 1, csize, arch->fp) != csize) {
            free(buf);
            break;
        }

        ssize_t off = -1;
        for (size_t i = csize - PYINST_MAGIC_LEN; i-- > 0;) {
            if (memcmp(buf + i, PYINST_MAGIC, PYINST_MAGIC_LEN) == 0) {
                off = (ssize_t)i;
                break;
            }
        }
        free(buf);

        if (off != -1) {
            arch->cookie_pos = (off_t)(start + (size_t)off);
            break;
        }

        end = start + PYINST_MAGIC_LEN - 1;
        if (start == 0) break;
    }

    if (arch->cookie_pos == -1) {
        fprintf(stderr,
                "[!] Error: Missing cookie, unsupported PyInstaller version, "
                "or not a PyInstaller archive\n");
        return false;
    }

    /* Distinguish v2.0 from v2.1+: v2.1+ stores the python library name in
     * the 64 bytes immediately following the fixed-size cookie. */
    if (fseek(arch->fp, arch->cookie_pos + COOKIE_V20_SIZE, SEEK_SET) != 0)
        return false;
    unsigned char probe[64];
    if (fread(probe, 1, 64, arch->fp) != 64) return false;

    bool has_python = false;
    for (int i = 0; i + 5 < 64; i++) {
        if (probe[i] == 'p' || probe[i] == 'P') {
            if (tolower((unsigned char)probe[i + 1]) == 'y' &&
                tolower((unsigned char)probe[i + 2]) == 't' &&
                tolower((unsigned char)probe[i + 3]) == 'h' &&
                tolower((unsigned char)probe[i + 4]) == 'o' &&
                tolower((unsigned char)probe[i + 5]) == 'n') {
                has_python = true;
                break;
            }
        }
    }

    if (has_python) {
        arch->pyinst_ver = 21;
        printf("[+] PyInstaller version: 2.1+\n");
    } else {
        arch->pyinst_ver = 20;
        printf("[+] PyInstaller version: 2.0\n");
    }
    return true;
}

static bool get_carchive_info(pyinst_archive_t *arch) {
    if (fseek(arch->fp, arch->cookie_pos, SEEK_SET) != 0) return false;

    char magic[8];
    uint32_t length_of_package = 0, toc = 0, toc_len = 0, pyver = 0;
    if (!read_exact(arch->fp, magic, 8)) return false;
    if (!read_be32(arch->fp, &length_of_package)) return false;
    if (!read_be32(arch->fp, &toc)) return false;
    if (!read_be32(arch->fp, &toc_len)) return false;
    if (!read_be32(arch->fp, &pyver)) return false;

    if (memcmp(magic, PYINST_MAGIC, 8) != 0) {
        fprintf(stderr, "[!] Error: The file is not a PyInstaller archive\n");
        return false;
    }

    if (arch->pyinst_ver == 21) {
        char pylibname[64];
        if (!read_exact(arch->fp, pylibname, 64)) return false;
        (void)pylibname;
    }

    arch->py_maj = pyver >= 100 ? (int)(pyver / 100) : (int)(pyver / 10);
    arch->py_min = pyver >= 100 ? (int)(pyver % 100) : (int)(pyver % 10);
    printf("[+] Python version: %d.%d\n", arch->py_maj, arch->py_min);

    size_t cookie_size = (arch->pyinst_ver == 20) ? COOKIE_V20_SIZE : COOKIE_V21_SIZE;
    size_t tail_bytes = arch->file_size - (size_t)arch->cookie_pos - cookie_size;
    arch->length_of_package = length_of_package;
    size_t overlay_size = length_of_package + tail_bytes;
    arch->overlay_pos = (off_t)(arch->file_size - overlay_size);
    arch->table_of_contents_pos = arch->overlay_pos + (off_t)toc;
    arch->table_of_contents_size = (size_t)toc_len;

    printf("[+] Length of package: %u bytes\n", length_of_package);
    return true;
}

/* Parse the CArchive table of contents into toc_entries */
static bool parse_toc(pyinst_archive_t *arch) {
    if (fseek(arch->fp, arch->table_of_contents_pos, SEEK_SET) != 0) return false;

    toc_entry_t *list = NULL;
    size_t count = 0, cap = 0;
    size_t parsed = 0;

    while (parsed < arch->table_of_contents_size) {
        int32_t entry_size;
        if (!read_be32(arch->fp, (uint32_t *)&entry_size)) break;
        if (entry_size <= 0) break;

        /* Fixed portion after the length field:
         *   uint32 position
         *   uint32 compressed size
         *   uint32 uncompressed size
         *   uint8  compress flag
         *   char   type code
         *   char   name[entry_size - 4 - 14]
         */
        size_t fixed = 4 + 4 + 4 + 1 + 1; /* 14 bytes */
        if ((size_t)entry_size < 4 + fixed) break;

        size_t data_len = (size_t)entry_size - 4;
        unsigned char *buf = malloc(data_len);
        if (!buf) break;
        if (fread(buf, 1, data_len, arch->fp) != data_len) {
            free(buf);
            break;
        }

        uint32_t entry_pos = be32_ptr(buf);
        uint32_t csize     = be32_ptr(buf + 4);
        uint32_t usize     = be32_ptr(buf + 8);
        uint8_t  cflag     = buf[12];
        uint8_t  type      = buf[13];

        char *name = NULL;
        size_t name_len = data_len - fixed;
        if (name_len > 0) {
            name = malloc(name_len + 1);
            if (!name) { free(buf); break; }
            memcpy(name, buf + fixed, name_len);
            name[name_len] = '\0';
        } else {
            name = calloc(1, 1);
            if (!name) { free(buf); break; }
        }
        free(buf);

        /* Strip trailing NULs */
        size_t nl = strlen(name);
        while (nl > 0 && name[nl - 1] == '\0') name[--nl] = '\0';

        /* Path safety */
        if (name[0] == '/') {
            char *tmp = name;
            while (*tmp == '/') tmp++;
            memmove(name, tmp, strlen(tmp) + 1);
        }
        if (nl == 0) {
            char rnd[64];
            snprintf(rnd, sizeof(rnd), "unnamed_%zu", count);
            free(name);
            name = xstrdup(rnd);
            if (!name) break;
            printf("[!] Warning: Found an unnamed file in CArchive. "
                   "Using random name '%s'\n", rnd);
        }

        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            toc_entry_t *tmp = realloc(list, cap * sizeof(*list));
            if (!tmp) { free(name); free(list); return false; }
            list = tmp;
        }

        list[count].position = arch->overlay_pos + (off_t)entry_pos;
        list[count].cmprsd_data_size = csize;
        list[count].uncmprsd_data_size = usize;
        list[count].cmprs_flag = cflag;
        list[count].type_cmprs_data = type;
        list[count].name = name;
        count++;

        parsed += (size_t)entry_size;
    }

    arch->toc_entries = list;
    arch->toc_count = count;
    printf("[+] Found %zu files in CArchive\n", count);

    if (arch->verbose) {
        for (size_t i = 0; i < count; i++) {
            printf("    [%3zu] type '%c' %8u -> %8u bytes  %s\n", i,
                   (char)list[i].type_cmprs_data,
                   list[i].cmprsd_data_size, list[i].uncmprsd_data_size,
                   list[i].name);
        }
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Extraction                                                                 */
/* -------------------------------------------------------------------------- */

/* Build the full output path for a member inside extract_dir */
static char *output_path(pyinst_archive_t *arch, const char *member) {
    char safe[PATH_MAX];
    sanitize_path(member, safe, sizeof(safe));
    char full[PATH_MAX];
    if (path_join(arch->extract_dir, safe, full, sizeof(full)) != 0) return NULL;
    return xstrdup(full);
}

static int extract_files(pyinst_archive_t *arch) {
    if (!arch->extract_dir) {
        /* Default: "<executable>_extracted" alongside the executable. Doing
         * this relative to the executable rather than the working directory
         * means the tool can be run from anywhere without scattering output. */
        const char *slash = strrchr(arch->file_path, '/');
        const char *fname = slash ? slash + 1 : arch->file_path;
        char dflt[PATH_MAX];
        int n;
        if (slash) {
            n = snprintf(dflt, sizeof(dflt), "%.*s/%s_extracted",
                         (int)(slash - arch->file_path), arch->file_path, fname);
        } else {
            n = snprintf(dflt, sizeof(dflt), "%s_extracted", fname);
        }
        if (n < 0 || (size_t)n >= sizeof(dflt)) return -1;
        arch->extract_dir = xstrdup(dflt);
        if (!arch->extract_dir) return -1;
    }

    if (make_dirs(arch->extract_dir) != 0 && !file_exists(arch->extract_dir)) {
        fprintf(stderr, "[!] Error: Could not create extraction directory '%s'\n",
                arch->extract_dir);
        return -1;
    }

    printf("[+] Beginning extraction... please standby\n");

    for (size_t i = 0; i < arch->toc_count; i++) {
        toc_entry_t *e = &arch->toc_entries[i];

        if (fseek(arch->fp, (long)e->position, SEEK_SET) != 0) continue;

        unsigned char *comp = malloc(e->cmprsd_data_size ? e->cmprsd_data_size : 1);
        if (!comp) continue;
        if (e->cmprsd_data_size > 0 &&
            fread(comp, 1, e->cmprsd_data_size, arch->fp) != e->cmprsd_data_size) {
            free(comp);
            continue;
        }

        unsigned char *data = comp;
        size_t data_len = e->cmprsd_data_size;

        if (e->cmprs_flag == 1) {
            unsigned char *decomp = NULL;
            size_t decomp_len = 0;
            int ret = decompress_zlib(comp, e->cmprsd_data_size,
                                      &decomp, &decomp_len, e->uncmprsd_data_size);
            free(comp);
            if (ret != Z_OK) {
                printf("[!] Error: Failed to decompress '%s'\n", e->name);
                continue;
            }
            if (e->uncmprsd_data_size != 0 && decomp_len != e->uncmprsd_data_size) {
                printf("[!] Warning: Size mismatch for '%s' (got %zu, expected %u)\n",
                       e->name, decomp_len, e->uncmprsd_data_size);
            }
            data = decomp;
            data_len = decomp_len;
        }

        char type = (char)e->type_cmprs_data;

        /* Runtime options / dependencies are not files */
        if (type == ARCHIVE_ITEM_DEPENDENCY || type == ARCHIVE_ITEM_RUNTIME_OPTION) {
            free(data);
            continue;
        }

        char *out = output_path(arch, e->name);
        if (!out) { free(data); continue; }

        char base_path[PATH_MAX];
        strncpy(base_path, out, sizeof(base_path));
        base_path[sizeof(base_path) - 1] = '\0';
        char *slash = strrchr(base_path, '/');
        if (slash) *slash = '\0';
        if (base_path[0] != '\0' && make_dirs(base_path) != 0) {
            fprintf(stderr, "[!] Error: Could not create directory '%s'\n", base_path);
            free(out);
            free(data);
            continue;
        }

        if (type == ARCHIVE_ITEM_PYSOURCE) {
            printf("[+] Possible entry point: %s.pyc\n", e->name);
            char *pyc_path = malloc(strlen(out) + 8);
            if (pyc_path) {
                snprintf(pyc_path, strlen(out) + 8, "%s.pyc", out);
                char *unique = ensure_unique(pyc_path);
                if (unique) {
                    free(pyc_path);
                    pyc_path = unique;
                }
                if (!arch->pyc_magic_set) {
                    /* Bare pyc: write header with zeroed magic, fix later */
                    uint8_t zero_magic[4] = {0, 0, 0, 0};
                    if (write_pyc(pyc_path, data, data_len, zero_magic,
                                  arch->py_maj, arch->py_min) != 0) {
                        fprintf(stderr, "[!] Error: Could not write '%s'\n", pyc_path);
                    }
                    arch->bare_count++;
                    if (arch->bare_count > arch->bare_cap) {
                        arch->bare_cap = arch->bare_count ? arch->bare_count : 16;
                        char **tmp = realloc(arch->bare_pycs,
                                             arch->bare_cap * sizeof(char *));
                        if (!tmp) { free(pyc_path); free(out); free(data); continue; }
                        arch->bare_pycs = tmp;
                    }
                    arch->bare_pycs[arch->bare_count - 1] = pyc_path;
                } else {
                    if (write_pyc(pyc_path, data, data_len,
                                  arch->pyc_magic, arch->py_maj, arch->py_min) != 0) {
                        fprintf(stderr, "[!] Error: Could not write '%s'\n", pyc_path);
                    }
                    free(pyc_path);
                }
            }
            free(out);
            free(data);
            continue;
        }

        if (type == ARCHIVE_ITEM_PYPACKAGE || type == ARCHIVE_ITEM_PYMODULE) {
            char *pyc_path = malloc(strlen(out) + 8);
            if (pyc_path) {
                snprintf(pyc_path, strlen(out) + 8, "%s.pyc", out);
                char *unique = ensure_unique(pyc_path);
                if (unique) { free(pyc_path); pyc_path = unique; }

                /* PyInstaller < 5.3 stored the full pyc header inside the
                 * archive. We detect it by the CRLF signature at offset 2. */
                bool has_header = data_len >= 4 &&
                                  data[2] == '\r' && data[3] == '\n';

                if (has_header) {
                    if (!arch->pyc_magic_set) {
                        memcpy(arch->pyc_magic, data, 4);
                        arch->pyc_magic_set = true;
                    }
                    if (write_file(pyc_path, data, data_len) != 0) {
                        fprintf(stderr, "[!] Error: Could not write '%s'\n", pyc_path);
                    }
                } else {
                    if (!arch->pyc_magic_set) {
                        arch->bare_count++;
                        if (arch->bare_count > arch->bare_cap) {
                            arch->bare_cap = arch->bare_count ? arch->bare_count : 16;
                            char **tmp = realloc(arch->bare_pycs,
                                                 arch->bare_cap * sizeof(char *));
                            if (!tmp) { free(pyc_path); free(out); free(data); continue; }
                            arch->bare_pycs = tmp;
                        }
                        arch->bare_pycs[arch->bare_count - 1] = xstrdup(pyc_path);
                    }
                    if (write_pyc(pyc_path, data, data_len,
                                  arch->pyc_magic, arch->py_maj, arch->py_min) != 0) {
                        fprintf(stderr, "[!] Error: Could not write '%s'\n", pyc_path);
                    }
                    free(pyc_path);
                }
            }
            free(out);
            free(data);
            continue;
        }

        /* Any other type: raw copy */
        char *unique = ensure_unique(out);
        free(out);
        if (unique) {
            FILE *f = fopen(unique, "wb");
            if (f) {
                if (data_len > 0 && fwrite(data, 1, data_len, f) != data_len) {
                    /* ignore */
                }
                fclose(f);
            }
            if (type == ARCHIVE_ITEM_PYZ || type == ARCHIVE_ITEM_PYZ_UPPER) {
                extract_pyz(arch, unique);
            }
            free(unique);
        }

        free(data);
    }

    fix_bare_pycs(arch);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* PYZ archive extraction                                                     */
/* -------------------------------------------------------------------------- */

/*
 * Minimal Python marshal reader for format version 4 (Python 3.8+).
 *
 * Marshal v4 sets a high-bit flag (0x80) on some type codes, so 0xa9 and
 * 0x29 are both tuple, 0xe9 and 0x69 are both int, 0xda/0xfa/0x7a/0x5a are
 * all string. We mask off 0x80 before classifying.
 *
 * Base type codes (after masking 0x80):
 *   0x5b '[' list        -> int32 LE size, then that many items
 *   0x29 ')' tuple       -> uint8 size, then that many items
 *   0x7b '{' dict        -> key/value pairs, terminated by 0x30 '0'
 *   0x69 'i' int         -> int32 LE value
 *   0x7a 'z' / 0x5a 'Z'  string -> uint8 length, then that many bytes
 *   0x30 '0'             dict terminator
 *
 * A PYZ table of contents is either a list of (name, (ispkg, pos, length))
 * tuples (PyInstaller 3.1+) or a dict of name -> (ispkg, pos, length)
 * (older releases).
 */

/* An entry in the marshal "refs" table. Python's marshal adds an object to the
 * refs table only when its type byte carries the FLAG_REF (0x80) bit. Refs may
 * point at ints (which we need) or at other objects (which we record so that
 * their slot is not mistaken for an int slot).                                      */
typedef enum { MREF_OTHER = 0, MREF_INT } mref_kind_t;

typedef struct {
    mref_kind_t kind;
    int32_t     ival;
} mref_t;

typedef struct {
    const unsigned char *data;
    size_t               len;
    size_t               pos;
    mref_t              *refs;
    size_t               refs_count;
    size_t               refs_cap;
} mstate_t;

static bool m_byte(mstate_t *m, unsigned char *out) {
    if (m->pos >= m->len) return false;
    *out = m->data[m->pos++];
    return true;
}

static bool m_read_u32(mstate_t *m, uint32_t *out) {
    if (m->pos + 4 > m->len) return false;
    *out = (uint32_t)m->data[m->pos] |
           ((uint32_t)m->data[m->pos + 1] << 8) |
           ((uint32_t)m->data[m->pos + 2] << 16) |
           ((uint32_t)m->data[m->pos + 3] << 24);
    m->pos += 4;
    return true;
}

/* Record an object in the refs table when it carries the FLAG_REF bit. */
static bool m_note_ref(mstate_t *m, unsigned char type, mref_kind_t kind,
                       int32_t ival) {
    if (!(type & 0x80)) return true;
    if (m->refs_count == m->refs_cap) {
        size_t ncap = m->refs_cap ? m->refs_cap * 2 : 128;
        mref_t *tmp = realloc(m->refs, ncap * sizeof(*tmp));
        if (!tmp) return false;
        m->refs = tmp;
        m->refs_cap = ncap;
    }
    m->refs[m->refs_count].kind = kind;
    m->refs[m->refs_count].ival = ival;
    m->refs_count++;
    return true;
}

/* Look up a ref by index. Only ints are useful to us; anything else yields 0. */
static int32_t m_ref_get(const mstate_t *m, uint32_t index) {
    if (index >= m->refs_count) return 0;
    return m->refs[index].kind == MREF_INT ? m->refs[index].ival : 0;
}

/* Read a marshal int, following FLAG_REF type bytes (0xe9 direct, 0xf2 ref). */
static bool m_int(mstate_t *m, int32_t *out) {
    unsigned char t;
    if (!m_byte(m, &t)) return false;

    switch (t & 0x7f) {
    case 0x69: { /* TYPE_INT */
        uint32_t v;
        if (!m_read_u32(m, &v)) return false;
        *out = (int32_t)v;
        return m_note_ref(m, t, MREF_INT, *out);
    }
    case 0x72: { /* TYPE_REF */
        uint32_t index;
        if (!m_read_u32(m, &index)) return false;
        *out = m_ref_get(m, index);
        return true;
    }
    default:
        return false;
    }
}

/* Consume a tuple header, recording it in the refs table if flagged. */
static bool m_tuple_header(mstate_t *m, unsigned char *size_out) {
    unsigned char t;
    if (!m_byte(m, &t) || (t & 0x7f) != 0x29) return false; /* TYPE_TUPLE */
    if (!m_byte(m, size_out)) return false;
    return m_note_ref(m, t, MREF_OTHER, 0);
}

/* Read a marshal string whose type byte has already been consumed. */
static bool m_string_after_type(mstate_t *m, unsigned char t,
                                char **out, size_t *out_len) {
    if ((t & 0x7f) != 0x7a && (t & 0x7f) != 0x5a) return false;
    if (m->pos >= m->len) return false;
    uint8_t slen = m->data[m->pos++];
    if (m->pos + slen > m->len) return false;
    char *s = malloc((size_t)slen + 1);
    if (!s) return false;
    memcpy(s, m->data + m->pos, slen);
    s[slen] = '\0';
    m->pos += slen;
    if (!m_note_ref(m, t, MREF_OTHER, 0)) { free(s); return false; }
    *out = s;
    *out_len = (size_t)slen;
    return true;
}

static bool m_string(mstate_t *m, char **out, size_t *out_len) {
    unsigned char t;
    if (!m_byte(m, &t)) return false;
    return m_string_after_type(m, t, out, out_len);
}

/* Append a parsed PYZ entry to the dynamic array. */
static int pyz_entry_add(pyz_entry_t **entries, size_t *count, size_t *cap,
                         char *name, int ispkg, uint32_t pos, uint32_t length) {
    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        pyz_entry_t *tmp = realloc(*entries, ncap * sizeof(*tmp));
        if (!tmp) { free(name); return -1; }
        *entries = tmp;
        *cap = ncap;
    }
    (*entries)[*count].name = name;
    (*entries)[*count].ispkg = ispkg;
    (*entries)[*count].pos = pos;
    (*entries)[*count].length = length;
    (*count)++;
    return 0;
}

/* Parse the (ispkg, pos, length) tuple that follows a module name. */
static int pyz_parse_info(mstate_t *m, char *key,
                          pyz_entry_t **entries, size_t *count, size_t *cap) {
    unsigned char n;
    if (!m_tuple_header(m, &n) || n != 3) goto fail;

    int32_t ispkg, pos, length;
    if (!m_int(m, &ispkg) || !m_int(m, &pos) || !m_int(m, &length)) goto fail;

    return pyz_entry_add(entries, count, cap, key, ispkg,
                         (uint32_t)pos, (uint32_t)length);
fail:
    free(key);
    return -1;
}

/* Parse one (name, (ispkg, pos, length)) tuple from a list-style TOC. */
static int pyz_parse_list_item(mstate_t *m, pyz_entry_t **entries,
                               size_t *count, size_t *cap) {
    unsigned char n;
    if (!m_tuple_header(m, &n) || n != 2) return -1;

    char *key = NULL;
    size_t klen = 0;
    if (!m_string(m, &key, &klen)) return -1;

    return pyz_parse_info(m, key, entries, count, cap);
}

static int pyz_parse_toc(const unsigned char *data, size_t len,
                         pyz_entry_t **out_entries, size_t *out_count) {
    mstate_t m;
    memset(&m, 0, sizeof(m));
    m.data = data;
    m.len = len;

    pyz_entry_t *entries = NULL;
    size_t count = 0, cap = 0;

    unsigned char top;
    if (!m_byte(&m, &top)) goto done;

    if ((top & 0x7f) == 0x5b) { /* TYPE_LIST: list of tuples */
        uint32_t size;
        if (!m_note_ref(&m, top, MREF_OTHER, 0)) goto done;
        if (!m_read_u32(&m, &size)) goto done;
        for (uint32_t i = 0; i < size; i++) {
            if (pyz_parse_list_item(&m, &entries, &count, &cap) != 0)
                goto done;
        }
    } else if ((top & 0x7f) == 0x7b) { /* TYPE_DICT: name -> tuple, ends 0x30 */
        if (!m_note_ref(&m, top, MREF_OTHER, 0)) goto done;
        for (;;) {
            unsigned char t;
            if (!m_byte(&m, &t)) goto done;
            if ((t & 0x7f) == 0x30) break; /* TYPE_NULL terminator */

            char *key = NULL;
            size_t klen = 0;
            if (!m_string_after_type(&m, t, &key, &klen)) { free(key); goto done; }
            if (pyz_parse_info(&m, key, &entries, &count, &cap) != 0) goto done;
        }
    } else {
        goto done;
    }

    *out_entries = entries;
    *out_count = count;
    free(m.refs);
    return 0;

done:
    for (size_t j = 0; j < count; j++) free(entries[j].name);
    free(entries);
    free(m.refs);
    return -1;
}

static int extract_pyz(pyinst_archive_t *arch, const char *pyz_path) {
    char dir_name[PATH_MAX];
    int n = snprintf(dir_name, sizeof(dir_name), "%s_extracted", pyz_path);
    if (n < 0 || (size_t)n >= sizeof(dir_name)) return -1;
    if (make_dirs(dir_name) != 0 && !file_exists(dir_name)) {
        fprintf(stderr, "[!] Error: Could not create directory '%s'\n", dir_name);
        return -1;
    }

    FILE *f = fopen(pyz_path, "rb");
    if (!f) {
        fprintf(stderr, "[!] Error: Could not open '%s'\n", pyz_path);
        return -1;
    }

    unsigned char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PYZ\0", 4) != 0) {
        fprintf(stderr, "[!] Error: Not a valid PYZ archive\n");
        fclose(f);
        return -1;
    }

    unsigned char pyz_pyc_magic[4];
    if (fread(pyz_pyc_magic, 1, 4, f) != 4) { fclose(f); return -1; }

    if (!arch->pyc_magic_set) {
        memcpy(arch->pyc_magic, pyz_pyc_magic, 4);
        arch->pyc_magic_set = true;
    } else if (memcmp(arch->pyc_magic, pyz_pyc_magic, 4) != 0) {
        printf("[!] Warning: pyc magic inside PYZ differs from CArchive magic\n");
        memcpy(arch->pyc_magic, pyz_pyc_magic, 4);
    }

    /* TOC offset is a big-endian int32 right after the 8-byte header */
    uint32_t toc_pos;
    if (!read_be32(f, &toc_pos)) { fclose(f); return -1; }
    if (fseek(f, (long)toc_pos, SEEK_SET) != 0) { fclose(f); return -1; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long pyz_end = ftell(f);
    if (pyz_end < 0) { fclose(f); return -1; }
    size_t marshal_len = (size_t)((long)toc_pos < pyz_end
                                  ? pyz_end - (long)toc_pos : 0);
    unsigned char *marshal_data = malloc(marshal_len ? marshal_len : 1);
    if (!marshal_data) { fclose(f); return -1; }
    if (fseek(f, (long)toc_pos, SEEK_SET) != 0) {
        free(marshal_data); fclose(f); return -1;
    }
    if (marshal_len > 0 &&
        fread(marshal_data, 1, marshal_len, f) != marshal_len) {
        free(marshal_data); fclose(f); return -1;
    }
    fclose(f);

    pyz_entry_t *entries = NULL;
    size_t count = 0;
    if (pyz_parse_toc(marshal_data, marshal_len, &entries, &count) != 0) {
        fprintf(stderr, "[!] Warning: Unmarshalling PYZ archive failed; "
                        "skipping PYZ contents\n");
        free(marshal_data);
        return -1;
    }
    free(marshal_data);

    printf("[+] Found %zu files in PYZ archive\n", count);

    for (size_t i = 0; i < count; i++) {
        pyz_entry_t *e = &entries[i];

        /* Convert dotted module name to a path */
        char file_name[PATH_MAX];
        size_t klen = strlen(e->name);
        size_t j;
        for (j = 0; j < klen && j + 1 < sizeof(file_name); j++) {
            file_name[j] = (e->name[j] == '.') ? '/' : e->name[j];
        }
        file_name[j] = '\0';

        char file_path[PATH_MAX];
        if (e->ispkg == 1) {
            n = snprintf(file_path, sizeof(file_path), "%s/%s/__init__.pyc",
                         dir_name, file_name);
        } else {
            n = snprintf(file_path, sizeof(file_path), "%s/%s.pyc",
                         dir_name, file_name);
        }
        if (n < 0 || (size_t)n >= sizeof(file_path)) continue;

        char file_dir[PATH_MAX];
        strncpy(file_dir, file_path, sizeof(file_dir));
        file_dir[sizeof(file_dir) - 1] = '\0';
        char *dslash = strrchr(file_dir, '/');
        if (dslash) *dslash = '\0';
        make_dirs(file_dir);

        f = fopen(pyz_path, "rb");
        if (!f) continue;
        if (fseek(f, (long)e->pos, SEEK_SET) != 0) { fclose(f); continue; }
        unsigned char *comp = malloc(e->length ? e->length : 1);
        if (!comp) { fclose(f); continue; }
        if (e->length > 0 && fread(comp, 1, e->length, f) != e->length) {
            free(comp); fclose(f); continue;
        }
        fclose(f);

        if (e->length == 0) {
            printf("[!] Warning: Empty file %s\n", file_path);
            write_pyc(file_path, NULL, 0, arch->pyc_magic, arch->py_maj, arch->py_min);
            free(comp);
            continue;
        }

        unsigned char *decomp = NULL;
        size_t decomp_len = 0;
        int ret = decompress_zlib(comp, e->length, &decomp, &decomp_len, e->length);
        if (ret != Z_OK) {
            /* Encrypted PYZ entries are not zlib streams; keep the ciphertext
             * so the user can still get at it. */
            printf("[!] Error: Failed to decompress %s, probably encrypted. "
                   "Saving as .encrypted\n", file_path);
            char enc_path[PATH_MAX + 16];
            int en = snprintf(enc_path, sizeof(enc_path), "%s.encrypted", file_path);
            if (en >= 0 && (size_t)en < sizeof(enc_path)) {
                if (write_file(enc_path, comp, e->length) != 0)
                    fprintf(stderr, "[!] Error: Could not write '%s'\n", enc_path);
            }
            free(comp);
            continue;
        }
        free(comp);

        if (write_pyc(file_path, decomp, decomp_len,
                      arch->pyc_magic, arch->py_maj, arch->py_min) != 0) {
            fprintf(stderr, "[!] Error: Could not write '%s'\n", file_path);
        }
        free(decomp);
    }

    for (size_t i = 0; i < count; i++) free(entries[i].name);
    free(entries);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Bare .pyc magic fixing                                                     */
/* -------------------------------------------------------------------------- */

static int fix_bare_pycs(pyinst_archive_t *arch) {
    if (!arch->pyc_magic_set) {
        printf("[!] Warning: No pyc magic discovered; bare pyc files were "
               "written without a header\n");
        return 0;
    }
    for (size_t i = 0; i < arch->bare_count; i++) {
        if (fix_pyc_magic(arch->bare_pycs[i], arch->pyc_magic) != 0) {
            fprintf(stderr, "[!] Warning: Could not fix pyc header for '%s'\n",
                    arch->bare_pycs[i]);
        }
    }
    for (size_t i = 0; i < arch->bare_count; i++) free(arch->bare_pycs[i]);
    free(arch->bare_pycs);
    arch->bare_pycs = NULL;
    arch->bare_count = 0;
    arch->bare_cap = 0;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

int pyinst_extract(const char *exe_path, const char *output_dir, bool verbose) {
    pyinst_archive_t arch;
    memset(&arch, 0, sizeof(arch));
    arch.file_path = exe_path;
    arch.verbose = verbose;
    arch.cookie_pos = -1;

    if (output_dir) {
        arch.extract_dir = xstrdup(output_dir);
        if (!arch.extract_dir) return 1;
    }

    int rc = 1;
    if (!archive_open(&arch)) goto cleanup;
    if (!find_cookie(&arch)) goto cleanup;
    if (!get_carchive_info(&arch)) goto cleanup;
    if (!parse_toc(&arch)) goto cleanup;

    rc = extract_files(&arch);

cleanup:
    for (size_t i = 0; i < arch.toc_count; i++) free(arch.toc_entries[i].name);
    free(arch.toc_entries);
    for (size_t i = 0; i < arch.bare_count; i++) free(arch.bare_pycs[i]);
    free(arch.bare_pycs);
    free(arch.extract_dir);
    archive_close(&arch);

    if (rc == 0) {
        printf("[+] Successfully extracted PyInstaller archive: %s\n", exe_path);
        printf("\nYou can now use a Python decompiler on the .pyc files "
               "within the extracted directory\n");
    }
    return rc;
}

void pyinst_print_usage(const char *progname) {
    printf("Usage: %s [options] <executable> [output_directory]\n\n", progname);
    printf("Options:\n");
    printf("  -v, --verbose   Be verbose\n");
    printf("  -h, --help      Show this help\n");
    printf("\nExtracts a PyInstaller-generated executable into a\n");
    printf("<executable>_extracted directory (or the given output directory).\n");
}
