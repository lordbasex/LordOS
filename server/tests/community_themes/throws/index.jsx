// A test fixture: a theme that throws while its code starts. The session goes back to the theme before, and this one
// is marked failed with the error kept for Report.
import { startShell } from '@lordos/sdk';

throw new Error('community fixture throws at start');
// eslint-disable-next-line no-unreachable
startShell(null);
