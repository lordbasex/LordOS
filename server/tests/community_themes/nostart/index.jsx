// A test fixture: a theme whose code runs but never calls startShell. It is marked failed: nothing would ever be drawn.
import { configureWindows } from '@lordos/sdk';
import { apps } from '@lordos/apps';

configureWindows({ apps, workArea: (d) => ({ x: d.x, y: d.y, w: d.w, h: d.h }), chrome: { w: 0, h: 0 } });
// startShell(<App />) is missing
