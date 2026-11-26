// react-made.cjs — React as the theme host bundle sees it (tools/build-theme.mjs buildThemeHost; never in LordOS's own
// themes): every element LordOS's code makes with createElement, cloneElement or the JSX runtime has its props object
// recorded. React's host guard (sdk/themeapi.js guardHost) then refuses a host node whose props were not recorded: an
// element a community theme forged (an object with React's $$typeof) can't become a node, wherever it got in.
const R = require('react-original');
const made = new WeakSet();
const mark = (el) => {
  if (el !== null && typeof el === 'object' && el.props !== null && typeof el.props === 'object') made.add(el.props);
  return el;
};
module.exports = Object.assign({}, R, {
  createElement: (...a) => mark(R.createElement(...a)),
  cloneElement: (...a) => mark(R.cloneElement(...a)),
  lordMadeProps: (p) => made.has(p),
  lordMark: mark,
});
