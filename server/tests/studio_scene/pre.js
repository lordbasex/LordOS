// pre.js — goes before the theme bundle in tests/studio_headless.sh (a test tool, never shipped): the headless session
// must never open a real camera, so lord.media.cameras() lists made-up ones, and LordOS Studio's preview shows a still
// picture of the pipeline's output (written by build/studio-test) instead of starting lord-studio. Everything else
// (lord.studio.status/set/setBackground, the popover, the capture item) is the real code. It also keeps which text each
// node shows and where it hangs, so driver.js can click them.
(function () {
  const still = globalThis.STUDIO_STILL || '';
  lord.media.cameras = () => [
    { device: '/dev/video90', name: 'Test camera', bus: 'usb-test-1' },
    { device: '/dev/video91', name: 'Second test camera', bus: 'usb-test-2' },
    { device: 'studio', name: 'LordOS Studio', bus: 'studio', studio: true },
  ];
  lord.media.camera = () => { throw new Error('no camera in the headless scene'); };
  if (lord.studio) lord.studio.preview = () => (still ? lord.media.image('studio', still) : null);
  // the headless session has no PipeWire: Mic Mode is shown with one made-up microphone and Noise Reduction loaded
  if (lord.sound && typeof lord.sound.state === 'function') {
    const real = lord.sound.state;
    lord.sound.state = () => {
      const st = real() || {};
      if (Array.isArray(st.sources) && st.sources.length) return st;
      return { ...st, connected: true, defaultSource: 'alsa_input.test-mic', sources: [
        { index: 1, name: 'alsa_input.test-mic', description: 'Test microphone', monitorOf: -1, default: true },
        { index: 2, name: 'lordos_voice_isolation', description: 'LordOS Noise Reduction', monitorOf: -1 },
      ] };
    };
  }
  const texts = new Map(), parent = new Map();
  const wrap = (name, fn) => { const real = lord[name]; lord[name] = (...a) => { fn(...a); return real(...a); }; };
  wrap('setText', (id, s) => texts.set(id, String(s)));
  wrap('appendChild', (p, c) => parent.set(c, p));
  wrap('insertBefore', (p, c) => parent.set(c, p));
  wrap('destroy', (id) => { texts.delete(id); parent.delete(id); });
  globalThis.__studioScene = { texts, parent };
})();
