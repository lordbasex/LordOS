/* ifwatch.h — which interfaces lord-netd still has to bring up (issue #8; see ifwatch.c). */
#ifndef LORD_IFWATCH_H
#define LORD_IFWATCH_H

#define IFW_MAX      64
#define IFW_WAIT_MS  30000      /* a device udev never finishes naming is brought up anyway */
#define IFW_GRACE_MS 1000       /* one that appears later: the system's network manager goes first */
#define IFW_RETRY_MS 5000
#define IFW_TRIES    3

typedef struct { int index; char name[16]; } IfNow;

typedef struct {
    int (*candidate)(const char *name);             /* has a saved config, or is a physical NIC */
    int (*initialized)(int index);                  /* udev finished with it: the name is the final one */
    int (*managed)(const char *name);               /* it belongs to the system's network manager */
    int (*busy)(const char *name);                  /* the daemons of an earlier device are still exiting */
    int (*apply)(const char *name);                 /* 0: applied */
    void (*stop)(const char *name);                 /* the device left: stop its daemons */
} IfOps;

typedef struct { int index, done, boot, fails; long first_ms, next_ms; char name[16]; } IfSeen;
typedef struct { IfSeen v[IFW_MAX]; int n; } IfTable;

/* One pass over the interfaces there are now. booting: those seen in this pass were there when lord-netd
 * started. *applied counts the ones brought up. Returns how many are still waiting. */
int ifwatch_step(IfTable *t, const IfNow *now, int n, const IfOps *o, long ms, int booting, int *applied);

#endif
