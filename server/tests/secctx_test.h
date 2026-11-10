/* secctx_test.h — what tests/secctx_test.c (the server) and secctx_test_client.c (the client) share. */
#ifndef LORD_SECCTX_TEST_H
#define LORD_SECCTX_TEST_H

#define SECT_VISIBLE "lord_test_visible"            /* a global every client sees */
#define SECT_HIDDEN "ext_data_control_manager_v1"   /* named like a trusted-only one */

/* The client's connections to the server, one per case (a protocol error ends a connection) */
enum { SECT_MAIN, SECT_BAD_FD, SECT_DENIED, SECT_TWICE, SECT_BAD_META, SECT_AFTER_COMMIT, SECT_CONNS };

/* Runs in the forked child over fds[SECT_CONNS]; returns how many checks failed */
int sect_client(const int *fds);

#endif
