/* lockkey.h — the Test Bench's pointer lock. Every key of the drawn keyboard is there to be pressed, so
 * a plain letter must not take the pointer away: it asks for Ctrl+L, and only with the pointer on the
 * window (a lock that waits would hide the pointer later, when it walks in). It ends by itself, which
 * also covers a keyboard that went to sleep. */
#ifndef LORD_TESTBENCH_LOCKKEY_H
#define LORD_TESTBENCH_LOCKKEY_H

#include <linux/input-event-codes.h>

#define TB_LOCK_MS 10000.0

static inline int tb_lock_key(unsigned key, int pressed, int ctrl, int locked, int pointer_in) {
    return pressed && key == KEY_L && ctrl && (locked || pointer_in);
}
static inline int tb_lock_over(int locked, double since_ms, double now_ms) { return locked && now_ms - since_ms >= TB_LOCK_MS; }

#endif
