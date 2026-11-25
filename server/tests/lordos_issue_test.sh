#!/bin/sh
# lordos_issue_test.sh — the parts of packaging/bin/lordos-issue that read untrusted text, on fixtures
# (tests/data/lordos-issue/):
#   - --scrub-wayland-debug on a WAYLAND_DEBUG log full of titles, paths, typed text, a monitor's serial and an app id:
#     none may come out; interface names and clipboard types stay; the program's own messages are dropped;
#   - --test-clients on `ss -xpnH` text: the programs connected to the Wayland and X11 sockets, lord-server left out,
#     a hostile process name neutralised;
#   - --test-globals on lordos-testbench --globals' JSON;
#   - --app with a fake program that prints protocol lines with secrets: the saved file is scrubbed, 0600, at most 200
#     protocol lines, and the program is stopped after --seconds.
# Run from server/ (tests/run_suites.sh does). A private HOME; nothing of the session is read.
cd "$(dirname "$0")/.." || exit 1
I=../packaging/bin/lordos-issue
F=tests/data/lordos-issue
T=$(mktemp -d "${TMPDIR:-/tmp}/lissue.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0
bad() { fails=$((fails + 1)); echo "FAIL: $*"; }
has() { grep -qF -- "$2" "$1" || bad "$3: missing «$2»"; }
hasnt() { if grep -qiF -- "$2" "$1"; then bad "$3: leaked «$2»"; grep -iF -- "$2" "$1" | head -3 | sed 's/^/    /'; fi; }

# ---- the scrubbing ----
sh "$I" --scrub-wayland-debug < "$F/wayland-debug.log" > "$T/scrubbed" || bad "scrub exited $?"
for secret in secretuser secret-plans Budget hello letter.txt Private PrivateApp secretcorp taxes.pdf hunter2 password Alice \
              "Secret window" "Dell" SN-ABC123 "rm -rf" "my secret line" canberra Gtk-WARNING /home; do
    hasnt "$T/scrubbed" "$secret" scrub
done
has "$T/scrubbed" 'wl_registry#2.global(1, "wl_compositor", 6)' "interface name kept"
has "$T/scrubbed" 'wl_registry#2.bind(2, "xdg_wm_base", 5, new id [unknown]@3)' "bind kept"
has "$T/scrubbed" 'xdg_toplevel#25.set_title(<text>)' "title"
has "$T/scrubbed" 'wl_data_offer#40.offer("text/plain;charset=utf-8")' "mime type kept"
has "$T/scrubbed" 'wl_data_offer#40.receive("text/plain;charset=utf-8", fd 31)' "receive's mime type kept"
has "$T/scrubbed" 'wl_data_offer#41.offer(<text>)' "a path posing as a mime type"
has "$T/scrubbed" 'wl_registry#2.global(9, <text>, 1)' "quotes inside a string"
has "$T/scrubbed" 'zwp_text_input_v3#30.set_surrounding_text(<text>, 22, 22)' "typed text"
has "$T/scrubbed" 'wl_display#1.error(xdg_surface#24, 3, <text>)' "error text"
has "$T/scrubbed" 'wl_surface#20.commit()' "a line without strings"
has "$T/scrubbed" 'wl_data_source#60.offer(<text>' "an unterminated string"
has "$T/scrubbed" 'wl_data_source#60.offer("text/plain")' "plain mime type"
has "$T/scrubbed" '(3 lines that were not Wayland protocol left out)' "dropped lines counted"
# the app id: a stable hash, the same in both requests
ids=$(grep -o '"app-id:[0-9a-f]\{8\}"' "$T/scrubbed" | sort -u | wc -l)
[ "$ids" = 1 ] || bad "app id hash: $ids distinct (want 1)"
[ "$(grep -c 'app-id:' "$T/scrubbed")" = 2 ] || bad "app id: both requests hashed"
[ "$(grep -c '^\[' "$T/scrubbed")" = 21 ] || bad "scrub kept $(grep -c '^\[' "$T/scrubbed") protocol lines, want 21"

# ---- the programs connected to the sockets ----
sh "$I" --test-clients /run/user/1000/wayland-0 /tmp/.X11-unix/X1 < "$F/ss.txt" > "$T/clients" || bad "clients exited $?"
has "$T/clients" "foot             Wayland" clients
has "$T/clients" "firefox-esr      Wayland (2 connections)" clients
has "$T/clients" "Xwayland         Wayland" clients
has "$T/clients" "xterm            X11 (Xwayland)" clients
has "$T/clients" "(another user)   Wayland" clients
hasnt "$T/clients" "lord-server" clients
hasnt "$T/clients" "secret-app" clients
hasnt "$T/clients" "dbus" clients
hasnt "$T/clients" '"' clients
has "$T/clients" "evil?" "hostile name"
[ "$(wc -l < "$T/clients")" = 6 ] || { bad "clients: $(wc -l < "$T/clients") lines, want 6"; cat "$T/clients"; }
printf '' | sh "$I" --test-clients /run/user/1000/wayland-0 '' > "$T/none"
has "$T/none" "  none" "no clients"

# ---- lordos-testbench --globals' summary ----
J='{"tool":"lordos-testbench","v":1,"mode":"globals","at":1,"globals":[{"interface":"wl_compositor","version":6},{"interface":"wl_seat","version":9},{"interface":"zwlr_layer_shell_v1","version":4}],"table":{"source":"docs/wayland-protocols.json","expected":48,"offered":45,"newer":0,"older":1,"missing":1,"notHere":1,"extra":["zwlr_layer_shell_v1"],"rows":[{"interface":"wl_compositor","protocol":"wayland-wl_compositor","expected":6,"advertised":6,"state":"offered","offeredTo":"all","partial":false},{"interface":"xdg_wm_base","protocol":"xdg-shell","expected":7,"advertised":5,"state":"older","offeredTo":"all","partial":false},{"interface":"wp_viewporter","protocol":"viewporter","expected":1,"advertised":0,"state":"missing","offeredTo":"all","partial":false},{"interface":"xwayland_shell_v1","protocol":"xwayland-shell-v1","expected":1,"advertised":0,"state":"not-here","offeredTo":"xwayland","partial":false}]}}'
printf '%s\n' "$J" | sh "$I" --test-globals > "$T/glob"
has "$T/glob" "advertised: 3 globals" globals
has "$T/glob" "LordOS table: 48 expected; 45 offered, 0 newer, 1 older, 1 missing, 1 offered only to some clients" globals
has "$T/glob" "  xdg_wm_base: older (advertised 5, table 7)" globals
has "$T/glob" "  wp_viewporter: missing (advertised 0, table 1)" globals
has "$T/glob" "  xwayland_shell_v1: not offered to this client (only to: xwayland)" globals
has "$T/glob" "not in the table: zwlr_layer_shell_v1" globals
hasnt "$T/glob" "wl_compositor: " "an offered global is not listed"

# ---- --app with a fake program ----
mkdir -p "$T/home" "$T/bin"
cat > "$T/bin/fakeapp" <<'EOF'
#!/bin/sh
[ "$WAYLAND_DEBUG" = 1 ] || { echo "no WAYLAND_DEBUG" >&2; exit 1; }
echo "stdout: /home/secretuser/notes.txt"
i=0
while [ $i -lt 300 ]; do
    echo "[100$i.000] {Default Queue}  -> xdg_toplevel#5.set_title(\"secret title $i $1\")" >&2
    echo "[100$i.001] {Default Queue}  -> wl_surface#3.commit()" >&2
    i=$((i + 1))
done
echo "warning: cannot read /home/secretuser/.config/secret" >&2
sleep 30
EOF
chmod 755 "$T/bin/fakeapp"
start=$(date +%s)
HOME="$T/home" PATH="$T/bin:$PATH" WAYLAND_DISPLAY=wayland-test sh "$I" --seconds 2 --app fakeapp "arg-secret" 2> "$T/app.err" || bad "--app exited $?"
took=$(( $(date +%s) - start ))
[ "$took" -lt 15 ] || bad "--app took ${took}s with --seconds 2"
out=$(ls "$T/home"/lordos-wayland-*.txt 2>/dev/null | head -n 1)
if [ -z "$out" ]; then bad "--app saved nothing"; cat "$T/app.err"
else
    has "$T/app.err" "Saved to $out" "--app says where"
    has "$T/app.err" "Read it first" "--app says to read it"
    [ "$(stat -c %a "$out")" = 600 ] || bad "--app file mode $(stat -c %a "$out")"
    for secret in "secret title" arg-secret secretuser notes.txt "/home"; do hasnt "$out" "$secret" "--app"; done
    has "$out" 'xdg_toplevel#5.set_title(<text>)' "--app title"
    has "$out" 'wl_surface#3.commit()' "--app"
    n=$(grep -c '^\[' "$out")
    [ "$n" = 200 ] || bad "--app kept $n protocol lines, want 200"
    has "$out" "[100299.001]" "--app keeps the last lines"
fi
# never a program as root (checked without being root: the message's branch is the uid test)
grep -q 'lordos-issue --app runs the program as you' "$I" || bad "--app's root refusal is gone"

if [ "$fails" = 0 ]; then echo "lordos_issue_test: ok"; exit 0; fi
echo "lordos_issue_test: $fails failed"
exit 1
