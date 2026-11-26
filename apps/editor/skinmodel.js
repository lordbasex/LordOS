// skinmodel.js — what the Text Editor hands a theme's skin (docs/THEMES-GUIDE.md, "App skins → Text Editor"): the view
// model of its four regions, `toolbar` (the Text Editor has none of its own), `tabBar`, `statusBar` and `contextMenu`. The
// text itself, the project tree, the find bar and every dialog stay the Text Editor's: a skin never gets the document's
// text and can only ask for a closed list of the editor's own commands (SKIN_COMMANDS), each run exactly as its menu
// item; a tab with unsaved changes still asks before it closes. Node-tested by server/tests/appskin_test.mjs; the editor
// with every region stubbed: server/tests/editor_skin_scene.mjs.

export const SKIN_COMMANDS = ['new', 'open', 'openFolder', 'save', 'saveAs', 'close', 'print', 'undo', 'redo', 'cut', 'copy', 'paste', 'selectAll',
  'find', 'findNext', 'gotoLine', 'gotoFile', 'language', 'toggleProject'];

// s: { tabs: [{ id, name, dirty, active }], status: { line, col, cursors, selected, language, indent, eol, message, ok } }
export function editorViewModel(s, a) {
  return {
    app: 'editor',
    tabs: s.tabs.map((x, i) => ({ key: x.id, name: String(x.name || ''), dirty: !!x.dirty, active: !!x.active, index: i })),
    status: {
      line: s.status.line | 0, col: s.status.col | 0, cursors: s.status.cursors | 0, selected: s.status.selected | 0,
      language: String(s.status.language || ''), indent: String(s.status.indent || ''), eol: s.status.eol === 'CRLF' ? 'CRLF' : 'LF',
      message: s.status.message ? { text: String(s.status.message.text || ''), ok: !!s.status.message.ok } : null,
    },
    commands: SKIN_COMMANDS.map((id) => ({ id, label: s.label(id) })),
    actions: a,
  };
}

export const skinCommand = (id) => (SKIN_COMMANDS.includes(id) ? id : null);
