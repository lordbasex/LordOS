/* lanservices.h — the DNS-SD service types Files → Network asks for, and what each one is called
 * in the interface (a key of files.net.kind.*). */
#ifndef LORD_LAN_SERVICES_H
#define LORD_LAN_SERVICES_H

static const struct { const char *type, *kind; } lan_services[] = {
    { "_smb._tcp", "smb" }, { "_nfs._tcp", "nfs" }, { "_afpovertcp._tcp", "afp" }, { "_webdav._tcp", "webdav" },
    { "_webdavs._tcp", "webdavs" }, { "_sftp-ssh._tcp", "sftp" }, { "_ftp._tcp", "ftp" }, { "_ssh._tcp", "ssh" },
    { "_workstation._tcp", "computer" }, { "_http._tcp", "http" }, { "_https._tcp", "https" }, { "_ipp._tcp", "printer" },
    { "_ipps._tcp", "printer" }, { "_rfb._tcp", "screen" }, { "_rdp._tcp", "screen" },
};
#define LAN_SERVICES ((int)(sizeof lan_services / sizeof lan_services[0]))

#endif
