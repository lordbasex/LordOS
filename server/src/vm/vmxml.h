/* vmxml.h — the XML reader for the settings files of other virtualization apps (docs/VIRTUALIZATION.md 6.22, Import):
 * .vbox, config.pvs, DiskDescriptor.xml and OVF descriptors. Every one of them is hostile input, so it is strict and
 * bounded:
 *   - a <!DOCTYPE or <!ENTITY is refused: no declared entities, so no external entities and no "billion laughs";
 *   - only the five predefined entities and numeric character references (no NUL, no surrogates, at most U+10FFFF);
 *   - at most VX_MAX_INPUT bytes, VX_MAX_DEPTH levels, VX_MAX_NODES elements, VX_MAX_ATTRS attributes per element;
 *   - every read is checked against the length; it never reads outside the input; no recursion;
 *   - namespace prefixes are dropped: an element or attribute is looked up by its local name ("rasd:Address" is
 *     "Address"), which is all the importer needs and never resolves a URL.
 * The unit test (tests/vm_test.c) runs it over truncated and randomly mutated inputs under ASan. */
#ifndef LORD_VM_XML_H
#define LORD_VM_XML_H

#include <stddef.h>

#define VX_MAX_INPUT (4u << 20)
#define VX_MAX_DEPTH 64
#define VX_MAX_NODES 50000
#define VX_MAX_ATTRS 64

typedef struct { const char *name, *value; } VxAttr;
typedef struct VxNode VxNode;
struct VxNode {
    const char *name;                 /* local name, without the prefix */
    char *text;                       /* direct text, entities decoded, trimmed of surrounding white space */
    VxAttr *attrs;
    int nattrs;
    VxNode *kid, *last, *next, *parent;
};

/* the root element, or NULL; err (optional) gets a short reason */
VxNode *vx_parse(const char *s, size_t len, const char **err);
void vx_free(VxNode *root);
VxNode *vx_child(const VxNode *n, const char *name);          /* first direct child with that local name */
VxNode *vx_next(const VxNode *n, const char *name);           /* next sibling with that name */
VxNode *vx_find(const VxNode *n, const char *name);           /* first descendant, depth first */
const char *vx_attr(const VxNode *n, const char *name);       /* by local name; NULL if absent */
const char *vx_text(const VxNode *n);                         /* "" if none */
const char *vx_child_text(const VxNode *n, const char *name); /* a child's text, "" if absent */

#endif
