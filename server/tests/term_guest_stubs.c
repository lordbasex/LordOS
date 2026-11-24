/* term_guest_stubs.c — what term.o needs from the rest of lord-server (fonts, drawing, cgroups, the virtual console),
 * none of which term_guest_test.c reaches: no drawing, no font, no process. Each returns 0 / NULL if called. */
#include <stddef.h>
#define STUB(n) void *n(void); void *n(void) { return NULL; }
STUB(cgroup_enter)
STUB(damage_add)
STUB(fonts_by_ps)
STUB(fonts_color)
STUB(fonts_pick)
STUB(fonts_set_builtin_dir)
STUB(node_get)
STUB(term_row_visual)
STUB(text_font_add)
STUB(text_font_load)
STUB(text_font_vmetrics)
STUB(text_glyph)
STUB(vc_request_private)
