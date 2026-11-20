// Settings → Users: the machine's accounts (docs/SESSIONS.md). Reads the list without root
// (lord.users) and makes changes through the administration helper: user-add, user-del, user-passwd,
// user-admin, user-avatar, user-autologin. Passwords travel only in the action's argument
// (never in argv) and with quiet, so they do not end up in the "in progress" notice.
import {
  View, Text, Image, Button, Toggle, TextInput, useTheme, useLocale, admin, useSystemInfo, remote, fs, spacing, typeScale,
} from '@lordos/sdk';
import { useEffect, useState } from 'react';
import { Stack, Section, SettingRow, FieldRow, Note, Message, Actions } from './ui.jsx';
import { AvatarEditor } from './AvatarEditor.jsx';

const COLORS = ['#5b9cff', '#f0915a', '#4cc37a', '#e0609d', '#a77bf3', '#4cc4d3', '#f2c14e', '#8e8e93'];
const NAME_RE = /^[a-z_][a-z0-9_-]{0,31}$/;

function shade(hex, f) {
  const n = parseInt(hex.slice(1), 16);
  const c = (v) => Math.max(0, Math.min(255, Math.round(v * f)));
  return `#${[n >> 16, (n >> 8) & 255, n & 255].map((v) => c(v).toString(16).padStart(2, '0')).join('')}`;
}
function hash(s) { let h = 0; for (const c of s) h = (h * 31 + c.codePointAt(0)) >>> 0; return h; }
const PALETTE = ['#5b9cff', '#f0915a', '#4cc37a', '#e0609d', '#a77bf3', '#4cc4d3', '#f2c14e'];
const colorOf = (u) => u.color || PALETTE[hash(u.name) % PALETTE.length];
function initials(u) {
  const w = (u.full || u.name).trim().split(/\s+/).filter(Boolean);
  return (w.length > 1 ? w[0][0] + w[1][0] : (w[0] || u.name).slice(0, 1)).toUpperCase();
}

export function UserAvatar({ user, size = 40 }) {
  const c = colorOf(user);
  return (
    <View style={{ width: size, height: size, borderRadius: size / 2, overflow: 'hidden', alignItems: 'center', justifyContent: 'center',
      background: `linear-gradient(170deg, ${c}, ${shade(c, 0.78)})` }}>
      {user.image ? <Image src={user.image} style={{ width: size, height: size, objectFit: 'cover' }} />
        : <Text style={{ fontSize: Math.round(size * 0.4), fontWeight: 600, color: '#fff' }}>{initials(user)}</Text>}
    </View>
  );
}

const listUsers = () => (typeof lord.users === 'undefined' ? { users: [], autologin: '' } : lord.users.list());

export function UsersSettings() {
  const t = useTheme();
  const { t: tr } = useLocale();
  const info = useSystemInfo(60000);
  const [data, setData] = useState(listUsers);
  const [sel, setSel] = useState(null);
  const [adding, setAdding] = useState(false);
  const [msg, setMsg] = useState(null);
  const [busy, setBusy] = useState(false);
  const isAdmin = admin.available() && info.admin;
  const reload = () => setData(listUsers());
  const st = remote.status();
  const open = new Set((st.sessions || []).map((s) => s.user));
  const users = isAdmin ? data.users : data.users.filter((u) => u.self);
  const cur = users.find((u) => u.name === sel) || users.find((u) => u.self) || users[0];

  const run = (action, arg, ok) => {
    setBusy(true);
    setMsg(null);
    return admin.run(action, arg, null, { quiet: true }).then(
      () => { setMsg({ ok: true, text: ok }); reload(); return true; },
      (e) => { setMsg({ ok: false, text: e.message }); return false; }).finally(() => setBusy(false));
  };

  return (
    <Stack>
      {!isAdmin && <Note tone="warn">{tr('settings.users.onlyOwn')}</Note>}
      {!isAdmin && data.textMode && info.admin && <Note>{tr('settings.users.textModeNoAdmin')}</Note>}
      <Section>
        {users.map((u) => {
          const on = cur && cur.name === u.name && !adding;
          return (
            <SettingRow key={u.name} icon={<UserAvatar user={u} size={32} />} onClick={() => { setSel(u.name); setAdding(false); setMsg(null); }}
              label={`${u.full || u.name}${u.self ? ` · ${tr('settings.users.you')}` : ''}`} labelStyle={{ fontWeight: on ? 600 : 400 }}
              hint={[u.name, u.admin ? tr('settings.users.admin') : tr('settings.users.standard'), open.has(u.name) ? tr('settings.users.sessionOpen') : '']
                .filter(Boolean).join(' · ')}>
              {on ? <Text style={{ fontSize: 14, fontWeight: 600, color: t.accent }}>✓</Text> : null}
            </SettingRow>
          );
        })}
      </Section>
      {isAdmin && !adding && <Actions><Button label={tr('settings.users.add')} onClick={() => { setAdding(true); setMsg(null); }} /></Actions>}
      {busy && <Note>{tr('settings.users.working')}</Note>}
      <Message msg={msg} />
      {isAdmin && !adding && (
        // the whole computer's: no login screen at the next boots, a login prompt on tty1 and startlordos there (as startx)
        <Section title={tr('settings.users.loginTitle')} footer={tr('settings.users.textModeHint')}>
          <SettingRow label={tr('settings.users.textMode')}>
            <Toggle small pending on={!!data.textMode} onChange={(on) => run('boot-text', on ? 'on' : 'off', tr('settings.users.saved'))} />
          </SettingRow>
        </Section>
      )}
      {adding ? <AddUser onDone={(name) => { setAdding(false); if (name) setSel(name); }} run={run} />
        : cur ? <UserDetail key={cur.name} user={cur} isAdmin={isAdmin} run={run} autologin={data.autologin} admins={data.users.filter((x) => x.admin).length} /> : null}
    </Stack>
  );
}

