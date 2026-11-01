/* lan.h — Files → Network, lord-server's side (docs/FILES.md, "Network"). Off until the page is
 * opened: one bounded pass over the local network on a thread of its own, the answers read by the
 * sandboxed lord-lan helper. lord-server decides every address; JS names none.
 *
 * What a pass touches: interfaces with a real device and a private IPv4 address; mDNS on those
 * links; ports 445 and 2049 of the known neighbours and of the interface's subnet (at most its /24),
 * at most LAN_RATE connections a second; port 111 and mountd only where 2049 answered; port 22 and a
 * reverse mDNS question only for the machines that turned out to be there. */
#ifndef LORD_LAN_H
#define LORD_LAN_H

#define LAN_RATE 30                     /* TCP connections a second */
#define LAN_REST_S 30                   /* the button: a new pass at most this often */
#define LAN_KEEP_S 300                  /* opening the page: what the last pass found stays this long */

/* 1 a pass started; 0 one is running or the last one is recent. asked: the user pressed the button. */
int   lan_start(int asked);
void  lan_stop(void);                   /* cancels the pass and ends the helper */
char *lan_poll(void);                   /* JSON of what is known (malloc), NULL out of memory */

/* The shares a server shows a user. One at a time, on a thread and a helper of its own, on a local subnet only;
 * the password lives until the helper answered and is wiped. user "": what an anonymous session shows. */
int   lan_list_start(const char *addr, const char *domain, const char *user, const char *password);
char *lan_list_poll(void);              /* {"state":"running|done|denied|off-link|no-answer|denied-net|no-helper|failed","shares":[...]} */

#endif
