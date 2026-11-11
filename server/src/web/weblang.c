/* weblang.c — the few words lord-web shows itself (the tray menu, download notifications, the file chooser's title),
 * from LordOS's catalogs: assets/locales/<lang>/webruntime.json next to the binary (/usr/lib/lordos/assets, or the
 * source tree's assets/ for a development build), in the language of LC_ALL, LC_MESSAGES or LANG, English for a key
 * the language doesn't have. */
#include "web.h"
#include "../sec/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JDoc lang_doc, en_doc;

static void load(const char *lang, JDoc *doc) {
    const char *tries[] = { "%s/../assets/locales/%s/webruntime.json", "%s/../../assets/locales/%s/webruntime.json" };
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        char path[PATH_MAX + 64];
        snprintf(path, sizeof path, tries[i], web_self_dir(), lang);
        gchar *text = NULL;
        gsize len = 0;
        if (!g_file_get_contents(path, &text, &len, NULL)) continue;
        int bad = len > 64 * 1024 || json_parse(text, len, doc, NULL, 0) || !doc->root || doc->root->type != J_OBJ;
        g_free(text);
        if (bad) { json_free(doc); memset(doc, 0, sizeof *doc); continue; }
        return;
    }
}

void weblang_init(void) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    char lang[8] = "en";
    if (l && l[0] >= 'a' && l[0] <= 'z' && l[1] >= 'a' && l[1] <= 'z') { lang[0] = l[0]; lang[1] = l[1]; lang[2] = 0; }
    load("en", &en_doc);
    if (strcmp(lang, "en")) load(lang, &lang_doc);
}

const char *weblang(const char *key) {
    const char *s = lang_doc.root ? json_str(lang_doc.root, key) : NULL;
    if (!s || !s[0]) s = en_doc.root ? json_str(en_doc.root, key) : NULL;
    return s && s[0] ? s : key;
}
