/* librice-test.c — tools/pkg/build-librice.sh's test: the packaged rice-proto and rice-io work from C, as WebKit uses
 * them (WebCore links rice-proto, WebKit's network process rice-io). Prints what it checked; exits 1 on a failure. */
#include <rice-proto.h>
#include <rice-io.h>
#include <stdio.h>

int main(void) {
    unsigned major = 0, minor = 0, patch = 0;
    rice_version(&major, &minor, &patch);
    printf("rice_version: %u.%u.%u (header %d.%d.%d)\n", major, minor, patch, RICE_PROTO_MAJOR, RICE_PROTO_MINOR, RICE_PROTO_PATCH);
    if (major != RICE_PROTO_MAJOR || minor != RICE_PROTO_MINOR || patch != RICE_PROTO_PATCH) return 1;
    RiceAgent *agent = rice_agent_new(true, true);
    if (!agent) { puts("rice_agent_new failed"); return 1; }
    RiceStream *stream = rice_agent_add_stream(agent);
    if (!stream) { puts("rice_agent_add_stream failed"); return 1; }
    printf("agent: controlling %d, a stream added\n", rice_agent_get_controlling(agent));
    rice_stream_unref(stream);
    rice_agent_unref(agent);
    /* rice-io: a UDP socket on the loopback, its own address back */
    RiceAddress *any = rice_address_new_from_string("127.0.0.1:0");
    if (!any) { puts("rice_address_new_from_string failed"); return 1; }
    RiceUdpSocket *udp = rice_udp_socket_new(any);
    rice_address_free(any);
    if (!udp) { puts("rice_udp_socket_new failed"); return 1; }
    RiceAddress *local = rice_udp_socket_local_addr(udp);
    if (!local) { puts("rice_udp_socket_local_addr failed"); return 1; }
    puts("rice-io: a UDP socket bound on 127.0.0.1");
    rice_address_free(local);
    rice_udp_socket_unref(udp);
    puts("ok");
    return 0;
}
