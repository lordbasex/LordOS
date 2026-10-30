/* rules.h — Cleaner's compiled rule table (rules.c). There are no rule files and no commands: a rule
 * names a fixed location and picks from the fixed list of actions. */
#ifndef LORD_CLEAN_RULES_H
#define LORD_CLEAN_RULES_H

#include <stddef.h>

typedef enum { CLEAN_RISK_REGENERABLE, CLEAN_RISK_REVIEW, CLEAN_RISK_PROTECTED } CleanRisk;
typedef enum { CLEAN_ACT_NONE, CLEAN_ACT_TRASH } CleanAction;

typedef struct {
    const char *id;                 /* [a-z0-9-], at most 32 bytes */
    int version;
    const char *category;           /* [a-z0-9-] */
    const char *root;               /* relative to the home folder, at least two levels */
    const char *pattern;            /* file names: literal text with at most one `*` */
    int min_age_days;               /* 0 = any age */
    const char *const *exclude;     /* names (patterns) never offered, NULL-terminated; NULL = none */
    CleanRisk risk;
    CleanAction action;
    const char *text_key;           /* cleaner.rule.<id> in assets/locales/<lang>/cleaner.json */
} CleanRule;

const CleanRule *clean_rules(int *n);
/* Is this entry acceptable? 0 or -1 with the reason (a short English sentence, for tests and logs).
 * Needs fsops_init: a root can't be inside a protected path or hold one. */
int clean_rule_check(const CleanRule *r, char *why, size_t n);
/* Every entry of the table, and no id or root twice. */
int clean_rules_check(const CleanRule *t, int n, char *why, size_t wn);
const char *clean_risk_name(CleanRisk r);
const char *clean_action_name(CleanAction a);

#ifdef CLEAN_TEST
void clean_rules_override(const CleanRule *t, int n);   /* tests only: not in lord-server */
#endif

#endif
