#!/bin/sh
# fetch_fallback_test.sh — packaging/fallback-bin/fastfetch (also neofetch) and packaging/profile.d/lordos-fallback.sh:
# with no real fastfetch or neofetch in PATH they run lordos-fetch with only the options it knows (and a hint for
# the rest); a real one earlier in PATH wins, also when the fallback is called by its full path (a shell's stale
# command hash); the profile.d snippet appends the folder last, once, and only when it exists. Run from server/ by
# tests/run_suites.sh.
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-fetchfb.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0 n=0
check() { n=$((n + 1)); if [ "$2" = "$3" ]; then :; else echo "FAIL $1: got [$2], want [$3]"; fails=$((fails + 1)); fi; }

# the package's layout: fastfetch and a neofetch link to it in one folder; a fake lordos-fetch that prints its args
mkdir -p "$T/fallback" "$T/usrbin" "$T/real"
cp "$R/packaging/fallback-bin/fastfetch" "$T/fallback/fastfetch"
chmod 755 "$T/fallback/fastfetch"
ln -s fastfetch "$T/fallback/neofetch"
printf '#!/bin/sh\necho "lordos-fetch[$*]"\n' > "$T/lordos-fetch"
printf '#!/bin/sh\necho "real-fastfetch[$*]"\n' > "$T/real/fastfetch"
printf '#!/bin/sh\necho "real-neofetch[$*]"\n' > "$T/real/neofetch"
chmod 755 "$T/lordos-fetch" "$T/real/fastfetch" "$T/real/neofetch"
export LORDOS_FETCH_BIN="$T/lordos-fetch"
SYS="$T/usrbin:$T/bin"   # no /usr/bin: the host may have the real fastfetch; the wrapper needs only sh builtins

run() { PATH="$1" /bin/sh -c "$2" 2>"$T/err"; }

check "fastfetch, nothing installed" "$(run "$SYS:$T/fallback" 'fastfetch')" "lordos-fetch[]"
check "no hint without options" "$(cat "$T/err")" ""
check "neofetch, nothing installed" "$(run "$SYS:$T/fallback" 'neofetch')" "lordos-fetch[]"
check "known options pass" "$(run "$SYS:$T/fallback" 'fastfetch --share --color=always')" "lordos-fetch[--share --color=always]"
check "--pipe" "$(run "$SYS:$T/fallback" 'fastfetch --pipe')" "lordos-fetch[--color=never]"
check "--stdout" "$(run "$SYS:$T/fallback" 'neofetch --stdout --share')" "lordos-fetch[--color=never --share]"
check "unknown options dropped" "$(run "$SYS:$T/fallback" 'fastfetch --logo arch --share -c x.jsonc')" "lordos-fetch[--share]"
check "hint names fastfetch" "$(cat "$T/err")" "lordos-fetch: this is LordOS's fetch; install the fastfetch package for its options"
run "$SYS:$T/fallback" 'neofetch --ascii_distro debian' >/dev/null
check "hint names neofetch" "$(cat "$T/err")" "lordos-fetch: this is LordOS's fetch; install the neofetch package for its options"
check "argument with spaces kept" "$(run "$SYS:$T/fallback" 'fastfetch "--color=never"')" "lordos-fetch[--color=never]"

# the real package installed: /usr/bin comes before the fallback folder
cp "$T/real/fastfetch" "$T/real/neofetch" "$T/usrbin/"
check "real fastfetch wins" "$(run "$SYS:$T/fallback" 'fastfetch --logo arch')" "real-fastfetch[--logo arch]"
check "real neofetch wins" "$(run "$SYS:$T/fallback" 'neofetch --stdout')" "real-neofetch[--stdout]"
check "stale hash: real one still runs" "$(run "$SYS:$T/fallback" "$T/fallback/fastfetch -c a b")" "real-fastfetch[-c a b]"
check "stale hash: real neofetch" "$(run "$SYS:$T/fallback" "$T/fallback/neofetch")" "real-neofetch[]"
rm -f "$T/usrbin/fastfetch" "$T/usrbin/neofetch"
check "fallback first in PATH skips itself" "$(run "$T/fallback:$SYS" 'fastfetch')" "lordos-fetch[]"

# profile.d: appended last, once, only when the folder exists
P="$T/profile.sh"
sed "s|^__lordos_fallback=.*|__lordos_fallback=$T/fallback|" "$R/packaging/profile.d/lordos-fallback.sh" > "$P"
check "profile.d appends last" "$(PATH=/usr/bin:/bin sh -c ". '$P'; echo \"\$PATH\"")" "/usr/bin:/bin:$T/fallback"
check "profile.d is idempotent" "$(PATH=/usr/bin:/bin sh -c ". '$P'; . '$P'; echo \"\$PATH\"")" "/usr/bin:/bin:$T/fallback"
check "profile.d leaves no variable" "$(PATH=/usr/bin:/bin sh -c ". '$P'; echo \"\${__lordos_fallback-unset}\"")" "unset"
sed "s|^__lordos_fallback=.*|__lordos_fallback=$T/missing|" "$R/packaging/profile.d/lordos-fallback.sh" > "$P"
check "profile.d without the folder" "$(PATH=/usr/bin:/bin sh -c ". '$P'; echo \"\$PATH\"")" "/usr/bin:/bin"
command -v bash >/dev/null 2>&1 && {
    sed "s|^__lordos_fallback=.*|__lordos_fallback=$T/fallback|" "$R/packaging/profile.d/lordos-fallback.sh" > "$P"
    check "profile.d under bash" "$(PATH=/usr/bin:/bin bash -c ". '$P'; echo \"\$PATH\"")" "/usr/bin:/bin:$T/fallback"
}

# lord-sessiond's session PATH ends with the same folder (zsh and fish in the Terminal keep it)
check "session PATH" "$(grep -c '"PATH=/usr/local/bin:/usr/bin:/bin:/usr/games:/usr/lib/lordos/fallback-bin"' "$R/server/src/remote/sess.c")" "1"

[ "$fails" -eq 0 ] && echo "fetch-fallback: $n checks passed"
exit "$fails"
