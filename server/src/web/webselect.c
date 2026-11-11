/* webselect.c — selecting text by dragging the mouse (docs/WEBAPPS.md, "Editing").
 *
 * WPE WebKit 2.48 is built without ENABLE_DRAG_SUPPORT (OptionsWPE.cmake leaves it off), and in WebCore that switch
 * also holds the code that extends a selection while the left button is dragged (EventHandler's
 * handleMouseDraggedEvent and updateSelectionForMouseDrag): a press places the caret, a double click selects a word,
 * a triple click the paragraph, Shift+click extends, but dragging across text selects nothing. A script of lord-web's
 * own, in an isolated world (the page's scripts can't see or change it), in every frame, does what WebCore would:
 *
 *   - a left press with no Ctrl, Alt or Meta, one click, on something that can be selected (not user-select: none,
 *     not a link, an image or a draggable element: those drag in a browser), whose default the page didn't prevent:
 *     the selection WebKit placed there is the anchor;
 *   - each move with the button held, past 4 px from the press, extends the selection to the point:
 *     document.caretRangeFromPoint for text and editable content (a contenteditable editor), and for a text field
 *     (input, textarea), whose inside caretRangeFromPoint can't reach, the character under the point measured with
 *     the field's own font (a canvas of the script's, never added to the page), lines wrapped as the field wraps them;
 *   - the release ends it. A drag near the top or bottom edge scrolls the page a little.
 *
 * Nothing else: no event is made or prevented, nothing is added to the page, and the selection is the page's own,
 * the one Copy (Ctrl+C, the Edit menu, the context menu) copies.
 *
 * The same script reports a right click the page left alone (its contextmenu event, trusted, not prevented by the
 * page's listeners on the element or the document) to lord-web, with what is under the pointer: an editable field, a
 * selection, a link's address, an image's, and prevents the event's default. WebKit 2.48's WPE port never asks its
 * embedder for a menu (the view's "context-menu" signal doesn't come) and, once its default action asked for one,
 * the view takes no more mouse input (seen on VM 127): so WebKit is never left to ask, and lord-web shows LordOS's
 * own menu from here (webengine.c, web_context_menu). The message handler exists only in this isolated world: the page's scripts can't post to it,
 * and lord-web takes a message only right after the person's own click or key in the window.
 *
 * And Ctrl+C (Ctrl+Insert) and Ctrl+A outside an editable field: WPE's WebEditorClient runs its key bindings only
 * where the page can be edited (handleKeyboardEvent returns when !canEdit()), so text the page shows could be
 * selected but never copied from the keyboard. The script reports the key (not prevented by the page) and lord-web
 * runs Copy or Select All. */
#include "web.h"
#include "webwin.h"
#include "../sec/json.h"

#include <string.h>

#define SELECT_WORLD "lordos-web-select"

