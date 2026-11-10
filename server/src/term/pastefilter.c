/* pastefilter.c — what a paste may type into the Terminal. A paste comes from the clipboard or the primary
 * selection, which any app (or a web page whose text is selected) can fill. Inside bracketed paste the shell takes
 * it as text until ESC[201~; removing that marker in one pass let "ESC[201" ESC[201~ "~" rebuild it and run what
 * followed. As xterm and VTE do, every control character but tab and the line ends is dropped instead, so no
 * escape sequence survives at all. Tests: tests/pastefilter_test.c. */
#include "pastefilter.h"

size_t paste_filter(const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r' && i + 1 < n && in[i + 1] == '\n') continue;     /* CRLF: one line end */
        if (c == '\n' || c == '\r') { out[o++] = '\r'; continue; }
        if (c == '\t') { out[o++] = '\t'; continue; }
        if (c < 0x20 || c == 0x7f) continue;                            /* ESC, the other C0 controls and DEL */
        out[o++] = (char)c;
    }
    return o;
}
