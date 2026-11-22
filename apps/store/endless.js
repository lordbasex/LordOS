// endless.js — the Store's long lists load their next page by themselves when the list is scrolled near its end
// (Flathub's thousands of apps, a Debian category's hundreds). The catalogs are already local: a page is only more
// cards drawn, PAGE at a time, so the cards already shown are never drawn again (stable keys) and a hover or a scroll
// never redraws the whole grid. A small "Show more" stays at the end for when the list can't scroll (the keyboard, a
// very tall window).
//
// The Store's ScrollView reports its offset (onScroll); ends.check measures the content and, when what is in sight is
// within a screen of the end, tells the lists that page. A list that grew checks again once laid out, so a window
// taller than a page fills itself.
import { createContext, createElement as h, useContext, useEffect, useRef, useState } from 'react';
import { View, Button, spacing, t } from '@lordos/sdk';

export const PAGE = 60;

// The Store's end-of-scroll signal: one per Store window, given to its main ScrollView's scrollRef.
export function makeEnds(scrollRef) {
  const subs = new Set();
  let view = 0, fired = -1;
  const check = (y, v) => {
    if (v > 0) view = v;
    const s = scrollRef.current;
    if (!s || !s.inner || !s.inner.current || !view) return;
    let content = 0;
    try { content = lord.getLayout(s.inner.current.id)[3]; } catch (e) { return; }
    const at = y == null ? s.offset() : y;
    // once per content height: a scroll near the end asks for one page, not one per wheel step until it is laid out
    if (at + view < content - Math.max(600, view) || content === fired) return;
    fired = content;
    for (const f of [...subs]) f();
  };
  return {
    check,
    // how far each list had grown, so back from an app's page the list is as long as it was (and where it was)
    limits: new Map(),
    recheck: () => check(null, 0),
    reset: () => { fired = -1; },
    subscribe: (f) => { subs.add(f); return () => subs.delete(f); },
  };
}
export const EndsContext = createContext(null);

// How many of total items a list shows: PAGE more each time the view nears its end; back to PAGE when resetKey changes
// (or what it was before an app's page was opened from it: index.jsx clears them when the list changes).
export function usePaged(total, resetKey) {
  const ends = useContext(EndsContext);
  const start = () => (ends && ends.limits.get(resetKey)) || PAGE;
  const [limit, setLimit] = useState(start);
  const [key, setKey] = useState(resetKey);
  if (key !== resetKey) { setKey(resetKey); setLimit(start()); }
  useEffect(() => {
    if (!ends) return;
    if (limit > PAGE) ends.limits.set(resetKey, limit);
  }, [ends, resetKey, limit]);
  const totalRef = useRef(total);
  totalRef.current = total;
  useEffect(() => (ends ? ends.subscribe(() => setLimit((l) => (l < totalRef.current ? l + PAGE : l))) : undefined), [ends]);
  // after a page is drawn and laid out: still near the end (a tall window)? then the next one
  useEffect(() => {
    if (!ends || limit >= total) return undefined;
    const id = setTimeout(() => ends.recheck(), 60);
    return () => clearTimeout(id);
  }, [ends, limit, total]);
  return { limit, more: () => setLimit((l) => l + PAGE) };
}

// The light "Show more (N left)" at the end of a paged list, for when scrolling doesn't bring the next page.
export function MoreButton({ left, onMore }) {
  if (left <= 0) return null;
  return h(View, { style: { alignItems: 'center', marginTop: spacing.l } },
    h(Button, { small: true, label: t('flathub.showMore', { count: left }), onClick: onMore }));
}
