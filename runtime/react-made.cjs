// react-made.cjs — React as the theme host bundle sees it (tools/build-theme.mjs buildThemeHost; never in LordOS's own
// themes): every element LordOS's code makes with createElement, cloneElement or the JSX runtime has its props object
// recorded with its type, and both frozen. React's host guard (sdk/themeapi.js guardHost) then refuses a host node whose props were not recorded: an
// element a community theme forged (an object with React's $$typeof) can't become a node, wherever it got in.
const R = require('react-original');
// props object → the element's type: a private record (the theme can't reach this map), checked against the type
// React creates the node with, and the props frozen so a theme holding an element can't change what was recorded
const made = new WeakMap();
const mark = (el) => {
  if (el !== null && typeof el === 'object' && el.props !== null && typeof el.props === 'object') {
    made.set(el.props, el.type);
    Object.freeze(el.props);
    Object.freeze(el);
  }
  return el;
};
module.exports = Object.assign({}, R, {
  createElement: (...a) => mark(R.createElement(...a)),
  cloneElement: (...a) => mark(R.cloneElement(...a)),
  lordMadeProps: (p, type) => made.has(p) && made.get(p) === type,
  lordMark: mark,
});
