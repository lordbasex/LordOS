/* xml.h — in-house XML reader for WebDAV (CalDAV and CardDAV) in axia-maild (see docs/MAIL.md).
 *
 * Understands namespaces (xmlns and xmlns:p), so elements are looked up by (namespace, name)
 * regardless of the prefix the server chose ("d:", "D:", none). It is strict and bounded:
 *   - never expands declared entities: a <!DOCTYPE is an error (no "billion laughs" and no
 *     external entities); only the five predefined ones and valid numeric ones;
 *   - maximum depth MX_MAX_DEPTH, MX_MAX_NODES elements, input of up to MX_MAX_INPUT;
 *   - every index is checked against the length: it never reads outside the input.
 * The test (tests/mail_test.c) exercises it with randomly mutated inputs. */
#ifndef LORD_MAIL_XML_H
#define LORD_MAIL_XML_H

#include <stddef.h>

#define MX_MAX_DEPTH 64
#define MX_MAX_NODES 400000
#define MX_MAX_INPUT (32u << 20)

#define MX_NS_DAV  "DAV:"
#define MX_NS_CAL  "urn:ietf:params:xml:ns:caldav"
#define MX_NS_CARD "urn:ietf:params:xml:ns:carddav"
#define MX_NS_CS   "http://calendarserver.org/ns/"
#define MX_NS_APPLE "http://apple.com/ns/ical/"

typedef struct MxNode MxNode;
struct MxNode {
    const char *ns;           /* resolved namespace ("" if none) */
    const char *name;         /* local name, without prefix */
    char *text;               /* direct text (excluding children's), entities already decoded */
    size_t tlen;
    MxNode *kid, *last, *next, *parent;
};

/* Document tree (the root element) or NULL. err (optional) receives a short message. */
MxNode *mx_parse(const char *s, size_t len, const char **err);
void    mx_free(MxNode *root);
/* first direct child with that namespace and name (ns NULL = any) */
MxNode *mx_child(const MxNode *n, const char *ns, const char *name);
/* next sibling with that namespace and name */
MxNode *mx_next(const MxNode *n, const char *ns, const char *name);
/* first descendant (depth-first, without recursion) */
MxNode *mx_find(const MxNode *n, const char *ns, const char *name);
const char *mx_text(const MxNode *n);     /* "" if none */
/* text of a child (ns, name), trimmed, into out; "" if absent */
void    mx_child_text(const MxNode *n, const char *ns, const char *name, char *out, size_t cap);
/* Escapes text for embedding in XML (&, <, >, "). Returns bytes written (excluding the NUL). */
size_t  mx_escape(char *out, size_t cap, const char *s);

#endif
