// wlconf.mjs — lordos.org/wayland-conformance/: the conformance run of tools/wl-conformance.mjs in full (every global,
// every X11 item, every check), the comparison with KDE and GNOME, and the steps to run the same tests and compare.
//
// Data, both written by tools/gen-wayland-table.mjs from docs/wayland-conformance.json and docs/wayland-protocols.json:
//   web/content/wayland/protocols.json     the table page's data, with the scores, depths and screenshots (validateWeb)
//   web/content/wayland/conformance.json   every check of the run (validateDetail: shapes, and each item's checks
//                                          must add up to the points protocols.json shows for it)
// Every number on the page comes from those files; the text around them is in web/i18n/pages.json (wlconf), en/es/pt.
// Every value is escaped; the tool's messages are shown in English, as recorded.

// the commands of "Run it yourself", the same in every language (checked on Debian 13, amd64)
export const PKGS = 'build-essential pkg-config git ca-certificates nodejs npm libxkbcommon-dev xkb-data libwayland-dev wayland-protocols libwayland-bin libcrypt-dev xwayland libxcb1-dev';
export const PKGS_APPS = 'foot vulkan-tools mesa-vulkan-drivers xterm';

export function renderWlconf(ctx) {
  const { t, l, D, X, esc, fill, fmtD, GITHUB, tableUrl, proofFigs, repoUrl } = ctx;
  const T = t.pages.wlconf, W = t.pages.wayland, CF = D.conformance, S = CF.summary, E = CF.env;
  const pct = (x) => `${x} %`;
  const list = (xs) => new Intl.ListFormat(l, { style: 'long', type: 'conjunction' }).format(xs);
  const num = (n) => n.toLocaleString(l === 'en' ? 'en-US' : l === 'es' ? 'es-AR' : 'pt-BR');
  const bar = (score) => { const n = Math.round(score / 10); return `<span class="wp-bar" aria-hidden="true"><span>${'█'.repeat(n)}</span>${'·'.repeat(10 - n)}</span>`; };
  const depthPill = (dp) => `<span class="wp-depth wp-d-${esc(dp)}" title="${esc(W.conf.depthText[dp])}">${esc(W.conf.depth[dp])}</span>`;
  const resPill = (r) => `<span class="wc-r wc-${esc(r)}">${esc(T.result[r])}</span>`;
  const code = (x) => `<code>${esc(x)}</code>`;
  const KINDS = ['static', 'flow', 'generic', 'claim', 'dynamic', 'unit'];
  const detG = new Map(X.globals.map((g) => [g.global, g]));
  const detX = new Map(X.x11.map((x) => [x.id, x]));
  const rowOf = new Map(D.protocols.map((p) => [p.id, p]));
  const anchor = (s) => String(s).replace(/[^a-z0-9_-]/gi, '-');

  // ---- counts, all from the checks
  const allChecks = [...X.globals.flatMap((g) => g.checks), ...X.x11.flatMap((x) => x.checks)];
  const count = (list, r) => list.filter((c) => c.result === r).length;
  const N = { total: allChecks.length, pass: count(allChecks, 'pass'), allowed: count(allChecks, 'allowed'), skip: count(allChecks, 'skip'), fail: count(allChecks, 'fail') };
  const byKind = (checks) => KINDS.map((k) => {
    const cs = checks.filter((c) => c.kind === k);
    if (!cs.length) return null;
    const run = cs.filter((c) => c.result !== 'skip');
    return { k, n: run.length, ok: run.filter((c) => c.result === 'pass' || c.result === 'allowed').length, skip: cs.length - run.length };
  }).filter(Boolean);
  const kindCell = (checks, id) => {
    const parts = byKind(checks).map((x) => `<span class="wc-k"><span class="wc-kn">${esc(T.kind[x.k])}</span> ${esc(x.ok)}/${esc(x.n)}${x.skip ? ` <span class="wc-sk">${esc(fill(T.skippedN, { n: x.skip }))}</span>` : ''}</span>`);
    const al = count(checks, 'allowed');
    if (al) parts.push(`<span class="wc-k wc-al">${esc(fill(T.allowedN, { n: al }))}</span>`);
    return `${parts.join(' ')}<a class="wc-all" href="#c-${esc(anchor(id))}">${esc(fill(T.allChecks, { n: checks.length }))}</a>`;
  };
  const proofLinks = (ids) => (ids && ids.length ? ids.map((id, i) => `<a href="#proof-${esc(id)}">${esc(ids.length > 1 ? fill(W.conf.proofN, { n: i + 1 }) : W.conf.proof)}</a>`).join(' ') : '<span class="wp-no">—</span>');
  const scoreCell = (x) => `<span class="wp-score">${bar(x.score)} <strong>${esc(pct(x.score))}</strong></span><span class="wp-pts">${esc(fill(W.conf.points, { p: x.points, t: x.total }))}${x.skipped ? ' · ' + esc(fill(W.conf.skipped, { n: x.skipped })) : ''}</span>`;

  // ---- summary
  const rows = new Set(CF.globals.map((g) => g.row).filter(Boolean));
  const x11Other = CF.x11.filter((x) => x.depth !== 'complete');
  const partialButComplete = D.protocols.filter((p) => p.lordos.status === 'partial' && p.conf && p.conf.depth === 'complete');
  const hidden = CF.globals.filter((g) => !g.advertised);
  const tile = (n, label) => `<li><strong>${esc(n)}</strong><span>${esc(label)}</span></li>`;
  const tiles = [
    tile(`${S.complete} / ${S.protocols}`, T.tileComplete),
    tile(pct(S.mean), T.tileMean),
    tile(`${S.x11Complete} / ${S.x11Items}`, fill(T.tileX11, { mean: S.x11Mean })),
    tile(num(N.total), fill(T.tileChecks, { fail: N.fail })),
  ].join('\n');
  const x11List = x11Other.map((x) => {
    const skips = detX.get(x.id).checks.filter((c) => c.result === 'skip');
    const why = skips.length ? skips.map((c) => `<span lang="en">${esc(c.msg)}</span>`).join(' ') : esc(T.x11Why[x.depth] || W.conf.depthText[x.depth]);
    return `<li><a href="#x-${esc(anchor(x.id))}">${code(x.id)}</a> ${depthPill(x.depth)} <span class="wc-why">${skips.length ? esc(T.skippedBecause) + ' ' : ''}${why}</span></li>`;
  }).join('\n');
  const allowedList = [...X.globals.flatMap((g) => g.checks.filter((c) => c.result === 'allowed').map((c) => [g.global, c])), ...X.x11.flatMap((x) => x.checks.filter((c) => c.result === 'allowed').map((c) => [x.id, c]))]
    .map(([who, c]) => `<li>${code(who)} ${code(c.id)}<span class="wc-why" lang="en">${esc(c.msg)}</span></li>`).join('\n');
  const caveats = [
    S.complete === S.protocols ? fill(T.cavComplete, { n: S.protocols }) : fill(T.cavNotComplete, { n: S.protocols - S.complete }),
    partialButComplete.length ? fill(T.cavPartial, { list: list(partialButComplete.map((p) => p.name)) }) : '',
    hidden.length ? fill(T.cavHidden, { list: list(hidden.map((g) => g.global)) }) : '',
    T.cavTiming,
    W.conf.limits,
    T.cavOthers,
    CF.dirty ? fill(T.cavDirty, { commit: CF.commit }) : '',
  ].filter(Boolean).map((s) => `<li>${esc(s)}</li>`);
  const notes = X.notes.length ? `<h3>${esc(T.notesTitle)}</h3>\n<ul class="wc-list">${X.notes.map((n) => `<li lang="en">${esc(n)}</li>`).join('')}</ul>` : `<p class="hw-note">${esc(T.noNotes)}</p>`;
  const summary = `<dl class="wp-meta">
<div><dt>${esc(W.conf.runDate)}</dt><dd><time datetime="${esc(X.date)}">${esc(fmtD(X.date))}</time></dd></div>
<div><dt>${esc(T.commit)}</dt><dd><a href="${esc(`${GITHUB}/commit/${X.commit}`)}" rel="noopener"><code>${esc(X.commit.slice(0, 10))}</code></a>${X.dirty ? ` <span class="wp-dev">${esc(W.conf.dirty)}</span>` : ''}</dd></div>
<div><dt>${esc(W.conf.runTool)}</dt><dd><code>${esc(CF.tool)}</code></dd></div>
<div><dt>${esc(W.conf.runEnv)}</dt><dd>wayland-protocols ${esc(E.waylandProtocols)} · libwayland ${esc(E.libwayland)} · Xwayland ${esc(E.xwayland)}</dd></div>
<div><dt>${esc(W.conf.runMode)}</dt><dd>${esc(T.modeText)}</dd></div>
<div><dt>${esc(T.duration)}</dt><dd>${esc(fill(T.seconds, { n: X.seconds }))}</dd></div>
</dl>
<ul class="wp-stats">
${tiles}
</ul>
<p class="wp-sum">${esc(fill(T.sumWayland, { n: S.protocols, rows: rows.size, mean: S.mean, complete: S.complete }))}</p>
<p class="wp-sum">${esc(fill(x11Other.length ? T.sumX11 : T.sumX11All, { n: S.x11Items, mean: S.x11Mean, complete: S.x11Complete, other: x11Other.length }))}</p>
${x11Other.length ? `<ul class="wc-list">\n${x11List}\n</ul>` : ''}
<p class="wp-sum">${esc(fill(T.sumChecks, { ...N, total: num(N.total), pass: num(N.pass) }))}</p>
${N.allowed ? `<details class="wc-det"><summary>${esc(fill(T.allowedTitle, { n: N.allowed }))}</summary><ul class="wc-list">\n${allowedList}\n</ul></details>` : ''}
<h3>${esc(T.cavTitle)}</h3>
<ul class="wc-list">
${caveats.join('\n')}
</ul>
${notes}`;

  // ---- Wayland and X11 tables
  const gRows = CF.globals.map((g) => {
    const d = detG.get(g.global), p = g.row ? rowOf.get(g.row) : null;
    return `<tr id="g-${esc(anchor(g.global))}">
<th scope="row" data-label="${esc(T.hGlobal)}"><code class="wc-g">${esc(g.global)}</code>${g.advertised ? '' : `<span class="wc-hid">${esc(T.notAdvertised)}</span>`}</th>
<td data-label="${esc(W.hProtocol)}"><div>${p ? `<a href="${esc(tableUrl)}#p-${esc(p.id)}">${esc(p.name)}</a>${p.lordos.status === 'partial' ? ` <span class="wp-st wp-partial">${esc(W.status.partial)}</span>` : ''}` : '—'}${d.xml ? `<span class="wc-xml">${code(d.xml.replace(/^.*\//, ''))}</span>` : ''}</div></td>
<td data-label="${esc(T.hVersion)}"><div>${g.version == null ? '—' : esc(g.version)}</div></td>
<td data-label="${esc(W.conf.hConf)}" class="wp-cf"><div>${scoreCell(g)}</div></td>
<td data-label="${esc(T.hDepth)}"><div>${depthPill(g.depth)}</div></td>
<td data-label="${esc(T.hChecks)}" class="wc-kc"><div>${kindCell(d.checks, 'g-' + g.global)}</div></td>
<td data-label="${esc(T.hProof)}"><div>${proofLinks(g.proofs)}</div></td>
</tr>`;
  }).join('\n');
  const xRows = CF.x11.map((x) => {
    const d = detX.get(x.id);
    return `<tr id="x-${esc(anchor(x.id))}">
<th scope="row" data-label="${esc(W.conf.hItem)}"><code class="wc-g">${esc(x.id)}</code><span class="wp-sumy" lang="en">${esc(x.title)}</span></th>
<td data-label="${esc(W.conf.hConf)}" class="wp-cf"><div>${scoreCell(x)}</div></td>
<td data-label="${esc(T.hDepth)}"><div>${depthPill(x.depth)}</div></td>
<td data-label="${esc(T.hChecks)}" class="wc-kc"><div>${kindCell(d.checks, 'x-' + x.id)}</div></td>
<td data-label="${esc(T.hProof)}"><div>${proofLinks(x.proofs)}</div></td>
</tr>`;
  }).join('\n');
  const kindLegend = KINDS.filter((k) => allChecks.some((c) => c.kind === k)).map((k) => `<div><dt><span class="wc-kn">${esc(T.kind[k])}</span></dt><dd>${esc(fill(T.kindText[k], X.weights))}</dd></div>`).join('\n');
  const depthLegend = ['complete', 'partial', 'general', 'code'].map((k) => `<div><dt>${depthPill(k)}</dt><dd>${esc(W.conf.depthText[k])}</dd></div>`).join('\n');
  const resultLegend = ['pass', 'allowed', 'skip', 'fail'].map((r) => `<div><dt>${resPill(r)}</dt><dd>${esc(T.resultText[r])}</dd></div>`).join('\n');

  // ---- every check
  const checkTable = (checks) => `<div class="wc-ct"><table>
<thead><tr><th scope="col">${esc(T.hResult)}</th><th scope="col">${esc(T.hKind)}</th><th scope="col">${esc(T.hCheck)}</th><th scope="col">${esc(T.hPoints)}</th><th scope="col">${esc(T.hMsg)}</th></tr></thead>
<tbody>
${checks.map((c) => {
    const same = c.what.replace(/\s+/g, '.') === c.id || c.what === c.id;
    // no data-label per cell (1,300 rows): on a phone each row stacks as result and points, check, message
    return `<tr><td>${resPill(c.result)}</td><td>${esc(T.kind[c.kind])}</td><td><code>${esc(c.id)}</code>${same ? '' : `<span class="wc-what" lang="en">${esc(c.what)}</span>`}</td><td class="wc-pt" data-label="${esc(T.hPoints)}">${c.result === 'skip' ? '—' : esc(c.weight)}</td><td lang="en">${esc(c.msg)}${c.where ? ` <code class="wc-at">${esc(c.where)}</code>` : ''}</td></tr>`;
  }).join('\n')}
</tbody>
</table></div>`;
  const covLine = (d) => (d.coverage && d.coverage.total ? `<p class="wc-cov">${esc(fill(T.coverage, d.coverage))}${d.coverage.missing.length ? ` ${esc(T.coverageMissing)} ${d.coverage.missing.map(code).join(' ')}` : ''}</p>` : '');
  const kindSummary = (checks) => byKind(checks).map((x) => `${T.kind[x.k]} ${x.ok}/${x.n}`).join(' · ');
  const gDetails = CF.globals.map((g) => {
    const d = detG.get(g.global);
    return `<details class="wc-det" id="c-g-${esc(anchor(g.global))}"><summary>${code(g.global)} <span class="wc-ds">${esc(pct(g.score))} · ${esc(kindSummary(d.checks))}</span></summary>
${d.where ? `<p class="wc-cov">${esc(T.createdAt)} ${code(d.where)}${d.interfaces.length ? ` · ${esc(T.interfaces)} ${d.interfaces.map(code).join(' ')}` : ''}</p>` : ''}${covLine(d)}
${checkTable(d.checks)}
</details>`;
  }).join('\n');
  const xDetails = CF.x11.map((x) => {
    const d = detX.get(x.id);
    return `<details class="wc-det" id="c-x-${esc(anchor(x.id))}"><summary>${code(x.id)} <span class="wc-ds">${esc(pct(x.score))} · ${esc(kindSummary(d.checks))}</span></summary>
${checkTable(d.checks)}
</details>`;
  }).join('\n');

  // ---- real programs
  const clients = X.clients.map((c) => `<tr>
<th scope="row" data-label="${esc(T.hProgram)}"><span lang="en">${esc(c.client)}</span></th>
<td data-label="${esc(T.hResult)}"><div>${resPill(c.result)}</div></td>
<td data-label="${esc(T.hMsg)}" lang="en"><div>${esc(c.msg)}</div></td>
<td data-label="${esc(T.hBound)}"><div>${c.bound.length ? c.bound.map(code).join(' ') : '—'}</div></td>
</tr>`).join('\n');

  // ---- comparison (wayland.app's lists of what each desktop offers; nothing about how well)
  const c = D.compared, st = D.stats;
  const offered = (v) => v != null;
  const CATS = ['core', 'stable', 'staging', 'unstable', 'experimental', 'wlr', 'kde', 'other'].filter((k) => D.protocols.some((p) => p.cat === k));
  const catRows = CATS.map((k) => {
    const ps = D.protocols.filter((p) => p.cat === k);
    return `<tr><th scope="row" data-label="${esc(W.hCategory)}">${esc(W.cats[k])}</th><td data-label="${esc(T.hListed)}"><div>${ps.length}</div></td><td data-label="LordOS"><div>${ps.filter((p) => offered(p.lordos.version)).length}</div></td><td data-label="${esc(W.kde)}"><div>${ps.filter((p) => offered(p.kwin)).length}</div></td><td data-label="${esc(W.gnome)}"><div>${ps.filter((p) => offered(p.mutter)).length}</div></td></tr>`;
  }).join('\n');
  const ver = (v) => (v == null ? `<span class="wp-no">—<span class="vh"> ${esc(W.notOffered)}</span></span>` : `<span class="wp-yes">${esc(W.version)} ${esc(v)}</span>`);
  const pill = (s) => `<span class="wp-st wp-${esc(s)}">${esc(W.status[s])}</span>`;
  const protoCell = (p) => `<a href="${esc(p.url)}" rel="noopener">${esc(p.name)}</a><code>${esc(p.id)}</code>`;
  const lordosRows = D.protocols.filter((p) => offered(p.lordos.version)).map((p) => `<tr>
<th scope="row" data-label="${esc(W.hProtocol)}">${protoCell(p)}</th>
<td data-label="LordOS"><div>${ver(p.lordos.version)}${p.lordos.status === 'partial' ? ' ' + pill('partial') : ''}</div></td>
<td data-label="${esc(W.kde)}"><div>${ver(p.kwin)}</div></td>
<td data-label="${esc(W.gnome)}"><div>${ver(p.mutter)}</div></td>
<td data-label="${esc(T.hConfLordos)}" class="wp-cf"><div>${p.conf ? `<a href="#g-${esc(anchor(p.conf.globals[0]))}" class="wp-score">${bar(p.conf.score)} <strong>${esc(pct(p.conf.score))}</strong></a> ${depthPill(p.conf.depth)}` : '—'}</div></td>
</tr>`).join('\n');
  const notLordos = D.protocols.filter((p) => !offered(p.lordos.version) && (offered(p.kwin) || offered(p.mutter)));
  const otherRows = notLordos.map((p) => `<tr>
<th scope="row" data-label="${esc(W.hProtocol)}">${protoCell(p)}</th>
<td data-label="${esc(W.kde)}"><div>${ver(p.kwin)}</div></td>
<td data-label="${esc(W.gnome)}"><div>${ver(p.mutter)}</div></td>
<td data-label="LordOS"><div>${pill(p.lordos.status)}</div></td>
<td data-label="${esc(W.hNote)}" class="wp-note"><div><span lang="en">${esc(p.lordos.note)}</span></div></td>
</tr>`).join('\n');
  // left out on purpose, grouped by reason (several protocols share one)
  const groups = new Map();
  for (const p of D.protocols.filter((x) => x.lordos.status === 'on-purpose')) { if (!groups.has(p.lordos.note)) groups.set(p.lordos.note, []); groups.get(p.lordos.note).push(p); }
  const purpose = [...groups].map(([note, ps]) => `<li><p class="wc-pp">${ps.map((p) => `<a href="${esc(tableUrl)}#p-${esc(p.id)}"><code>${esc(p.id)}</code></a>`).join(' ')}</p><p class="wc-why" lang="en">${esc(note)}</p></li>`).join('\n');
  const compare = `<p class="g-lead">${esc(fill(T.compareLead, { kwin: c.kwin.version, mutter: c.mutter.version, kwinDate: fmtD(c.kwin.dataDate), mutterDate: fmtD(c.mutter.dataDate) }))} <a href="${esc(c.source)}" rel="noopener">wayland.app</a></p>
<ul class="wp-stats">
${[tile(st.total, T.cTotal), tile(st.lordos, T.cLordos), tile(st.kwin, fill(T.cKwin, { v: c.kwin.version })), tile(st.mutter, fill(T.cMutter, { v: c.mutter.version })), tile(`${st.lordosOfBoth} / ${st.both}`, T.cBoth)].join('\n')}
</ul>
<p class="wp-sum">${esc(fill(W.summary, st))}</p>
<h3>${esc(T.catTitle)}</h3>
<div class="wp-table wc-narrow"><table>
<thead><tr><th scope="col">${esc(W.hCategory)}</th><th scope="col">${esc(T.hListed)}</th><th scope="col">LordOS</th><th scope="col">${esc(`${W.kde} (KWin ${c.kwin.version})`)}</th><th scope="col">${esc(`${W.gnome} (Mutter ${c.mutter.version})`)}</th></tr></thead>
<tbody>
${catRows}
</tbody></table></div>
<h3>${esc(fill(T.offeredTitle, { n: D.protocols.filter((p) => offered(p.lordos.version)).length }))}</h3>
<p>${esc(T.offeredLead)}</p>
<div class="wp-table"><table>
<thead><tr><th scope="col">${esc(W.hProtocol)}</th><th scope="col">LordOS</th><th scope="col">${esc(`${W.kde} (KWin ${c.kwin.version})`)}</th><th scope="col">${esc(`${W.gnome} (Mutter ${c.mutter.version})`)}</th><th scope="col">${esc(T.hConfLordos)}</th></tr></thead>
<tbody>
${lordosRows}
</tbody></table></div>
<h3>${esc(fill(T.othersTitle, { n: notLordos.length }))}</h3>
<p>${esc(T.othersLead)}</p>
<div class="wp-table"><table>
<thead><tr><th scope="col">${esc(W.hProtocol)}</th><th scope="col">${esc(`${W.kde} (KWin ${c.kwin.version})`)}</th><th scope="col">${esc(`${W.gnome} (Mutter ${c.mutter.version})`)}</th><th scope="col">LordOS</th><th scope="col">${esc(W.hNote)}</th></tr></thead>
<tbody>
${otherRows}
</tbody></table></div>
<h3>${esc(fill(T.purposeTitle, { n: st.onPurpose }))}</h3>
<p>${esc(T.purposeLead)}</p>
<ul class="wc-purpose">
${purpose}
</ul>
<p class="hw-note">${esc(W.sourceNote)}</p>
<p><a class="btn btn-ghost" href="${esc(tableUrl)}">${esc(T.tableLink)}</a></p>`;

  // ---- run it yourself: every command was run on Debian 13 before it went here
  let cmdN = 0;
  const cmd = (text) => { const id = `wc-cmd-${++cmdN}`; return `<div class="cmd"><div class="cmd-bar"><span class="cmd-t" aria-hidden="true"><i></i><i></i><i></i>shell</span><button type="button" class="copy" data-copy="${id}" data-done="${esc(t.copied)}" aria-label="${esc(t.copy)}: ${esc(text.split('\n')[0])}" hidden>${esc(t.copy)}</button></div><pre id="${id}" tabindex="0">${esc(text)}</pre></div>`; };
  const R = T.steps;
  const step = (k, cmds, vars = {}, after = R[k].after) => `<li><h3>${esc(R[k].title)}</h3><p>${esc(fill(R[k].text, vars))}</p>${cmds.map(cmd).join('')}${after ? `<p class="hw-note">${esc(fill(after, vars))}</p>` : ''}</li>`;
  const steps = [
    step('pkgs', [`sudo apt-get update\nsudo apt-get install -y --no-install-recommends ${PKGS}`]),
    step('apps', [`sudo apt-get install -y --no-install-recommends ${PKGS_APPS}`], { list: list(X.clients.filter((x) => x.result === 'pass').map((x) => x.client)) }),
    step('clone', [`git clone ${repoUrl}.git\ncd LordOS`, `git checkout ${X.commit}`], { commit: X.commit.slice(0, 10) }, X.dirty ? R.clone.afterDirty : null),
    step('build', ['npm ci\nnpm run build\nmake -C server -j4']),
    step('run', ['make -C server wl-conformance'], { s: X.seconds }),
    step('read', ['less server/build/wl-conformance.json']),
    step('compare', ['node tools/wl-conformance/compare.mjs']),
    step('one', ['node tools/wl-conformance.mjs --only wp_fifo_manager_v1 --no-x11', 'node tools/wl-conformance.mjs --only xdg_session_manager_v1 --no-x11 --verbose', 'node tools/wl-conformance.mjs --static-only']),
    step('session', ['./server/build/wl-globals'], { n: CF.globals.filter((g) => g.advertised).length, hidden: list(hidden.map((g) => g.global)) }),
    step('proofs', ['node tools/wl-proof.mjs --out /tmp/wl-proofs']),
  ].join('\n');

  return {
    summary, tableRows: gRows, x11Rows: xRows, kindLegend, depthLegend, resultLegend,
    gDetails, xDetails, clients, proofs: proofFigs, compare, steps,
    hasClients: X.clients.length > 0,
  };
}
