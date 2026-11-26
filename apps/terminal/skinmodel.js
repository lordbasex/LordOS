// skinmodel.js — what the Terminal hands a theme's skin (docs/THEMES-GUIDE.md, "App skins → Terminal"): the view model of
// its three regions, `tabBar`, `statusBar` and `contextMenu`. The panes themselves are never a skin's: what a session
// shows and what is typed into it stay the Terminal's. A skin gets the tabs and a few facts about the session in front,
// and can only ask for a closed list of the Terminal's own commands (SKIN_COMMANDS), each run exactly as its menu item
// would; there is no action that writes to a session. Node-tested by server/tests/appskin_test.mjs; the Terminal with
// every region stubbed: server/tests/terminal_skin_scene.mjs.

// The Terminal's commands a skin may ask for (apps/terminal/keys.js action names).
export const SKIN_COMMANDS = ['newTab', 'newWindow', 'copy', 'paste', 'find', 'splitRight', 'splitDown', 'selectAll', 'clearBuffer', 'preferences', 'closePane'];

// s: { tabs: [{ id, title, color, active, busy, bell }], status: { cwd, process, busy, shell, cols, rows } | null,
// broadcast }; a: the actions.
export function terminalViewModel(s, a) {
  return {
    app: 'terminal',
    tabs: s.tabs.map((x, i) => ({ key: x.id, title: String(x.title || ''), color: x.color || null, active: !!x.active, busy: !!x.busy, bell: !!x.bell, index: i })),
    status: s.status ? { cwd: s.status.cwd || '', process: s.status.process || '', busy: !!s.status.busy, shell: s.status.shell || '', cols: s.status.cols | 0, rows: s.status.rows | 0 } : null,
    broadcast: !!s.broadcast,
    commands: SKIN_COMMANDS.map((id) => ({ id, label: s.label(id) })),
    actions: a,
  };
}

// The command a skin asked for, if it is one it may ask for.
export const skinCommand = (id) => (SKIN_COMMANDS.includes(id) ? id : null);
