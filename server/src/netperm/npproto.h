/* npproto.h — the closed protocol between lord-server (each session, not root) and lord-netperm (root).
 * See docs/FIREWALL.md, step 2, "The socket".
 *
 * Unix socket /run/lordos/netperm.sock (0666: anyone can connect; lord-netperm knows who it is
 * via SO_PEERCRED and only tells it about that user's apps). Each message is:
 *
 *   u32 length (little endian, counts the type and the data) · u8 type · data
 *
 * From lord-server to lord-netperm there is ONE single message, with no data:
 *   NP_MSG_RELOAD   "reread my user's ~/.config/lordos/firewall.json" (and, 0.8.5, fwlists.txt, the
 *                   compiled blocklists, through the same reader)
 * Rules never travel over the socket: lord-netperm reads the user's file itself, from a fixed,
 * validated path (no links, owned by the user, regular file, up to 4 MB) with the strict reader
 * of fw/rules.c. Any other message, or one longer than NP_IN_MAX bytes, closes the connection.
 *
 * From lord-netperm to lord-server (compact JSON in the data):
 *   NP_MSG_STATUS   {"bpf":true,"devices":true,"bluetooth":true,"bytes":true,"dns":"resolved","error":""}
 *                                                                        on connect and when it changes
 *   NP_MSG_EVENT    {"app":"desktop:x","v":"deny","dir":"out","proto":"tcp","addr":"1.2.3.4",
 *                    "port":443,"by":"rule"|"restricted"|"trusted"|"mode"|"list","rule":7,"flag":"","record":0,
 *                    "hosts":["example.com"]}                          a new connection; hosts:
 *                                                                       up to 4 names the resolver
 *                                                                       gave for addr
 *   NP_MSG_COUNTS   {"desktop:x":[allowed,denied,rx,tx,rxRate,txRate,
 *                                 [["1.2.3.4",rx,tx,rxRate,txRate,blocked,
 *                                   [[port,"tcp"|"udp"|"other","out"|"in",rx,tx,rxRate,txRate],...]],...],blocked],
 *                    "system:dns":[0,0,rx,tx,rxRate,txRate,[],0],...}
 *                                                                        (0.8.5: each address's 8 busiest
 *                                                                        ports; the essential system
 *                                                                        services' bytes, the same for
 *                                                                        every user)
 *                                                                        every second, if it changed:
 *                                                                        connections, bytes (the
 *                                                                        cgroup_skb accounting), the
 *                                                                        busiest addresses and the
 *                                                                        bytes the filter dropped of
 *                                                                        connections a rule now denies */
#ifndef LORD_NP_PROTO_H
#define LORD_NP_PROTO_H

#define NP_SOCK_PATH  "/run/lordos/netperm.sock"
#define NP_MSG_RELOAD 1
#define NP_MSG_STATUS 10
#define NP_MSG_EVENT  11
#define NP_MSG_COUNTS 12
#define NP_IN_MAX     64            /* the longest lord-netperm accepts */
#define NP_OUT_MAX    (1u << 20)    /* the longest lord-server accepts */

#endif