function AddUser({ onDone, run }) {
  const { t: tr } = useLocale();
  const [f, setF] = useState({ full: '', name: '', pass: '', pass2: '', admin: false });
  const [nameTouched, setNameTouched] = useState(false);
  const put = (p) => setF((o) => ({ ...o, ...p }));
  // suggested user name from the full name
  const suggest = (full) => full.toLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g, '').split(/\s+/)[0].replace(/[^a-z0-9_-]/g, '').slice(0, 32);
  const err = !NAME_RE.test(f.name) ? tr('settings.users.badName') : !f.pass ? tr('settings.users.noPass')
    : f.pass !== f.pass2 ? tr('settings.users.mismatch') : /[:,=\n]/.test(f.full) ? tr('settings.users.badFull') : '';
  const create = () => run('user-add', [f.name, f.full.trim(), f.pass, f.admin ? '1' : '0'].join('\n'), tr('settings.users.created'))
    .then((ok) => ok && onDone(f.name));
  const field = { flex: 1 };
  return (
    <View style={{ gap: spacing.m }}>
      <Section title={tr('settings.users.newTitle')} footer={err}>
        <FieldRow label={tr('settings.users.fullName')}>
          <TextInput value={f.full} placeholder={tr('settings.users.fullName')} autoFocus style={field}
            onChange={(full) => put(nameTouched ? { full } : { full, name: suggest(full) })} />
        </FieldRow>
        <FieldRow label={tr('settings.users.userName')}>
          <TextInput value={f.name} placeholder={tr('settings.users.userName')} style={field} onChange={(name) => { setNameTouched(true); put({ name: name.toLowerCase() }); }} />
        </FieldRow>
        <FieldRow label={tr('settings.users.password')}>
          <TextInput secure value={f.pass} placeholder={tr('settings.users.password')} style={field} onChange={(pass) => put({ pass })} />
        </FieldRow>
        <FieldRow label={tr('settings.users.repeat')}>
          <TextInput secure value={f.pass2} placeholder={tr('settings.users.repeat')} style={field} onChange={(pass2) => put({ pass2 })} onSubmit={() => !err && create()} />
        </FieldRow>
        <SettingRow label={tr('settings.users.makeAdmin')} hint={tr('settings.users.makeAdminHint')}><Toggle small on={f.admin} onChange={(admin) => put({ admin })} /></SettingRow>
      </Section>
      <Actions>
        <Button label={tr('settings.users.cancel')} onClick={() => onDone(null)} />
        <Button primary label={tr('settings.users.create')} disabled={!!err} onClick={() => !err && create()} />
      </Actions>
    </View>
  );
}

