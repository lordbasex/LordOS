/* vmperfwin.c — Windows Search while a Windows machine's performance profile is on: the two fixed scripts lord-vm runs
 * as SYSTEM through qemu-guest-agent, and the reading of the one line they print (vmperfwin.h says what they do). */
#include "vmperfwin.h"

#include <stdio.h>
#include <string.h>

/* built from constants only (never a printf format); run with -EncodedCommand like PTR_PS and GVTWIN_PS */
#define PW_HEAD(O)                                                                                                        \
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};\n" \
    "$O='" O "';$K='HKLM:\\SOFTWARE\\LordOS\\GuestWin';$N='WSearchStartWas';$S='HKLM:\\SYSTEM\\CurrentControlSet\\Services\\WSearch';\n" \
    "$mx=New-Object System.Threading.Mutex($false,'Global\\LordOS-wsearch');$own=$false;\n"                             \
    "try{$own=$mx.WaitOne(120000)}catch [System.Threading.AbandonedMutexException]{$own=$true};\n"                      \
    "if(-not $own){[Console]::Out.Write($O+' busy -');exit 0};\n"                                                         \
    "try{\n"                                                                                                              \
    "$svc=Get-Service -Name WSearch -ErrorAction SilentlyContinue;\n"                                                     \
    "$w=$null;if(Test-Path -LiteralPath $K){$w=(Get-ItemProperty -LiteralPath $K -Name $N -ErrorAction SilentlyContinue).$N};\n"
#define PW_TAIL                                                                                                           \
    "[Console]::Out.Write($O+' '+$a+' '+$b)\n"                                                                            \
    "}finally{$mx.ReleaseMutex();$mx.Dispose()}\n"

const char PERFWIN_SEARCH_ON[] =
    PW_HEAD("on")
    "if(-not $svc){$a='none';$b='-'}else{\n"
    "$p=Get-ItemProperty -LiteralPath $S;$st=[int]$p.Start;\n"
    "if($st -eq 2){\n"
    "if(-not(Test-Path -LiteralPath $K)){New-Item -Path $K -Force|Out-Null};\n"
    "$v=if([int]$p.DelayedAutostart -eq 1){18}else{2};\n"
    "Set-ItemProperty -LiteralPath $K -Name $N -Value $v -Type DWord;\n"
    "sc.exe config WSearch start= demand|Out-Null;\n"
    "if($LASTEXITCODE -ne 0){Remove-ItemProperty -LiteralPath $K -Name $N;throw ('sc.exe config: '+$LASTEXITCODE)};\n"
    "$a='set'}\n"
    "elseif($st -eq 3){$a=if($null -ne $w){'ours'}else{'manual'}}\n"
    "elseif($st -eq 4){$a='disabled'}\n"
    "else{throw ('WSearch start type '+$st)};\n"
    "$b='idle';\n"
    "if($st -ne 4 -and $svc.Status -ne 'Stopped'){Stop-Service -Name WSearch -Force;$b='stopped'}};\n"
    PW_TAIL;

const char PERFWIN_SEARCH_OFF[] =
    PW_HEAD("off")
    "$b='-';\n"
    "if($null -eq $w){$a='nothing'}\n"
    "elseif(-not $svc){$a='none'}\n"
    "else{\n"
    "$x=$w -as [int];$st=[int](Get-ItemProperty -LiteralPath $S).Start;\n"
    "if($x -ne 2 -and $x -ne 18){$a='dropped'}\n"
    "elseif($st -ne 3){$a='changed'}\n"
    "else{\n"
    "$t=if($x -eq 18){'delayed-auto'}else{'auto'};\n"
    "sc.exe config WSearch start= $t|Out-Null;\n"
    "if($LASTEXITCODE -ne 0){throw ('sc.exe config: '+$LASTEXITCODE)};\n"
    "$a='restored';\n"
    "try{Start-Service -Name WSearch;$b='started'}catch{$b='notstarted'}}};\n"
    "if($null -ne $w){Remove-ItemProperty -LiteralPath $K -Name $N};\n"
    PW_TAIL;

_Static_assert(sizeof PERFWIN_SEARCH_ON < 4000 && sizeof PERFWIN_SEARCH_OFF < 4000, "the scripts must fit Windows' command line once encoded");

static int word(const char *const *names, const char *w, size_t l) {
    for (int i = 0; names[i]; i++) if (strlen(names[i]) == l && !memcmp(names[i], w, l)) return i + 1;
    return 0;
}

int perfwin_parse(const char *said, PerfWinResult *r) {
    /* in the order of PW_S_* and PW_V_* */
    static const char *const S[] = { "set", "ours", "manual", "disabled", "restored", "changed", "nothing", "dropped", "none", "busy", NULL };
    static const char *const V[] = { "stopped", "idle", "started", "notstarted", "-", NULL };
    memset(r, 0, sizeof *r);
    if (!said) return -1;
    const char *sp1 = strchr(said, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || strchr(sp2 + 1, ' ')) return -1;
    size_t l0 = (size_t)(sp1 - said);
    if (l0 == 2 && !memcmp(said, "on", 2)) r->on = 1;
    else if (!(l0 == 3 && !memcmp(said, "off", 3))) return -1;
    if (!(r->start = word(S, sp1 + 1, (size_t)(sp2 - sp1 - 1))) || !(r->svc = word(V, sp2 + 1, strlen(sp2 + 1)))) return -1;
    int s = r->start, v = r->svc, ok;
    if (s == PW_S_NONE || s == PW_S_BUSY) ok = v == PW_V_NONE;
    else if (r->on) ok = (s == PW_S_SET || s == PW_S_OURS || s == PW_S_MANUAL) ? (v == PW_V_STOPPED || v == PW_V_IDLE)
                       : s == PW_S_DISABLED ? v == PW_V_IDLE : 0;
    else ok = s == PW_S_RESTORED ? (v == PW_V_STARTED || v == PW_V_NOTSTARTED)
            : (s == PW_S_CHANGED || s == PW_S_NOTHING || s == PW_S_DROPPED) ? v == PW_V_NONE : 0;
    if (!ok) { memset(r, 0, sizeof *r); return -1; }
    return 0;
}

void perfwin_describe(const PerfWinResult *r, char *out, size_t n) {
    if (!n) return;
    const char *s = "?", *v = "";
    switch (r->start) {
    case PW_S_SET: s = "Windows Search set to start manually (its automatic start kept to put back)"; break;
    case PW_S_OURS: s = "Windows Search starts manually (since an earlier run)"; break;
    case PW_S_MANUAL: s = "Windows Search already starts manually (left as it is)"; break;
    case PW_S_DISABLED: s = "Windows Search is disabled (left as it is)"; break;
    case PW_S_RESTORED: s = "Windows Search starts automatically again"; break;
    case PW_S_CHANGED: s = "Windows Search's start type was changed in the guest since: left as it is"; break;
    case PW_S_NOTHING: s = "Windows Search: nothing to put back"; break;
    case PW_S_DROPPED: s = "Windows Search: the start type kept was not one the script writes: dropped"; break;
    case PW_S_NONE: s = "the guest has no Windows Search service"; break;
    case PW_S_BUSY: s = "Windows Search: another run of the script is still going"; break;
    }
    switch (r->svc) {
    case PW_V_STOPPED: v = ", stopped"; break;
    case PW_V_IDLE: v = ", it wasn't running"; break;
    case PW_V_STARTED: v = ", started"; break;
    case PW_V_NOTSTARTED: v = ", but it didn't start"; break;
    }
    snprintf(out, n, "performance profile: %s%s", s, v);
}
