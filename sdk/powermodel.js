// powermodel.js — what Restart… and Shut Down…'s 60 s countdown does when it runs out (sdk/power.jsx), pure.
// A countdown a community theme's code started (sdk/themeapi.js session.restart/shutdown: byTheme) never finishes on
// its own: it is cancelled, so only the person's real click or key on the dialog restarts or shuts down (a theme
// can't end the session as if the person had, which would clear its crash count; docs/THEMES-GUIDE.md §6).
export function countdownEnds(ask) {
  if (!ask || ask.state !== 'ask') return 'none';
  return ask.byTheme ? 'cancel' : 'confirm';
}
