/*
 * main.c - Command-line front-end for pyinstxtractor
 */

#include "pyinstxtractor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *exe_path = NULL;
    const char *output_dir = NULL;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            pyinst_print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[!] Error: -o requires an argument\n");
                return 1;
            }
            output_dir = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[!] Error: Unknown option '%s'\n", argv[i]);
            pyinst_print_usage(argv[0]);
            return 1;
        } else if (!exe_path) {
            exe_path = argv[i];
        } else if (!output_dir) {
            output_dir = argv[i];
        } else {
            fprintf(stderr, "[!] Error: Too many positional arguments\n");
            pyinst_print_usage(argv[0]);
            return 1;
        }
    }

    if (!exe_path) {
        pyinst_print_usage(argv[0]);
        return 1;
    }

    return pyinst_extract(exe_path, output_dir, verbose);
}