function UserDetail({ user, isAdmin, run, autologin, admins }) {
  const t = useTheme();
  const { t: tr } = useLocale();
  const [pw, setPw] = useState({ old: '', pass: '', pass2: '' });
  const [editing, setEditing] = useState(false);       // the picture chooser and its fitting (AvatarEditor.jsx)
  const [del, setDel] = useState(false);
  const [keep, setKeep] = useState(true);
  useEffect(() => { setPw({ old: '', pass: '', pass2: '' }); }, [user.name]);
  const canEdit = isAdmin || user.self;
  const pwErr = !pw.pass ? '' : pw.pass !== pw.pass2 ? tr('settings.users.mismatch') : user.self && !pw.old ? tr('settings.users.needOld') : '';
  const lastAdmin = user.admin && admins <= 1;
  const setColor = (c) => run('user-avatar', [user.name, c, ''].join('\n'), tr('settings.users.avatarSaved'));
  const field = { flex: 1 };
  return (
    <View style={{ gap: spacing.xl }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', gap: spacing.l, paddingHorizontal: spacing.xs }}>
        <UserAvatar user={user} size={64} />
        <View style={{ flex: 1, minWidth: 0, gap: 2 }}>
          <Text style={{ fontSize: typeScale.title, fontWeight: 600, color: t.text, textAlign: 'start' }}>{user.full || user.name}</Text>
          <Text style={{ fontSize: typeScale.callout, color: t.textDim, textAlign: 'start' }}>{user.name}</Text>
        </View>
      </View>
      {canEdit && (
        <Section title={tr('settings.users.avatar')}>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: spacing.s, paddingVertical: spacing.m, direction: 'ltr' }}>
            {COLORS.map((c) => (
              <View key={c} onClick={() => setColor(c)} style={{ width: 22, height: 22, borderRadius: 11, alignItems: 'center', justifyContent: 'center',
                background: `linear-gradient(170deg, ${c}, ${shade(c, 0.78)})` }}>
                {user.color === c ? <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: '#fff' }} /> : null}
              </View>
            ))}
          </View>
          <View style={{ flexDirection: 'row', gap: spacing.s, paddingVertical: spacing.s, alignItems: 'center' }}>
            <Button small label={tr('settings.users.chooseImage')} onClick={() => setEditing(true)} />
            {user.image ? <Button small label={tr('settings.users.removeImage')} onClick={() => run('user-avatar', [user.name, user.color || '', '-'].join('\n'), tr('settings.users.avatarSaved'))} /> : null}
          </View>
          {editing ? (
            <AvatarEditor onClose={() => setEditing(false)}
              onSave={(path) => {
                setEditing(false);
                // the fitted copy (~/.cache/lordos/avatar.jpg) goes to /var/lib/lordos/users
                run('user-avatar', [user.name, user.color || '', path].join('\n'), tr('settings.users.avatarSaved'));
              }} />
          ) : null}
        </Section>
      )}
      {canEdit && (
        <View style={{ gap: spacing.m }}>
          <Section title={tr('settings.users.changePassword')} footer={pwErr}>
            {user.self ? (
              <FieldRow label={tr('settings.users.current')}>
                <TextInput secure value={pw.old} placeholder={tr('settings.users.current')} style={field} onChange={(old) => setPw((p) => ({ ...p, old }))} />
              </FieldRow>
            ) : null}
            <FieldRow label={tr('settings.users.newPassword')}>
              <TextInput secure value={pw.pass} placeholder={tr('settings.users.newPassword')} style={field} onChange={(pass) => setPw((p) => ({ ...p, pass }))} />
            </FieldRow>
            <FieldRow label={tr('settings.users.repeat')}>
              <TextInput secure value={pw.pass2} placeholder={tr('settings.users.repeat')} style={field} onChange={(pass2) => setPw((p) => ({ ...p, pass2 }))} />
            </FieldRow>
          </Section>
          <Actions>
            <Button label={tr('settings.users.changePassword')} disabled={!pw.pass || !!pwErr} onClick={() => !pwErr && pw.pass &&
              run('user-passwd', [user.name, pw.pass, pw.old].join('\n'), tr('settings.users.passwordChanged')).then((ok) => ok && setPw({ old: '', pass: '', pass2: '' }))} />
          </Actions>
        </View>
      )}
      {isAdmin && (
        <Section>
          <SettingRow label={tr('settings.users.admin')} hint={lastAdmin ? tr('settings.users.lastAdmin') : tr('settings.users.makeAdminHint')}>
            <Toggle small pending on={user.admin} onChange={(on) => !(lastAdmin && !on) && run('user-admin', `${user.name}\n${on ? 1 : 0}`, tr('settings.users.saved'))} />
          </SettingRow>
          <SettingRow label={tr('settings.users.autologin')} hint={tr('settings.users.autologinHint')}>
            <Toggle small pending on={autologin === user.name} onChange={(on) => run('user-autologin', on ? user.name : '', tr('settings.users.saved'))} />
          </SettingRow>
        </Section>
      )}
      {isAdmin && !user.self && (!del ? (
        <Actions><Button destructive label={tr('settings.users.delete')} onClick={() => setDel(true)} /></Actions>
      ) : (
        <View style={{ gap: spacing.m }}>
          <Section footer={tr('settings.users.deleteConfirm', { name: user.full || user.name })}>
            <SettingRow label={tr('settings.users.keepHome')}><Toggle small on={keep} onChange={setKeep} /></SettingRow>
          </Section>
          <Actions>
            <Button label={tr('settings.users.cancel')} onClick={() => setDel(false)} />
            <Button destructive label={tr('settings.users.delete')} onClick={() => run('user-del', `${user.name}\n${keep ? 1 : 0}`, tr('settings.users.deleted'))} />
          </Actions>
        </View>
      ))}
    </View>
  );
}
