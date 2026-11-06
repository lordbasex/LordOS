#!/bin/sh
# servers.sh — the Cloud end-to-end bench's servers on a throwaway Debian 13 VM (VM 116), every one bound to
# loopback, every credential a throwaway one (README.md lists them). Run as root:
#
#   sudo sh tools/cloud/e2e/servers.sh setup     users, keys, certificates, configs (idempotent)
#   sudo sh tools/cloud/e2e/servers.sh start     start every server (transient systemd units e2e-*)
#   sudo sh tools/cloud/e2e/servers.sh stop      stop them (the "offline" cases stop one at a time: stop-one NAME)
#   sudo sh tools/cloud/e2e/servers.sh status    what listens where
#   sudo sh tools/cloud/e2e/servers.sh purge     stop, remove users, configs and data
#
# Servers (all 127.0.0.1):
#   sshd   :2222   a second OpenSSH with its own config (the VM's own sshd on :22 is not touched)
#   http   :8080   Apache mod_dav, basic auth, plain HTTP              /dav/
#   https  :8443   the same over HTTPS, a self-signed certificate nobody trusts
#   httpsca:8444   the same over HTTPS, a certificate from the bench's own CA (installed in the system's trust store)
#   minio  :9000   MinIO S3 (HTTP), two buckets, a root and a second access key        console :9001
#   minios :9443   MinIO again over TLS (CA certificate), so presigned links are https
#   rs3    :9100   rclone serve s3, a second S3 implementation
#   ftp    :2121   vsftpd, local users, passive 40000-40050
#   smb    :445    Samba on lo only, share [e2e]
set -eu
ACT=${1:-status}
E=/srv/e2e
CONF=/etc/lordos-e2e
RC=/usr/lib/lordos/rclone/rclone
MINIO=${MINIO:-/usr/local/lib/lordos-e2e/minio}
MC=${MC:-/usr/local/lib/lordos-e2e/mc}
PW_USER=e2epw   PW_PASS='E2e-pw-0001'
KEY_USER=e2ekey
DAV_USER=davuser DAV_PASS='E2e-dav-0001'
MINIO_ROOT=e2eminio MINIO_ROOT_PASS='E2e-minio-root-0001'
MINIO_KEY=e2eappkey MINIO_SECRET='E2e-minio-app-secret-0001'
RS3_KEY=e2ers3key RS3_SECRET='E2e-rs3-secret-0001'

unit() { systemctl is-active --quiet "e2e-$1" 2>/dev/null; }
run() {   # NAME COMMAND...   a transient unit, restarted by hand only
    n=$1; shift
    unit "$n" && return 0
    systemctl reset-failed "e2e-$n" 2>/dev/null || true
    systemd-run --quiet --unit="e2e-$n" --property=KillMode=mixed "$@"
}

