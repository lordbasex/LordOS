/* handsmark.h — what an agent's screenshot leaves out (docs/HANDS.md "Screenshots"): every surface of the system band
 * (the root's layers at zIndex 100 and above), every handsDeny node and, with an allowlist, the nodes hands.c names
 * (other apps' windows) are painted as the protected fill for the length of the capture, then put back. Fail closed:
 * when not every one of them can be marked, none stays marked and the capture must not happen. Tested by
 * tests/hands_test.c on a tree of its own. */
#ifndef LORD_HANDS_HANDSMARK_H
#define LORD_HANDS_HANDSMARK_H

#include "../ui/ui.h"

#define HANDS_SYSTEM_Z 100
#define HANDS_MARK_MAX 512          /* system surfaces and handsDeny nodes (with HANDS_BLACK_MAX more for an allowlist) */

/* marks into marked[max] (their ids, *n of them): 0, or -1 with every mark undone (too many, or a black id unknown) */
int  hands_mark_nodes(Node *root, Node *(*get)(int id), const int *black, int nblack, int *marked, int max, int *n);
void hands_unmark_nodes(Node *(*get)(int id), const int *marked, int n);

#endif
