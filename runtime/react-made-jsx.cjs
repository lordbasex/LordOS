// react-made-jsx.cjs — the JSX runtime of the theme host bundle: its elements recorded like react-made.cjs's
const J = require('react-original/jsx-runtime');
const { lordMark } = require('react');
module.exports = { Fragment: J.Fragment, jsx: (...a) => lordMark(J.jsx(...a)), jsxs: (...a) => lordMark(J.jsxs(...a)) };
