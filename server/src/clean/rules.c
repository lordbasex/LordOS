/* rules.c — see rules.h. Only well-known locations are rules: `~/.cache` as a whole is not one, since
 * applications keep sessions and databases there (docs/CLEANER-ARCHITECTURE.md, "Rules"). */
#include "rules.h"
#include "cleanfs.h"
#include "cleanproto.h"
#include "../fsops.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const CleanRule table[] = {
    { .id = "thumbnails", .version = 2, .category = "cache", .root = ".cache/thumbnails", .pattern = "*.png",
      .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE, .text_key = "cleaner.rule.thumbnails" },
    { .id = "fontconfig", .version = 1, .category = "cache", .root = ".cache/fontconfig", .pattern = "*",
      .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE, .text_key = "cleaner.rule.fontconfig" },
    { .id = "shader-cache", .version = 1, .category = "cache", .root = ".cache/mesa_shader_cache", .pattern = "*",
      .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE, .text_key = "cleaner.rule.shader-cache" },
    { .id = "shader-db", .version = 1, .category = "cache", .root = ".cache/mesa_shader_cache_db", .pattern = "*",
      .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE, .text_key = "cleaner.rule.shader-db" },
    { .id = "media-registry", .version = 1, .category = "cache", .root = ".cache/gstreamer-1.0", .pattern = "*",
      .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE, .text_key = "cleaner.rule.media-registry" },
    /* the name proves little, so review, a week old, and only to the Trash */
    { .id = "temporary", .version = 1, .category = "temporary", .root = ".cache/tmp", .pattern = "*", .min_age_days = 7,
      .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH, .text_key = "cleaner.rule.temporary" },
    /* shown and measured; only the real Trash empties it (lord.clean.emptyTrash) */
    { .id = "trash", .version = 1, .category = "trash", .root = ".local/share/Trash/files", .pattern = "*",
      .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE, .text_key = "cleaner.rule.trash" },
};
#define TABLE_N ((int)(sizeof table / sizeof *table))

#ifdef CLEAN_TEST
static const CleanRule *active = table;
static int active_n = TABLE_N;
void clean_rules_override(const CleanRule *t, int n) { active = t ? t : table; active_n = t ? n : TABLE_N; }
const CleanRule *clean_rules(int *n) { *n = active_n; return active; }
#else
const CleanRule *clean_rules(int *n) { *n = TABLE_N; return table; }
#endif

const char *clean_risk_name(CleanRisk r) {
    return r == CLEAN_RISK_REGENERABLE ? "regenerable" : r == CLEAN_RISK_REVIEW ? "review" : "protected";
}
const char *clean_action_name(CleanAction a) { return a == CLEAN_ACT_TRASH ? "trash" : a == CLEAN_ACT_DELETE ? "delete" : "none"; }

static int say(char *why, size_t n, const char *id, const char *what) {
    if (why && n) snprintf(why, n, "%s: %s", id ? id : "(no id)", what);
    return -1;
}

static int word_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > max) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return 0;
    return 1;
}

static int pattern_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    int stars = 0;
    if (!n || n > 32) return 0;
    for (; *s; s++) {
        if (*s == '*') stars++;
        else if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-')) return 0;
    }
    return stars <= 1;
}

int clean_rule_check(const CleanRule *r, char *why, size_t n) {
    static const char *const too_wide[] = { ".cache", ".config", ".local", ".local/share", ".local/state", NULL };
    if (!word_ok(r->id, 32)) return say(why, n, r->id, "bad id");
    if (r->version < 1) return say(why, n, r->id, "bad version");
    if (!word_ok(r->category, 32)) return say(why, n, r->id, "bad category");
    if (!r->root || !clean_relpath_ok(r->root, CLEAN_MAX_DEPTH)) return say(why, n, r->id, "bad root");
    if (!strchr(r->root, '/')) return say(why, n, r->id, "root is a top-level folder");
    for (int i = 0; too_wide[i]; i++) if (!strcmp(r->root, too_wide[i])) return say(why, n, r->id, "root is too wide");
    char abs[PATH_MAX + CLEAN_PATH_MAX + 2];
    if ((size_t)snprintf(abs, sizeof abs, "%s/%s", fsops_home(), r->root) >= sizeof abs) return say(why, n, r->id, "root too long");
    if (fsops_protected(abs)) return say(why, n, r->id, "root is protected");
    if (fsops_protected_below(abs)) return say(why, n, r->id, "root holds a protected path");
    if (!pattern_ok(r->pattern)) return say(why, n, r->id, "bad pattern");
    if (r->min_age_days < 0 || r->min_age_days > 3650) return say(why, n, r->id, "bad age");
    for (const char *const *e = r->exclude; e && *e; e++) if (!pattern_ok(*e)) return say(why, n, r->id, "bad exclusion");
    if (r->risk != CLEAN_RISK_REGENERABLE && r->risk != CLEAN_RISK_REVIEW && r->risk != CLEAN_RISK_PROTECTED) return say(why, n, r->id, "bad risk");
    if (r->action != CLEAN_ACT_NONE && r->action != CLEAN_ACT_TRASH && r->action != CLEAN_ACT_DELETE) return say(why, n, r->id, "bad action");
    if (r->action == CLEAN_ACT_DELETE && r->risk != CLEAN_RISK_REGENERABLE) return say(why, n, r->id, "delete is for regenerable rules only");
    if (r->risk == CLEAN_RISK_PROTECTED && r->action != CLEAN_ACT_NONE) return say(why, n, r->id, "a protected rule has an action");
    char key[64];
    snprintf(key, sizeof key, "cleaner.rule.%s", r->id);
    if (!r->text_key || strcmp(r->text_key, key)) return say(why, n, r->id, "bad text key");
    return 0;
}

int clean_rules_check(const CleanRule *t, int n, char *why, size_t wn) {
    if (n < 1 || n > CLEAN_MAX_ROOTS) return say(why, wn, NULL, "bad table size");
    for (int i = 0; i < n; i++) {
        if (clean_rule_check(&t[i], why, wn)) return -1;
        for (int j = 0; j < i; j++) {
            if (!strcmp(t[i].id, t[j].id)) return say(why, wn, t[i].id, "duplicate id");
            if (!strcmp(t[i].root, t[j].root)) return say(why, wn, t[i].id, "duplicate root");
        }
    }
    return 0;
}
