// streams.jsx — a network stream on screen (0.8.8.10): the desktop's video widget and the Player's "Open address…".
// lord-stream plays it in its own sandboxed process; this side only opens, watches and reconnects, so a dead camera
// never holds the desktop: when the stream is lost, the helper ends and a new one starts after retryDelay(n), while
// the picture shows Reconnecting.
//
//   const s = useStream({ app, url, user, active, paused, muted, audio })
//     url: the address without credentials; user: its user name (the password is in the keyring, service hostKey(url))
//     s: { id (for <MediaView media={id}/>), status: 'idle'|'locked'|'connecting'|'playing'|'reconnecting'|'paused'|'failed',
//          error (an error code), attempt, live (no duration), info }
//   saveStreamPassword(app, url, user, pass)   into the keyring (Passwords), never settings.json
import { useEffect, useRef, useState } from 'react';
import { media } from './media.js';
import { keyring } from './vault.js';
import { hostKey, withCredentials, retryDelay, fatal } from './streammodel.js';

export const STREAM_KEYRING_APP = 'player';    // one owner for the widget and the Player: a camera's password is shared

export function saveStreamPassword(url, user, pass) {
  const service = hostKey(url);
  if (!service || !pass) return Promise.resolve(false);
  return keyring.set(STREAM_KEYRING_APP, { service, account: user || '', secret: pass, label: service.replace(/^stream:/, '') }).then(() => true, () => false);
}

async function passwordFor(url, user) {
  const item = await keyring.get(STREAM_KEYRING_APP, { service: hostKey(url), account: user || '' });
  return item ? item.secret || '' : '';
}

export function useStream({ app = 'player', url, user = '', active = true, paused = false, muted = false, audio = true, name = '' }) {
  const [st, setSt] = useState({ id: null, status: 'idle', error: null, attempt: 0, live: true, info: null });
  const run = useRef({ gen: 0, id: null, timer: 0, attempt: 0 });
  // a live stream paused is closed (no bandwidth, nothing decoding); one with a duration (a film over HLS) is paused in place
  const liveRef = useRef(true);
  const want = active && !!url && !(paused && liveRef.current);
  useEffect(() => {
    const r = run.current;
    const gen = ++r.gen;
    const stop = () => { clearTimeout(r.timer); r.timer = 0; if (r.id) { media.close(r.id); r.id = null; } };
    stop();
    if (!want) { setSt((s) => ({ ...s, id: null, status: url && active ? 'paused' : 'idle' })); return undefined; }
    let off = () => {};
    const open = async () => {
      if (gen !== r.gen) return;
      setSt((s) => ({ ...s, status: r.attempt ? 'reconnecting' : 'connecting', error: s.error && r.attempt ? s.error : null }));
      let pass = '';
      if (user) {
        try { pass = await passwordFor(url, user); } catch (e) {
          if (gen !== r.gen) return;
          setSt((s) => ({ ...s, status: 'locked', error: e.code === 'locked' ? 'locked' : 'media.streamAuth' }));
          return;
        }
      }
      if (gen !== r.gen) return;
      let id = null;
      try { id = media.stream(app, withCredentials(url, user, pass), { audio, autoplay: true, name, volume: muted ? 0 : 1 }); } catch (e) { id = null; fail(e.code || 'media.streamAddress'); return; }
      pass = '';
      if (!id) { fail('media.noStreamHelper'); return; }
      r.id = id;
      setSt((s) => ({ ...s, id }));
      off = media.on(id, (type, d) => {
        if (gen !== r.gen) return;
        if (type === 'info') { liveRef.current = !(d.duration > 0); setSt((s) => ({ ...s, info: d, live: liveRef.current })); }
        else if (type === 'state' && d.state === 'playing') { r.attempt = 0; setSt((s) => ({ ...s, status: 'playing', error: null, attempt: 0 })); }
        else if (type === 'error') fail(String((d.message || '').split(/\s/)[0] || 'media.streamLost'));
        else if (type === 'closed') fail('media.streamLost');
      });
    };
    // lost or refused: a wrong address or password stays failed; anything else tries again, waiting longer each time
    const fail = (code) => {
      if (gen !== r.gen) return;
      off();
      off = () => {};
      if (r.id) { media.close(r.id); r.id = null; }
      if (fatal(code)) { setSt((s) => ({ ...s, id: null, status: 'failed', error: code })); return; }
      const wait = retryDelay(r.attempt++);
      setSt((s) => ({ ...s, id: null, status: 'reconnecting', error: code, attempt: r.attempt }));
      r.timer = setTimeout(open, wait);
    };
    r.attempt = 0;
    open();
    return () => { off(); stop(); };
  }, [want, url, user, audio, app]);
  useEffect(() => { if (st.id && !st.live) media.control(st.id, paused ? 'pause' : 'play'); }, [paused, st.id, st.live]);
  // mute and unmute without reconnecting
  useEffect(() => { if (st.id) media.control(st.id, 'volume', muted ? 0 : 1); }, [muted, st.id]);
  return st;
}
