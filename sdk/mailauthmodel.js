// mailauthmodel.js — Mail's account passwords and the session keyring: pure logic, no lord.* (issue #63;
// server/tests/mail_auth_model_test.mjs). sdk/mail.js uses it.
//
//   keyringTrouble(code)                  what a failed keyring call means for Mail
//   askUnlock(trouble, asked)             should the session show the system keyring prompt now?
//   authAfterStatus(authFailed, acct, st) the "password needed" marks after a status event

// 'locked': the keyring exists but is closed (automatic login, or the login password changed outside
// LordOS): the system prompt can open it. 'missing': this session has no keyring yet; lord-vault
// creates it the next time the person logs in or unlocks the screen with their password (the prompt
// cannot). 'error': anything else. null: no error.
export function keyringTrouble(code) {
  if (!code) return null;
  return code === 'locked' ? 'locked' : code === 'no-keyring' ? 'missing' : 'error';
}

// Only a keyring that exists can be opened from the prompt, and the session asks once: Cancel is an
// answer, so the next account (or the next boot of lord-maild) does not bring the box back.
export const askUnlock = (trouble, asked) => trouble === 'locked' && !asked;

// An account that is connected again (the session answered its need-auth once the keyring opened, or
// the person typed the password) loses its "password needed" mark: the isolated Mail app had set it
// when its own read of the keyring failed, and nothing else clears it.
export function authAfterStatus(authFailed, acct, status) {
  if ((status !== 'idle' && status !== 'sync') || !authFailed || !authFailed[acct]) return authFailed;
  return { ...authFailed, [acct]: false };
}
