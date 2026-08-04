/* vtdump — feed a file (or stdin) through libvt, print the resulting grid.
 *
 * Usage: vtdump [-r rows] [-c cols] [file]
 * Output: one line per row; empty cells print '.', wide-char spacers '_'.
 * Used by golden-replay tests and manual conformance triage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vt/vt.h"

int main(int argc, char **argv) {
    uint16_t rows = 24, cols = 80;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) rows = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) cols = (uint16_t)atoi(argv[++i]);
        else path = argv[i];
    }

    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { perror(path); return 1; }

    vt *v = vt_new(rows, cols, NULL, NULL);
    if (!v) return 1;

    uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        vt_feed(v, buf, n);
    if (path) fclose(f);

    for (uint16_t r = 0; r < rows; r++) {
        const vt_cell *line = vt_line(v, r);
        for (uint16_t c = 0; c < cols; c++) {
            uint32_t cp = line[c].cp;
            if (line[c].attrs & VT_ATTR_WIDE_SPACER) { putchar('_'); continue; }
            if (cp == 0) { putchar('.'); continue; }
            if (cp < 0x80) { putchar((int)cp); continue; }
            char u[5] = {0};
            if (cp < 0x800) {
                u[0] = (char)(0xc0 | (cp >> 6));
                u[1] = (char)(0x80 | (cp & 0x3f));
            } else if (cp < 0x10000) {
                u[0] = (char)(0xe0 | (cp >> 12));
                u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
                u[2] = (char)(0x80 | (cp & 0x3f));
            } else {
                u[0] = (char)(0xf0 | (cp >> 18));
                u[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
                u[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
                u[3] = (char)(0x80 | (cp & 0x3f));
            }
            fputs(u, stdout);
        }
        putchar('\n');
    }

    uint16_t cr, cc;
    bool vis;
    vt_get_cursor(v, &cr, &cc, &vis);
    fprintf(stderr, "cursor: %u,%u %s modes: 0x%x\n", cr, cc,
            vis ? "visible" : "hidden", vt_get_modes(v));
    vt_free(v);
    return 0;
}
