#!/bin/sh
# sftp_e2e.sh — the Terminal's Files mode end to end with real OpenSSH, as the user, no root: a private sshd on a
# free high port with throwaway keys (nothing in ~/.ssh is read or changed), an ssh ControlMaster to it as the
# Terminal's shell integration makes one, and build/sftpc-test driving lord-server's side (term/sftpc.c) through it.
#   sh tests/sftp_e2e.sh <build dir>
set -u
B=${1:?build dir}
case $B in /*) ;; *) B=$(pwd)/$B ;; esac
SSHD=/usr/sbin/sshd
for x in "$SSHD" /usr/bin/ssh /usr/bin/ssh-keygen /usr/lib/openssh/sftp-server; do
    [ -x "$x" ] || { echo "skipped: needs $x"; exit 0; }
done
T=$(mktemp -d "${TMPDIR:-/tmp}/sftp-e2e.XXXXXX") || exit 1
SPID= MPID=
cleanup() {
    [ -n "$MPID" ] && kill "$MPID" 2>/dev/null
    [ -n "$SPID" ] && kill "$SPID" 2>/dev/null
    sleep 0.2
    rm -rf "$T"
}
trap cleanup EXIT INT TERM
ssh-keygen -q -t ed25519 -N '' -f "$T/hostkey" >/dev/null
ssh-keygen -q -t ed25519 -N '' -f "$T/key" >/dev/null
cp "$T/key.pub" "$T/authorized_keys"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$T/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $T/hostkey
AuthorizedKeysFile $T/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile $T/sshd.pid
AllowTcpForwarding yes
Subsystem sftp /usr/lib/openssh/sftp-server
EOF
"$SSHD" -D -f "$T/sshd_config" -E "$T/sshd.log" &
SPID=$!
mkdir -p -m 700 "$T/rt" "$T/rt/lordos-term" "$T/rt/lordos-term/ssh" "$T/home" "$T/remote"
echo "hello from the server" > "$T/remote/hello.txt"
for i in 1 2 3 4 5 6 7 8 9 10; do
    python3 -c "import socket,sys; socket.create_connection(('127.0.0.1',$PORT),1)" 2>/dev/null && break
    sleep 0.3
done
# the master, as the shell integration's ssh() makes it
ssh -F /dev/null -i "$T/key" -p "$PORT" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR -o ControlMaster=auto -o "ControlPath=$T/rt/lordos-term/ssh/%C-$$" -o ControlPersist=no -N 127.0.0.1 &
MPID=$!
for i in $(seq 1 30); do ls "$T/rt/lordos-term/ssh/" 2>/dev/null | grep -q . && break; sleep 0.2; done
if ! ls "$T/rt/lordos-term/ssh/" | grep -q .; then
    echo "skipped: the private sshd did not take the login (see its log)"; tail -5 "$T/sshd.log"; exit 0
fi
XDG_RUNTIME_DIR="$T/rt" "$B/sftpc-test" "$MPID" "$T/home" "$T/remote" "$PORT"
