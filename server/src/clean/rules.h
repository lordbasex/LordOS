/* rules.h — Cleaner's compiled rule table (rules.c). There are no rule files and no commands: a rule
 * names a fixed location and picks from the fixed list of actions. */
#ifndef LORD_CLEAN_RULES_H
#define LORD_CLEAN_RULES_H

#include <stddef.h>

typedef enum { CLEAN_RISK_REGENERABLE, CLEAN_RISK_REVIEW, CLEAN_RISK_PROTECTED } CleanRisk;
typedef enum { CLEAN_ACT_NONE, CLEAN_ACT_TRASH, CLEAN_ACT_DELETE } CleanAction;

typedef struct {
    const char *id;                 /* [a-z0-9-], at most 32 bytes */
    int version;
    const char *category;           /* [a-z0-9-] */
    const char *root;               /* relative to the home folder, at least two levels; one level may be `*` */
    const char *pattern;            /* file names: literal text with at most one `*` */
    int min_age_days;               /* 0 = any age */
    const char *const *exclude;     /* names (patterns) never offered, NULL-terminated; NULL = none */
    CleanRisk risk;
    CleanAction action;             /* CLEAN_ACT_DELETE: regenerable rules only, approved 2026-10-30 */
    const char *text_key;           /* cleaner.rule.<id> in assets/locales/<lang>/cleaner.json */
    /* a process's lock: while it is alive the rule's folder isn't read or changed. Relative to the home
     * folder, $ stands for what `*` matched in root (a browser's profile) */
    const char *lock;
    int unit_depth;                 /* > 0: an entry this deep is one candidate as a whole (a model's folder) */
    int explicit_only;              /* never in a cleanup by category: chosen one by one, with its own confirmation */
    int on_mounts;                  /* root is relative to each of the user's mounts, with $ the user id: measured only */
    int downloads;                  /* root is the user's download folder (XDG_DOWNLOAD_DIR), with its name in their language */
    int leftover;                   /* a folder per app, offered only when that app is no longer installed */
    int system_root;                /* root is under `/`, not the home folder: the session user's own files, measured only */
} CleanRule;

const CleanRule *clean_rules(int *n);
/* Is this entry acceptable? 0 or -1 with the reason (a short English sentence, for tests and logs).
 * Needs fsops_init: a root can't be inside a protected path or hold one. */
int clean_rule_check(const CleanRule *r, char *why, size_t n);
/* Every entry of the table, and no id or root twice. */
int clean_rules_check(const CleanRule *t, int n, char *why, size_t wn);
const char *clean_risk_name(CleanRisk r);
const char *clean_action_name(CleanAction a);
/* does this path relative to the home folder fit a rule's root? star gets what `*` matched */
int clean_root_fits(const char *root, const char *rel, char *star, size_t n);

#ifdef CLEAN_TEST
void clean_rules_override(const CleanRule *t, int n);   /* tests only: not in lord-server */
#endif

#endif
