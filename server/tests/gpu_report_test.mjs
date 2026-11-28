// gpu_report_test.mjs — Settings → Graphics cards' compatibility report (sdk/gpureport.js): the JS scrub (no home path,
// host or user name, serial, MAC, UUID or e-mail leaks), the GitHub form's address (template, fields, length cap, cut on
// whole lines) and the card and driver summaries. Run: node tests/gpu_report_test.mjs
import { scrubReport, gpuIssueUrl, cardSummary, driverSummary, isNvidia, reportCard, GPU_TEMPLATE, NVIDIA_ROLES, nvidiaRoleState } from '../../sdk/gpureport.js';
import { ISSUE_URL_MAX } from '../../sdk/cloudcheck.js';

let ok = 0, bad = 0;
function eq(name, got, want) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) ok++;
  else { bad++; console.log(`FAIL ${name}\n  got  ${g}\n  want ${w}`); }
}
const yes = (name, c) => eq(name, !!c, true);

// ---- the scrub ----
const who = { user: 'secretuser', host: 'secrethost', home: '/home/secretuser/' };
const dirty = [
  'home /home/secretuser/.cache/lordos/x and /home/bob/y',
  'mac de:ad:be:ef:00:11 DE-AD-BE-EF-00-11 slot 0000:01:00.0',
  'uuid 0b1e6c1a-1111-2222-3333-444455556666',
  'serial: ABC123, SerialNumber=XYZ9',
  'mail secretuser@example.org',
  'user secretuser host secrethost; secretuser2 and nosecretuser stay',
  'nvidia 560.35.03 [10de:2504] Mesa 25.0.7-2+deb13u1',
].join('\n');
const clean = scrubReport(dirty, who);
for (const leak of ['secretuser@', '/home/secretuser', '/home/bob', 'de:ad', 'DE-AD', '0b1e6c1a', 'ABC123', 'XYZ9', 'example.org', 'secrethost', ' secretuser ']) {
  eq(`no ${leak}`, clean.includes(leak), false);
}
yes('home → ~', clean.includes('home ~/.cache/lordos/x and /home/<user>/y'));
yes('PCI slot stays', clean.includes('slot 0000:01:00.0'));
yes('whole words only', clean.includes('user <user> host <host>; secretuser2 and nosecretuser stay'));
yes('versions and ids stay', clean.includes('nvidia 560.35.03 [10de:2504] Mesa 25.0.7-2+deb13u1'));
yes('serials hidden', clean.includes('serial: <hidden>, SerialNumber=<hidden>'));
eq('root, localhost and short names untouched', scrubReport('root on localhost, a b', { user: 'root', host: 'localhost', home: '/' }), 'root on localhost, a b');
eq('one-letter user untouched', scrubReport('a b', { user: 'a' }), 'a b');
eq('regex characters in a name', scrubReport('x a.b+c y axbxc', { user: 'a.b+c' }), 'x <user> y axbxc');
eq('nothing', scrubReport(undefined), '');

// ---- the form's address ----
const short = gpuIssueUrl({ report: 'LordOS graphics report (format 1)\nGPU 1: x', version: '0.8.10-beta', gpu: 'NVIDIA GA106 [10de:2504]', driver: 'nvidia 560.35.03 (open)' });
yes('template', short.startsWith(`https://github.com/lordbasex/LordOS/issues/new?template=${GPU_TEMPLATE}&title=`));
yes('fields', ['&version=0.8.10-beta', '&gpu=NVIDIA%20GA106%20%5B10de%3A2504%5D', '&driver=nvidia%20560.35.03%20%28open%29', '&report='].every((f) => short.includes(f)));
yes('no raw parentheses (lord.apps.launchUri refuses them)', !/[!'()*]/.test(short));
const long = Array.from({ length: 400 }, (_, i) => `line ${i} ${'x'.repeat(30)}`).join('\n');
const cut = gpuIssueUrl({ report: long, version: '1', gpu: 'g' });
yes('long report fits', cut.length <= ISSUE_URL_MAX);
yes('cut is marked', decodeURIComponent(cut).includes('[truncated: the whole report is in the clipboard]'));
yes('cut on whole lines', /line \d+ x{30}\n\[truncated/.test(decodeURIComponent(cut)));
const huge = gpuIssueUrl({ report: 'y'.repeat(10000), gpu: 'z'.repeat(500), driver: 'd'.repeat(500), version: 'v'.repeat(500) });
yes('one huge line: the head alone, still short', huge.length <= ISSUE_URL_MAX && !huge.includes('&report='));
yes('no report: the head', !gpuIssueUrl({ gpu: 'g' }).includes('&report='));

// ---- summaries ----
const info = {
  cards: [{ slot: '0000:01:00.0', vendorId: '10de', deviceId: '2504', vendor: 'NVIDIA Corporation', device: 'GA106 [GeForce RTX 3060]', driver: 'nvidia' },
    { slot: '0000:00:02.0', vendorId: '8086', deviceId: '3e92', vendor: 'Intel Corporation', device: 'UHD 630', driver: 'i915' }],
  nvidia: { version: '560.35.03', open: 1 }, mesa: '25.0.7-2',
};
eq('card summary', cardSummary(info.cards[0]), 'NVIDIA Corporation GA106 [GeForce RTX 3060] [10de:2504]');
eq('card summary from lord.vm.gpus', cardSummary({ vendor: 0x10de, device: 0x2504, vendorName: 'NVIDIA' }), 'NVIDIA [10de:2504]');
eq('nvidia driver', driverSummary(info.cards[0], info), 'nvidia 560.35.03 (open)');
eq('proprietary', driverSummary(info.cards[0], { nvidia: { version: '535.247.01', open: 0 } }), 'nvidia 535.247.01 (proprietary)');
eq('mesa driver', driverSummary(info.cards[1], info), 'i915 (Mesa 25.0.7-2)');
eq('nouveau', driverSummary({ driver: 'nouveau' }, info), 'nouveau (Mesa 25.0.7-2)');
eq('no driver', driverSummary({ driver: '' }, info), '');
eq('isNvidia', [isNvidia({ vendor: 0x10de }), isNvidia({ vendorId: '10de' }), isNvidia({ vendor: 0x1002 }), isNvidia(null)], [true, true, false, false]);
eq('reportCard', reportCard(info, '0000:00:02.0')?.driver, 'i915');
eq('reportCard missing', reportCard(info, '0000:09:00.0'), null);
eq('the roles an NVIDIA card lists', NVIDIA_ROLES.map((r) => r[0]), ['compute', 'desktop']);
// compute stays planned; the desktop is experimental, and on the card drawing this session it is the session's composition
eq('compute is planned', nvidiaRoleState('compute', { desktop: 1 }, { session: { composition: 'gpu' } }), 'planned');
eq('desktop on the drawing card, composed on the GPU (#276)', nvidiaRoleState('desktop', { desktop: 1 }, { session: { composition: 'gpu' } }), 'gpu');
eq('desktop on the drawing card, composed by the CPU (#171)', nvidiaRoleState('desktop', { desktop: 1 }, { session: { composition: 'cpu' } }), 'cpu');
eq('desktop of a card that doesn\'t draw it (#274\'s GTX 1650)', nvidiaRoleState('desktop', { desktop: 0 }, { session: { composition: 'gpu' } }), 'experimental');
eq('desktop with no report facts', nvidiaRoleState('desktop', null, null), 'experimental');
eq('desktop with no session in the report', nvidiaRoleState('desktop', { desktop: 1 }, { session: null }), 'experimental');

console.log(`gpu-report-test: ${ok} ok, ${bad} failed`);
process.exit(bad ? 1 : 0);
