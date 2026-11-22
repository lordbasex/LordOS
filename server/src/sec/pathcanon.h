/* pathcanon.h — is a path written the one way realpath writes it? A policy that compares path strings (prefix deny lists:
 * fsops_protected, apps.c's grant rules) is only sound on such a path: "/home/u/./.config" or "/home/u//.ssh" name the
 * same folders as "/home/u/.config" and "/home/u/.ssh" but match none of their entries. Header only, so the view's PID 1
 * (sec/sandbox.c), lord-server and the tests share it. */
#ifndef LORD_PATHCANON_H
#define LORD_PATHCANON_H

#include <string.h>

/* 1 when path is absolute and has no empty, "." or ".." element and no '/' at the end ("/" itself is canonical). It says
 * nothing about links: a canonical spelling may still go through one (callers open with RESOLVE_NO_SYMLINKS). */
static inline int path_canonical(const char *path) {
    if (!path || path[0] != '/') return 0;
    if (!path[1]) return 1;
    for (const char *p = path; *p;) {
        const char *e = strchr(p + 1, '/');
        size_t n = e ? (size_t)(e - p - 1) : strlen(p + 1);
        if (!n || (n == 1 && p[1] == '.') || (n == 2 && p[1] == '.' && p[2] == '.')) return 0;
        if (!e) break;
        p = e;
    }
    return 1;
}

#endif
