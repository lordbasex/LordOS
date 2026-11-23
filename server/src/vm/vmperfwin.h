/* vmperfwin.h — Windows Search while a Windows machine's performance profile is on (docs/VIRTUALIZATION.md 7.1.4;
 * AGENTS.md "LordOS guest tools for Windows", approved by the maintainer 2026-11-23). The tools (w11) apply the rest of
 * the profile as the person; the indexer's service needs an administrator, so lord-vm runs one of two fixed scripts
 * once per run, as SYSTEM through qemu-guest-agent, when the tools say "perf" in their hello: PERFWIN_SEARCH_ON when
 * machine.json has perf_profile, PERFWIN_SEARCH_OFF when it hasn't. Both are compile-time constants: the on/off bit
 * picks one, and nothing from the guest, the person, JS or machine.json reaches either.
 *
 *   on:  when the WSearch service starts automatically (Start 2, delayed or not), that start type is kept in
 *        HKLM\SOFTWARE\LordOS\GuestWin WSearchStartWas (DWORD 2 automatic, 18 automatic delayed) and the service is
 *        set to Manual (sc.exe config start= demand); a Manual one with a value kept is ours from an earlier run; a
 *        Manual or Disabled one without is left as it is. Then the service is stopped if it runs (unless Disabled).
 *   off: with a value kept, and the service still Manual (what the script set), the start type goes back and the
 *        service is started; a start type the person changed in between stays. The value goes either way.
 *
 * Each prints one line, "on|off <start type> <service>", read by perfwin_parse and only logged. A named mutex
 * (Global\LordOS-wsearch) keeps two runs apart ("busy"). */
#ifndef LORD_VM_VMPERFWIN_H
#define LORD_VM_VMPERFWIN_H

#include <stddef.h>

extern const char PERFWIN_SEARCH_ON[], PERFWIN_SEARCH_OFF[];

/* the start type's word: on: set (Manual now, the earlier one kept), ours (Manual from an earlier run), manual (Manual
 * already, nothing kept), disabled (left alone); off: restored, changed (the person changed it: left, the value
 * dropped), nothing (no value kept), dropped (a kept value that isn't 2 or 18, dropped); both: none (no WSearch service),
 * busy (another run holds the mutex) */
enum { PW_S_SET = 1, PW_S_OURS, PW_S_MANUAL, PW_S_DISABLED, PW_S_RESTORED, PW_S_CHANGED, PW_S_NOTHING, PW_S_DROPPED, PW_S_NONE, PW_S_BUSY };
/* the service's word: stopped (it ran, now stopped), idle (it wasn't running), started, notstarted (the start type
 * went back but Start-Service failed), - (nothing done with it) */
enum { PW_V_STOPPED = 1, PW_V_IDLE, PW_V_STARTED, PW_V_NOTSTARTED, PW_V_NONE };
typedef struct { int on, start, svc; } PerfWinResult;

/* the script's line (as ag_exec_text gives it): 0, or -1 when it isn't one of the combinations the scripts print */
int perfwin_parse(const char *said, PerfWinResult *r);
/* the line lord-vm logs, "performance profile: ..." */
void perfwin_describe(const PerfWinResult *r, char *out, size_t n);

#endif
