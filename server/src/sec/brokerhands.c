/* brokerhands.c — Hands' rule for the broker's requests (broker.h brokersrv_hands_refuses, docs/HANDS.md) */
#include "broker.h"

/* Hands (hands.h): what an isolated app asks that changes or reads a secret never runs on an AI agent's input: the
 * keyring, the vault's FIDO key, a secret to the clipboard; and the clipboard holding a secret is nobody's while an agent
 * is connected. 1 refused. */
int brokersrv_hands_refuses(int op, int agent_owns, int active, int sensitive) {
    if ((op == BOP_KEYRING || op == BOP_VAULT_FIDO || op == BOP_CLIP_SECRET) && agent_owns) return 1;
    if (op == BOP_CLIP_GET && active && sensitive) return 1;
    return 0;
}