static const char SELECT_JS[] =
    "(() => {\n"
    "  if (globalThis.__lordosSelect) return;\n"
    "  globalThis.__lordosSelect = true;\n"
    "  const TEXT_TYPES = ['', 'text', 'search', 'url', 'tel', 'email', 'password'];\n"
    "  const fieldOf = (t) => (t instanceof HTMLTextAreaElement || (t instanceof HTMLInputElement && TEXT_TYPES.includes(t.type))) ? t : null;\n"
    "  // where a browser drags rather than selects (a link, an image, a draggable element), or nothing selects\n"
    "  const blocked = (t) => { if (t.closest('a[href], img, [draggable=\"true\"]')) return true; const cs = getComputedStyle(t); return (cs.webkitUserSelect || cs.userSelect) === 'none'; };\n"
    "  let canvas = null;\n"
    "  const widths = new Map();\n"
    "  // the character index under (x, y) in a text field: its font, padding, scroll and wrapping\n"
    "  const fieldIndex = (f, x, y) => {\n"
    "    const cs = getComputedStyle(f), r = f.getBoundingClientRect();\n"
    "    canvas = canvas || document.createElement('canvas');\n"
    "    const ctx = canvas.getContext('2d');\n"
    "    const font = cs.font || `${cs.fontStyle} ${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;\n"
    "    if (ctx.font !== font) { ctx.font = font; widths.clear(); }\n"
    "    const pw = f.type === 'password';\n"
    "    const cw = (ch) => { const c = pw ? '\\u2022' : ch; let w = widths.get(c); if (w === undefined) { w = ctx.measureText(c).width; widths.set(c, w); } return w; };\n"
    "    const px = (v) => parseFloat(v) || 0;\n"
    "    const left = px(cs.paddingLeft) + px(cs.borderLeftWidth), top = px(cs.paddingTop) + px(cs.borderTopWidth);\n"
    "    const inner = f.clientWidth - px(cs.paddingLeft) - px(cs.paddingRight);\n"
    "    let lx = x - r.left - left + f.scrollLeft;\n"
    "    const ly = y - r.top - top + f.scrollTop;\n"
    "    if (cs.direction === 'rtl') lx = inner - lx;\n"
    "    const v = f.value;\n"
    "    const lines = [];\n"
    "    if (!(f instanceof HTMLTextAreaElement)) lines.push([0, v.length]);\n"
    "    else {\n"
    "      const wrap = cs.whiteSpace !== 'pre' && cs.whiteSpace !== 'nowrap';\n"
    "      let i = 0;\n"
    "      for (const para of v.split('\\n')) {\n"
    "        const end = i + para.length;\n"
    "        let ls = i, brk = -1, acc = 0;\n"
    "        for (let k = i; wrap && k < end; k++) {\n"
    "          const w = cw(v[k]);\n"
    "          if (acc + w > inner && k > ls) {\n"
    "            const cut = brk > ls ? brk : k;\n"
    "            lines.push([ls, cut]);\n"
    "            ls = cut; brk = -1; acc = 0;\n"
    "            for (let j = cut; j <= k; j++) acc += cw(v[j]);\n"
    "          } else acc += w;\n"
    "          if (v[k] === ' ' || v[k] === '\\t') brk = k + 1;\n"
    "        }\n"
    "        lines.push([ls, end]);\n"
    "        i = end + 1;\n"
    "      }\n"
    "    }\n"
    "    const lh = px(cs.lineHeight) || px(cs.fontSize) * 1.2;\n"
    "    const li = Math.max(0, Math.min(lines.length - 1, Math.floor(ly / lh)));\n"
    "    if (ly < 0 && lines.length > 1) return 0;\n"
    "    const [ls, le] = lines[li];\n"
    "    let acc = 0;\n"
    "    for (let k = ls; k < le; k++) { const w = cw(v[k]); if (acc + w / 2 > lx) return k; acc += w; }\n"
    "    return li < lines.length - 1 && le > ls && v[le - 1] === ' ' ? le - 1 : le;\n"
    "  };\n"
    "  let st = null;\n"
    "  addEventListener('mousedown', (e) => {\n"
    "    st = null;\n"
    "    if (!e.isTrusted || e.button !== 0 || e.detail > 1 || e.ctrlKey || e.altKey || e.metaKey) return;\n"
    "    const t = e.target instanceof Element ? e.target : e.target && e.target.parentElement;\n"
    "    if (!t || blocked(t)) return;\n"
    "    st = { down: e, x: e.clientX, y: e.clientY, field: fieldOf(t), on: false, anchor: null };\n"
    "  }, true);\n"
    "  addEventListener('mousemove', (e) => {\n"
    "    if (!st) return;\n"
    "    if (!(e.buttons & 1) || st.down.defaultPrevented) { st = null; return; }\n"
    "    if (!st.on) {\n"
    "      if (Math.abs(e.clientX - st.x) < 4 && Math.abs(e.clientY - st.y) < 4) return;\n"
    "      st.on = true;\n"
    "      // the anchor: where WebKit put the caret at the press (the other end after a Shift+click)\n"
    "      if (st.field) {\n"
    "        const f = st.field;\n"
    "        if (document.activeElement !== f) { st = null; return; }\n"
    "        st.anchor = f.selectionDirection === 'backward' ? f.selectionEnd : f.selectionStart;\n"
    "      } else {\n"
    "        const s = getSelection();\n"
    "        if (!s || !s.anchorNode) { st = null; return; }\n"
    "        st.anchor = { node: s.anchorNode, off: s.anchorOffset };\n"
    "      }\n"
    "    }\n"
    "    if (e.clientY < 6) scrollBy(0, -16);\n"
    "    else if (e.clientY > innerHeight - 6) scrollBy(0, 16);\n"
    "    if (st.field) {\n"
    "      const f = st.field, a = st.anchor, i = fieldIndex(f, e.clientX, e.clientY);\n"
    "      if (i < a) f.setSelectionRange(i, a, 'backward'); else f.setSelectionRange(a, i, 'forward');\n"
    "      return;\n"
    "    }\n"
    "    const x = Math.max(0, Math.min(innerWidth - 1, e.clientX)), y = Math.max(0, Math.min(innerHeight - 1, e.clientY));\n"
    "    const r = document.caretRangeFromPoint(x, y);\n"
    "    if (!r || !st.anchor.node.isConnected) return;\n"
    "    try { getSelection().setBaseAndExtent(st.anchor.node, st.anchor.off, r.startContainer, r.startOffset); } catch (err) { st = null; }\n"
    "  }, true);\n"
    "  addEventListener('mouseup', () => { st = null; }, true);\n"
    "  const h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lordosMenu;\n"
    "  // after the page's own listeners on the element and the document (registered first, this one runs before any\n"
    "  // the page adds to the window later); its default is always prevented: WPE 2.48's own menu never shows and, once\n"
    "  // asked for, leaves the view taking no more mouse input\n"
    "  if (h) addEventListener('contextmenu', (e) => {\n"
    "    if (!e.isTrusted) return;\n"
    "    const mine = !e.defaultPrevented;\n"
    "    e.preventDefault();\n"
    "    const t = e.target instanceof Element ? e.target : e.target && e.target.parentElement;\n"
    "    if (!mine || !t) return;\n"
    "    const f = fieldOf(t);\n"
    "    const editable = !!(f ? !f.readOnly && !f.disabled : t.isContentEditable);\n"
    "    const selection = f ? f.selectionEnd > f.selectionStart && f.type !== 'password' : String(getSelection() || '').length > 0;\n"
    "    const a = t.closest('a[href]'), img = t.closest('img');\n"
    "    h.postMessage(JSON.stringify({ editable, selection, link: a ? a.href : '', image: img ? (img.currentSrc || img.src || '') : '' }));\n"
    "  });\n"
    "  // Ctrl+C, Ctrl+Insert and Ctrl+A outside an editable field: WPE's key bindings run only where the page can be\n"
    "  // edited, so a message's text couldn't be copied from the keyboard; lord-web runs the command (after the page's\n"
    "  // own listeners, unless one prevented the key)\n"
    "  if (h) addEventListener('keydown', (e) => {\n"
    "    if (!e.isTrusted || e.defaultPrevented || !e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;\n"
    "    const k = String(e.key).toLowerCase();\n"
    "    const key = k === 'c' || k === 'insert' ? 'copy' : k === 'a' ? 'selectAll' : '';\n"
    "    const t = e.target instanceof Element ? e.target : null;\n"
    "    if (!key || (t && (fieldOf(t) || t.isContentEditable))) return;\n"
    "    h.postMessage(JSON.stringify({ key }));\n"
    "  });\n"
    "})();\n";

