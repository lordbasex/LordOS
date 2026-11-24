/* gnotes.h — lordos-guest 9 (docs/VIRTUALIZATION.md 6.12): the guest desktop's own notifications, passed on to the host.
 *
 * Only while the host's hello says "notify" (machine.json "guest_notify": the person turned "Notifications from the
 * machine" on), the helper, as the person signed in at the guest's display and with its own D-Bus client (dbusmin.c: no
 * libdbus, no program started), becomes a monitor of that person's own session bus (org.freedesktop.DBus.Monitoring
 * BecomeMonitor, which the bus allows its own user) for exactly one match: method calls to
 * org.freedesktop.Notifications.Notify. Of each it keeps only the summary and the body (the app's name when the summary is
 * empty), made plain text (db_note_plain), and sends them as AGK_NOTIFY within its own rate (one a second); lord-vm and
 * lord-server check, rate-limit and show them as from the machine. Nothing else on the bus is read; a message too large to
 * keep (an image in its hints) is skipped whole. The connection goes when the host stops saying "notify" or the port
 * goes; it is tried again every 30 s while wanted. */
#ifndef LORDOS_GUEST_GNOTES_H
#define LORDOS_GUEST_GNOTES_H

/* on: the host takes notifications; bus: the session bus's socket (NULL: $DBUS_SESSION_BUS_ADDRESS, $XDG_RUNTIME_DIR/bus) */
void gnotes_want(int on, const char *bus);
int gnotes_fd(void);                    /* the monitor's socket, -1 none */
void gnotes_read(int port);             /* it is readable: notifications on to the host's port */

#endif
