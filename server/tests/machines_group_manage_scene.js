// machines_group_manage_scene.js — goes after the theme bundle in tests/machines_group_manage_headless.sh (a test tool,
// never shipped): through the real lord.vm calls of a headless lord-server (ui/js_vm.c → lord-vm), on the throwaway
// library the script made: groups() names a damaged group (a member folder removed, a base removed) with what is
// missing; groupRenamePlan and groupRename; groupRepair (the member made again from its base, the base made again from
// the image store's verified archive); groupDelete; and the calls refuse what isn't a group of this library (a path
// outside it, a group folder that is a link). Lines start with "GMAN" in the shell log; the script checks them.
(function () {
  const log = (s) => lord.log(`GMAN ${s}`);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const check = (what, ok, extra = '') => log(`${ok ? 'ok' : 'FAIL'} ${what}${extra ? ` (${extra})` : ''}`);
  const job = async (id) => { if (!id) return { ok: false, exit: -1, out: 'refused' }; for (;;) { const r = lord.vm.job(id); if (!r || r.done) return r || { ok: false, out: 'gone' }; await sleep(300); } };
  const lib = `${lord.fs.home()}/Machines`;
  const byName = (n) => (lord.vm.groups() || []).find((g) => g.name === n);
  (async () => {
    await sleep(2500);
    let a = byName('Whonix'), b = byName('Tor2');
    check('groups() lists both groups', !!a && !!b);
    check('a base missing is named', JSON.stringify(b?.basesMissing) === '["workstation"]', JSON.stringify(b?.basesMissing));
    check('the archive is in the store', b?.archiveThere === true);
    const ws = (lord.vm.list() || []).filter((m) => m.groupDir === a.dir).map((m) => m.role).sort().join(',');
    check('the removed member isn\'t listed', ws === 'gateway', ws);
    // refusals: never outside the library, never a link
    check('delete refuses a path outside the library', lord.vm.groupDelete('/tmp/x.lordgroup') === 0);
    check('delete refuses a group folder that is a link', lord.vm.groupDelete(`${lib}/Link.lordgroup`) === 0);
    check('repair refuses a machine folder', lord.vm.groupRepair(`${a.dir}/Gateway.lordvm`) === 0);
    check('rename refuses a bad name', lord.vm.groupRename(a.dir, '.hidden') === 0);
    // rename: the plan, then the job
    const plan = lord.vm.groupRenamePlan(a.dir, 'tor2');
    check('the plan names another group\'s name', plan.problem === 'name-taken' && plan.other === 'Tor2', JSON.stringify(plan));
    const plan2 = lord.vm.groupRenamePlan(a.dir, 'Tor Work');
    check('a free name moves the folder', plan2.problem === '' && plan2.moves && plan2.folder === 'Tor Work.lordgroup', JSON.stringify(plan2));
    let r = await job(lord.vm.groupRepair(a.dir));
    check('repair (a member folder removed)', r.ok, r.out.trim().split('\n').pop());
    a = byName('Whonix');
    const ws2 = (lord.vm.list() || []).filter((m) => m.groupDir === a.dir && m.groupOk).map((m) => m.role).sort().join(',');
    check('both members are whole again', ws2 === 'gateway,workstation', ws2);
    r = await job(lord.vm.groupRename(a.dir, 'Tor Work'));
    check('rename', r.ok, r.out.trim().split('\n').pop());
    check('the group has its new name and folder', !!byName('Tor Work') && byName('Tor Work').dir === `${lib}/Tor Work.lordgroup`);
    r = await job(lord.vm.groupRepair(b.dir));
    check('repair (a base removed: made again from the store\'s archive)', r.ok, r.out.trim().split('\n').pop());
    b = byName('Tor2');
    check('the base is there again', b?.basesThere === true && (b?.basesMissing || []).length === 0);
    r = await job(lord.vm.groupDelete(byName('Tor Work').dir));
    check('delete', r.ok, r.out.trim().split('\n').pop());
    check('the group is gone', !byName('Tor Work') && !(lord.vm.list() || []).some((m) => String(m.groupDir || '').includes('Tor Work')));
    r = await job(lord.vm.groupDelete(b.dir));
    check('delete the last group', r.ok, r.out.trim().split('\n').pop());
    check('no group left', (lord.vm.groups() || []).length === 0);
    log('done');
  })().catch((e) => log(`FAIL error ${e && e.stack}`));
})();
