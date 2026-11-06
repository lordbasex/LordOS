/* folderrole.c — folder roles from SPECIAL-USE flags and names (see folderrole.h). */
#include "folderrole.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

static const struct { const char *flag, *role; } FLAGS[] = {
    { "\\Sent", "sent" }, { "\\Drafts", "drafts" }, { "\\Trash", "trash" }, { "\\Junk", "junk" }, { "\\Archive", "archive" },
    { "\\All", "all" }, { "\\Flagged", "flagged" },
};

/* Whole-token match: "\Sent" is not "\SentItems". */
static int has_flag(const char *flags, const char *flag) {
    size_t n = strlen(flag);
    for (const char *p = flags; p && *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && !strncasecmp(p, flag, n)) return 1;
        p = e;
    }
    return 0;
}

const char *folder_flag_role(const char *flags) {
    for (size_t i = 0; flags && i < sizeof FLAGS / sizeof FLAGS[0]; i++) if (has_flag(flags, FLAGS[i].flag)) return FLAGS[i].role;
    return "";
}

static int is_inbox_child(const char *name, char delim) { return delim && !strncasecmp(name, "INBOX", 5) && name[5] == delim; }

/* 1 for a top-level folder, 2 for a direct child of INBOX, 0 for anything deeper. */
static int name_level(const char *name, char delim) {
    if (!delim || !strchr(name, delim)) return 1;
    if (is_inbox_child(name, delim) && !strchr(name + 6, delim)) return 2;
    return 0;
}

static const struct { const char *n, *role; } NAMES[] = {
    { "Sent", "sent" }, { "Sent Items", "sent" }, { "Sent Messages", "sent" }, { "Sent Mail", "sent" }, { "Enviados", "sent" }, { "Elementos enviados", "sent" },
    { "Drafts", "drafts" }, { "Borradores", "drafts" }, { "Rascunhos", "drafts" },
    { "Trash", "trash" }, { "Deleted Items", "trash" }, { "Deleted Messages", "trash" }, { "Papelera", "trash" }, { "Lixeira", "trash" }, { "Bin", "trash" },
    { "Junk", "junk" }, { "Spam", "junk" }, { "Junk E-mail", "junk" }, { "Correo no deseado", "junk" },
    { "Archive", "archive" }, { "Archivo", "archive" }, { "Arquivo", "archive" }, { "All Mail", "all" }, /* i18n-ok: folder names servers use */
};
#define NNAMES ((int)(sizeof NAMES / sizeof NAMES[0]))

/* Index in NAMES (earlier = the more usual name for its role), or -1. */
static int name_index(const char *name, char delim) {
    if (!name) return -1;
    int lvl = name_level(name, delim);
    if (!lvl) return -1;
    const char *base = lvl == 2 ? name + 6 : name;
    for (int i = 0; i < NNAMES; i++) if (!strcasecmp(base, NAMES[i].n)) return i;
    return -1;
}

const char *folder_name_role(const char *name, char delim) {
    if (!name) return "";
    if (!strcasecmp(name, "INBOX")) return "inbox";
    int i = name_index(name, delim);
    return i < 0 ? "" : NAMES[i].role;
}

void folder_roles_resolve(const FolderRoleIn *in, int n, const char **out) {
    for (int i = 0; i < n; i++) out[i] = "";
    /* INBOX is INBOX, flagged or not */
    for (int i = 0; i < n; i++) if (in[i].name && !strcasecmp(in[i].name, "INBOX")) { out[i] = "inbox"; break; }
    for (size_t r = 0; r < sizeof FLAGS / sizeof FLAGS[0]; r++) {
        const char *role = FLAGS[r].role;
        int pick = -1;
        /* 1. the first folder the server flags with it */
        for (int i = 0; i < n && pick < 0; i++)
            if (!in[i].noselect && !*out[i] && !strcmp(folder_flag_role(in[i].flags), role)) pick = i;
        /* 2. else a name match: top-level before an INBOX child, then the more usual name
         * ("Sent" before "Sent Items", "Junk" before "Spam"), then LIST order */
        int flagged = pick >= 0, best = 0;
        for (int i = 0; i < n && !flagged; i++) {
            if (in[i].noselect || *out[i] || !in[i].name || *folder_flag_role(in[i].flags)) continue;
            int k = name_index(in[i].name, in[i].delim);
            if (k < 0 || strcmp(NAMES[k].role, role)) continue;
            int score = name_level(in[i].name, in[i].delim) * 100 + k;
            if (pick < 0 || score < best) { pick = i; best = score; }
        }
        if (pick >= 0) out[pick] = role;
    }
}
