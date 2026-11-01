/* lanproto.h — lord-server ↔ lord-lan, the helper behind Files → Network (docs/FILES.md, "Network").
 * The channel and the framing are lord-doc's (doc/wire.h: header + payload + fds over a
 * SOCK_SEQPACKET pair on fd 3).
 *
 *   LQ_MDNS     4 bytes: the sender's IPv4 address, then one raw mDNS datagram; no reply
 *   LQ_MDNS_END {}: → LR_MDNS with every service gathered, and the table starts over
 *   LQ_SMB      the server's address as text + 1 fd: a TCP socket connected to port 445 → LR_SMB
 *   LQ_SMB_AUTH {"addr","user","domain","password"} + 1 fd: connected to port 445 → LR_SMB with session
 *               "user" and its shares, or LR_ERR "denied". The password lives in this one request and is wiped
 *   LQ_PORTMAP  {} + 1 fd: connected to port 111 → LR_PORTMAP
 *   LQ_EXPORTS  {} + 1 fd: connected to the port LR_PORTMAP named → LR_EXPORTS
 *   LQ_END      {}: the helper exits
 *
 *   LR_MDNS     {"services":[{"type","name","host","addr","port","path","model"}],"names":[{"addr","host"}]}
 *   LR_SMB      {"name","domain","dialect","signing":0|1,"session":"none|anonymous|guest|refused",
 *                "listed":0|1,"shares":[{"name","comment"}]}
 *   LR_PORTMAP  {"mountd":port}          (0: none registered)
 *   LR_EXPORTS  {"exports":[{"path","open":0|1}]}
 *   LR_ERR      {"error":"code"}
 *
 * lord-lan has no sockets, no files and nothing of the user's: it gets datagrams and sockets
 * lord-server already connected, and everything it answers is only text lord-server validates
 * again (lan.c) before Files sees it. It never chooses where to connect: the one port it names,
 * mountd's, goes through lord-server, which connects to that same host only. */
#ifndef LORD_LAN_PROTO_H
#define LORD_LAN_PROTO_H

enum { LQ_MDNS = 1, LQ_MDNS_END, LQ_SMB, LQ_PORTMAP, LQ_EXPORTS, LQ_END, LQ_SMB_AUTH };
enum { LR_MDNS = 101, LR_SMB, LR_PORTMAP, LR_EXPORTS, LR_ERR };

#endif