#define MENU_MSG_MAX (64 * 1024)

/* the script's report of a right click, { editable, selection, link, image }, or of a key, { key: copy|selectAll },
 * as JSON text */
static void on_menu_msg(WebKitUserContentManager *m, JSCValue *v, gpointer d) {
    (void)m;
    Web *w = d;
    if (!w->win || !webwin_user_active(w->win) || !jsc_value_is_string(v)) return;      /* only right after a click */
    char *s = jsc_value_to_string(v);
    JDoc doc;
    size_t n = s ? strlen(s) : 0;
    if (!s || n > MENU_MSG_MAX || json_parse(s, n, &doc, NULL, 0)) { g_free(s); return; }
    const JVal *r = doc.root;
    unsigned at = 0;
    const char *link = NULL, *image = NULL;
    const char *key = r && r->type == J_OBJ ? json_str(r, "key") : NULL;
    if (key) {
        if (!strcmp(key, "copy")) web_edit(w, WEBEDIT_COPY);
        else if (!strcmp(key, "selectAll")) web_edit(w, WEBEDIT_SELECT_ALL);
    } else if (r && r->type == J_OBJ) {
        const JVal *e = json_get(r, "editable"), *sel = json_get(r, "selection"), *l = json_get(r, "link"), *i = json_get(r, "image");
        if (e && e->type == J_BOOL && e->num) at |= WEBMENU_AT_EDITABLE;
        if (sel && sel->type == J_BOOL && sel->num) at |= WEBMENU_AT_SELECTION;
        if (l && l->type == J_STR && l->str[0]) link = l->str;
        if (i && i->type == J_STR && i->str[0]) image = i->str;
        web_context_menu(w, at, link, image);
    }
    json_free(&doc);
    g_free(s);
}

void webselect_init(Web *w) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(w->view);
    if (!ucm) return;
    if (webkit_user_content_manager_register_script_message_handler(ucm, "lordosMenu", SELECT_WORLD))
        g_signal_connect(ucm, "script-message-received::lordosMenu", G_CALLBACK(on_menu_msg), w);
    WebKitUserScript *s = webkit_user_script_new_for_world(SELECT_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                           SELECT_WORLD, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, s);
    webkit_user_script_unref(s);
}
