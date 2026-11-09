/* termconf.h — the Terminal's own configuration file (docs/TERMINAL.md, "Configuration file"): profiles, color
 * schemes, key mappings, shortcuts and the hotkey, apart from settings.json. By default
 * ~/.config/lordos/terminal.json; the user may choose another file in the home folder or on a mounted disk, and then
 * the default one is kept as a copy of the last good version (the cache) for when that place isn't there.
 *
 * A profile can type a command when a session opens, so the file is a launch path: written only here (atomically,
 * 0600, fsync, five rotating backups), and the active file and its family (".tmp…", ".1"…".5") are protected from
 * lord.fs (fsops_protect_file). Nothing here creates a missing folder for a chosen file: a disk that isn't mounted
 * must not get an empty configuration in its mount point. */
#ifndef LORD_TERMCONF_H
#define LORD_TERMCONF_H

#include <stddef.h>

#define TERMCONF_MAX (4 * 1024 * 1024)
#define TERMCONF_BACKUPS 5
#define TERMCONF_BACKUP_EVERY 600.0      /* seconds between rotations: every save would push the old ones out */

void termconf_init(const char *home);
/* The file to use: "" for the default one. Absolute, ".json", in the home folder or under /media/<user>, not
 * a protected path. 0, or -1 with a code line in err. */
int termconf_set_custom(const char *path, char *err, size_t n);
const char *termconf_path(void);         /* the active file */
const char *termconf_cache(void);        /* the default file (also the cache of a chosen one) */
int termconf_is_custom(void);

/* The file's text (malloc, NUL-terminated) and its mtime; NULL with a code line in err: termconf.missing (no file),
 * termconf.unavailable (its folder isn't there: a disk not mounted, a folder renamed), termconf.denied,
 * termconf.notRegular, termconf.tooLarge */
char *termconf_read(const char *path, size_t *len, double *mtime, char *err, size_t n);
/* Atomic write of path (0600, fsync), a backup of the previous version first (path.1 … path.5) when the last one
 * is older than TERMCONF_BACKUP_EVERY. 0 and *mtime, or -1 with a code line. */
int termconf_write(const char *path, const char *txt, size_t len, double *mtime, char *err, size_t n);
/* An export of the profiles (Export Profiles…): any file in the home folder or on a mounted disk that isn't
 * protected (so never the active file), written atomically and 0600 like the configuration itself. */
int termconf_export(const char *path, const char *txt, size_t len, char *err, size_t n);
/* Moving the "terminal" section out of settings.json: settings.json is copied to
 * settings.json.pre-terminal-migration.bak (kept if there already is one), txt is written to the default file and
 * read back. Refused (termconf.exists) when the default file is already there. */
int termconf_migrate(const char *txt, size_t len, char *err, size_t n);

#endif
