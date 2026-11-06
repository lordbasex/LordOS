/* sysbus.h — one connection to the D-Bus system bus for Settings (BlueZ in hw/bt.c,
 * power-profiles-daemon in hw/ppd.c), with our own D-Bus client (vault/dbus.c). Asynchronous:
 * replies go to callbacks by serial, signals and incoming method calls to the registered
 * handlers. Its fd is in the main loop's poll; nothing is polled. Only Hello blocks (at start). */
#ifndef LORD_HW_SYSBUS_H
#define LORD_HW_SYSBUS_H

#include <stdint.h>
#include "../vault/dbus.h"

/* m is the reply (DBUS_RETURN or DBUS_ERROR) or NULL if the bus went away. A reply may carry fds
 * (m->fds, closed after the callback: dup one to keep it); no other message does. */
typedef void (*SbReply)(const DMsg *m, void *ctx);
typedef void (*SbSignal)(const DMsg *m);
/* An incoming method call: 1 if handled (the handler may keep *m: set m->raw = NULL to own it). */
typedef int (*SbCall)(DMsg *m);
/* A well-known name appeared (owner != "") or went away (owner == ""). */
typedef void (*SbOwner)(const char *name, const char *owner);

int  sysbus_start(void);                /* 0 if connected */
int  sysbus_up(void);
int  sysbus_fd(void);
int  sysbus_wants_write(void);
void sysbus_dispatch(void);
DConn *sysbus_conn(void);

/* Method call; cb may be NULL (no reply wanted). Returns the serial or 0. */
uint32_t sysbus_call(const char *dest, const char *path, const char *iface, const char *member,
                     const char *sig, const DBuf *body, SbReply cb, void *ctx);
void sysbus_add_match(const char *rule);
void sysbus_on_signal(SbSignal fn);
void sysbus_on_call(SbCall fn);
/* Watch a well-known name: fn is called with its current owner (GetNameOwner) and on every
 * NameOwnerChanged. */
void sysbus_watch_name(const char *name, SbOwner fn);

/* Reading helpers over a validated body. */
int16_t  sb_i16(DRd *r);
uint16_t sb_u16(DRd *r);

#endif
