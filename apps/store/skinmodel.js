// skinmodel.js — what the Store hands a theme's skin (docs/THEMES-GUIDE.md, "App skins → Store"): the view model of its two
// regions, `sidebar` (the pages: the Store's own, the origins, the picks) and `toolbar` (the title, Back, refresh and the
// search). Every page, every app's card and every install, update and removal stay the Store's, with its own
// confirmations and the administrator prompt: a skin can only choose a page, go back, search and ask for a refresh.
// Node-tested by server/tests/appskin_test.mjs; the Store with both regions stubbed: server/tests/store_skin_scene.mjs.

// s: { pages: [{ id, label, count, group, soon }], page, title, inApp, query, searchHint, refresh: { title, busy } | null }
export function storeViewModel(s, a) {
  return {
    app: 'store',
    pages: s.pages.map((p) => ({ id: p.id, label: String(p.label || ''), count: typeof p.count === 'number' && p.count > 0 ? p.count : 0, group: p.group,
      soon: !!p.soon, selected: !s.inApp && p.id === s.page })),
    page: s.page,
    title: String(s.title || ''),
    inApp: !!s.inApp,
    query: s.query || '',
    searchHint: String(s.searchHint || ''),
    refresh: s.refresh ? { title: String(s.refresh.title || ''), busy: !!s.refresh.busy } : null,
    actions: a,
  };
}
