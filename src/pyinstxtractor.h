#ifndef PYINST_EXTRACTOR_H
#define PYINST_EXTRACTOR_H
/*
 * pyinstxtractor - PyInstaller archive extractor
 * C rewrite by mayshecry, based on the original pyinstxtractor.py by
 * Extreme Coders <extremecoders(at)hotmail(dot)com>.
 * Licensed under the GNU General Public License v3.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/* PyInstaller cookie magic: MEI\014\013\012\013\016 */
#define PYINST_MAGIC "MEI\014\013\012\013\016"
#define PYINST_MAGIC_LEN 8

#define COOKIE_V20_SIZE 24            /* PyInstaller 2.0 cookie size      */
#define COOKIE_V21_SIZE (24 + 64)     /* PyInstaller 2.1+ cookie size     */

/* CArchive item type codes */
#define ARCHIVE_ITEM_DEPENDENCY   'd'
#define ARCHIVE_ITEM_RUNTIME_OPTION 'o'
#define ARCHIVE_ITEM_PYSOURCE     's'
#define ARCHIVE_ITEM_PYPACKAGE    'M'
#define ARCHIVE_ITEM_PYMODULE     'm'
#define ARCHIVE_ITEM_PYZ          'z'
#define ARCHIVE_ITEM_PYZ_UPPER    'Z'

/* A single entry from the CArchive table of contents */
typedef struct {
    off_t    position;             /* absolute offset into the file        */
    uint32_t cmprsd_data_size;     /* compressed size on disk              */
    uint32_t uncmprsd_data_size;   /* uncompressed size                    */
    uint8_t  cmprs_flag;           /* 1 = zlib compressed                  */
    uint8_t  type_cmprs_data;      /* item type code                       */
    char    *name;                 /* NUL-terminated member name           */
} toc_entry_t;

/* A single entry parsed from a PYZ archive marshal table of contents */
typedef struct {
    char    *name;
    int      ispkg;
    uint32_t pos;
    uint32_t length;
} pyz_entry_t;

/* Opaque archive handle */
typedef struct {
    FILE           *fp;
    const char     *file_path;
    size_t          file_size;
    off_t           cookie_pos;
    int             pyinst_ver;   /* 20 or 21                            */
    int             py_maj;       /* Python major version used to build  */
    int             py_min;       /* Python minor version used to build  */
    uint8_t         pyc_magic[4]; /* .pyc header magic, once discovered  */
    bool            pyc_magic_set;
    off_t           overlay_pos;  /* start of the appended archive       */
    off_t           table_of_contents_pos;
    size_t          table_of_contents_size;
    uint32_t        length_of_package;
    size_t          toc_count;
    toc_entry_t    *toc_entries;
    char           *extract_dir;
    bool            verbose;
    char          **bare_pycs;    /* .pyc files needing magic fixed later */
    size_t          bare_count;
    size_t          bare_cap;
} pyinst_archive_t;

/* High-level entry point. Returns 0 on success, non-zero on failure. */
int pyinst_extract(const char *exe_path, const char *output_dir, bool verbose);

/* Print usage to stdout */
void pyinst_print_usage(const char *progname);

#endif /* PYINST_EXTRACTOR_H */
