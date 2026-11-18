// stream_model_test.mjs — network streams' plain logic (sdk/streammodel.js), the same address rules as
// server/src/media/streamurl.c. node server/tests/stream_model_test.mjs
import { streamUrlOk, splitCredentials, withCredentials, hostKey, protocolOf, retryDelay, fatal, lowQuality, parseM3U } from '../../sdk/streammodel.js';

let ok = 0, bad = 0;
const eq = (name, a, b) => { if (JSON.stringify(a) === JSON.stringify(b)) ok++; else { bad++; console.error(`FAIL ${name}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } };
const cam = 'rtsp://admin:22222ddddd@172.16.0.75:554/cam/realmonitor?channel=1&subtype=1';
eq('addresses allowed', ['rtsp://h/x', cam, 'srt://10.0.0.5:9000?mode=caller', 'udp://@239.1.1.1:1234', 'https://e.org/a.m3u8', 'rtmp://e.org/app/k'].map(streamUrlOk), [true, true, true, true, true, true]);
eq('addresses refused', ['file:///etc/passwd', 'concat:http://a|http://b', 'http://e.org/a b', 'ftp://e.org/x', 'rtsp:///x', 'HTTP://e.org/x', 'http://e.org/ñ'].map(streamUrlOk), [false, false, false, false, false, false, false]);
const sp = splitCredentials(cam);
eq('credentials out of the address', [sp.url, sp.user, sp.pass, sp.key], ['rtsp://172.16.0.75:554/cam/realmonitor?channel=1&subtype=1', 'admin', '22222ddddd', 'stream:rtsp://172.16.0.75:554']);
eq('and back in', withCredentials(sp.url, sp.user, sp.pass), cam);
eq('encoded user and password', withCredentials('rtsp://h/x', 'a@b', 'p:w/d'), 'rtsp://a%40b:p%3Aw%2Fd@h/x');
eq('a password with a colon survives', splitCredentials(withCredentials('rtsp://h/x', 'u', 'a:b')).pass, 'a:b');
eq('no credentials', splitCredentials('https://e.org/live.m3u8'), { url: 'https://e.org/live.m3u8', user: '', pass: '', key: 'stream:https://e.org' });
eq('the key ignores the credentials and the case', hostKey('RTSP://x'), '');
eq('protocols', ['rtsp://h/x', 'rtmps://h/x', 'srt://h:1', 'rtp://h:1', 'udp://@h:1', 'https://h/a.m3u8?t=1', 'https://h/m.mpd', 'http://h/video.mjpg', 'http://h/a.ts'].map(protocolOf),
  ['RTSP', 'RTMP', 'SRT', 'RTP', 'UDP', 'HLS', 'DASH', 'MJPEG', 'HTTP']);
eq('reconnecting waits more each time, at most 30 s', [0, 1, 2, 3, 4, 5, 9].map(retryDelay), [1000, 2000, 4000, 8000, 16000, 30000, 30000]);
eq('errors no retry fixes', ['media.streamAuth', 'media.streamAddress', 'media.streamLost', 'media.streamTimeout'].map(fatal), [true, true, false, false]);
eq('the sub stream of a camera', [lowQuality('rtsp://h/cam/realmonitor?channel=1&subtype=0'), lowQuality('rtsp://h/Streaming/Channels/101'), lowQuality('rtsp://h/live')],
  ['rtsp://h/cam/realmonitor?channel=1&subtype=1', 'rtsp://h/Streaming/Channels/102', 'rtsp://h/live']);
const list = parseM3U('#EXTM3U\n#EXTINF:-1 group-title="News" tvg-logo="https://l/a.png",A, news\nhttps://a/a.m3u8\n#EXTINF:-1,Bad\nfile:///etc/passwd\nudp://@239.1.1.1:1234\n');
eq('a local M3U list', list, [{ name: 'news', group: 'News', logo: 'https://l/a.png', url: 'https://a/a.m3u8' }, { name: 'udp://@239.1.1.1:1234', group: '', logo: '', url: 'udp://@239.1.1.1:1234' }]);
console.log(bad ? `stream-model-test: ${bad} FAILED` : `stream-model-test: ${ok} ok`);
if (bad) process.exit(1);
