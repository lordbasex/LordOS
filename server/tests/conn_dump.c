/* conn_dump.c — prints what Monitor → Network gets from src/monitor/conn.c (conn_scan): every socket of
 * the user with its protocol (TCP, UDP, ICMP, raw, packet), direction, addresses, process and app.
 *   build/conn-dump        one reading as JSON (for checking a change by hand, on a VM) */
#include "../src/monitor/monpriv.h"

#include <stdio.h>

int main(void) {
    Buf b = { 0 };
    conn_scan(&b, NULL);
    if (b.bad || !b.p) { fprintf(stderr, "conn-dump: no reading\n"); return 1; }
    fwrite(b.p, 1, b.n, stdout);
    putchar('\n');
    mon_buf_free(&b);
    return 0;
}
