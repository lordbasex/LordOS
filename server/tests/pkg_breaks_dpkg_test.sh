#!/bin/sh
# pkg_breaks_dpkg_test.sh — updating lordos and lordos-web together, against the real dpkg in a throwaway root
# (dpkg --root, an empty database: never this machine's). Two dummy packages carry the relationships
# tools/iso/mkdeb.sh and tools/iso/mkdeb-web.sh write (lordos "Breaks: lordos-web (<< V)", lordos-web
# "Depends: lordos (= V)"), read from those scripts. dpkg checks a Breaks when it unpacks, against what's
# installed then, so in one run lordos first fails (found on 0.8.8.9~test1 -> ~test2, 2026-11-15); the
# engine's order (build/pkg-test order: plan_make + plan_order, as apply_plan runs them) puts lordos-web first.
# Needs dpkg and dpkg-deb (tests/run_suites.sh skips it without them). Run from server/.
set -u
TOP=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$(pwd)/build/pkg-test
[ -x "$BIN" ] || { echo "FAIL: build/pkg-test is missing"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-pkgbreaks.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
PATH=$PATH:/usr/sbin:/sbin             # dpkg looks for ldconfig and start-stop-daemon
export PATH

# the relationships as the build scripts write them, with $VERSION / $V standing for the version
BRK=$(sed -n 's/^Breaks: *\(lordos-web ([^)]*)\).*/\1/p' "$TOP/tools/iso/mkdeb.sh")
DEP=$(sed -n 's/^Depends: *\(lordos ([^)]*)\).*/\1/p' "$TOP/tools/iso/mkdeb-web.sh")
[ -n "$BRK" ] || fail "mkdeb.sh: no 'Breaks: lordos-web (...)'"
[ -n "$DEP" ] || fail "mkdeb-web.sh: no 'Depends: lordos (...)'"
[ $fails -eq 0 ] || exit 1

V1=0.8.8.9~test1 V2=0.8.8.9~test2
mk() {                                  # NAME VERSION RELATION-FIELD
	d=$T/src/$1_$2
	mkdir -p "$d/DEBIAN" "$d/usr/share/$1"
	echo "$2" > "$d/usr/share/$1/version"
	printf 'Package: %s\nVersion: %s\nArchitecture: all\nMaintainer: t <t@t>\nDescription: t\n%s\n' "$1" "$2" "$3" > "$d/DEBIAN/control"
	dpkg-deb -Zgzip --root-owner-group -b "$d" "$T/$1_$2_all.deb" > /dev/null || fail "dpkg-deb $1 $2"
}
for v in $V1 $V2; do
	mk lordos "$v" "Breaks: $(echo "$BRK" | sed "s/\\\$VERSION/$v/; s/\\\${VERSION}/$v/")"
	mk lordos-web "$v" "Depends: $(echo "$DEP" | sed "s/\\\$V\\b/$v/; s/\\\${V}/$v/; s/\\\$V)/$v)/")"
done
grep -q "($V2)\|$V2)" "$T/src/lordos-web_$V2/DEBIAN/control" || fail "lordos-web control: $(cat "$T/src/lordos-web_$V2/DEBIAN/control")"
grep -q "$V2" "$T/src/lordos_$V2/DEBIAN/control" || fail "lordos control: $(cat "$T/src/lordos_$V2/DEBIAN/control")"

R=$T/root
D() { LC_ALL=C dpkg --root="$R" --force-not-root --force-script-chrootless --log=/dev/null "$@"; }
old() {                                 # a fresh root with both at V1
	rm -rf "$R"
	mkdir -p "$R/var/lib/dpkg/info" "$R/var/lib/dpkg/updates" "$R/usr/share"
	: > "$R/var/lib/dpkg/status"
	D -i "$T/lordos_${V1}_all.deb" "$T/lordos-web_${V1}_all.deb" > "$T/out" 2>&1 || { fail "installing $V1: $(cat "$T/out")"; return 1; }
}
state() { D -l lordos lordos-web 2>/dev/null | awk '/^[a-zA-Z][a-zA-Z] /{printf "%s %s %s;", $1, $2, $3}'; }
BOTH2="ii lordos $V2;ii lordos-web $V2;"
BOTH1="ii lordos $V1;ii lordos-web $V1;"

