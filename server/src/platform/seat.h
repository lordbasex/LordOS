#ifndef LORD_SEAT_H
#define LORD_SEAT_H

/* libseat owns session access; the compositor still owns all graphics/input.
 * With SEAT=0 these calls retain the original direct-device behavior. */
int seat_start(void);
void seat_stop(void);
int seat_fd(void);
int seat_dispatch(void);
int seat_managed(void);
int seat_device_open(const char *path, int flags);
void seat_device_close(int fd);
void seat_callbacks(void (*enable)(void), void (*disable)(void));
int seat_switch(int vt);

#endif
