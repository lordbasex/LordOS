// The login screen's notices from lord-sessiond (__lordGreeter), without React (tested in
// server/tests/greeter_inbox_test.mjs).
//
// lord-sessiond starts the login screen for a lock and sends its {t:'mode'} notice as soon as the screen's
// socket answers. The screen's listener is added by a React effect, which runs a few turns of the main loop
// after the theme is loaded (a concurrent root), so that notice could come first and was dropped: the lock's
// idle deadlines (display off, sleep) never reached lord-server and the login screen stayed lit and awake
// (GitHub #172). Now the idle state is applied when the notice arrives (`onMode`), and the last mode notice
// that came with nobody listening is handed to the first listener.
export function makeInbox(onMode) {
  const listeners = new Set();
  let pending = null;
  return {
    dispatch(json) {
      let m = null;
      try { m = JSON.parse(json); } catch (e) { return; }
      if (!m || typeof m !== 'object') return;
      if (m.t === 'mode') {
        try { onMode(m); } catch (e) { /* an older lord-server */ }
        if (!listeners.size) pending = m;
      }
      for (const fn of [...listeners]) fn(m);
    },
    // fn(m) for every notice; a mode notice that came before anyone listened is given now (its idle state was
    // applied already: listeners only update what the screen shows)
    listen(fn) {
      listeners.add(fn);
      if (pending) { const m = pending; pending = null; fn(m); }
      return () => listeners.delete(fn);
    },
  };
}
