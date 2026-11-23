/* perf.h — the machine's performance profile in a Linux guest (AGK_PERF, docs/VIRTUALIZATION.md 7.1.4; approved by the
 * maintainer 2026-11-23). lordos-guest, as the signed-in person, with its own D-Bus client (dbusmin.c) and no program
 * started:
 *   - POWER: on the system bus only, power-profiles-daemon's ActiveProfile (org.freedesktop.UPower.PowerProfiles, else
 *     the older net.hadess.PowerProfiles) to "performance" (polkit lets the active session switch it);
 *   - EFFECTS: on the session bus only, GNOME's org.gnome.desktop.interface enable-animations to false: read through
 *     the desktop portal's Settings, written through dconf's own service (ca.desrt.dconf Writer.Change, the path
 *     GSettings takes). Another desktop (KDE, ...) or neither service: refused.
 * What was there before is kept in ~/.local/state/lordos-guest/perf (a fixed text record, read back strictly) and put
 * back when the host says 0, when the helper lets the port go or ends (SIGTERM at log off), and at its next start after
 * a crash; each part only while it is still what the helper set (a change the person made stays). A part already
 * there counts as done and is left alone. Nothing from the host but the on/off bit. */
#ifndef LORDOS_GUEST_PERF_H
#define LORDOS_GUEST_PERF_H

/* tests: other sockets for the system and the session bus (NULL: the usual ones) */
void perf_buses(const char *system_bus, const char *session_bus);
/* on 1 applies, 0 puts back; the AGPF_* parts done and refused */
void perf_set(int on, unsigned *done, unsigned *failed);
/* something applied and not yet put back */
int perf_active(void);
/* at the start: a record a run that ended badly left behind is put back */
void perf_recover(void);

/* the record's text and its reading (tests/guest_dbus_test.c): power "" (nothing of ours) or the earlier profile;
 * anim -1 (nothing of ours), 0 or 1. Write: the bytes, 0 when it doesn't fit. Parse: 0, -1 */
int perf_record_write(char *out, int cap, const char *power, int anim);
int perf_record_parse(const char *s, int n, char *power, int cap, int *anim);

#endif
