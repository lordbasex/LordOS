/* portal.h — our own org.freedesktop.portal.Settings on the session bus (see portal.c). */
#ifndef LORD_PORTAL_H
#define LORD_PORTAL_H

int  portal_start(void);                /* after dropping privileges; 0 if it owns the name */
int  portal_fd(void);                   /* for poll; -1 if absent */
int  portal_wants_write(void);          /* there are unsent messages (POLLOUT) */
void portal_dispatch(void);
/* The answer to a file dialog: n paths, or 0 if it was cancelled. */
void portal_file_chosen(int id, const char *const *paths, int n);
/* From JS (lord.portal.set): mode, accent (0..1), GNOME accent name and GTK button order. */
void portal_set(int dark, const double rgb[3], const char *accent_name, const char *button_layout);

#endif
