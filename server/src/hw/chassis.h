/* chassis.h — what kind of computer an SMBIOS chassis type (/sys/class/dmi/id/chassis_type) names.
 * Header only and libc free, so the small root hook lordos-lid-wake (hw/lidwake.h) and lord-server
 * (osk.c) share one list. */
#ifndef LORD_CHASSIS_H
#define LORD_CHASSIS_H

/* A computer that carries its own keyboard and folds or closes: 8 portable, 9 laptop, 10 notebook,
 * 14 sub notebook, 31 convertible, 32 detachable. 30 (tablet) and 11 (hand held) have no keyboard of
 * their own; every other value (desktops, servers, 0 = no DMI, a Raspberry Pi) is not portable. */
static inline int chassis_portable(int type) {
    return type == 8 || type == 9 || type == 10 || type == 14 || type == 31 || type == 32;
}

#endif
