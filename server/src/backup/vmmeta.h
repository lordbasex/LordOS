/* vmmeta.h — what Backups reads from a machine's machine.json (engine.h, BkRun.vm_meta).
 *
 * A header so that the engine itself needs no JSON parser (tests/backup_test.c links it alone):
 * backup.c and tests/backup_machines_test.c pass bk_vm_meta, which uses src/sec/json.c.
 * Strict: the text must be a JSON object; "backup", when present, must be true or false; "name",
 * when present, must be text. Anything else is -1 and the machine is backed up. */
#ifndef LORD_BACKUP_VMMETA_H
#define LORD_BACKUP_VMMETA_H

#include "../sec/json.h"
#include "engine.h"

#include <stdio.h>

static inline int bk_vm_meta(const char *text, size_t len, int *backup, char *name, size_t n) {
    JDoc d;
    *backup = 1;
    if (n) name[0] = 0;
    if (len > BK_VM_JSON_MAX || json_parse(text, len, &d, NULL, 0)) return -1;
    int r = 0;
    const JVal *o = d.root;
    if (!o || o->type != J_OBJ) r = -1;
    const JVal *b = r ? NULL : json_get(o, "backup");
    if (b && b->type != J_BOOL) r = -1;
    const JVal *nm = r ? NULL : json_get(o, "name");
    if (nm && nm->type != J_STR) r = -1;
    if (!r) {
        if (b && !b->num) *backup = 0;
        if (nm && n) snprintf(name, n, "%s", nm->str);   /* bk_snapshot cleans it up */
    }
    json_free(&d);
    return r;
}

#endif
