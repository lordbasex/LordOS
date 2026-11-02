/* mon.h — system metrics for the Monitor app (see mon.c and docs/MONITOR.md).
 *
 * Its own thread reads /proc and /sys four times per second, only while the app is open
 * and querying it: if nobody asks for a sample for 3 s, the thread goes to sleep (zero cost). The
 * screen thread never reads files: it only copies the latest ready-made JSON. */
#ifndef LORD_MON_H
#define LORD_MON_H

#include <stdint.h>

/* Assets folder (for assets/monitor/geoip-country.mmdb.xz). */
void mon_init(const char *assets_dir);
/* A new JS context (theme change): the previous one's start() calls are forgotten. */
void mon_reset(void);
void mon_start(void);
void mon_stop(void);
/* Which lists the thread builds besides the sample: processes and connections (1 per second each);
 * geo: look up each connection's country in the local database. */
void mon_want(int procs, int conns, int geo);

/* Copies (malloc) of the latest JSON, or NULL if there isn't one newer than *seq (which is updated).
 * Requesting a sample keeps the thread alive. */
char *mon_sample_json(uint64_t *seq);
char *mon_procs_json(uint64_t *seq);
char *mon_conns_json(uint64_t *seq);
/* Get Info (procctl.c): the process whose details the thread builds once per second (pid 0: none)
 * and the latest JSON. start: its start time in ticks, so a reused PID shows as gone. */
void mon_detail(int pid, unsigned long long start);
char *mon_detail_json(uint64_t *seq);
/* Disk temperatures for the session's notifications (sdk/diskwatch.js): with on, the thread also
 * wakes every 30 s while the Monitor is closed and reads only the disk sensors (spinning disks only
 * when awake). mon_disk_temps_json: {"t", "disks": [{name, model, rota, sensor, temp, tempT, tempMax,
 * tempCrit, tempIdle}]} if newer than *seq; reading it does not keep the 4 Hz sampling alive. */
void mon_watch_disks(int on);
char *mon_disk_temps_json(uint64_t *seq);
/* Monitor → Voltage (voltage.h): with on, the thread reads the voltages about every 2 s; JS asks for it only while
 * the view is on screen. mon_voltage_json: the latest if newer than *seq. Every sample also says "volt": whether this
 * machine has anything to show. */
void mon_want_voltage(int on);
char *mon_voltage_json(uint64_t *seq);
/* Fixed data: CPU model, cores, location from the time zone, country database. */
char *mon_info_json(void);

#endif
