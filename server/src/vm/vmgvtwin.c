/* vmgvtwin.c — Intel's driver for a Windows machine's GVT-g vGPU: the fixed script lord-vm runs as SYSTEM through
 * qemu-guest-agent, and the reading of the one line it prints (vmgvtwin.h says what it does and why). */
#include "vmgvtwin.h"

#include <stdio.h>
#include <string.h>

/* one statement or block per line, each line ending in a newline as in a .ps1 file; built from constants only (never a
 * printf format). Run with -EncodedCommand: UTF-16 and base64 make it about 2.7 times longer, inside Windows' 32 767
 * characters for a command line. */
const char GVTWIN_PS[] =
    "$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';trap{[Console]::Error.Write($_.Exception.Message);exit 1};\n"
    "$V='" GVTWIN_DRV_VERSION "';$U='" GVTWIN_DRV_URL "';$H='" GVTWIN_DRV_SHA256 "';$I='" GVTWIN_DRV_INF "';\n"
    "$K='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\DeviceInstall\\Restrictions';$L=$K+'\\DenyDeviceIDs';\n"
    "$dr='failed';$bl='none';$pw='failed';$id='-';$why='';\n"
    "function W($m){if(-not $script:why){$script:why=[string]$m}}\n"
    "function DV($iid){[string](Get-PnpDeviceProperty -InstanceId $iid -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data}\n"
    "$mx=New-Object Threading.Mutex($false,'Global\\LordOS-gvt-driver');$own=$false;\n"
    "try{$own=$mx.WaitOne(0)}catch{if($_.Exception -is [Threading.AbandonedMutexException] -or $_.Exception.InnerException -is [Threading.AbandonedMutexException]){$own=$true}else{throw}};\n"
    "if(-not $own){[Console]::Out.Write('busy none failed - another run is at work');exit 0};\n"
    "try{\n"
    "try{\n"
    "$pk='HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Power';$sk='HKLM:\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power';\n"
    "$h0=(Get-ItemProperty -LiteralPath $pk -Name HibernateEnabled -ErrorAction SilentlyContinue).HibernateEnabled;\n"
    "$b0=(Get-ItemProperty -LiteralPath $sk -Name HiberbootEnabled -ErrorAction SilentlyContinue).HiberbootEnabled;\n"
    "if($h0 -ne 0){powercfg.exe /hibernate off|Out-Null;$e=$LASTEXITCODE;if((Get-ItemProperty -LiteralPath $pk -Name HibernateEnabled -ErrorAction SilentlyContinue).HibernateEnabled -eq 1){throw ('powercfg ended with '+$e)}};\n"
    "if($b0 -ne 0){Set-ItemProperty -LiteralPath $sk -Name HiberbootEnabled -Value 0 -Type DWord};\n"
    "$pw=if($h0 -eq 0 -and $b0 -eq 0){'off'}else{'turned'}\n"
    "}catch{$pw='failed';W ('hibernation: '+$_.Exception.Message)};\n"
    "try{\n"
    "$dev=$null;\n"
    "foreach($d in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)){\n"
    "if($d.InstanceId -notmatch '^PCI\\\\VEN_8086&DEV_[0-9A-F]{4}&'){continue};\n"
    "$c=@((Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction SilentlyContinue).Data);\n"
    "if($c -contains 'PCI\\CC_0300' -or $c -contains 'PCI\\CC_0380'){$dev=$d;break}};\n"
    "if(-not $dev){$dr='nodevice'}else{\n"
    "$id=[regex]::Match($dev.InstanceId,'^PCI\\\\VEN_8086&DEV_[0-9A-F]{4}').Value;$was=DV $dev.InstanceId;\n"
    "if($was -eq $V){$dr='already'}else{\n"
    "if((Get-PSDrive -Name ($env:SystemDrive.TrimEnd(':'))).Free -lt 3GB){$dr='refused';throw 'less than 3 GB free on the system drive'};\n"
    "$t=Join-Path $env:SystemRoot ('Temp\\lordos-gvt-'+[guid]::NewGuid().ToString('N'));\n"
    "if(Test-Path -LiteralPath $t){throw 'the private folder is there already'};\n"
    "$sids='S-1-5-18','S-1-5-32-544';\n"
    "$a=New-Object Security.AccessControl.DirectorySecurity;$a.SetAccessRuleProtection($true,$false);\n"
    "foreach($x in $sids){$a.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule((New-Object Security.Principal.SecurityIdentifier($x)),'FullControl','ContainerInherit,ObjectInherit','None','Allow')))};\n"
    "[IO.Directory]::CreateDirectory($t,$a)|Out-Null;\n"
    "try{\n"
    "if((Get-Item -LiteralPath $t -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'the private folder is a reparse point'};\n"
    "$g=Get-Acl -LiteralPath $t;if($g.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $sids){throw 'the private folder is not ours'};\n"
    "foreach($r in $g.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){if($r.IdentityReference.Value -notin $sids){throw 'the private folder is not private'}};\n"
    "$f=Join-Path $t 'drv.cab';\n"
    "[Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12;\n"
    "$wc=New-Object Net.WebClient;try{$wc.DownloadFile($U,$f)}catch{throw ('the download: '+$_.Exception.InnerException.Message+' '+$_.Exception.Message)}finally{$wc.Dispose()};\n"
    "$dr='refused';\n"
    "if((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash -ne $H){throw 'the download is not the driver LordOS checked (SHA-256)'};\n"
    "$x=Join-Path $t 'x';[IO.Directory]::CreateDirectory($x)|Out-Null;\n"
    "expand.exe -F:* $f $x|Out-Null;if($LASTEXITCODE){$dr='failed';throw ('expand ended with '+$LASTEXITCODE)};\n"
    "$fi=Join-Path $x $I;$fc=Join-Path $x 'igdlh.cat';$fs=Join-Path $x 'igdkmd64.sys';\n"
    "foreach($q in $fi,$fc,$fs){if(-not (Test-Path -LiteralPath $q -PathType Leaf)){throw ('the driver has no '+(Split-Path -Leaf $q))}};\n"
    "if(-not (Select-String -LiteralPath $fi -Pattern ('^\\s*DriverVer\\s*=[^;]*,\\s*'+[regex]::Escape($V)+'(\\s|;|$)') -Quiet)){throw ('the INF is not version '+$V)};\n"
    "$s=Get-AuthenticodeSignature -LiteralPath $fc;$n=[string]$s.SignerCertificate.Subject;\n"
    "if($s.Status -ne 'Valid' -or $n -notmatch '^CN=Microsoft Windows Hardware Compatibility Publisher,' -or $n -notmatch ', O=Microsoft Corporation(,|$)'){throw ('the catalog''s signature: '+$s.Status+' '+$n)};\n"
    "$s=Get-AuthenticodeSignature -LiteralPath $fs;$n=[string]$s.SignerCertificate.Subject;\n"
    "if($s.Status -ne 'Valid' -or $n -notmatch '(^|, )O=Intel Corporation(,|$)'){throw ('the kernel driver''s signature: '+$s.Status+' '+$n)};\n"
    "$dr='failed';$rb=$false;\n"
    "if(Test-Path -LiteralPath $L){$g=Get-Item -LiteralPath $L;foreach($n in $g.GetValueNames()){if($n -and [string]$g.GetValue($n) -eq $id){Remove-ItemProperty -LiteralPath $L -Name $n}}};\n"
    "pnputil.exe /add-driver $fi /install|Out-Null;$e=$LASTEXITCODE;\n"
    "if($e -ne 0 -and $e -ne 3010 -and $e -ne 259){throw ('pnputil ended with '+$e)};if($e -eq 3010){$rb=$true};\n"
    "if((DV $dev.InstanceId) -ne $V){\n"
    "Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class LordGvtDrv{[DllImport(\"newdev.dll\",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr w,string id,string inf,uint f,out bool rb);public static int Force(string id,string inf,out bool rb){return UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero,id,inf,5,out rb)?0:Marshal.GetLastWin32Error();}}';\n"
    "$r2=$false;$e=[LordGvtDrv]::Force($id,$fi,[ref]$r2);if($e){throw ('the driver could not be put on the device ('+$e+')')};if($r2){$rb=$true}};\n"
    "$now=DV $dev.InstanceId;\n"
    "if($now -eq $V){$dr=if($rb){'restart'}else{'installed'}}elseif($rb){$dr='restart'}else{throw ('the device still has '+$now)}\n"
    "}finally{Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction SilentlyContinue}\n"
    "}}\n"
    "}catch{W $_.Exception.Message};\n"
    "if($dr -in 'already','installed','restart'){\n"
    "try{\n"
    "$b=0;if(-not (Test-Path -LiteralPath $L)){New-Item -Path $L -Force|Out-Null};\n"
    "$g=Get-Item -LiteralPath $L;$have=$false;foreach($n in $g.GetValueNames()){if($n -and [string]$g.GetValue($n) -eq $id){$have=$true}};\n"
    "if(-not $have){$j=1;while($null -ne $g.GetValue([string]$j)){$j++};New-ItemProperty -LiteralPath $L -Name ([string]$j) -Value $id -PropertyType String|Out-Null;$b=1};\n"
    "$p=Get-ItemProperty -LiteralPath $K;\n"
    "if($p.DenyDeviceIDs -ne 1){Set-ItemProperty -LiteralPath $K -Name DenyDeviceIDs -Value 1 -Type DWord;$b=1};\n"
    "if($p.DenyDeviceIDsRetroactive -ne 0){Set-ItemProperty -LiteralPath $K -Name DenyDeviceIDsRetroactive -Value 0 -Type DWord;$b=1};\n"
    "$bl=if($b){'added'}else{'kept'}\n"
    "}catch{$bl='failed';W ('the block: '+$_.Exception.Message)}};\n"
    "$why=($why -replace '[^\\x20-\\x7E]',' ').Trim();if($why.Length -gt 150){$why=$why.Substring(0,150)};\n"
    "[Console]::Out.Write($dr+' '+$bl+' '+$pw+' '+$id+' '+$why)\n"
    "}finally{$mx.ReleaseMutex();$mx.Dispose()}\n";
