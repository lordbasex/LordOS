#!/bin/sh
# session.sh — a real headless LordOS session (lord-server) for Machines' end-to-end bench, the way the Cloud bench
# (branch cloud-e2e, tools/cloud/e2e/session.sh) makes them, with one difference: lord-server starts as root with
# --user, as lord-sessiond starts it, so the session has its administration helper (lord-admin: vm-image-list,
# vm-image-get, vm-image-remove) and drops to the user itself. Run as root on the build VM. Test only.
#
#   sudo sh session.sh install TREE    copy TREE (a built checkout) to /opt/lordos-vm-e2e, root-owned, with the bench's
#                                      theme bundle; the download catalog and the makers' keys where the helper reads
#                                      them (/usr/lib/lordos/assets/machines)
#   sudo sh session.sh start USER      create USER (in sudo and kvm) if needed and start its session
#   sudo sh session.sh stop USER | status USER | purge USER
set -eu
ACT=${1:?usage}
OPT=/opt/lordos-vm-e2e
pw() { echo "Vm-e2e-$1-0001"; }
rt() { echo "/run/vm-e2e-rt/$1"; }

case $ACT in
install)
    T=${2:?tree}
    mkdir -p $OPT
    rsync -a --delete --exclude server/build/obj --exclude server/build/qjs --exclude out --exclude node_modules "$T"/ $OPT/
    cp $OPT/themes/default/theme.vm-e2e.bundle.js $OPT/themes/default/theme.bundle.js
    cp $OPT/themes/default/code-manifest.vm-e2e.json $OPT/code-manifest.json; rm -f $OPT/code-manifest.sig
    chown -R root:root $OPT; chmod -R go-w $OPT
    install -d /usr/lib/lordos/assets/machines/keys
    install -m 644 $OPT/assets/machines/systems.json /usr/lib/lordos/assets/machines/systems.json
    install -m 644 $OPT/packaging/keys/machines/*.asc /usr/lib/lordos/assets/machines/keys/
    ls -la $OPT/server/build/lord-server $OPT/server/build/lord-vm $OPT/server/build/lord-tls /usr/lib/lordos/assets/machines/keys ;;
start)
    U=${2:?user}
    id "$U" >/dev/null 2>&1 || { useradd -m -s /bin/bash -G sudo,kvm "$U"; echo "$U:$(pw "$U")" | chpasswd; }
    chmod 700 /home/"$U"
    R=$(rt "$U")
    mkdir -p /run/vm-e2e-rt; chmod 755 /run/vm-e2e-rt
    install -d -m 700 -o "$U" -g "$U" "$R"
    H=/home/$U
    install -d -m 700 -o "$U" -g "$U" "$H/.config" "$H/.config/lordos" "$H/.e2e"
    rm -f "$H"/.e2e/cmd-* "$H"/.e2e/out-* "$H"/.e2e/next
    [ -f "$H/.config/lordos/settings.json" ] || cat > "$H/.config/lordos/settings.json" <<J
{ "mode": "dark", "language": "en", "languages": ["en"], "welcomeShown": true, "schema": 1, "reduceMotion": true }
J
    chown "$U": "$H/.config/lordos/settings.json"
    systemctl reset-failed "vm-e2e-$U" 2>/dev/null || true
    systemd-run --quiet --unit="vm-e2e-$U" --working-directory=$OPT/server \
        --property=KillMode=mixed --property=StandardOutput=file:/var/log/vm-e2e-$U.log --property=StandardError=inherit \
        -E HOME="$H" -E XDG_RUNTIME_DIR="$R" -E PATH=/usr/bin:/bin:/usr/sbin:/sbin -E LANG=C.UTF-8 \
        $OPT/server/build/lord-server --headless 1280x800 --user "$U" \
        --theme ../themes/default/theme.bundle.js --fonts ../assets/fonts --shell ../assets/shell
    i=0; while [ ! -S "$R/lordos/remote.sock" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
    [ -S "$R/lordos/remote.sock" ] || { echo "no remote.sock for $U"; tail -20 /var/log/vm-e2e-$U.log; exit 1; }
    echo "session $U up" ;;
stop)
    U=${2:?user}
    systemctl stop "vm-e2e-$U" 2>/dev/null || true ;;
status)
    U=${2:?user}
    systemctl is-active "vm-e2e-$U" || true
    pgrep -u "$U" -a | cut -c1-200 ;;
purge)
    U=${2:?user}
    systemctl stop "vm-e2e-$U" 2>/dev/null || true
    pkill -u "$U" 2>/dev/null || true; sleep 1
    userdel -r "$U" 2>/dev/null || true
    rm -rf "$(rt "$U")" ;;
esac
