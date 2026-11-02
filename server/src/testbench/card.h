/* card.h — the test card of lordos-testbench (see card.c). */
#ifndef LORD_TESTBENCH_CARD_H
#define LORD_TESTBENCH_CARD_H

#include <stdint.h>

typedef struct { int x, y, w, h; } CardBox;

typedef struct {
    int w, h;
    uint32_t *px;                       /* XRGB, w × h, the static card */
    CardBox clock, pad;                 /* the black boxes: clock (top) and input (bottom) */
} Card;

/* (Re)draws the static card at w × h (made even). 0, or -1 if too small or out of memory. */
int  card_build(Card *c, int w, int h);
void card_free(Card *c);

#endif
