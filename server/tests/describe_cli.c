/* describe_cli.c — monitor/describe.c and exeown.c from the command line, for tests/describe_coverage.mjs (a test tool,
 * never installed): each line of stdin "kind arg lang" (kind: pkg, man, unit, owner) gets one line of JSON (or "null"). */
#include "../src/monitor/describe.h"
#include "../src/monitor/exeown.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    char line[4400];
    while (fgets(line, sizeof line, stdin)) {
        char kind[16], arg[4096], lang[16];
        if (sscanf(line, "%15s %4095s %15s", kind, arg, lang) != 3) { puts("null"); fflush(stdout); continue; }
        DescOut o;
        int r = -1;
        if (!strcmp(kind, "owner")) {
            const char *p = arg;
            char out[1][EXEOWN_PKG_MAX];
            r = exeown_scan(exeown_info_dir, &p, 1, out);
            printf(r >= 0 && out[0][0] ? "\"%s\"\n" : "null\n", out[0]);
            fflush(stdout);
            continue;
        }
        if (!strcmp(kind, "pkg")) r = desc_pkg(desc_status_path, desc_lists_dir, arg, lang, &o);
        else if (!strcmp(kind, "man")) r = desc_man(desc_man_root, arg, lang, &o);
        else if (!strcmp(kind, "unit")) r = desc_unit(desc_unit_dirs, arg, &o);
        if (r) puts("null");
        else {
            /* text and lang only, quotes and backslashes escaped */
            printf("{\"text\":\"");
            for (const char *s = o.text; *s; s++) { if (*s == '"' || *s == '\\') putchar('\\'); putchar(*s); }
            printf("\",\"lang\":\"%s\"}\n", o.lang);
        }
        fflush(stdout);
    }
    return 0;
}
