#!/bin/sh
# session.sh — real headless LordOS sessions (lord-server) for the Cloud end-to-end bench, one per throwaway Linux user,
# each with its own HOME, runtime dir and keyring, as a transient unit e2e-client-<user>. Run as root on the VM.
#
#   sudo sh session.sh install TREE    copy TREE (a built checkout: server/build, themes, assets) to /opt/lordos-e2e,
#                                      root-owned (lord-vault must be owned by root or the user, docs/PASSWORDS.md)
#   sudo sh session.sh start USER [WINDOWS_JSON]   create USER if needed, start its session, unlock its keyring
#   sudo sh session.sh stop USER        log out (stop the unit; rclone ends with lord-server)
#   sudo sh session.sh status USER
#   sudo sh session.sh purge USER       stop, unmount, remove the user and its home
#
# The theme is themes/default/theme.e2e.bundle.js (build-e2e.mjs): the real theme plus driver.js, so lc.py can call
# the SDK's cloud.* in the session. The keyring is unlocked the way lord-sessiond does it after a login: the user's
# password over the session's remote.sock (RP_KEYRING, root only), which creates the login keyring the first time.
set -eu
ACT=${1:?usage}
OPT=/opt/lordos-e2e
HERE=$(cd "$(dirname "$0")" && pwd)
pw() { echo "E2e-client-$1-0001"; }
rt() { echo "/run/e2e-rt/$1"; }

case $ACT in
install)
    T=${2:?tree}
    mkdir -p $OPT
    rsync -a --delete --exclude server/build/obj --exclude server/build/qjs --exclude out --exclude node_modules "$T"/ $OPT/
    # the test bundle (the real theme + driver.js) where the theme is, and the manifest that lists it (build-e2e.mjs)
    cp $OPT/themes/default/theme.e2e.bundle.js $OPT/themes/default/theme.bundle.js
    cp $OPT/themes/default/code-manifest.e2e.json $OPT/code-manifest.json; rm -f $OPT/code-manifest.sig
    chown -R root:root $OPT; chmod -R go-w $OPT
    ls -la $OPT/server/build/lord-server $OPT/server/build/lord-vault $OPT/themes/default/theme.bundle.js ;;
start)
    U=${2:?user}
    WINS=${3:-[]}
    id "$U" >/dev/null 2>&1 || { useradd -m -s /bin/bash "$U"; echo "$U:$(pw "$U")" | chpasswd; }
    chmod 700 /home/"$U"
    R=$(rt "$U")
    mkdir -p /run/e2e-rt; chmod 755 /run/e2e-rt
    install -d -m 700 -o "$U" -g "$U" "$R"
    H=/home/$U
    install -d -m 700 -o "$U" -g "$U" "$H/.config" "$H/.config/lordos" "$H/.e2e"
    rm -f "$H"/.e2e/cmd-* "$H"/.e2e/out-* "$H"/.e2e/next
    [ -f "$H/.config/lordos/settings.json" ] && [ "$WINS" = "[]" ] || cat > "$H/.config/lordos/settings.json" <<J
{ "mode": "dark", "language": "en", "languages": ["en"], "welcomeShown": true, "schema": 1, "reduceMotion": true,
  "sessionRestore": "always", "sessionSaved": { "clean": true, "active": 0, "windows": $WINS } }
J
    chown "$U": "$H/.config/lordos/settings.json"
    systemctl reset-failed "e2e-client-$U" 2>/dev/null || true
    systemd-run --quiet --unit="e2e-client-$U" --uid="$U" --gid="$U" --working-directory=$OPT/server \
        --property=KillMode=mixed --property=StandardOutput=file:/var/log/e2e-client-$U.log --property=StandardError=inherit \
        -E HOME="$H" -E USER="$U" -E LOGNAME="$U" -E XDG_RUNTIME_DIR="$R" -E PATH=/usr/bin:/bin -E LANG=C.UTF-8 \
        /usr/bin/dbus-run-session -- $OPT/server/build/lord-server --headless 1280x800 --user "$U" \
        --theme ../themes/default/theme.bundle.js --fonts ../assets/fonts --shell ../assets/shell
    i=0; while [ ! -S "$R/lordos/remote.sock" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
    [ -S "$R/lordos/remote.sock" ] || { echo "no remote.sock for $U"; tail -20 /var/log/e2e-client-$U.log; exit 1; }
    sleep 2
    python3 -I "$HERE/ui.py" "$R/lordos/remote.sock" "keyring:$(pw "$U")" >/dev/null
    echo "session $U up (keyring password sent)" ;;
stop)
    U=${2:?user}
    systemctl stop "e2e-client-$U" 2>/dev/null || true
    sleep 1
    for m in /home/"$U"/Cloud/*; do [ -d "$m" ] && mountpoint -q "$m" && echo "still mounted after logout: $m"; done; true ;;
status)
    U=${2:?user}
    systemctl is-active "e2e-client-$U" || true
    pgrep -u "$U" -a | cut -c1-200 ;;
purge)
    U=${2:?user}
    systemctl stop "e2e-client-$U" 2>/dev/null || true
    for m in /home/"$U"/Cloud/*; do mountpoint -q "$m" 2>/dev/null && umount -l "$m"; done
    pkill -u "$U" 2>/dev/null || true; sleep 1
    userdel -r "$U" 2>/dev/null || true
    rm -rf "$(rt "$U")" ;;
esac