_Static_assert(sizeof GVTWIN_PS < 11000, "GVTWIN_PS must fit Windows' command line once encoded");

static int word(const char *const *names, const char *w, size_t l) {
    for (int i = 0; names[i]; i++) if (strlen(names[i]) == l && !memcmp(names[i], w, l)) return i + 1;
    return 0;
}

/* the next space-separated word: its start and length; *p moves past it and one space */
static const char *next(const char **p, size_t *l) {
    const char *s = *p, *e = strchr(s, ' ');
    if (!e) e = s + strlen(s);
    *l = (size_t)(e - s);
    *p = *e ? e + 1 : e;
    return s;
}

int gvtwin_parse(const char *said, GvtWinResult *r) {
    static const char *const D[] = { "already", "installed", "restart", "refused", "failed", "nodevice", "busy", NULL };
    static const char *const B[] = { "kept", "added", "none", "failed", NULL };
    static const char *const P[] = { "off", "turned", "failed", NULL };
    memset(r, 0, sizeof *r);
    if (!said) return -1;
    const char *p = said, *w;
    size_t l;
    w = next(&p, &l); if (!(r->driver = word(D, w, l))) return -1;
    w = next(&p, &l); if (!(r->block = word(B, w, l))) return -1;
    w = next(&p, &l); if (!(r->power = word(P, w, l))) return -1;
    w = next(&p, &l);
    if (l == 1 && w[0] == '-') r->id[0] = 0;
    else {
        static const char pre[] = "PCI\\VEN_8086&DEV_";
        if (l != GVTWIN_ID_MAX || memcmp(w, pre, sizeof pre - 1)) return -1;
        for (size_t i = sizeof pre - 1; i < l; i++) if (!((w[i] >= '0' && w[i] <= '9') || (w[i] >= 'A' && w[i] <= 'F'))) return -1;
        memcpy(r->id, w, l);
        r->id[l] = 0;
    }
    /* a driver said to be on a device, or a block, names that device */
    if (!r->id[0] && (r->driver == GW_D_ALREADY || r->driver == GW_D_INSTALLED || r->driver == GW_D_RESTART || r->block == GW_B_KEPT || r->block == GW_B_ADDED)) return -1;
    /* only a driver that is there gets the block */
    if ((r->block == GW_B_KEPT || r->block == GW_B_ADDED || r->block == GW_B_FAILED) && r->driver != GW_D_ALREADY && r->driver != GW_D_INSTALLED && r->driver != GW_D_RESTART) return -1;
    l = strlen(p);
    if (l > GVTWIN_WHY_MAX) return -1;
    for (size_t i = 0; i < l; i++) if (p[i] < 0x20 || p[i] > 0x7e) return -1;
    memcpy(r->why, p, l);
    r->why[l] = 0;
    return 0;
}