setup() {
    mkdir -p $E $CONF
    # ---- users
    id $PW_USER >/dev/null 2>&1 || useradd -m -s /bin/bash $PW_USER
    echo "$PW_USER:$PW_PASS" | chpasswd
    id $KEY_USER >/dev/null 2>&1 || useradd -m -s /bin/bash $KEY_USER
    usermod -p '*' $KEY_USER                         # no password: keys only
    chmod 755 /home/$PW_USER /home/$KEY_USER         # so the bench can look at what arrived
    # ---- keys (the client side keeps copies in $CONF/keys, readable by the bench's client users only through the bench)
    mkdir -p $CONF/keys
    [ -f $CONF/keys/id_plain ] || ssh-keygen -q -t ed25519 -N '' -C e2e-plain -f $CONF/keys/id_plain
    [ -f $CONF/keys/id_pass ] || ssh-keygen -q -t ed25519 -N 'E2e-key-pass-0001' -C e2e-pass -f $CONF/keys/id_pass
    [ -f $CONF/keys/id_other ] || ssh-keygen -q -t ed25519 -N '' -C e2e-not-authorized -f $CONF/keys/id_other
    install -d -m 700 -o $KEY_USER -g $KEY_USER /home/$KEY_USER/.ssh
    cat $CONF/keys/id_plain.pub $CONF/keys/id_pass.pub > /home/$KEY_USER/.ssh/authorized_keys
    chown $KEY_USER: /home/$KEY_USER/.ssh/authorized_keys; chmod 600 /home/$KEY_USER/.ssh/authorized_keys
    chmod 644 $CONF/keys/*
    # ---- sshd :2222
    mkdir -p $CONF/ssh
    [ -f $CONF/ssh/host_ed25519 ] || ssh-keygen -q -t ed25519 -N '' -f $CONF/ssh/host_ed25519
    cat > $CONF/ssh/sshd_config <<EOF
Port 2222
ListenAddress 127.0.0.1
HostKey $CONF/ssh/host_ed25519
PidFile /run/e2e-sshd.pid
AllowUsers $PW_USER $KEY_USER
PasswordAuthentication yes
KbdInteractiveAuthentication no
PubkeyAuthentication yes
PermitRootLogin no
UsePAM yes
Subsystem sftp internal-sftp
LogLevel VERBOSE
PerSourcePenalties no
EOF
    # ---- certificates: a bench CA (trusted system-wide) and a self-signed one (trusted by nobody)
    mkdir -p $CONF/tls
    cd $CONF/tls
    if [ ! -f ca.crt ]; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj '/CN=LordOS E2E Test CA' -keyout ca.key -out ca.crt 2>/dev/null
        openssl req -newkey rsa:2048 -nodes -subj '/CN=127.0.0.1' -keyout ca-leaf.key -out ca-leaf.csr 2>/dev/null
        printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\nextendedKeyUsage=serverAuth\n' > leaf.ext
        openssl x509 -req -in ca-leaf.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 365 -extfile leaf.ext -out ca-leaf.crt 2>/dev/null
        openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj '/CN=127.0.0.1' -addext 'subjectAltName=IP:127.0.0.1,DNS:localhost' \
            -keyout self.key -out self.crt 2>/dev/null
    fi
    cp ca.crt /usr/local/share/ca-certificates/lordos-e2e-ca.crt && update-ca-certificates >/dev/null 2>&1
    cd /
    # ---- Apache WebDAV
    mkdir -p $E/dav/lock
    chown -R www-data: $E/dav
    a2enmod -q dav dav_fs auth_basic authn_file ssl headers >/dev/null
    htpasswd -bc $CONF/dav.htpasswd $DAV_USER "$DAV_PASS" 2>/dev/null
    chmod 640 $CONF/dav.htpasswd; chgrp www-data $CONF/dav.htpasswd
    cat > /etc/apache2/ports.conf <<EOF
Listen 127.0.0.1:8080
Listen 127.0.0.1:8443
Listen 127.0.0.1:8444
EOF
    a2dissite -q 000-default default-ssl >/dev/null 2>&1 || true
    cat > /etc/apache2/sites-available/e2e-dav.conf <<EOF
DavLockDB $E/dav/lock/DavLock
Alias /dav $E/dav/files
<Directory $E/dav/files>
    Dav On
    AuthType Basic
    AuthName "e2e"
    AuthUserFile $CONF/dav.htpasswd
    Require valid-user
    Options Indexes
    AllowOverride None
</Directory>
<VirtualHost 127.0.0.1:8080>
</VirtualHost>
<VirtualHost 127.0.0.1:8443>
    SSLEngine on
    SSLCertificateFile $CONF/tls/self.crt
    SSLCertificateKeyFile $CONF/tls/self.key
</VirtualHost>
<VirtualHost 127.0.0.1:8444>
    SSLEngine on
    SSLCertificateFile $CONF/tls/ca-leaf.crt
    SSLCertificateKeyFile $CONF/tls/ca-leaf.key
</VirtualHost>
EOF
    mkdir -p $E/dav/files; chown -R www-data: $E/dav
    a2ensite -q e2e-dav >/dev/null
    apache2ctl -t 2>&1 | grep -v 'Syntax OK' || true
    # ---- MinIO
    id minio-e2e >/dev/null 2>&1 || useradd -r -M -d $E/minio -s /usr/sbin/nologin minio-e2e
    mkdir -p $E/minio/data $E/minio/certs
    cp $CONF/tls/ca-leaf.crt $E/minio/certs/public.crt; cp $CONF/tls/ca-leaf.key $E/minio/certs/private.key
    chown -R minio-e2e: $E/minio
    # ---- rclone serve s3
    id rs3-e2e >/dev/null 2>&1 || useradd -r -M -d $E/rs3 -s /usr/sbin/nologin rs3-e2e
    mkdir -p $E/rs3/bucket-a $E/rs3/bucket-b; chown -R rs3-e2e: $E/rs3
    # ---- vsftpd
    cat > $CONF/vsftpd.conf <<EOF
listen=YES
listen_address=127.0.0.1
listen_port=2121
anonymous_enable=NO
local_enable=YES
write_enable=YES
local_umask=022
pasv_enable=YES
pasv_min_port=40000
pasv_max_port=40050
pasv_address=127.0.0.1
userlist_enable=YES
userlist_deny=NO
userlist_file=$CONF/vsftpd.users
secure_chroot_dir=/var/run/vsftpd/empty
pam_service_name=vsftpd
seccomp_sandbox=NO
EOF
    echo $PW_USER > $CONF/vsftpd.users
    mkdir -p /var/run/vsftpd/empty
    # ---- Samba (lo only)
    mkdir -p $E/smb; chown $PW_USER: $E/smb
    cat > $CONF/smb.conf <<EOF
[global]
   interfaces = lo
   bind interfaces only = yes
   smb ports = 445
   server role = standalone server
   map to guest = never
   disable netbios = yes
   log file = /var/log/samba/e2e.log
   pid directory = /run/e2e-samba
   lock directory = /var/lib/samba/e2e-lock
   state directory = /var/lib/samba/e2e-state
   cache directory = /var/cache/samba/e2e
   private dir = /var/lib/samba/e2e-private
[e2e]
   path = $E/smb
   valid users = $PW_USER
   read only = no
EOF
    mkdir -p /run/e2e-samba /var/lib/samba/e2e-lock /var/lib/samba/e2e-state /var/cache/samba/e2e /var/lib/samba/e2e-private
    printf '%s\n%s\n' "$PW_PASS" "$PW_PASS" | smbpasswd -c $CONF/smb.conf -s -a $PW_USER >/dev/null
    echo "setup done"
}

start_one() {
    case $1 in
    sshd) mkdir -p /run/sshd; run sshd /usr/sbin/sshd -D -e -f $CONF/ssh/sshd_config ;;
    dav) systemctl start apache2 ;;
    minio) [ -x $MINIO ] || { echo "no $MINIO"; return 0; }
        run minio --uid=minio-e2e -E MINIO_ROOT_USER=$MINIO_ROOT -E MINIO_ROOT_PASSWORD=$MINIO_ROOT_PASS -E MINIO_BROWSER=off \
            $MINIO server --address 127.0.0.1:9000 --console-address 127.0.0.1:9001 --certs-dir $E/minio/nocerts $E/minio/data ;;
    minios) [ -x $MINIO ] || return 0
        # a second MinIO process on the same data would fight over locks: its own data folder, the same buckets
        mkdir -p $E/minio/data-tls; chown minio-e2e: $E/minio/data-tls
        run minios --uid=minio-e2e -E MINIO_ROOT_USER=$MINIO_ROOT -E MINIO_ROOT_PASSWORD=$MINIO_ROOT_PASS -E MINIO_BROWSER=off \
            $MINIO server --address 127.0.0.1:9443 --console-address 127.0.0.1:9444 --certs-dir $E/minio/certs $E/minio/data-tls ;;
    rs3) run rs3 --uid=rs3-e2e $RC serve s3 $E/rs3 --addr 127.0.0.1:9100 --auth-key "$RS3_KEY,$RS3_SECRET" ;;
    ftp) run ftp /usr/sbin/vsftpd $CONF/vsftpd.conf ;;
    smb) mkdir -p /run/samba /run/e2e-samba; run smb /usr/sbin/smbd --foreground --no-process-group -s $CONF/smb.conf ;;
    esac
}
stop_one() {
    case $1 in dav) systemctl stop apache2 ;; *) systemctl stop "e2e-$1" 2>/dev/null || true ;; esac
}

minio_init() {   # buckets and the second access key (needs mc)
    [ -x $MC ] || { echo "no $MC"; return 0; }
    export MC_CONFIG_DIR=$CONF/mc
    for t in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null http://127.0.0.1:9000/minio/health/live && break; sleep 1; done
    $MC alias set e2e http://127.0.0.1:9000 $MINIO_ROOT "$MINIO_ROOT_PASS" >/dev/null
    $MC mb -p e2e/e2e-alpha e2e/e2e-beta >/dev/null
    $MC admin user add e2e $MINIO_KEY "$MINIO_SECRET" >/dev/null 2>&1 || true
    $MC admin policy attach e2e readwrite --user $MINIO_KEY >/dev/null 2>&1 || true
    if unit minios; then
        for t in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null https://127.0.0.1:9443/minio/health/live && break; sleep 1; done
        $MC alias set e2es https://127.0.0.1:9443 $MINIO_ROOT "$MINIO_ROOT_PASS" >/dev/null
        $MC mb -p e2es/e2e-alpha e2es/e2e-beta >/dev/null
        $MC admin user add e2es $MINIO_KEY "$MINIO_SECRET" >/dev/null 2>&1 || true
        $MC admin policy attach e2es readwrite --user $MINIO_KEY >/dev/null 2>&1 || true
    fi
    $MC ls e2e
}

ALL="sshd dav minio minios rs3 ftp smb"
case $ACT in
setup) setup ;;
start) for s in $ALL; do start_one $s; done; sleep 2; minio_init ;;
start-one) start_one "$2" ;;
stop-one) stop_one "$2" ;;
stop) for s in $ALL; do stop_one $s; done ;;
status) ss -ltnp | grep -E '127.0.0.1:(2222|8080|8443|8444|9000|9001|9443|9444|9100|2121|445) ' || true
    for s in sshd minio minios rs3 ftp smb; do printf '%-7s %s\n' $s "$(systemctl is-active e2e-$s 2>/dev/null || true)"; done
    printf '%-7s %s\n' dav "$(systemctl is-active apache2)" ;;
purge) for s in $ALL; do stop_one $s; done
    userdel -r $PW_USER 2>/dev/null || true; userdel -r $KEY_USER 2>/dev/null || true
    userdel minio-e2e 2>/dev/null || true; userdel rs3-e2e 2>/dev/null || true
    rm -f /usr/local/share/ca-certificates/lordos-e2e-ca.crt; update-ca-certificates --fresh >/dev/null 2>&1
    a2dissite -q e2e-dav >/dev/null 2>&1 || true
    rm -rf $E $CONF ;;
*) echo "usage: $0 setup|start|stop|status|start-one NAME|stop-one NAME|purge"; exit 2 ;;
esac
