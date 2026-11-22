// An error boundary around each part of the desktop: if one part throws while rendering, it is logged (shell.log)
// and left out, and the rest of the desktop keeps working.
import { Component } from 'react';

export class Guard extends Component {
  constructor(props) {
    super(props);
    this.state = { failed: false };
  }
  static getDerivedStateFromError() {
    return { failed: true };
  }
  componentDidCatch(err) {
    console.error(`celeste: ${this.props.name || 'part'} failed:`, err && err.stack ? err.stack : String(err));
  }
  render() {
    return this.state.failed ? null : this.props.children;
  }
}
