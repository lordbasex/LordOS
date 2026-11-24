/* gsend.h — lordos-guest 10 (docs/VIRTUALIZATION.md 6.21): "Send to LordOS", files from this guest to the host.
 *
 * A program of the person's (`lordos-guest --send-to-host FILE…`, which the file manager's "Send to LordOS" entry runs)
 * hands the helper a file's absolute path over $XDG_RUNTIME_DIR/lordos-guest.sock (gshell.c: same uid only); the helper
 * queues it (at most GSEND_QUEUE) and sends one file at a time with vmagent.h's AGK_SEND_* frames: an offer with its size,
 * name and a resume key (the SHA-256 of its path, device, inode, size and change time), then, once the host took it, the
 * data from where the host says (a resume: only when this file's first bytes have the SHA-256 the host has), never more
 * than AG_SEND_WINDOW past the host's last acknowledgement, then the whole file's SHA-256. Files are opened without
 * following a link and must be regular files of this person's. The host decides where they go: its quarantine folder for
 * this machine. "Not now" (another file, no window there) and "later" keep a file queued and offer it again every 5 s, for
 * at most 10 minutes; a lost port offers it again at the next hello. */
#ifndef LORDOS_GUEST_GSEND_H
#define LORDOS_GUEST_GSEND_H

#include <stdint.h>

#define GSEND_QUEUE 64

/* a path from the local socket (an AGK_SEND_OFFER frame's payload there: the file's absolute path): 0 queued, 1 not a
 * file this helper sends (not absolute, not a regular file, a name the host won't take), 2 the queue is full */
int gsend_queue(const char *path);
/* an AGK_SEND_ACCEPT, AGK_SEND_ACK or AGK_SEND_DONE from the host: 1 it was one */
int gsend_frame(int port, uint16_t kind, const uint8_t *p, uint32_t n);
/* the host's hello: it takes files (AG_HELLO_SEND) or not */
void gsend_host(int port, int takes);
/* sends what may go now (an offer, data within the window, the end); the ms until it wants to run again (-1: only on a
 * frame) */
int gsend_pump(int port);
void gsend_port_lost(void);
/* the command line: lordos-guest --send-to-host FILE…: the exit status (0 queued, 1 not connected, 2 a file refused, 3 this
 * machine doesn't take files) */
int gsend_cli(int argc, char **argv);

#endif