# 1. the engine's order, Update all and lordos alone: one dpkg run, lordos-web first, no deconfiguration needed
pk() {                                  # the Packages stanza of a dummy .deb
	dpkg-deb -f "$1"
	printf 'Filename: %s\nSize: 1\nSHA256: %064d\n\n' "$(basename "$1")" 0
}
{ pk "$T/lordos_${V2}_all.deb"; pk "$T/lordos-web_${V2}_all.deb"; } > "$T/Packages"
for req in --up lordos lordos-web; do
	old || continue
	cp "$R/var/lib/dpkg/status" "$T/status"
	rm -rf "$T/idx"; mkdir -p "$T/idx"
	"$BIN" order "$T/idx" "$T/Packages" "$T/status" "$req" > "$T/order" 2>&1 || { fail "engine plan ($req): $(cat "$T/order")"; continue; }
	[ "$(wc -l < "$T/order")" -eq 1 ] || fail "engine ($req): more than one dpkg run: $(cat "$T/order")"
	[ "$(cat "$T/order")" = "lordos-web_${V2}_all.deb lordos_${V2}_all.deb" ] || fail "engine ($req): order $(cat "$T/order")"
	set -- $(sed "s|[^ ]*|$T/&|g" "$T/order")
	D --install "$@" > "$T/out" 2>&1 || fail "engine order ($req): dpkg failed: $(cat "$T/out")"
	[ "$(state)" = "$BOTH2" ] || fail "engine order ($req): $(state)"
	! grep -q "De-configuring" "$T/out" || fail "engine order ($req): something was deconfigured"
	# apply_plan's own flags
	old || continue
	D --force-confdef --force-confold --auto-deconfigure --install "$@" > "$T/out" 2>&1 || fail "apply_plan's flags ($req): $(cat "$T/out")"
	[ "$(state)" = "$BOTH2" ] || fail "apply_plan's flags ($req): $(state)"
done

# 2. plain dpkg, both in one call: lordos-web first works; lordos first needs --auto-deconfigure (dpkg's rule:
#    docs/PACKAGES.md); without it dpkg refuses lordos and leaves lordos-web unpacked, as on the maintainer's PC
old && { D -i "$T/lordos-web_${V2}_all.deb" "$T/lordos_${V2}_all.deb" > "$T/out" 2>&1 || fail "web first: $(cat "$T/out")"; }
[ "$(state)" = "$BOTH2" ] || fail "web first: $(state)"
old && { D --auto-deconfigure -i "$T/lordos_${V2}_all.deb" "$T/lordos-web_${V2}_all.deb" > "$T/out" 2>&1 || fail "lordos first, --auto-deconfigure: $(cat "$T/out")"; }
[ "$(state)" = "$BOTH2" ] || fail "lordos first, --auto-deconfigure: $(state)"
old && { if D -i "$T/lordos_${V2}_all.deb" "$T/lordos-web_${V2}_all.deb" > "$T/out" 2>&1; then
	echo "note: this dpkg accepts lordos first without --auto-deconfigure"
else
	grep -q "installing lordos would break lordos-web" "$T/out" || fail "lordos first: unexpected error: $(cat "$T/out")"
fi; }

# 3. lordos alone over an old lordos-web: refused before anything is unpacked, both stay configured
old && { if D -i "$T/lordos_${V2}_all.deb" > "$T/out" 2>&1; then fail "lordos alone was accepted: $(state)"; fi; }
grep -q "lordos breaks lordos-web" "$T/out" || fail "lordos alone: no clear message: $(cat "$T/out")"
[ "$(state)" = "$BOTH1" ] || fail "lordos alone changed the system: $(state)"
D --audit > "$T/out" 2>&1; [ ! -s "$T/out" ] || fail "lordos alone: dpkg --audit: $(cat "$T/out")"

[ $fails -eq 0 ] && echo "pkg-breaks-dpkg-test: ok"
exit $fails
