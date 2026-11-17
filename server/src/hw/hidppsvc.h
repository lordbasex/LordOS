/* hidppsvc.h — lord-server's side of lord-hidpp (docs/DEVICES.md): runs the helper on a thread at most once a minute
 * and keeps its answers, each one turned into the kernel power supply of that device. */
#ifndef LORD_HW_HIDPPSVC_H
#define LORD_HW_HIDPPSVC_H

#include <stddef.h>

/* The latest answers as JSON, [{"supply":"hidpp_battery_0","percent":85,"charging":false}], and a refresh started
 * when they are older than a minute (never blocks). Returns bytes written. */
size_t hidpp_json(char *out, size_t n);

/* One line of lord-hidpp's output to its parts; 0 when it is well formed and in range (pure, tested). */
int hidpp_parse_line(const char *line, int *hidraw, int *percent, int *status);
/* The power supply under root ("/sys/class/hidraw") for hidraw N, "hidpp_battery_<n>" only; 0 when found. */
int hidpp_supply_of(const char *root, int hidraw, char *out, size_t n);

#endif
