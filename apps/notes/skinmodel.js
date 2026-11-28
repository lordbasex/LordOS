// skinmodel.js — what Notes hands a theme's skin (docs/THEMES-GUIDE.md, "App skins → Notes"): `sidebar` (the list of notes,
// a list pane with the search) and `toolbar` (the open note's commands, a command bar). The note itself, its editor and
// every dialog stay Notes': a skin gets each note's title and date, never its text, and asks only for Notes' own commands.
import { commandBar, listPane } from '../../sdk/appskinmodel.js';

export const NOTE_COMMANDS = ['new', 'read', 'edit', 'checklist', 'emoji', 'image', 'attach', 'duplicate', 'desktop', 'folder', 'delete'];

// s: { notes: [{ slug, title, date, selected }], query, title, mode, onDesktop, has }; label(id)
export function notesModels(s, label, a) {
  const pane = listPane({ title: label('list'), items: s.notes.map((n) => ({ key: n.slug, label: n.title, detail: n.date, selected: n.selected })) });
  const bar = commandBar({ title: s.title, commands: NOTE_COMMANDS.filter((id) => id === 'new' || s.has).map((id) => ({ id, label: label(id),
    active: (id === 'read' && s.mode === 'read') || (id === 'edit' && s.mode === 'edit') || (id === 'desktop' && s.onDesktop),
    group: ['new'].includes(id) ? 'file' : ['read', 'edit'].includes(id) ? 'mode' : ['checklist', 'emoji', 'image', 'attach'].includes(id) ? 'insert' : 'note' })) });
  return {
    sidebar: { app: 'notes', ...pane, query: s.query || '', actions: { select: a.select, search: a.search, command: a.command } },
    toolbar: { app: 'notes', ...bar, actions: { command: a.command } },
    bar,
    pane,
  };
}
