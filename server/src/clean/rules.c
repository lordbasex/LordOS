/* rules.c — see rules.h. Only well-known locations are rules: `~/.cache` as a whole is not one, since
 * applications keep sessions and databases there (docs/CLEANER-ARCHITECTURE.md, "Rules"). */
#include "rules.h"
#include "cleanfs.h"
#include "cleanproto.h"
#include "../fsops.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *const installer_types[] = { "*.rpm", "*.AppImage", "*.iso", "*.dmg", "*.pkg", "*.exe", "*.msi", NULL };
static const char *const video_types[] = { "*.mov", "*.mkv", "*.webm", NULL };

static const CleanRule table[] = {
    /* caches anything rebuilds by itself */
    { .id = "thumbnails", .version = 1, .category = "cache", .root = ".cache/thumbnails", .pattern = "*.png", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.thumbnails" },
    { .id = "lordos-thumbnails", .version = 1, .category = "cache", .root = ".cache/axia/thumbs", .pattern = "*.png", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.lordos-thumbnails" },
    { .id = "fontconfig", .version = 1, .category = "cache", .root = ".cache/fontconfig", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.fontconfig" },
    { .id = "shader-cache", .version = 1, .category = "cache", .root = ".cache/mesa_shader_cache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.shader-cache" },
    { .id = "shader-db", .version = 1, .category = "cache", .root = ".cache/mesa_shader_cache_db", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.shader-db" },
    { .id = "media-registry", .version = 1, .category = "cache", .root = ".cache/gstreamer-1.0", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.media-registry" },
    /* package managers: what they keep from downloads, fetched again when needed */
    { .id = "pip", .version = 1, .category = "developer", .root = ".cache/pip", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.pip" },
    { .id = "npm", .version = 1, .category = "developer", .root = ".npm/_cacache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.npm" },
    { .id = "yarn", .version = 1, .category = "developer", .root = ".cache/yarn", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.yarn" },
    { .id = "go-build", .version = 1, .category = "developer", .root = ".cache/go-build", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.go-build" },
    /* projects hard-link into this store: it is not a cache to delete */
    { .id = "pnpm-store", .version = 1, .category = "developer", .root = ".local/share/pnpm/store", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.pnpm-store" },
    /* browsers: only the disk cache folders, never a profile; not while that browser runs */
    { .id = "firefox-cache", .version = 1, .category = "browser", .root = ".cache/mozilla/firefox/*/cache2", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.firefox-cache", .lock = ".mozilla/firefox/$/lock" },
    { .id = "firefox-startup", .version = 1, .category = "browser", .root = ".cache/mozilla/firefox/*/startupCache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.firefox-startup", .lock = ".mozilla/firefox/$/lock" },
    { .id = "chromium-cache", .version = 1, .category = "browser", .root = ".cache/chromium/*/Cache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chromium-cache", .lock = ".config/chromium/SingletonLock" },
    { .id = "chromium-code", .version = 1, .category = "browser", .root = ".cache/chromium/*/Code Cache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chromium-code", .lock = ".config/chromium/SingletonLock" },
    { .id = "chromium-gpu", .version = 1, .category = "browser", .root = ".config/chromium/*/GPUCache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chromium-gpu", .lock = ".config/chromium/SingletonLock" },
    { .id = "chromium-shader", .version = 1, .category = "browser", .root = ".config/chromium/ShaderCache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chromium-shader", .lock = ".config/chromium/SingletonLock" },
    { .id = "chromium-grshader", .version = 1, .category = "browser", .root = ".config/chromium/GrShaderCache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chromium-grshader", .lock = ".config/chromium/SingletonLock" },
    { .id = "chrome-cache", .version = 1, .category = "browser", .root = ".cache/google-chrome/*/Cache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chrome-cache", .lock = ".config/google-chrome/SingletonLock" },
    { .id = "chrome-code", .version = 1, .category = "browser", .root = ".cache/google-chrome/*/Code Cache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chrome-code", .lock = ".config/google-chrome/SingletonLock" },
    { .id = "chrome-gpu", .version = 1, .category = "browser", .root = ".config/google-chrome/*/GPUCache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chrome-gpu", .lock = ".config/google-chrome/SingletonLock" },
    { .id = "chrome-shader", .version = 1, .category = "browser", .root = ".config/google-chrome/ShaderCache", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.chrome-shader", .lock = ".config/google-chrome/SingletonLock" },
    /* the name proves little: reviewed, aged, and only to the Trash */
    { .id = "temporary", .version = 1, .category = "temporary", .root = ".cache/tmp", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.temporary", .min_age_days = 7 },
    { .id = "partial-downloads", .version = 1, .category = "temporary", .root = "Downloads", .pattern = "*.part", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.partial-downloads", .min_age_days = 7, .downloads = 1 },
    { .id = "partial-downloads-chromium", .version = 1, .category = "temporary", .root = "Downloads", .pattern = "*.crdownload", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.partial-downloads-chromium", .min_age_days = 7, .downloads = 1 },
    { .id = "crash-system", .version = 1, .category = "logs", .root = ".local/share/apport", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.crash-system", .min_age_days = 7 },
    { .id = "crash-firefox", .version = 1, .category = "logs", .root = ".mozilla/firefox/Crash Reports", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.crash-firefox", .min_age_days = 7 },
    { .id = "crash-chromium", .version = 1, .category = "logs", .root = ".config/chromium/Crash Reports", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.crash-chromium", .min_age_days = 7 },
    { .id = "logs-npm", .version = 1, .category = "logs", .root = ".npm/_logs", .pattern = "*.log", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.logs-npm", .min_age_days = 30 },
    { .id = "logs-display", .version = 1, .category = "logs", .root = ".local/share/xorg", .pattern = "*.old", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.logs-display", .min_age_days = 30 },
    /* downloaded by tools that fetch them again; extracted Go modules are read-only, so only its download cache */
    { .id = "playwright", .version = 1, .category = "developer", .root = ".cache/ms-playwright", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.playwright" },
    { .id = "puppeteer", .version = 1, .category = "developer", .root = ".cache/puppeteer", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.puppeteer" },
    { .id = "go-modules", .version = 1, .category = "developer", .root = "go/pkg/mod/cache/download", .pattern = "*", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.go-modules" },
    /* no process lock can be read for these, or a project may hold the only copy: reviewed, to the Trash */
    { .id = "jetbrains", .version = 1, .category = "developer", .root = ".cache/JetBrains", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.jetbrains" },
    { .id = "dart-pub", .version = 1, .category = "developer", .root = ".pub-cache/hosted", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.dart-pub" },
    { .id = "maven", .version = 1, .category = "developer", .root = ".m2/repository", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.maven" },
    { .id = "flatpak-cache", .version = 1, .category = "cache", .root = ".var/app/*/cache", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.flatpak-cache" },
    /* old installers and big recordings: the user's own files, reviewed */
    { .id = "installers", .version = 1, .category = "old-files", .root = "Downloads", .pattern = "*.deb", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.installers", .also = installer_types, .min_age_days = 30, .downloads = 1 },
    { .id = "recordings-desktop", .version = 1, .category = "old-files", .root = "Desktop", .pattern = "*.mp4", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.recordings-desktop", .also = video_types, .min_size = 104857600, .min_age_days = 7, .explicit_only = 1, .desktop = 1 },
    { .id = "recordings", .version = 1, .category = "old-files", .root = "Videos/Screencasts", .pattern = "*.mp4", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.recordings", .also = video_types, .min_size = 104857600, .explicit_only = 1 },
    /* AI models: chosen one by one, named in the confirmation, never in a cleanup by category. A folder per
     * model. Ollama shares blobs between models, so a model can't be removed by moving files: it is measured only */
    { .id = "ai-huggingface", .version = 1, .category = "ai", .root = ".cache/huggingface/hub", .pattern = "models--*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.ai-huggingface", .unit_depth = 1, .explicit_only = 1 },
    { .id = "ai-huggingface-partial", .version = 1, .category = "ai", .root = ".cache/huggingface/hub/*/blobs", .pattern = "*.incomplete", .risk = CLEAN_RISK_REGENERABLE, .action = CLEAN_ACT_DELETE,
      .text_key = "cleaner.rule.ai-huggingface-partial", .explicit_only = 1 },
    { .id = "ai-lmstudio", .version = 1, .category = "ai", .root = ".lmstudio/models", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.ai-lmstudio", .unit_depth = 2, .explicit_only = 1 },
    { .id = "ai-lmstudio-old", .version = 1, .category = "ai", .root = ".cache/lm-studio/models", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.ai-lmstudio-old", .unit_depth = 2, .explicit_only = 1 },
    { .id = "ai-ollama", .version = 1, .category = "ai", .root = ".ollama/models", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.ai-ollama" },
    /* what a removed LordOS app left: its folder, whole, chosen by name. An installed app's folder is never offered */
    { .id = "app-leftovers", .version = 1, .category = "apps", .root = ".local/share/axia/appdata", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.app-leftovers", .unit_depth = 1, .explicit_only = 1, .leftover = 1 },
    /* shown and measured, nothing offered */
    { .id = "old-downloads", .version = 1, .category = "info", .root = "Downloads", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.old-downloads", .min_age_days = 90, .downloads = 1 },
    /* the system's temporary folder: who else uses a file there can't be shown, so it is never offered */
    { .id = "system-tmp", .version = 1, .category = "info", .root = "tmp", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.system-tmp", .min_age_days = 7, .system_root = 1 },
    { .id = "cargo-registry", .version = 1, .category = "info", .root = ".cargo/registry", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.cargo-registry" },
    { .id = "gradle", .version = 1, .category = "info", .root = ".gradle/caches", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.gradle" },
    { .id = "containers", .version = 1, .category = "info", .root = ".local/share/docker", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.containers" },
    /* only the real Trash empties these (lord.clean.emptyTrash) */
    { .id = "trash", .version = 1, .category = "trash", .root = ".local/share/Trash/files", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.trash" },
    { .id = "trash-disks", .version = 1, .category = "trash", .root = ".Trash-$/files", .pattern = "*", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_NONE,
      .text_key = "cleaner.rule.trash-disks", .on_mounts = 1 },
    /* build outputs in the home folder's projects, each proved by the files around it (clean_project_kind) */
    { .id = "project-node", .version = 1, .category = "projects", .root = ".", .pattern = "node_modules", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-node", .explicit_only = 1, .project = 1, .min_age_days = 60 },
    { .id = "project-rust", .version = 1, .category = "projects", .root = ".", .pattern = "target", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-rust", .explicit_only = 1, .project = 2 },
    { .id = "project-maven", .version = 1, .category = "projects", .root = ".", .pattern = "target", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-maven", .explicit_only = 1, .project = 3 },
    { .id = "project-js", .version = 1, .category = "projects", .root = ".", .pattern = ".next", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-js", .explicit_only = 1, .project = 4 },
    { .id = "project-python-tools", .version = 1, .category = "projects", .root = ".", .pattern = ".pytest_cache", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-python-tools", .explicit_only = 1, .project = 5 },
    { .id = "project-venv", .version = 1, .category = "projects", .root = ".", .pattern = ".venv", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-venv", .explicit_only = 1, .project = 6 },
    { .id = "project-gradle", .version = 1, .category = "projects", .root = ".", .pattern = "build", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-gradle", .explicit_only = 1, .project = 7 },
    { .id = "project-dart", .version = 1, .category = "projects", .root = ".", .pattern = "build", .risk = CLEAN_RISK_REVIEW, .action = CLEAN_ACT_TRASH,
      .text_key = "cleaner.rule.project-dart", .explicit_only = 1, .project = 8 },
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
    if (r->project) {                               /* no fixed root: only an output a project proves, chosen, to the Trash */
        if (r->project < 1 || r->project > CLEAN_PROJECT_KINDS) return say(why, n, r->id, "bad project kind");
        if (r->action != CLEAN_ACT_TRASH || r->risk != CLEAN_RISK_REVIEW || !r->explicit_only) return say(why, n, r->id, "a project output is chosen, to the Trash");
        if (r->also || r->lock || r->unit_depth || r->downloads || r->desktop || r->leftover || r->on_mounts || r->system_root) return say(why, n, r->id, "a project rule has no other location");
        if (r->min_age_days < clean_project_idle_days(r->project - 1)) return say(why, n, r->id, "younger than the kind's idle time");
        if (!pattern_ok(r->pattern) || !clean_project_named(r->project - 1, r->pattern)) return say(why, n, r->id, "bad pattern");
        char want[64];
        snprintf(want, sizeof want, "cleaner.rule.%s", r->id);
        return r->text_key && !strcmp(r->text_key, want) ? 0 : say(why, n, r->id, "bad text key");
    }
    if (!r->root || !clean_relpath_ok(r->root, CLEAN_MAX_DEPTH)) return say(why, n, r->id, "bad root");
    /* a folder of the user's own, like Downloads: only a narrow name, or nothing offered */
    if (!strchr(r->root, '/') && r->action != CLEAN_ACT_NONE && (!strcmp(r->pattern ? r->pattern : "*", "*") || !r->min_age_days)) return say(why, n, r->id, "root is a top-level folder");
    int stars = 0;
    for (const char *p = r->root; (p = strchr(p, '*')); p++) {
        stars++;
        if ((p != r->root && p[-1] != '/') || (p[1] && p[1] != '/')) return say(why, n, r->id, "* must be a whole folder name");
    }
    if (stars > 1) return say(why, n, r->id, "more than one * in the root");
    if (r->unit_depth < 0 || r->unit_depth > 4) return say(why, n, r->id, "bad unit depth");
    if (r->on_mounts && r->action != CLEAN_ACT_NONE) return say(why, n, r->id, "a root on mounts is only measured");
    if (r->system_root && (r->action != CLEAN_ACT_NONE || strcmp(r->root, "tmp"))) return say(why, n, r->id, "a system root is /tmp, only measured");
    if (r->leftover && (r->action != CLEAN_ACT_TRASH || r->unit_depth != 1 || !r->explicit_only)) return say(why, n, r->id, "a leftover is one folder, chosen, to the Trash");
    if (r->lock && (!*r->lock || r->lock[0] == '/' || strstr(r->lock, ".."))) return say(why, n, r->id, "bad lock");
    for (int i = 0; too_wide[i]; i++) if (!strcmp(r->root, too_wide[i])) return say(why, n, r->id, "root is too wide");
    char abs[PATH_MAX + CLEAN_PATH_MAX + 2];
    if ((size_t)snprintf(abs, sizeof abs, "%s/%s", fsops_home(), r->root) >= sizeof abs) return say(why, n, r->id, "root too long");
    if (fsops_protected(abs)) return say(why, n, r->id, "root is protected");
    if (fsops_protected_below(abs)) return say(why, n, r->id, "root holds a protected path");
    if (!pattern_ok(r->pattern)) return say(why, n, r->id, "bad pattern");
    for (const char *const *e = r->also; e && *e; e++) if (!pattern_ok(*e) || !strcmp(*e, "*")) return say(why, n, r->id, "bad extra pattern");
    if (r->min_size < 0) return say(why, n, r->id, "bad size");
    if (r->downloads && r->desktop) return say(why, n, r->id, "one user folder");
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
    if (n < 1 || n > CLEAN_MAX_RULES) return say(why, wn, NULL, "bad table size");
    for (int i = 0; i < n; i++) {
        if (clean_rule_check(&t[i], why, wn)) return -1;
        for (int j = 0; j < i; j++) {
            if (!strcmp(t[i].id, t[j].id)) return say(why, wn, t[i].id, "duplicate id");
            if (!t[i].project && !strcmp(t[i].root, t[j].root) && !strcmp(t[i].pattern, t[j].pattern)) return say(why, wn, t[i].id, "duplicate root");
        }
    }
    return 0;
}

int clean_root_fits(const char *root, const char *rel, char *star, size_t n) {
    const char *st = strchr(root, '*');
    if (star && n) star[0] = 0;
    if (!st) return !strcmp(root, rel);
    size_t head = (size_t)(st - root), tail = strlen(st + 1), len = strlen(rel);
    if (len <= head + tail || strncmp(rel, root, head) || strcmp(rel + len - tail, st + 1)) return 0;
    size_t m = len - head - tail;
    if (memchr(rel + head, '/', m) || (star && m >= n)) return 0;
    if (star) { memcpy(star, rel + head, m); star[m] = 0; }
    return 1;
}
