/* folderrole.h — which IMAP folder is the account's Sent, Drafts, Trash, Junk, Archive… (docs/MAIL.md).
 * Pure functions (no I/O, no locks), shared by lord-maild and tests/mail_test.c.
 *
 * A role comes from the server's SPECIAL-USE flags (RFC 6154) when it sends them, otherwise from
 * the folder's name. Each role goes to at most one folder per account: a flagged folder always
 * wins over a name match, and a name only counts for a top-level folder or a direct child of
 * INBOX ("Trash", "INBOX.Trash", "INBOX/Trash"), never for a user folder deeper in the tree. */
#ifndef LORD_MAIL_FOLDERROLE_H
#define LORD_MAIL_FOLDERROLE_H

/* The role a LIST flag list gives ("\HasNoChildren \Sent " → "sent"), or "". */
const char *folder_flag_role(const char *flags);
/* The role a folder name suggests with that hierarchy delimiter (0 = flat), or "".
 * "INBOX" (any case) is always "inbox". */
const char *folder_name_role(const char *name, char delim);

typedef struct {
    const char *name;               /* the folder's name as LIST returned it */
    const char *flags;              /* its flags, space separated */
    char delim;                     /* its hierarchy delimiter, 0 when NIL */
    int noselect;                   /* \Noselect or \NonExistent: never gets a role */
} FolderRoleIn;

/* Gives each of the n folders (in LIST order) its role in out[i] ("" for an ordinary folder),
 * at most one folder per role. */
void folder_roles_resolve(const FolderRoleIn *in, int n, const char **out);

#endif