void gvtwin_describe(const GvtWinResult *r, char *out, size_t n) {
    if (!n) return;
    const char *d = "?";
    switch (r->driver) {
    case GW_D_ALREADY: d = "already " GVTWIN_DRV_VERSION; break;
    case GW_D_INSTALLED: d = GVTWIN_DRV_VERSION " installed"; break;
    case GW_D_RESTART: d = GVTWIN_DRV_VERSION " installed, the guest must restart to use it"; break;
    case GW_D_REFUSED: d = "refused"; break;
    case GW_D_FAILED: d = "not installed"; break;
    case GW_D_NODEVICE: d = "the guest has no Intel display device"; break;
    case GW_D_BUSY: d = "another run is still at work"; break;
    }
    const char *b = r->block == GW_B_KEPT ? "; its updates stay blocked" : r->block == GW_B_ADDED ? "; its updates are blocked now"
                  : r->block == GW_B_FAILED ? "; its updates could NOT be blocked" : "";
    const char *pw = r->power == GW_P_OFF ? "; hibernation and Fast Startup stay off" : r->power == GW_P_TURNED ? "; hibernation and Fast Startup turned off"
                   : r->driver == GW_D_BUSY ? "" : "; hibernation could NOT be turned off";
    snprintf(out, n, "intel driver: %s%s%s%s%s%s", d, r->id[0] ? " (" : "", r->id, r->id[0] ? ")" : "", b, pw);
    if (r->why[0]) {
        size_t o = strlen(out);
        if (o < n) snprintf(out + o, n - o, ": %s", r->why);
    }
}
