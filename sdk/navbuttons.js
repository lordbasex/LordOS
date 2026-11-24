// navbuttons.js — a mouse's back and forward buttons in a native app's window (GitHub #234).
//
// lord-server hands them to the desktop as buttons 3 and 4; runtime/events.js never makes them a mouse press (#159:
// most onMouseDown handlers don't look at ev.button) but a 'navigate' event for the window under the pointer. Linux
// apps get the raw buttons from lord-server as before.
//
//   useNavigateButtons(fn)   fn('back' | 'forward') when one is pressed over this app's window
//   navFor(ev, winId, shell) the pure rule (sdk/navmodel.js, server/tests/navbuttons_test.mjs): the direction for this window, or null
//   useVisitHistory(cur, go) the buttons walk the places a window showed (Settings' sections, Help's articles): go(place)
//   visitNote(h, cur) / visitStep(h, d)   its pure steps over { back: [], fwd: [], cur }
import { useEffect, useRef } from 'react';
import { addListener } from '@lordos/runtime';
import { useAppWindow } from './components.jsx';
import { shellLayers } from './layers.js';
import { navFor, visitNote, visitStep } from './navmodel.js';

export { navFor, visitNote, visitStep };

export function useNavigateButtons(fn) {
  const win = useAppWindow();
  const latest = useRef(fn);
  latest.current = fn;
  const id = win?.id ?? null;
  useEffect(() => addListener('navigate', (ev) => {
    const d = navFor(ev, id, shellLayers());
    if (d && latest.current) latest.current(d);
  }), [id]);
}

export function useVisitHistory(cur, go) {
  const h = useRef({ back: [], fwd: [], cur: undefined });
  h.current = visitNote(h.current, cur);
  useNavigateButtons((d) => {
    const next = visitStep(h.current, d);
    if (!next) return;
    h.current = next;
    go(next.cur);
  });
}
