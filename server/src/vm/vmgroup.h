/* vmgroup.h — a machine group's description, group.json (docs/VIRTUALIZATION.md 19.2 and 19.4; approved by the
 * maintainer on 2026-11-23). One folder per group, in the library:
 *
 *   <library>/<name>.lordgroup/group.json          this description (schema 1)
 *   <library>/<name>.lordgroup/<member>.lordvm/    each member: an ordinary machine folder (machine.h), whose
 *                                                  machine.json names the group's id and the member's role
 *
 * A group adds an order and a link between its members, not a new kind of process: each member keeps its own lord-vm,
 * QEMU, sandbox and lock. The only kind for now is "whonix" (a Tor gateway and a workstation wired only to it); the
 * format names roles and orders so that other kinds can come later without a new schema.
 *
 * Parsing is strict, as for machine.json: unknown keys, wrong types and values out of range are refused, and a newer
 * schema is opened read-only (VM_E_NEWER) and never started. Pure functions over bytes, apart from load and save; its
 * test is tests/vmgroup_test.c (with a fuzz pass under ASan). */
#ifndef LORD_VM_GROUP_H
#define LORD_VM_GROUP_H

#include "machine.h"

#include <stddef.h>

#define VMG_SCHEMA       1
#define VMG_MEMBERS_MAX  4
#define VMG_DIR_MAX      200            /* a member's folder name, ".lordvm" included */
#define VMG_RELEASE_MAX  32
#define VMG_EDITION_MAX  16
#define VMG_CATALOG_MAX  32
#define VMG_SUFFIX       ".lordgroup"

/* the group's kind ("kind") */
enum { VMG_KIND_WHONIX = 1 };
/* a member's role: the same numbers as machine.json's "role" (VM_ROLE_*, machine.h) */

typedef struct {
    int role;                           /* VM_ROLE_* */
    char dir[VMG_DIR_MAX + 1];          /* a plain folder name ending ".lordvm", inside the group's folder */
    int start, stop;                    /* its place in the start and the stop order, 1..n */
    int primary;                        /* the member the group's card shows and `machines open` opens */
} VmGroupMember;

typedef struct {
    int schema;
    char id[37];                        /* random UUID made once: the group's id, in each member's machine.json "group" */
    char name[VM_NAME_MAX + 1];
    int kind;                           /* VMG_KIND_* */
    int n;
    VmGroupMember m[VMG_MEMBERS_MAX];
    /* the image the members were made from (19.6): which catalog entry, release and edition, the verified archive's
     * SHA-512, and each role's read-only base with its SHA-256 (indexed by role). "" when not set */
    int has_image;
    char catalog[VMG_CATALOG_MAX + 1], release[VMG_RELEASE_MAX + 1], edition[VMG_EDITION_MAX + 1];
    char archive_sha512[129];
    char base[VM_ROLE_COUNT][VM_PATH_MAX];
    char base_sha256[VM_ROLE_COUNT][65];
    int disposable;                     /* the workstation's disposable session (19.7) */
    int fresh_gateway;                  /* Start Fresh's "Also reset the Gateway" box, remembered */
} VmGroup;

/* "gateway", "workstation" (index VM_ROLE_*; [0] is "") */
extern const char *const VMG_ROLE_NAMES[];

/* parses group.json text; err (optional) gets the reason in English. VM_OK or VM_E_* (machine.h) */
int vm_group_parse(const char *text, size_t len, VmGroup *g, char *err, size_t errlen);
/* the rules of 19.4 (parse calls it): members, dirs, roles, orders, one primary, the kind's roles, the image */
int vm_group_check(const VmGroup *g, char *err, size_t errlen);
/* JSON text of g (malloc'd); NULL on out of memory */
char *vm_group_json(const VmGroup *g);
/* <dir>/group.json, read and parsed */
int vm_group_load(const char *dir, VmGroup *g, char *err, size_t errlen);
/* <dir>/group.json written by rename (temporary file, fsync); refuses a newer schema or a group that doesn't check */
int vm_group_save(const char *dir, const VmGroup *g);

/* the members' indexes in start order (stop 0) or stop order (stop 1), into idx[n]; returns n */
int vm_group_order(const VmGroup *g, int stop, int idx[VMG_MEMBERS_MAX]);
/* the index of the member with that role, or -1 */
int vm_group_role(const VmGroup *g, int role);
/* the primary member's index (there is exactly one in a group that checks) */
int vm_group_primary(const VmGroup *g);

/* a member's folder name: 8 to VMG_DIR_MAX bytes, ending ".lordvm", no '/', not starting with '.', no control bytes */
int vm_group_member_name_ok(const char *s);
/* a group folder's path: absolute, no "." or ".." component, no "//", no ',' or control bytes, ending ".lordgroup" with
 * a name before it */
int vm_group_dir_ok(const char *path);
/* the group folder holding a machine folder `dir` (its parent) when that parent's name ends ".lordgroup": 1 and the
 * path in out, else 0 */
int vm_group_parent(const char *dir, char *out, size_t n);

#endif
