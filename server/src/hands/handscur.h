/* handscur.h — the agent's cursor on the screen (docs/HANDS.md "The agent's cursor"; approved by the maintainer on
 * 2026-11-26 with Hands).
 *
 * While an AI agent acts, the person sees its own cursor: a violet arrow with a small label of the name the client
 * gave ("Claude Code · reported"), separate from the person's pointer, which never moves for it. The shell builds its
 * nodes once, out of React's sight (sdk/handscursor.js), in a layer of its own at zIndex HANDSCUR_Z: above every layer a
 * theme draws in (notification, 95), below the system band (100: permission, password and polkit prompts, Hands'
 * banner), so it never covers a prompt. It takes no clicks (pointerEvents none). lord-server moves it here, from C, at the
 * compositor's pace: each frame changes only its position (its box is the damage), no JavaScript per frame.
 *
 *   move    where it is (a human-mode glide's frames, a machine-mode jump)
 *   click   a ripple where it clicks (human mode)
 *   flash   a quick ring where a machine-mode action lands
 *   pulse   a short ring at a field it types into or presses keys at
 *   hide    gone (Stop, the panic key, no agent left); it also fades out by itself HANDSCUR_IDLE_MS after its last action
 *
 * It is never in an agent's own screenshot (main.c hands_shot leaves it out: the agent sees the screen as it is), nor in
 * the tree read_window reads (it is no React node). The nodes are checked when the shell hands them over and again at
 * each use (the layer's place, its zIndex, pointerEvents none, the ids' serials): lord-server never moves another node. */
#ifndef LORD_HANDS_HANDSCUR_H
#define LORD_HANDS_HANDSCUR_H

#define HANDSCUR_Z        99.5f     /* sdk/handscursor.js: between notification (95) and system (100) */
#define HANDSCUR_TIP      24        /* the arrow's tip inside the root node (its left and top) */
#define HANDSCUR_IDLE_MS  4000      /* no action for this long: it fades out */
#define HANDSCUR_FADE_MS  400

/* the shell's nodes: the root (the layer), the ripple ring; 0, or -1 refused (not where the cursor's layer must be) */
int  handscur_set(int root, int ring);
int  handscur_root(void);                   /* the root's id while it is valid, else 0 */
/* an event of hands.c's (HANDS_CUR_*), at now (ms) */
void handscur_event(int what, double x, double y, double w, double h, double now);
/* its animations: the ripple and the fade (main.c each loop); the ms until its next frame, -1 none */
void handscur_tick(double now);
double handscur_timeout(double now);
/* while an agent's action goes on (hands_acting), it doesn't fade */
void handscur_keep(int on);

#endif
