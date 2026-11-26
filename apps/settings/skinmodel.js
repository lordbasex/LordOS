// skinmodel.js — what Settings hands a theme's skin (docs/THEMES-GUIDE.md, "App skins → Settings"): the view model of
// its two regions, `sidebar` (the list of sections) and `home` (a page of categories before a section is open, only when
// a theme draws one). Plain data and Settings' own actions; each section's page stays Settings' own, drawn by Settings,
// so a skin can choose where to go but never what a page does. Node-tested by server/tests/appskin_test.mjs; Settings
// with both regions stubbed: server/tests/settings_skin_scene.mjs.

// The four groups of the sidebar, named for a category view (the order is Settings' GROUPS).
export const SETTINGS_GROUPS = ['look', 'connect', 'input', 'system'];

// groups: Settings' visible groups, [[id, Body, color, glyph], …] each; tr: the translator.
export function settingsViewModel(s, a) {
  return {
    app: 'settings',
    section: s.atHome ? null : s.section,
    title: s.atHome ? null : s.tr(`settings.section.${s.section}`),
    query: s.query,
    groups: s.groups.map((g, i) => ({
      id: SETTINGS_GROUPS[s.groupIndex[i]] || 'system',
      label: s.tr(`settings.skin.group.${SETTINGS_GROUPS[s.groupIndex[i]] || 'system'}`),
      sections: g.map(([id, , color, glyph]) => ({ id, label: s.tr(`settings.section.${id}`), color, glyph, selected: !s.atHome && id === s.section })),
    })),
    user: { name: s.userName || '', admin: !!s.admin },
    SectionIcon: s.SectionIcon,
    actions: a,
  };
}
