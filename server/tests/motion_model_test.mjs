// motion_model_test.mjs — moving wallpapers without a screen (sdk/motionmodel.js).
import { motionCodecs, pickVariant, pauseReason, displayCovered, userVideoProblem, nextRandom } from '../../sdk/motionmodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => {
  if (JSON.stringify(a) === JSON.stringify(b)) ok++;
  else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); }
};

eq('gpu and hevc', motionCodecs({ hevc: true, h264: false }, 'gpu'), ['hevc']);
eq('gpu and both, hevc first', motionCodecs({ hevc: true, h264: true }, 'gpu'), ['hevc', 'h264']);
eq('h264 only when in hardware', motionCodecs({ hevc: false, h264: true }, 'gpu'), ['h264']);
eq('no decoder: nothing', motionCodecs({ hevc: false, h264: false }, 'gpu'), []);
eq('no gpu composition: nothing', motionCodecs({ hevc: true, h264: true }, 'gpu-off'), []);
eq('a virtual gpu: nothing', motionCodecs({ hevc: true, virtualGpu: true }, 'gpu'), []);
eq('unknown yet: nothing', motionCodecs(null, 'gpu'), []);

const item = { variants: [{ name: '2160.hevc', codec: 'hevc', w: 3840, h: 2160 }, { name: '1080.hevc', codec: 'hevc', w: 1920, h: 1080 }, { name: '1080.h264', codec: 'h264', w: 1920, h: 1080 }] };
eq('1080 screen', pickVariant(item, ['hevc', 'h264'], 1920, 1080).name, '1080.hevc');
eq('4K screen', pickVariant(item, ['hevc'], 3840, 2160).name, '2160.hevc');
eq('small screen: smallest', pickVariant(item, ['hevc'], 1280, 800).name, '1080.hevc');
eq('above the largest', pickVariant(item, ['hevc'], 5120, 2880).name, '2160.hevc');
eq('h264 machine', pickVariant(item, ['h264'], 3840, 2160).name, '1080.h264');
eq('no format', pickVariant(item, [], 1920, 1080), null);
eq('no matching format', pickVariant({ variants: [item.variants[2]] }, ['hevc'], 1920, 1080), null);

const base = { idleSec: 0 };
eq('plays', pauseReason(base), null);
eq('reduce motion', pauseReason({ ...base, reduceMotion: true }), 'reduceMotion');
eq('screen off', pauseReason({ ...base, screenOff: true }), 'screenOff');
eq('fullscreen', pauseReason({ ...base, fullscreen: true }), 'fullscreen');
eq('covered', pauseReason({ ...base, covered: true }), 'covered');
eq('idle', pauseReason({ ...base, idleSec: 300 }), 'idle');
eq('battery low', pauseReason({ ...base, battery: { discharging: true, percent: 20 } }), 'battery');
eq('battery charging', pauseReason({ ...base, battery: { discharging: false, percent: 5 } }), null);
eq('load', pauseReason({ ...base, busy: true }), 'busy');

const d = { x: 0, y: 0, w: 1920, h: 1080 };
eq('full window', displayCovered(d, [{ x: 0, y: 0, w: 1920, h: 1080 }]), true);
eq('bigger window', displayCovered(d, [{ x: -5, y: -5, w: 2000, h: 1200 }]), true);
eq('smaller window', displayCovered(d, [{ x: 0, y: 30, w: 1920, h: 1050 }]), false);
eq('another display', displayCovered(d, [{ x: 1920, y: 0, w: 1920, h: 1080 }]), false);

eq('short hevc', userVideoProblem({ duration: 8, codec: 'hevc' }, ['hevc']), null);
eq('long', userVideoProblem({ duration: 11, codec: 'hevc' }, ['hevc']), 'tooLong');
eq('10 s', userVideoProblem({ duration: 10, codec: 'hevc' }, ['hevc']), null);
eq('vp9', userVideoProblem({ duration: 8, codec: 'vp9' }, ['hevc']), 'format');
eq('unreadable', userVideoProblem(null, ['hevc']), 'unreadable');

eq('random skips the last', nextRandom(['a', 'b'], 'a', () => 0), 'b');
eq('random with one', nextRandom(['a'], 'a'), 'a');
eq('random with none', nextRandom([], 'a'), null);

console.log(`motion-model-test: ${ok} ok`);
if (bad) process.exit(1);
