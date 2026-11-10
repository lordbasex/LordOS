/* portalcheck.h — the checks behind the OpenURI portal (portal.c) that need no D-Bus: which web addresses
 * an app may hand to the browser and which local files it may open, and that a file confirmed later is
 * still the one that was checked (see portalcheck.c). */
#ifndef LORD_PORTALCHECK_H
#define LORD_PORTALCHECK_H

#include <stddef.h>
#include <sys/types.h>

enum { PORTAL_URI, PORTAL_FILE, PORTAL_FOLDER, PORTAL_REVEAL };

/* An http or https address every URL parser reads the same way (sdk/portalopenmodel.js, webHost, refuses
 * exactly the same ones): 0 with its host in lower case in host, -1 refused. */
int portal_web_host(const char *url, char *host, size_t hostlen);

/* What was seen of a local file when the app asked: device, inode and type */
typedef struct { dev_t dev; ino_t ino; mode_t type; } PortalSeen;

/* A local file an app asks to open (PORTAL_FILE: a folder becomes PORTAL_FOLDER) or show (PORTAL_REVEAL):
 * name is followed to its real path (real, PATH_MAX), which has to be in the home folder or the user's
 * mounts, not protected, and, to open, not an executable or a launcher. The kind, or -1. */
int portal_file_kind(const char *name, int kind, char *real, PortalSeen *seen);
/* The same for the file behind an fd an app passed: its name has to lead to the same file */
int portal_fd_kind(int fd, int kind, char *real, PortalSeen *seen);
/* Right before opening, after the person confirmed: real still is that same file (not swapped for
 * another or for a link, not made executable, not moved under a protected folder). 1 yes, 0 no. */
int portal_file_still(const char *real, int kind, const PortalSeen *seen);

#endif
