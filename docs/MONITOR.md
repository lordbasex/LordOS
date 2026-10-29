# Monitor

Preinstalled native app (`apps/monitor`) to see how this machine is being used: CPU, memory,
network, disks, processes and each app's network connections on a world map. It copies the look
of TangoOS's Monitor (`tangoos/web/packages/app-monitor`): segmented tabs, dark cards with an icon,
a big number and a live area chart, a row of small figures and a footer with the sampling rate.
The Network tab takes the shape of a per-app network monitor (apps on the left, map in the middle, summary on
the right).

Phase 1 was read-only. Since then the Network tab holds per-app firewall rules (Phase 2) and, in
0.8.1, the Processes tab can stop a process and show its details ("Process actions"). In 0.8.2
each destination of an app has its own allow/block switch, the traffic chart reaches back 30 days
("Traffic history"), the Disks tab shows each disk's temperature and health, and the session can
notify when a disk gets too hot or less healthy ("Disk temperature and health"). In 0.8.3 the
remote desktop is a row of its own and blocking an app cuts the connections it had open. In 0.8.5
(0.8.5-beta, released 2026-10-27) the Network tab became a tree with a view menu and search chips, LordOS's own
components and the system's services got rows of their own, and the menu bar network item and the
blocklists arrived ("Network, second generation").

## Tabs

| Tab | What it shows |
|---|---|
| Overview | CPU, Memory, Network and Disk with a 60 s chart; below, CPU temperature, fan, 1 min load, 5/15 min load and cached memory |
| CPU | total usage (big chart), one chart per core, 1/5/15 load, temperature and tasks |
| Memory | stacked bar (used, file cache, free), swap and a 60 s chart |
| Network | apps with connections, map, summary or detail of the chosen app (see below) |
| Disks | total read and write; one row per disk (model, `/dev`, size, temperature, health, rates); the chosen disk's temperature over the last 30 minutes and its health, with "Open in Disks" (see "Disk temperature and health") |
| Processes | sortable table (CPU, memory, threads, PID, user), search, "Mine" / "All"; Stop and Get Info for the selected process (see "Process actions") |
| Cooling | the cooling hardware drawn live, the active fan profile, a temperature and fan history and "full speed" (`apps/monitor/Cooling.jsx`, see `FANS.md`) |

Charts pause with the mouse over them and show a crosshair with the values at that instant, as in
TangoOS. Time always runs left to right, also in Arabic and Hebrew; the rest of the window mirrors
itself.

### Network

Since 0.8.5 the left column is a tree with a view menu, search chips and rows for LordOS's own
parts and the system's services: see "Network, second generation (0.8.5)" below. What follows is
the rest of the tab, and the first generation of the list.

- **Left:** search (by app, address or country) and one row per app with its icon, number of
  connections, a traffic bar (received in green, sent in orange, relative to the app that moved
  the most), a tiny live activity indicator (its traffic over the last 12 s, one bar per second),
  the bytes and the **Rule** column: a per-app allow/deny switch that `lord-netperm`
  enforces in the kernel (see "Phase 2"). Each app expands to show its destinations, one row per
  name the system resolver gave (its addresses together) or per address, with the country, the
  ones the firewall reported recently ("recent connection" or "recently denied") and, since 0.8.2,
  **each one's own allow/block switch** (see "Per-destination switches"); a click on a row selects
  it in the right panel. Since 0.8.3 the app's name gets the row's whole width and wraps to two
  lines (the connection count and the denied badge sit next to the traffic bar; the column is
  336 px and the Data and Rule columns are narrower). Since 0.8.5 the list covers what sits below TCP and
  UDP too (`conn.c`): TCP and UDP from `sock_diag`; ICMP "ping" sockets from `/proc/net/icmp` and `icmp6`,
  shown as "ICMP (ping)" without a destination (the kernel keeps none for an unconnected ping socket);
  raw IP sockets from `/proc/net/raw` and `raw6`, with their IP protocol (ICMP, IGMP, GRE, ESP, OSPF,
  VRRP…); and packet sockets from `/proc/net/packet`, with the ethertype (ALL, IPv4, ARP, IPv6, EAPOL,
  LLDP…) and the interface. Raw and packet sockets need `CAP_NET_RAW` and can see or forge traffic, so
  the row says so. `build/conn-dump` prints one reading for checking it by hand (on VM 116: a ping, a raw
  ICMP socket and an ARP packet socket on eth0, the last two from a process given `CAP_NET_RAW`). The remote desktop's bridge runs as the user but isn't dumpable, so
  its sockets can't be traced to a pid; `conn.c` gives the sockets in `lordos.service`'s root-owned
  `supervisor` group to Remote Desktop (its WebRTC UDP ports), instead of an unknown "?". At the bottom,
  the machine's traffic:
  - the current download and upload speed;
  - two pills with what was sent (↑, orange) and received (↓, green) in the visible minute;
  - a **mirrored chart** of the last 60 s (`TrafficChart.jsx`, `drawMirror` in `draw.js`): one bar
    per second, sent above the baseline and received below, every bar on a faint full-height track
    so the strip reads as one continuous chart even when idle; the newest second grows in from the
    right edge while the rest slides left, redrawn at the 4 samples per second with the window in
    front (once per second behind it). Auto-scaled to a "nice" maximum shown at both ends; any
    bucket that moved bytes is at least one pixel tall;
  - since 0.8.2, a zoom above the chart: **60 s** (the live minute), **1 h**, **24 h**, **7 d**
    and **30 d**, with ‹ › to step one window back or forward and **Now** to come back (see
    "Traffic history").
- **Center:** the map fills the whole height of the column. It has an arc from "This computer" to
  each country, one dot per country (bigger the more connections) and pill labels with the name
  and the count. It's controlled like this:
  - wheel: zoom toward the cursor;
  - touchpad: pinching zooms toward the center of the gesture and two fingers move the map (see
    `TRACKPAD.md`; with remote desktop, the browser's trackpad pinch works too).
    Screenshots: [before](trackpad-map-before.png) and [after pinching](trackpad-map-after.png);
  - drag: move;
  - double click: zoom in;
  - `+`, `−` and "show all" buttons in the corner.

  When zoomed in, the borders and names of the other countries show. The Lite base has no cities,
  so labels are always countries. (0.8.2: city labels would need a city-level IP database with a
  compatible license; Debian 13 only ships country-level ones, `geoip-database`, so none was added.)

  **Direction of the traffic.** While bytes move, dots travel along each arc: sent (orange) from
  "This computer" to the place, received (green) back. The higher the rate, the more dots (1 to 4
  each way) and the faster they go (3.4 s to 1 s per trip, on a log scale from 64 B/s to about
  4 MB/s); any traffic at all moves (one slow dot each way below 64 B/s, 0.8.5). An idle arc has no
  dots and is drawn thinner, still plainly visible (`idle` in the map request, `map.c`). The map
  follows the list: every app's places, the selected app's, or, with a search or a filter (direction,
  protocol, status, chips), only the places of the connections the list shows. A click on the empty space
  under the list clears the selection (0.8.5). Each app has its own
  arc per place, in its own color (the dot before it in the list; the order apps first reached the
  internet, twelve colors), lifted apart when several apps reach the same country (`arcs` in the map
  request: color and lift per arc). A host's own rate misses short connections, so what the app's rate
  doesn't account for is spread over its internet hosts by their bytes. Sockets the kernel reports
  without a cgroup (ping and raw sockets from /proc/net, packet sockets) take their process's
  (`/proc/<pid>/cgroup`, `conn.c`). The
  dots are small ready-made images (a ring and the direction's color, drawn once) moved with
  `lord.setStyle`, no React render per frame: 12 frames per second with the window in front, 5
  behind it, at most the 12 busiest arcs and only those on screen; the timer stops with the window
  minimized, while the map is being moved and when nothing moves. Each frame repaints only the
  small rectangles the dots left and entered (0.8.2, "Several damaged rectangles", off by default
  in the 0.8.2 release). With Appearance → Reduce motion, each busy arc shows two still markers instead: a short
  comet of three dots, the biggest in front, pointing the way the bytes go.

  When an app is chosen, the map shows only its places and frames them. A click on a dot or an arc
  chooses that country: the right panel shows its apps, addresses, ports and bytes. At the bottom,
  a line with the DB-IP attribution and the link to the privacy settings.
- **Right** (0.8.3: the overlay scroll bar sits in the panel's padding, so it never covers a value
  and no strip is reserved; long values, app names and destinations wrap to two lines instead of
  being cut). Every section folds with a click on its header (▾/▸) and the folded ones are
  remembered (`settings.monitor.netFolded`), for every app and the next time Monitor opens:
  - no selection: traffic (current and since boot), allowed and denied connections (0: there are
    no rules yet), incoming, listening and from other users, and the apps and destinations with the
    most traffic, each with "↓ received · ↑ sent" (destinations by the name the resolver gave,
    when known);
  - with a history zoom chosen (0.8.2): also "Most traffic · 24 h" (or the zoom chosen), the apps
    that moved the most in that window, from the history file;
  - with an app chosen, in the sections Process (executable path, user, PIDs), Rule (rule,
    counts, a "Rules for this app" link to Settings → Privacy with that app open), Connections
    (active, incoming, listening, destinations), Data (bytes and speed with the app's own mirrored chart of the last
    minute, one bar per second, or the history window chosen), Ports and protocols (with the
    listening ports) and Destinations. A selected address offers **Allow / Block this domain** (or "this address" when no
    name is known) for that app: a sheet prefilled with the destination (see `FIREWALL.md`, "Rules
    by app");
  - with a country chosen: bytes, apps and addresses (with ports and app).

Hosts are shown by the name the system resolver gave the app (see "Per-destination switches"), or
as IPs when there is none. There is no reverse DNS: asking for an IP's name sends it to the DNS
server, and this app's rule is that no address leaves the machine.

**"This computer"** is placed by the time zone (`/etc/localtime` → coordinates from
`/usr/share/zoneinfo/zone1970.tab`), without the network.

### Network, second generation (0.8.5)

![Monitor → Network: the tree, a listed destination and the system rows](monitor-net2-tree.png)

**Rows for LordOS's own parts.** Everything built in that uses the network is a row of its own,
backed by its own process and cgroup scope, so its rules are enforced by the kernel like any app's
(`FIREWALL.md`, "LordOS components in their own scopes"):

| Row | Firewall id | Its traffic |
|---|---|---|
| Mail and calendars | `lordos:mail` | lord-maild's servers (Mail, Calendar, Contacts, Reminders sync), made by Mail's connector |
| Store and updates | `lordos:store` | the package engine's downloads (the Store, Settings → General, the daily update check) |
| Printers and scanners | `lordos:print` | IPP and eSCL to network printers and scanners, and their mDNS discovery |
| Terminal | `lordos:terminal` | the shell of every terminal and every program it starts; the detail lists the programs by name |
| Blocklist updates | `lordos:lists` | the daily blocklist downloads (`lord-fetch`) |
| VPN | `lordos:vpn` | the counters of LordOS's own VPN interfaces (Settings → Network); not blockable |
| Name resolution (DNS) | `system:dns` | systemd-resolved; shown, never blockable |
| Time synchronization | `system:time` | systemd-timesyncd; shown, never blockable |
| Network service | `system:network` | lord-netd's own leaf (DHCP, Wi-Fi, the VPN daemons); shown, never blockable |

Apps that never touch the network (Calculator, Notes…) never appear: a row exists only for traffic.
The essential system rows show "System" instead of a connection count and a dash instead of a switch.
Blocking the Store asks first, because no security update arrives while it is blocked:

![Blocking the Store and updates asks first](monitor-net2-store-warning.png)

The **"LordOS" row** is now what is left in lord-server itself, close to zero; every socket a
component's connector made counts as that component's, also in the open-connections list (the
socket's cgroup, `INET_DIAG_CGROUP_ID`, mapped to the session's scopes: conn.c `cgApp`).

![The rows: Terminal with its program, the system services and the Store](monitor-net2-components.png)

**The tree.** Each app opens into its destinations (the name the system resolver gave, or the
address), a name with several addresses opens into them, and each address into its ports (TCP 443,
UDP 443…). Every row has its received and sent bytes, a tiny up/down meter of its current rate
(log scale), and, for a destination, its own allow/deny switch with a hint when something else
decides it (another rule, the app's default, a restriction, the deny mode or a blocklist: "Blocked
by the list Scam sites"). Long lists fold after eight rows ("N more…"). For an address reached
through a CNAME chain the row shows the shortest name (usually the one the app asked for) and
searches every name of the chain. The per-port bytes come from lord-netperm (np_flows is per
address and port since 0.8.5; COUNTS sends each address's eight busiest ports).

**The view menu** (one pop-up button at the top of the column) holds the whole view, kept per user in
`settings.monitor.netView`:

- group by: apps, domains, addresses, ports or countries (the tree's levels follow: a domain opens
  into its apps and their ports, a port into its apps and their destinations…);
- direction: all, outgoing, incoming; protocol: all, TCP, UDP and QUIC; status: everything,
  allowed, denied, with rules;
- sort by: name, most traffic, least traffic, most recent, most connections;
- "Show network activity in the menu bar" and "Blocklists…" (Settings → Privacy).

![The view menu](monitor-net2-view-menu.png) ![Grouped by port](monitor-net2-ports.png)

**Search with chips.** Typing `app:firefox`, `domain:example.com`, `ip:10.0.0.0/8`, `port:443`,
`proto:udp` (or `quic`) or `country:AR` and a space turns the token into a chip; Enter turns every
word (an address or network becomes an IP chip, a number a port, `tcp`/`udp` a protocol, two
capital letters a country, a name with a dot a domain, anything else an app). Chips of different
kinds combine (app *and* domain), two of the same kind are alternatives; the rest of the text
matches anywhere. Backspace in an empty field removes the last chip. There is no city chip: there
is no city-level IP database with a compatible licence (see "Map").

![Two chips: port 443 and domain debian.org](monitor-net2-chips.png)

**The time range.** The traffic chart's zoom (1 h, 24 h, 7 d, 30 d, with ‹ ›) now also drives the
list: in a history window it shows each app's bytes in that window, from the history file. The
history never keeps destinations (see "Traffic history", Privacy), so in a window the rows have no
children; the live view keeps the tree.

The pure part (tokens, filters, grouping, sorting, the saved view) is `sdk/netmodel.js`, tested by
`server/tests/netmodel_test.mjs`; the tab is `apps/monitor/Network.jsx` and `apps/monitor/NetTree.jsx`.

### The menu bar network item (0.8.5)

Optional and off by default (Settings → Privacy → Monitor → "Network activity in the menu bar", or
the view menu). It shows the upload and download rates in the status area, rounded ("↑5K ↓231K",
"0" under 1 KB/s), updated every other second and only redrawn when the rounded value changes.
Its panel:

- the sent (up) and received (down) graph of the last five minutes, 60 bars of 5 s, and their totals;
- the apps moving data now, with their rates;
- **Recently denied** in the last ten minutes, with a count, the same destination grouped (×N);
- the firewall mode: Allow silently, Deny silently, Off (`FIREWALL.md`, "The modes"). The ask mode
  is not offered until it exists; rule profiles come with the rules manager (its own release after
  0.8.5, `ROADMAP.md`);
- the blocklists (lists on, entries) and "Open Monitor…".

While the item is shown it reads `lord.monitor.netTotals()` (one read of `/proc/net/dev`) once
per second, without the Monitor's sampling thread; lord-netperm's notices are read only while the
panel is open. Hidden, it costs nothing. `sdk/netstatus.jsx`.

![The panel](menubar-network-panel.png) ![Deny silently cut the open download; DNS still works](menubar-network-deny.png)

### Blocklists (0.8.5)

Settings → Privacy → Blocklists (`apps/monitor/Blocklists.jsx`): the curated catalog by topic, each
list with its source, its licence, how many entries it had and when it was downloaded; a checkbox
turns it on, "Update Now" downloads again, and your own lists take an https address and a format.
How they are downloaded, checked and enforced is in `FIREWALL.md`, "Blocklists". The Monitor shows
the lists in its summary, "On the list …" under a destination, and "by a blocklist" in Recently
denied.

![Settings → Privacy → Blocklists](settings-blocklists.png)

![Malware lists on, and your own list by its https address](settings-blocklists-malware.png) ![A custom address list, downloaded and checked](settings-blocklists-custom.png)

### Measurements (0.8.5, VM 118, 4 vCPU Xeon Silver 4108, kernel 6.12)

Same session, same test traffic (curl in an app scope: a 120 KB/s download and HTTPS requests every
4 s), main's build (0.8.3 + 0.8.4 so far) against this one, 30 s each, lord-server's CPU in % of
one core:

| | main | this build |
|---|---|---|
| Monitor open on Network, traffic | 17.1 / 16.1 % | 14.8 / 14.0 % (the tree, chips, system rows; 4 blocklists on) |
| desktop idle, menu bar item off | 2.65 % (the VM's previous install) | 2.93 % (4 blocklists on) |
| desktop idle, menu bar item on | — | 2.95 % |
| desktop with traffic, item on / its panel open | — | 0.97 % / 3.87 % |

Memory with 4 lists (379,991 names, 19,405 networks): lord-netperm 13.8 MB (2.9 MB without lists),
lord-server 142 MB with the Monitor open (131 MB on main). The per-packet cost is in `FIREWALL.md`
("Measurements, 0.8.5").

**Before** (main, no lists: the scam site connects) **and after:**

![Before](monitor-net2-before.png) ![After, light](monitor-net2-tree-light.png)

### Remote desktop (0.8.3)

While the remote desktop is on (Settings → Sharing), it is one more row in the app list, **Remote
desktop**, with the Sharing icon: its traffic bar and bytes, the number of browsers connected and,
in the red badge, the failed sign-ins of the last 24 hours. `lord-sessiond` serves it as root,
outside the session's apps, so its sockets never appear among them (they are counted in "From other
users"); the row is built from `lord-sessiond`'s own state (`apps/monitor/RemoteDesktop.jsx`). Its
Rule switch is fixed: the per-app firewall doesn't apply to it; its own lists do.

The detail panel, in the same folding sections:

- **Connected:** each browser's address, user, how long, and its traffic (bytes each way and the
  current rate, reported by the bridge to its root parent every 2 s), with **Block**;
- **Failed sign-ins:** address, the name that was typed (cleaned to printable ASCII), how many times
  and when, with **Block**. Administrators only: it comes from the admin action `remote-failures`,
  which re-validates every row; passwords are never kept anywhere;
- **Temporarily blocked:** the lockout's blocks with the time left, **Unblock** and **Block
  permanently**;
- **Address lists:** the mode, the blocklist (with Unblock), the allowlist, and a field to block a
  typed address or network.

Every action is one of the fixed admin actions Settings uses (`remote-block`, `remote-unblock`;
`sdk/remote.js` `remoteAccess`), validated again on the root side. Everything else is read-only:
`/run/lordos/status.json` (clients, blocks, lists; no user names typed by strangers) through
`lord.remote.status()`. See [REMOTE.md](REMOTE.md), "Blocked and allowed addresses".

![Monitor → Network: the remote desktop (dark)](monitor-remote-desktop-dark.png)

![The same in light mode](monitor-remote-desktop-light.png)

### Blocking cuts what is open (0.8.3)

Switching an app's rule off (or a destination's switch, or adding a restriction) now also ends the
connections it already had: the kernel drops their packets and `lord-netperm` closes the sockets
(`FIREWALL.md`, "Existing connections are cut"). Each cut connection adds one to the app's
*denied* count, and the bytes dropped meanwhile show in its details as **Blocked traffic**
(`COUNTS` index 7). Within a second of the switch the row shows it.

### Per-destination switches (0.8.2)

An open app row lists the destinations it reached and gives each one an allow/block switch
(green: allowed, red: blocked), the same per-app rules Settings → Privacy → Network rules by app
edits (`FIREWALL.md`, "Rules by app"); there is no new enforcement:

- **What a row is:** one per name the system resolver gave for the app's addresses (from
  `lord-netperm`'s notices, `hosts`), with its addresses together ("cdn.example.net (4)"), or one
  per address when no name is known. The first eight show, then "N more…" (and "Show fewer").
  While the row is open the order is kept: busiest first when it opens, newcomers below, so a
  switch never moves under the pointer.
- **What the switch shows** is what the app's rules decide for that destination right now:
  `sdk/fwrule.js` `decide()` runs the same order as `fw_decide` (restrictions, then the most
  specific matching rule; no rule: the mode, which allows — since 0.8.5 the "Deny silently" mode and the
  blocklists can also decide, `decide()`'s `mode` and lists). When another rule decides, the row
  says which ("By the rule \*.example.com", "Follows this app's default"); a restriction ("no
  internet") locks the switch and says so.
- **What a click does** (`toggleOp()`): it writes the rule for exactly that name (or address),
  any port and protocol, with the other verdict; if that name already has its own rule it is flipped,
  or removed when the app's other rules alone already give the new verdict, so going back and forth
  never piles up rules. It goes through `lord.fw.addRule` / `updateRule` / `removeRule`, which save
  `firewall.json` and send `RELOAD`; `lord-netperm` applies it to the next connection.
- Domains match the names the system resolver returned (`FIREWALL.md`, "What domain rules can't
  see"): a browser using its own encrypted DNS shows addresses, and those rows write address rules.
- **Tests:** `server/tests/fw_domain_cases.txt`, 37 cases (exact names, wildcards, CNAME chains,
  networks, ports, protocols, LAN/internet, restrictions, IPv6), run through `fw_decide` by
  `build/fwdest-test` — once from the store and once after a round trip through the
  `firewall.json` text — and through `decide()` by `fwrule_input_test.mjs`, which also checks the
  switch's add / flip / remove choices. On VM 119 a click on a Firefox ESR destination wrote
  `{"app":"desktop:firefox-esr","dst":"scripts.webcontentassessor.com","action":"deny"}` and
  `lord-netperm` logged the reload.

### Traffic history (0.8.2)

The chart's zoom reads a history lord-server keeps: bytes received and sent **per minute for 7 days
and per hour for 30 days**, for the machine (every interface but `lo`, as the live totals) and for
each of the user's apps (`lord-netperm`'s per-app counters, the ones the live list uses).

| Zoom | Window | One bar |
|---|---|---|
| 60 s | the live minute (as before) | 1 s |
| 1 h | 1 hour | 1 minute |
| 24 h | 24 hours | 15 minutes |
| 7 d | 7 days | 1 hour |
| 30 d | 30 days | 6 hours |

‹ and › move one window back or forward (no further back than the oldest record), **Now** comes
back to the present; a window that reaches the present is read again every minute. With an app
selected its own chart follows the same window. Bars are average rates over the bar, the pills the
window's totals.

**Privacy.**

- **Whose file:** the apps are the user's own, so the history is the user's:
  `$XDG_STATE_HOME/lordos/net-history.bin` (default `~/.local/state/lordos/net-history.bin`),
  mode 0600 in a 0700 folder, written by the user's own lord-server. No root process writes into
  a home folder, and the machine's total is recorded only while that user's session runs.
- **What it holds:** app ids (`desktop:firefox-esr`, `lpk:…`, `lordos`) and byte counts. **Never an
  address, a domain name or a country**, whatever the "connections" switch says: the long-term
  file can't be used to rebuild where the machine went, only how much each app moved. (The live
  lists keep showing addresses and names while the Monitor is open and "connections" is on.)
- **Switch and Clear:** Settings → Privacy → Monitor → **Keep a traffic history** (on by default;
  `settings.monitor.history`) stops recording at once; **Clear History** deletes the file and what
  is in memory. Nothing leaves the machine.
- The login screen and isolated apps never record (lord-server only allows it in a user session).

**How it is written** (`server/src/monitor/netrec.c`, `nethist.c`):

- every 5 s lord-server reads `/proc/net/dev` and the latest per-app counters `lord-netperm`
  already sends it (`COUNTS`, read without waiting from the same socket `lord.fw` uses), and adds
  the difference to the minute in progress (in memory: its totals and one entry per app that
  moved bytes);
- when the minute ends it **appends one record** to the file, a few dozen bytes in one `write`;
- at startup and every 6 hours a **compaction** folds minutes older than 7 days into hours, drops
  hours older than 30 days and any damaged tail, and replaces the file atomically (a temporary file,
  `fsync`, `rename`). 40 days of one record every 10 minutes compact to under 64 KB (`nethist-test`); a
  week of minutes with a few apps is a few hundred KB.

**File format.** `"LNH1"`, a version byte and three reserved bytes, then records
`[type u8][length u16][payload][CRC-32]`; payloads use LEB128 numbers. `APP` gives an app id an
index; `MIN` (Unix time / 60) and `HOUR` (Unix time / 3600) hold the totals and `(index, rx, tx)`
per app. The reader checks every length, every varint and every CRC and stops at the first record
that doesn't check out (a crash in the middle of a write): what came before is kept and the next
compaction drops the rest. A file that isn't ours, that isn't a regular file of the user's or that
is bigger than 8 MB is not read; a symlink in its place is refused.

**Cost.** One small file read and a look at a buffer every 5 s, one append per minute: nothing
measurable next to the session (the main loop wakes every 5 s for it, only while recording is on).

`lord.monitor.history({ from, step, n, app })` returns `{ res, oldest, now, enabled, rx: [n], tx: [n],
top: [[app, rx, tx]] }`: bytes per bucket (`from` and `step` in Unix seconds, `n` ≤ 1500), for the
machine or one app id; `res` is the finest resolution found (60 or 3600 s), records longer than a
bucket are spread over it. `historyEnable(on)` and `historyClear()` go with the setting and the
button.

**Tests:** `build/nethist-test` (in `make -C server test`): totals survive writing, reopening and
bigger buckets; the top apps; 40 days of records rotate to 7 days of minutes and 30 days of hours
(totals checked exactly); a cut at every byte of a file and 2,000 random byte changes never crash,
keep what came before the damage and leave a clean file after the next write; a foreign file is
replaced; more than 1,024 apps go to "other"; Clear removes everything and recording goes on; a
symlink is refused. Also run with `-fsanitize=address,undefined`.

### Process actions (0.8.1)

Select a row in Processes and use the two buttons next to "Mine / All", the right-click menu, a
double click or the keyboard:

| Action | Toolbar | Menu | Keys |
|---|---|---|---|
| **Stop** | circle with a cross | "Quit Process…" | Delete (or ❖/Ctrl+Backspace) |
| **Get Info** | circled "i" | "Get Info" | ❖/Ctrl+I, Enter, double click |

The arrow keys move the selection. Both buttons are disabled while nothing is selected.
❖/Ctrl+Q is not used: it quits the Monitor.

**Stop** opens a sheet: *Are you sure you want to quit "name"?* with **Quit** (SIGTERM: the process
is asked to close), **Force Quit** (SIGKILL) and **Cancel**.

- **The user's own processes** (same uid) are signalled by `lord-server` itself with a syscall,
  not a program: `lord.monitor.signal(pid, start, 'TERM' | 'KILL')` (`js_monitor.c` →
  `proc_signal()` in `monitor/procctl.c`).
- **Another user's or root's processes**, only for administrators (sudo/admin/wheel): the root
  helper's fixed action `proc-signal <pid> <start> TERM|KILL` (`monitor/proc_admin.c`), the same
  authorization as every other privileged action (`admin.c` refuses a non-administrator with
  `admin.notAdmin`). The arguments are two decimal numbers and one of two words, checked on the root
  side; there is no shell and no program. A non-administrator gets an explanation instead of the
  buttons.
- **No PID-reuse races.** Every row carries `st`, the process's start time in clock ticks (field 22
  of `/proc/<pid>/stat`). `proc_signal()` first pins the process with `pidfd_open`, then compares
  the start time, and only then calls `pidfd_send_signal` on that pidfd. If the process ended and
  its PID was given to another one before the pin, the start time differs and the request is
  refused (`proc.gone`); if it ends after the pin, the signal can only reach the pinned (dead)
  process. Kernels without pidfd fall back to `kill(2)` right after the check.
- **Protected processes** can't be stopped from the Monitor, in either path (`proc_protect_reason()`;
  each row has `prot`, and the C side checks again). The sheet explains why instead:

  | `prot` | Which | Explanation |
  |---|---|---|
  | `init` | PID 1 | "This process starts and runs the whole system." |
  | `kernel` | kernel threads (`PF_KTHREAD`) | "This is part of the Linux kernel." |
  | `session` | the session's own `lord-server` | "This is your LordOS session. To end it, use Log Out…" |
  | `system` | `lord-sessiond`, `lord-netperm`, `lord-netd`, `lord-fand`, `lord-vault`, `lord-maild`, `lord-doc`, `lord-media`, the root helper (`lord-admin`), any `lord-server` that is not an isolated app (another session, a terminal holder), PipeWire (`pipewire`, `pipewire-pulse`, `wireplumber`) and the systemd managers | "This process is part of LordOS." |

  An isolated `.lpk` app (`lord-server --app <id>`) is not protected: a hung app can be quit. The
  names are matched against the process name and, when it can be read, the executable's name. This
  is a guard against mistakes, not a security boundary (the kernel's rules on who may signal whom
  still apply; a terminal can always `kill` the user's own processes).

**Get Info** opens a sheet that follows the process once per second while it is open:

- **Header:** icon, name, PID and user; the executable path and the command line (arguments with
  spaces are quoted; up to four lines, then "…"; at most 128 arguments of 2 KB each are read).
- **Memory:** real (RSS), virtual, shared, private, proportional share (PSS) and swapped out, plus
  the real memory by kind (anonymous, mapped files, shared memory objects). From
  `/proc/<pid>/status` (anyone can read it) and `/proc/<pid>/smaps_rollup` (the owner or root).
- **Statistics:** user (and the effective uid if it differs), parent process (a link: it selects the
  parent and shows its details), process group and session, CPU %, CPU time, threads, context
  switches (voluntary and forced), page faults (minor and major), start time, nice and priority, the
  control group (the app scope, as the firewall sees it) and the restrictions LordOS knows about:
  an isolated LordOS app (Landlock, seccomp, its own cgroup), a seccomp filter, "no new
  privileges", a network block rule and the app's restrictions from Settings → Privacy (`lord.fw`).
- **Open files and ports:** each descriptor of `/proc/<pid>/fd` (up to 300; the total is always
  shown): files, devices, pipes, kernel objects and sockets, with the protocol, local and remote
  address and TCP state from that process's own `/proc/<pid>/net/{tcp,tcp6,udp,udp6,unix}`. Same
  privacy rules as the rest of the Monitor: addresses are numbers, nothing is looked up on the
  network, and with Settings → Privacy → Monitor → connections off the remote addresses are hidden.
- **Another user's process:** what anyone can read (status, stat, cmdline) comes from the session;
  the memory breakdown, executable, working folder and open files need root. For an administrator
  they come from the helper's read-only `proc-info <pid> <start>` action (the same JSON, sent as LOG
  lines of at most 900 bytes that `sdk/monitor.js` joins); for anyone else those fields say "Not
  available" or "Only an administrator can see this".
- **Quit…** opens the stop sheet for the same process; **Close** (or Escape) closes it. If the
  process ends, the sheet says so.

The details are built on the Monitor's sampling thread (`mon_detail`, once per second and only
while the sheet is open), never during a frame. Sampling a process's stacks ("Sample process") is
not offered: it needs ptrace and an unwinder, which LordOS doesn't ship.

Tests: `build/procctl-test` (in `make -C server test`): stat parsing with a hostile name, the
protected list, a stale start time refused while the process survives, TERM and KILL on real
children, the session and PID 1 refused, and the Get Info JSON (a pipe and a listening TCP socket
among the open files; no private parts when not the owner; "gone" for another start time).

## Disk temperature and health (0.8.2)

![Monitor → Disks, dark](disks-monitor-dark.png) ![Monitor → Disks, light](disks-monitor-light.png)

**Temperature, from the kernel, without root.** Each physical disk's temperature comes from its
hwmon sensor, which the session reads like any other sysfs file (`server/src/monitor/disktemp.c`):

| Disk | Sensor | Where it sits in sysfs |
|---|---|---|
| NVMe / M.2 (also an M.2 drive on a Raspberry Pi 5 HAT) | the nvme driver's own (`CONFIG_NVME_HWMON`, on in Debian's and LordOS's Pi kernel) | `/sys/block/nvme0n1/device` is the controller; the sensor is its child `hwmonN` (name `nvme`). With native multipath the block device's `device` is the `nvme-subsysN`, and the sensor is found through the controllers it lists |
| SATA | `drivetemp` (`CONFIG_SENSORS_DRIVETEMP=m`), loaded at boot by `/usr/lib/modules-load.d/lordos-drivetemp.conf` from the `lordos` package (migration `003-0.8.2-drivetemp` loads it once on an upgraded running system) | `/sys/block/sda/device` is the SCSI device; the sensor is in its glue folder `hwmon/hwmonN` (name `drivetemp`) |
| microSD / eMMC cards, virtual disks (virtio, QEMU), optical drives, most USB bridges | none | the tab says **No sensor**: there is never a made-up value |

Only chips named `nvme` or `drivetemp` count, `temp1_input` is the drive's main temperature
("Composite" on NVMe) and a reading outside −40…150 °C is "no value". The drive's own limits come
from `temp1_max` (warning) and `temp1_crit` (critical) when it exposes them; otherwise the tab says
"the drive does not report its own" and uses a conservative default: 75 °C for NVMe, 70 °C for a SATA
SSD and 55 °C for a spinning disk (`diskTempLimit` in `sdk/smart.js`).

**Cost.** The temperatures are read by the Monitor's own sampling thread (`mon.c`), never on the screen
thread: every 3 s (one sysfs read per disk) while the Monitor is open. The chart keeps the last 600
readings (30 minutes) in `diskTempHistory` (`sdk/monitor.js`), drawn with the same `Chart.jsx` as
Cooling. **A spinning disk is only read while it is awake** (I/O in the last minute, from
`/proc/diskstats`): asking a resting drive for its temperature can keep it from spinning down
(drivetemp sends it an ATA command), so it shows **Resting** until it is used again.

**Health light: OK / Check / Failing.** Each row and the chosen disk's card show the health from a
cached SMART summary (`smartSummaries`, `smart.refresh` in `sdk/smart.js`):

- with smartmontools installed: the root helper's `smart-health` action (`smartctl -n standby -i -H
  -A -c -l selftest`), which **never wakes a resting disk** (`-n standby`: smartctl only asks the
  power mode, and a disk in standby keeps its previous summary);
- without it: LordOS's own reader (`disk-smart`, see `SYSTEM.md`), only for flash disks and for
  spinning disks that are awake;
- at most once every 5 minutes per disk, one disk at a time, and only for disks that can have
  SMART (`sd*`, `hd*`, `nvme*n*`). Cards, optical drives and virtual disks say **No SMART** or **No
  data**.

The verdict is the one in "SMART with smartmontools" (`SYSTEM.md`): **Failing** when the disk says
so (overall status, a pre-fail attribute below its threshold, NVMe reliability or read-only
bits), **Check** with reallocated, pending or uncorrectable sectors, low NVMe spare, media errors or
more than 90 % of its rated life used. "Open in Disks" opens the Disks app on that disk.

**Notifications** (`sdk/diskwatch.js`, started by `startShell`), on by default, with a switch at the
bottom of the tab (`settings.monitor.diskAlerts`):

- **too hot:** while the Monitor is closed, `lord.monitor.watchDisks(true)` lets the sampling thread
  wake every 30 s and read only the disk list and their sensors (spinning disks only when awake);
  the session polls `lord.monitor.diskTemps()` every 30 s, which does not keep the 4 Hz sampling
  alive. A notice when a disk reaches its limit ("critical temperature", and it stays on screen, at
  its critical limit), at most one per disk every 30 minutes and again only after it has cooled
  3 °C below the limit;
- **health got worse:** 3 minutes after login and then every 6 hours the summaries are refreshed
  (the same `smart-health`, never waking a disk). A notice only when a disk is worse than the last
  time it was seen (remembered by model and serial in `settings.monitor.diskHealth`), with "Open in
  Disks";
- notices go through the session's notifications, so Do Not Disturb and Settings → Notifications
  apply. With the switch off, the background reading stops too.

## Architecture

```
apps/monitor/            index.jsx (tabs), Network.jsx (Network), NetTree.jsx (0.8.5: the tree, chips and
                         view menu), Blocklists.jsx (0.8.5: Settings → Privacy → Blocklists), MapView.jsx (map), Chart.jsx,
                         TrafficChart.jsx (mirrored sent/received chart), History.jsx (zoom and
                         history windows), draw.js (pixels),
                         icons.jsx, Privacy.jsx, countries.js (generated)
apps/monitor/Processes.jsx   the Processes tab, its context menu, the stop sheet and Get Info
apps/monitor/DisksTab.jsx    the Disks tab: rates, temperature and health per disk, the chosen disk's cards
sdk/smart.js, sdk/smartparse.js   SMART summaries and smartctl's JSON → one model (Disks app too)
sdk/diskwatch.js         notices when a disk gets too hot or less healthy
sdk/netmodel.js          (0.8.5) search tokens, filters, the tree, sorting and the component rows (pure)
sdk/netstatus.jsx        (0.8.5) the menu bar network item and the blocklists' hourly update check
sdk/monitor.js           useMonitor(), stores monitorSamples / monitorProcesses / monitorConnections /
                         monitorDetail; monitor.watchProcess(), monitor.stopProcess()
server/src/ui/js_monitor.c   lord.monitor (read-only, validated API)
server/src/monitor/mon.c     sampling thread: /proc, /sys, processes
server/src/monitor/disktemp.c  which hwmon sensor belongs to which disk (nvme, drivetemp)
server/src/monitor/conn.c    connections via sock_diag + inode → process
server/src/monitor/mmdb.c    our own MaxMind DB reader (IP → country)
server/src/monitor/map.c     vector map, on its own thread
server/src/monitor/nethist.c the traffic history file (append, query, compaction)
server/src/monitor/netrec.c  feeds it every 5 s from /proc/net/dev and lord-netperm's counters
server/src/monitor/procctl.c the protected list, proc_signal() (pidfd) and the Get Info JSON
server/src/monitor/proc_admin.c  the root helper's proc-signal / proc-info (administrators)
assets/monitor/          land.bin (Natural Earth 1:50m), geoip-country.mmdb.xz (DB-IP Lite)
tools/gen-monitor-data.py    generates that data, countries.js and the country names
apps/monitor/RemoteDesktop.jsx  (0.8.3) the remote desktop row and its detail
apps/monitor/Cooling.jsx     the Cooling tab (lord.fan, see FANS.md)
apps/monitor/Firewall.jsx, FwRules.jsx, RuleSheet.jsx   Settings → Privacy's firewall sections and the rule sheet
server/tests/mmdb_test.c, server/tests/map_bench.c   reader test and map benchmark
```

### lord.monitor

| Function | What it does |
|---|---|
| `start()` / `stop()` | adds or removes an interested party; at 0, or with no queries for 3 s, the thread goes to sleep |
| `want(processes, connections, countries)` | which lists the thread builds besides the sample |
| `sample()` | the latest sample, or `null` if there is no new one |
| `processes(lang)` | `{ t, total, hz, list: [{ pid, ppid, uid, user, name, cmd, exe, state, threads, cpu, rss, start, st, self, prot, app, appName, appIcon }] }` (`st`: start time in ticks; `prot`: why it can't be stopped, or `null`) |
| `connections(lang)` | `{ t, ok, others, list: [{ id, proto, fam, state, dir, local, lport, remote, rport, scope, cc, pid, self, name, exe, sent, recv, app, appName, appIcon }] }` |
| `info()` | `{ cpuModel, cores, geo, tick, home: { zone, cc, lat, lon } }`; `cpuModel` comes from `js_sys_cpu_name()`, the same one About uses (on ARM, the core from "CPU part" and the board) |
| `map(view)` | requests a map frame: `{ width, height, lon, lat, ppd, land, border, accent, homeColor, selColor, home, dots: [{ lon, lat, r, sel }] }` (validated numbers, up to 2048×2048 and 256 dots) |
| `mapFrame()` | the latest ready frame `{ seq, width, height, lon, lat, ppd, ms, data }`, or `null` |
| `detail(pid, st)` | Get Info: the process the thread describes once per second (`0, 0`: none) |
| `detailInfo()` | its latest details, or `null` if there is nothing new; `{ pid, gone: true }` once it ended |
| `signal(pid, st, 'TERM' \| 'KILL')` | stops one of the user's own processes: `null`, or an error code (`proc.gone`, `proc.protected…`, `proc.notOwn`, `proc.denied`) |
| `watchDisks(on)` | (0.8.2) with the Monitor closed, the thread wakes every 30 s and reads only the disk sensors |
| `diskTemps()` | (0.8.2) `{ t, disks: [{ name, model, rota, sensor, temp, tempT, tempMax, tempCrit, tempIdle, awake }] }` or `null` if nothing new; it does not keep the 4 Hz sampling alive |

| `history({ from, step, n, app })` | the traffic history in `n` buckets (see "Traffic history"), or `null` |
| `netTotals()` | (0.8.5) `{ t, rx, tx }`: the machine's byte counters now (every interface but `lo`), without the sampling thread (the menu bar item) |
| `historyEnable(on)` / `historyClear()` | the "Keep a traffic history" setting; deletes the history |

Each disk in `sample().disk.disks` carries the same fields (`sensor`: `"nvme"`, `"drivetemp"` or
`null`; `temp` in °C or `null`; `tempT`: when it was read; `tempIdle`: a spinning disk at rest, not
read; `awake`: I/O in the last minute).

No function takes paths or commands: the only arguments are booleans, a language code (letters,
digits, `-` and `_`, up to 15), a PID with its start time (integers), `'TERM'` or `'KILL'`, and the
history's integers and app id (`[A-Za-z0-9._:@+~/-]`, up to 127).
`lord-server` runs nothing for the Monitor.

### Sampling without slowing the screen

- A dedicated thread (`mon.c`) builds the sample every **250 ms** (4 Hz, as TangoOS): `/proc/stat`
  (total and per-core CPU), `/proc/meminfo`, `/proc/loadavg`, `/proc/uptime`, `/proc/net/dev`,
  `/proc/diskstats` (only whole disks from `/sys/block` with `device/`), and the temperature and
  fans from `/sys/class/hwmon` (chip `coretemp`, `k10temp`, `zenpower`, `cpu_thermal`…; if there
  is none, `/sys/class/thermal`); every 3 s, the disks' own sensors (see "Disk temperature and
  health").
- Processes (`/proc/<pid>/stat`, `cmdline`, and `exe` for the user's own) and connections: **once
  per second**, and only if the tab that shows them is open.
- The thread leaves the JSON ready; the screen thread only copies and converts it
  (`JS_ParseJSON`). It never reads files during a frame.
- **Zero cost when closed:** without `start()` the thread doesn't even exist; after `stop()` (or if
  nobody asks for a sample for 3 s, for example when switching themes) it sleeps on a condition
  variable, and releases the country database.

### Connections

`conn.c` does the same as `ss`: a `NETLINK_SOCK_DIAG` dump for TCP and UDP, IPv4 and IPv6, with
`INET_DIAG_INFO` (`tcpi_bytes_acked` and `tcpi_bytes_received` come from `tcp_info`). Since 0.8.5 it
also reads ICMP "ping", raw IP and packet sockets from `/proc/net/{icmp,icmp6,raw,raw6,packet}` (see
"Network" above). No root needed. For other users' sockets, only how many there are is counted.

**Bytes per app (0.8.1).** `tcp_info` only exists for TCP and only while the socket is open, so up
to 0.8.0 UDP — and with it QUIC: HTTP/3, a browser playing a video — counted nothing, and a closed
connection took its bytes with it (a browser streaming music showed 13 KB). With `lord-netperm`
running, the bytes and rates of each app and of each of its addresses come from the kernel instead:
its `cgroup_skb` programs count every packet of the app's sockets, TCP and UDP, both directions
(`FIREWALL.md`, "Traffic accounting"), and its `COUNTS` notice brings, every second, the app's
totals since its scope appeared, its rates over the last second and its 64 busiest addresses
(`lord.fw.poll()`, `firewallState.counts`). An address whose socket already closed keeps its bytes
and its country (remembered from the last open connection to it). Without `lord-netperm` (or with
`STATUS` `bytes: false`) the Monitor falls back to `tcp_info`.

The owning process of each of the user's sockets comes from `/proc/<pid>/fd` (`socket:[inode]`) of
the user's own processes. That walk is the most expensive part, so there is an inode → pid cache
that is only rebuilt when an unknown socket appears, and at most once per second.

Direction: `listen` (TCP listening or UDP without a destination), `in` (the local port is a
listening one) or `out`. Scope: `internet`, `lan` (private, CGNAT, link-local, ULA, multicast) or
`local`.

**From process to app** (`js_monitor.c`, on the screen thread with the list from `apps.c`):

0. (0.8.5) a socket made in one of the session's scopes is that scope's app (`cgApp`, from the
   socket's cgroup id; conn.c reads the session's scope ids every 5 s): Mail's connector, the
   Terminal's programs, an app's own sockets;
1. `lord-server` itself is **LordOS** (native apps and the Store run inside it);
2. otherwise, the `.desktop` whose `Exec` program (`DesktopApp.bin`) matches the executable,
   `argv[0]` or the process name;
3. otherwise, `StartupWMClass` equal to the process name;
4. otherwise, the process name.

### Country of an IP

- **Database:** DB-IP Lite Country (CC BY 4.0, <https://db-ip.com>), converted to the MaxMind DB
  format by `tools/gen-monitor-data.py`. To keep it small:
  - it only stores `country.iso_code`;
  - IPv4 blocks smaller than /24 and IPv6 blocks smaller than /32 go to the country with the most
    addresses inside;
  - it's compressed with xz.
- **Size:** 854 KB in the package (4.2 MB in memory). The DB-IP original is 8.3 MB.
- **Accuracy:** against the full DB-IP file, over 200,000 random addresses, it matches
  **99.96 %** for IPv4 and **99.78 %** for IPv6 (`build/mmdb-test <ours> <dbip.mmdb>`).
- **The reader** (`mmdb.c`) is our own:
  - every access goes through a bounds check;
  - a decoder pointer is followed only once;
  - depth is bounded;
  - a broken file gives "no data".
- **Test:** `build/mmdb-test` (in `make -C server test`) looks up known addresses and opens 400
  broken copies (truncated, with random bytes, fake metadata); during development it is also run
  with `-fsanitize=address,undefined`.
- **Loading:** the database is decompressed on the Monitor thread the first time connections with
  countries are requested. It is released when the app closes or when location is turned off.

No address is sent to any service: neither online geolocation nor reverse DNS.

### Map

The map is drawn as vectors, sharp at any zoom (no scaled PNG).

- **Data:** `assets/monitor/land.bin` (200 KB), from Natural Earth 1:50m, public domain.
  - The outlines are simplified (Douglas-Peucker, 0.02°) and in hundredths of a degree.
  - No Antarctica: the map goes from 84° N to 56° S.
  - `countries.js` has each country's label point (`LABEL_X`/`LABEL_Y`).
  - The country names in `assets/locales/{es,en,pt}/countries.json` come from
    `NAME_ES`/`NAME_EN`/`NAME_PT`.
- **Drawing** (`map.c`, on its own thread, equirectangular projection):
  - fill with an active edge table, 4 sub-scanlines per row and exact horizontal coverage
    (antialiasing), even-odd over all countries;
  - borders and a 30° grid (10° when zoomed) with Wu lines;
  - arcs and dots on top.

  Segments shorter than one pixel are skipped, and edges are sorted by sub-scanline with a
  counting sort, in O(n).
- **Without stalling:**
  - the screen thread requests a frame (`map`) and picks it up when it's ready (`mapFrame`),
    without waiting; if several requests arrive, only the last one is drawn;
  - meanwhile, the app shows the previous frame shifted, and scaled if the zoom changed, so
    dragging and zooming respond immediately;
  - labels are engine text, always sharp, and are placed without overlapping.
- **Memory:** after 20 s without requests, the thread releases the outlines and the buffers.

### Charts

`Chart.jsx` draws in pixels at the box's real width (`lord.getLayout`):

- gradient fill from 0.32 to 0.02 opacity and a 1.6 px line, as in TangoOS;
- guides at 25/50/75 %;
- axis with a "nice" scale (1, 2, 2.5 or 5 × 10ⁿ).

With `Uint32Array` it costs one pixel per write.

## Privacy

Settings → Privacy → **Monitor** (registered with `registerPrivacySection` if the page exists) has
four switches, saved in `settings.monitor`, and a button:

- `connections`: see each app's connections. When off, the thread doesn't query sock_diag.
- `geo`: place them on the map. When off, the country database isn't loaded.
- `history` (0.8.2, on by default): keep the traffic history (bytes per minute and per hour, never
  addresses; see "Traffic history"). **Clear History** deletes it.
- `menuItem` (0.8.5, off by default): the network activity item in the menu bar (see "The menu bar
  network item").

## Phase 2

**Done (firewall, see `FIREWALL.md`):**

- The **Rule** column is a per-app switch (`lord.fw.setAppRule`). `lord-netperm` applies it in the
  kernel and a denied app gets EPERM.
- The **allowed** and **denied** counters in the summary and for each app come from the kernel.
- An app with no open connections but with denied attempts still shows up in the list.
- Each process is attributed to the scope of the app it runs in (`/proc/<pid>/cgroup`).
- **Per-destination rules:** "Allow / Block this domain" on an app's destination writes a rule for
  that app (domain, `*.domain`, address or network, optional port and protocol), and the app's
  rules are listed and edited in Settings → Privacy → Network rules by app. Domains match the names
  the system resolver returned; an app with its own encrypted DNS is held only by address rules.
  Screenshots: [light](firewall-domain-rules-monitor-light.png),
  [dark](firewall-domain-rules-monitor-dark.png).
- **Host names** without asking anyone (0.8.2): `lord-netperm` learns them from the system resolver
  and its notices carry them (`hosts`); the destinations are listed by those names.
- (0.8.2) Each destination's own switch and the traffic history; (0.8.3) blocking cuts the open
  connections; (0.8.5) the tree, the "Deny silently" mode, the blocklists and LordOS's own components
  as rows of their own (see "Network, second generation").

**Planned:**


- **Connection alerts** (the "ask" mode, `FIREWALL.md`) and rules per country.
- **Devices:** `monitor.deviceUsers()` → `[{ kind: 'camera' | 'microphone', app, name, pid }]` and
  the `monitorDevices` store (already exported, empty).
- Usage alerts (the history is in place since 0.8.2).

## Measurements

VM 123 (4 vCPU Xeon Silver 4108 at 1.8 GHz, `virtio_gpu` without 3D, CPU compositing, 1280×800).

### lord-server CPU

Percentage of one core, 30 s average (`~/thr.sh`, per thread, from `/proc/<pid>/task`). During the
measurement there were 13 downloads in progress and Firefox open on the Network tab.

| Situation | Main thread | Sampling thread | Map thread |
|---|---|---|---|
| Monitor closed | 0.4–0.5 % | (doesn't exist / asleep) | (doesn't exist / asleep) |
| Overview, in front (4 charts at 4 Hz) | 10.4 % | 0.1–0.2 % | — |
| Overview, in the background (drawn at 1 Hz) | 4.7 % | 0.2 % | — |
| Processes, in front | 5.6 % | 0.7 % | — |
| Network, map still | 5.1 % | 0.5 % | 0 % |
| Network, dragging the map nonstop | 20.6 % | 0.6 % | 4.0 % |
| Monitor closed again | 0.4 % | 0 % | 0 % |

- **Closed costs nothing:**
  - without `start()` the sampling thread doesn't exist;
  - afterwards, it sleeps on a condition variable;
  - the map thread releases everything after 20 s.
- **Sampling:** reading `/proc` and `/sys` four times per second costs less than 1 %.
- **Drawing the interface:** this is what costs.
  - Each tab subscribes only to what it shows and at its own rate: charts at 4 Hz with the window
    in front and 1 Hz behind; Processes and Network at 1 Hz.
  - Before splitting it this way, Processes cost 17 % and Network 14 %.
  - In Overview, most of it is repainting the box that holds the four charts (the engine uses a
    single damaged region per frame, about 12 ms) and drawing the charts in JS (around 1.5 %).

### 0.8.1: traffic accounting, chart and moving dots (VM 118)

VM 118: 4 vCPU Xeon Silver 4108 at 1.8 GHz, `virtio_gpu` without 3D, CPU compositing, 1280×800.
Network tab in front, five downloads running (one over HTTP/3), arcs to 4–6 countries.
`lord-server` (whole process, % of one core), 3 × 30 s each:

| Build | Runs | Mean |
|---|---|---|
| 0.8.0 theme (bars redrawn once per second, no animation) | 5.2 / 6.0 / 8.2 % | 6.5 % |
| 0.8.1, Reduce motion (chart at 4 Hz, still markers) | 8.9 / 9.4 % | 9.2 % |
| 0.8.1, dots moving at 12 frames per second | 11.7 / 12.7 / 13.1 % | 12.5 % |
| 0.8.1, window minimized | 2.0 % | |

- **The chart** (redrawn 4 times per second with the window in front) costs about 2.5 points; behind
  other windows it drops to once per second.
- **The moving dots** cost about 0.3 points per frame per second (5 fps: 10.1 %, 15 fps:
  13.6 %): each frame repaints the box around all the dots, which with arcs across the world is
  most of the map. 12 fps is the compromise. (0.8.2: several damaged rectangles, see below.)
- An `lord.setStyle` that only moves an absolutely positioned node (the dots, the map image while
  panning) now just places it again instead of remeasuring every ancestor (`js.c`, like a window
  drag); it made no measurable difference here because painting dominates.
- **`lord-netperm`** stays at 0.0–0.07 % with traffic flowing (one map read per app per second,
  plus one batch read of the address table only while bytes move).
- **The kernel side** of the accounting is in `FIREWALL.md` ("Measurements").

### 0.8.2: several damaged rectangles (VM 119)

**Off by default in the 0.8.2 release.** During the release integration test, a Terminal window
minimized over the animated map once (out of five tries) left a grey rectangle on the screen until
the window came back. The cause was not found in time: `test_minimize_damage` in
`build/engine-test` (a window remounted with a shrinking, fading transform towards a blurred Dock
and then removed, over moving dots, every frame compared with a full repaint) passes, so it is not
in the scene's painting as the test models it. Until it is found, lord-server repaints one box per
frame, exactly as in 0.8.1; `LORD_DAMAGE=multi` in its environment turns on the several-rectangles
path described here (with the clipped-subtree skip and the per-rectangle present, which follow the
same switch). The measurements below are with it on. The 9×9 dot images and the map frame's opaque
color (below, "The Monitor's side") stay on.

**0.8.6: the search for the grey rectangle (still off by default).** Two new `build/engine-test`
suites compare every frame with a full repaint that bypasses the transformed subtrees' raster cache
(`paint_tf_nocache`): `test_damage_pipeline` runs the reported scene (Monitor's moving map, a
translucent, blurred Terminal minimized into a blurred Dock with the default theme's frame and
`fxStyle` transform, a blurred menu bar, menus, title changes, the cursor) through a copy of
`main.c`'s bookkeeping (each frame's rectangles gathered while a flip is pending, `paint_out`'s
parts, `present_cpu`'s per-buffer lists and copy, the cursor), with 0, 30 and 60 % of the flips
pending; it also checks every presented buffer against the scene. `test_damage_fuzz` does the same
with random trees and random changes. The pipeline's double-buffer and cursor bookkeeping was
right in every frame. The fuzz found seven engine bugs, all in **both** damage modes (one box or
several), each able to leave a stale area until something repainted it, and all are fixed
(CHANGELOG, 0.8.6): a parent's border lost where an opaque child started a partial repaint; cached
rasters of transformed subtrees kept after a change made while hidden, unscaled or inside a nested
transform; a raster grid that moved with the subtree's extent; a raster changing kind (a blur
coming in) outside the damage; blurs taking in what is painted above them from outside the
repainted area; and several frames' rectangles cutting through a blurred node. Which one the 0.8.2
report was is not proven (the reported scene itself never failed), so `LORD_DAMAGE=multi` is still
needed to turn the several-rectangles path on. The margin of a blurred surface is now repainted
with it: in `test_multi_damage` the pixels repainted per frame went from 24,078 to 27,363.

**The 0.8.2 integration test, repeated (VM 116, 2026-10-29).** An isolated headless session of the
real default theme (own home, runtime folder and bus; 1280×800, CPU composition), driven over its
remote desktop socket. The viewer subscribes to the video with lossless tiles and keeps its own copy
of the screen from them; for the test, the VM copy of `mirror.c` sends the changed tiles every frame
instead of only once the screen is still (`LORD_TEST_TILES`, never committed), so the copy is exactly
what the partial repaints left. At each check a second connection makes the session repaint the whole
screen (`cb.refresh`), and the copy before is compared with the copy after; pixels that changed within
2.2 s before or after are left out as animation. The scene: Monitor → Network with ten slow downloads
to mirrors in ten countries, a Terminal at 30 % transparency with blur over the map, minimized into the
Dock and restored 22 times while the dots move, then its six menus; the downloads pause for each check
(2.5 s, or 7 s for the still runs) so the map stops. 54 checks per run, six runs:

| Build | Mode | Scene at the check | Differing pixels outside Monitor's live numbers |
|---|---|---|---|
| previous `main` | one box | still 7 s | 3 (one pixel each, the Dock's corner) |
| this commit | one box | still 7 s | **0** |
| previous `main` | one box | still 2.5 s | 1 |
| this commit | one box | still 2.5 s | 389 (an arc fading under the blurred Terminal, ≤ 3 levels; 2 pixels on the Dock's corner) |
| previous `main` | several (`multi`) | still 2.5 s | 6 (the Dock's corner) |
| this commit | several (`multi`) | still 2.5 s | 505 (the arc, ≤ 3 levels; 7 on the Dock's corner) |

No stale area in any run. The Dock's corner pixels (1 to 7, antialiased) change with Monitor's
"Load" line, which the Dock's blur takes in; the arc is a connection whose line faded between the
last partial frame and the full one; both are the scene changing between the two copies, and neither
appears when the scene is still for 7 s. The grey rectangle did not appear in either build, so the
0.8.2 report is still not reproduced, and several rectangles stay off by default.

Cost, the same sessions with one viewer (`lord-server` whole process, % of one core, 5 × 30 s, two
batches, the build alternated):

| Scene | Mode | Before | After |
|---|---|---|---|
| Monitor → Network, dots moving | one box | 56.7 / 57.2 % | 58.7 / 59.7 % |
| Monitor → Network, dots moving | several | 59.2 / 58.1 % | 57.2 / 58.0 % |
| the same with the blurred Terminal over the map | one box | 35.8 / 36.4 % | 38.0 / 37.7 % |
| the same with the blurred Terminal over the map | several | 36.7 / 36.6 % | 37.5 / 35.9 % |

Layout and paint per frame (`LORD_FRAME_STATS`) were the same within 0.1 ms in every row (14.6 to
14.8 ms). `--profile` shows why: in these sessions every frame repainted one box of 64 to 79 % of the
screen, also with `LORD_DAMAGE=multi`, so the blur margin adds next to nothing. The reason, found
afterwards (next section): those sessions had a translucent, blurred Terminal hidden behind Monitor,
and the blur rule repainted it whole every frame. Most of the process's CPU here is the remote
desktop's frame comparison and the session itself, not paint. LordBench's desktop mode in the headless harness (`perf-theme.js`, three runs each):
58.9–60.0 fps, build p50 1.25–1.26 ms, p99 5.9–7.6 ms before; 60.0 fps, p50 1.24–1.27 ms, p99
5.95–6.32 ms after.

**A blurred window hidden behind Monitor (0.8.6, VM 116).** Why the repeated scene gave one box of
64 to 79 % per frame where VM 119 gave 5 to 11 small rectangles: the harness left the Terminal's
session holder (`lord-term-holder`) of an earlier run alive (it killed holders with a pattern that
never matched the holder's process name), so each new session restored a Terminal window at 30 %
transparency with blur, and Monitor opened over it. A debug build printing `multi_rects`' steps
showed three blurred nodes every frame: the menu bar, the Dock and the hidden Terminal's body
(858×437 at 211,201, opacity 1, fully under Monitor's opaque window). The dots' rectangles all
touched that body's blur margin, so the blur rule joined them into the body and its margin, 918×497
(44.6 % of the screen); when the Terminal sat lower, its margin met the Dock's and the Dock joined
too (62 to 73 %; with the Terminal in front of the map, 75 to 80 %, which is the rule itself). The
one-box mode grew its box the same way. A fresh session with no Terminal gave the expected 5 to 10
rectangles under 1 % of the screen.

The blur rule now leaves out a blurred node that an opaque node painted after it covers whole
(`blur_hidden` in `paint.c`, the opaque-cover skip's test in paint order; CPU path without a dragged
window). The same scene, Terminal opened first and Monitor → Network over it, ten downloads, one
remote viewer, `--profile` over the last 150 frames, and `lord-server`'s CPU (whole process, % of
one core, 5 × 30 s, two batches):

| Mode | Repainted per frame | Paint per frame | CPU before | CPU after |
|---|---|---|---|---|
| one box | 46.4 → 14.5 % | 5.6 → 2.0 ms | 24.3 / 22.3 % | 13.2 / 15.3 % |
| several (`multi`) | 46.2 % in 1 → 3.8 % in 5.6 rectangles | 5.2 → 1.7 ms | 21.5 / 21.7 % | 15.0 / 14.3 % |

Without a hidden blurred window nothing changes (Monitor → Network alone, one box: 10.2 and 13.8 %
of the screen per frame, the live charts). Visual check with the same tools, both modes: the 0.8.2
scenario (54 checks) and a new one where the Terminal is hidden behind the moving map, raised over it
from the Dock and sent back behind it ten times (21 checks): no stale area, only Monitor's live
numbers and charts and the clocks changing between the partial and the full copy.
`build/engine-test` `test_hidden_blur_damage` covers it (three of its checks fail without the change).

VM 119: 4 vCPU Xeon Silver 4108 at 1.8 GHz, `virtio` GPU without 3D, CPU compositing, 1280×800.
Network tab in front, ten slow downloads to mirrors in ten countries (arcs to 8–11 countries, about
20 moving dots). `lord-server` (whole process, % of one core), 5 × 30 s each, the same session
restarted with each build:

| Build | Dots moving | Reduce motion (still) | Animation's cost |
|---|---|---|---|
| before (0.8.1 engine and Monitor, `origin/main`) | 11.3 / 11.5 / 10.8 / 10.7 / 10.7 = **11.0 %** | 7.9 / 8.1 / 8.0 / 8.5 / 8.0 = **8.1 %** | **2.9 points** |
| after (this branch) | 10.1 / 9.2 / 8.8 / 9.2 / 9.8 = **9.4 %** | 7.8 / 7.9 / 7.9 / 8.1 / 7.6 = **7.9 %** | **1.6 points** (−46 %) |

What changed, and what each step bought (main thread, instrumented builds, per second of the dots
moving, about 14 frames per second):

| Step | Repainted per frame | Paint | Render (layout, damage, paint, present) |
|---|---|---|---|
| one damaged box (before) | 164,000 px, 1 rectangle | 25 ms/s | 33 ms/s |
| several rectangles (`dlist_add`, joined when the union wastes < 6,144 px) | 41,000 px, 5 | 21 ms/s | 29 ms/s |
| + clipped subtrees outside the rectangle skipped, cover search pruned | 45,000 px, 5 | 13 ms/s | 21 ms/s |
| + joined only below 1,024 px of waste | 30,000 px, 10 | 11 ms/s | 19 ms/s |
| + only the rectangles copied to the screen (`present_cpu`) | 26,000 px, 11 | 11 ms/s | 16 ms/s |

- **Several rectangles** (`ui.dlist`, `paint_damage_rects`, `paint.c` "Several damaged
  rectangles"): `damage_add` keeps the changes as up to 16 non-overlapping rectangles besides
  the box around them; `paint_damage` joins pairs whose union wastes less than `DAMAGE_PASS_PX`
  (1,024 px, measured: 256 to 4,096 px all cost within 10 %), falls back to one box when they
  cover 70 % of it, and `paint_out` paints each one (clear, `paint_root` clipped to it). Only on
  the CPU path and without a window being dragged; only with `LORD_DAMAGE=multi` (off by default
  in 0.8.2, see above).
- **Blurred surfaces** keep the 0.8.1 rule ("Paint: repaint a blurred surface whole"): a rectangle
  that touches a blurred node or the margin its blur samples grows to the whole node, and every
  rectangle touching that margin joins it, so the margin is either repainted in the same pass or
  not at all this frame (`docs/LAYERS.md`).
- **Each pass is cheaper:** `paint_node` skips a node that clips its children (`overflow: hidden`)
  when it lies outside the clip (CPU path, not through transforms), and the opaque-cover search
  stops at such a node when it doesn't hold the region and keeps away from rounded corners.
- **Present:** the CPU path copies the painted rectangles (plus what the other buffer missed and
  the cursor) into the display's buffer instead of the box around them; Night Light tints the same
  rectangles.
- **The Monitor's side:** the dots are 9×9 images drawn once, and the map's frame has the card's
  own opaque color, so a repaint starts there instead of at the card.
- **What is left** of the animation's cost is mostly per frame and not per pixel: the tree walks of
  each pass (about 45 µs per rectangle), layout and damage walks, and the frame itself.
- **Tests:** `build/engine-test` `test_multi_damage`: 40 frames of 24 translucent round dots moving
  inside a clipped, rounded "map" under a translucent blurred bar (dots crossing its blur margin, a
  menu opening and closing), each frame painted rectangle by rectangle and compared with a full
  repaint: identical (at most 1 level, as `test_backdrop_band`); no rectangle outside the box or
  overlapping another; one box when it is off; a full repaint set directly stays one box.
  With the blur rule turned off, 15 checks fail. `perf` was installed on VM 119 to find where the
  time went (`VM-SETUP.md`).

### Map

Time for an 800×560 frame on the map thread (`build/map-bench`):

| View | Average | Worst case |
|---|---|---|
| Whole world | 13.0 ms | 16.0 ms |
| Europe zoomed in | 6.7 ms | 8.1 ms |
| South America zoomed in | 4.4 ms | 5.2 ms |

It runs outside the frame. While it draws, the screen shows the previous frame shifted.

### Country by IP

- **First load:** the database (854 KB → 4.2 MB) is decompressed only once, on the sampling
  thread, outside the frame.
  - The whole `build/mmdb-test`, which includes that load and 400 broken files, takes 0.5 s in the
    VM.
- **Lookup:** each one walks at most 128 nodes.

## Screenshots (0.8.2, VM 119, English)

- An app's destinations with their switches, one blocked, and the 24-hour history:
  ![dark](monitor-network-082-dark.png) ![light](monitor-network-082-light.png)
- Settings → Privacy → Monitor with "Keep a traffic history" and "Clear History":
  ![privacy](monitor-network-082-privacy.png)

## Screenshots (0.8.1, process actions, VM 118)

- The Stop and Get Info buttons with a row selected: ![light](monitor-process-toolbar-light.png)
  ![dark](monitor-process-toolbar-dark.png)
- The right-click menu: ![light](monitor-process-menu-light.png) ![dark](monitor-process-menu-dark.png)
- The stop sheet: ![light](monitor-process-stop-light.png) ![dark](monitor-process-stop-dark.png)
- Get Info of a download (curl), Memory, Statistics and Open files and ports:
  ![memory](monitor-process-info-memory-light.png) ![statistics](monitor-process-info-stats-light.png)
  ![files](monitor-process-info-files-light.png); dark: ![memory](monitor-process-info-memory-dark.png)
  ![statistics](monitor-process-info-stats-dark.png) ![files](monitor-process-info-files-dark.png)
- A root process as an administrator (read through `proc-info`): ![root](monitor-process-info-root.png)
- A non-administrator trying to stop another user's process: ![refused](monitor-process-needadmin.png)
- Right to left (Arabic): ![rtl](monitor-process-rtl.png)

## Screenshots (0.8.1, VM 118, real traffic: a browser on a video site over QUIC and five downloads)

- Network, dark and light: ![dark](monitor-network-dark.png) ![light](monitor-network-light.png)
- The browser chosen (detail panel, its own chart): ![dark](monitor-network-detail-dark.png)
  ![light](monitor-network-detail-light.png)
- The map with traffic moving (a still frame): ![map](monitor-network-map-flow.png); with Reduce
  motion: ![still](monitor-network-map-still.png)
- Minimum window size in Spanish (the longest values go on their own line):
  ![es](monitor-network-min-es.png); right to left (Arabic): ![rtl](monitor-network-rtl.png)
- Before the fix (0.8.0, Spanish): a 400 KB/s HTTP/3 download shows "—" and "0 B":
  ![before](monitor-network-before.png)

## Screenshots (VM 123)

- Overview: ![Overview](monitor-overview.png)
- CPU: ![CPU](monitor-cpu.png)
- Memory: ![Memory](monitor-memory.png)
- Disks: ![Disks](monitor-disks.png)
- Network, with curl expanded: ![Network](monitor-network.png)
- Firefox chosen: ![Chosen app](monitor-network-app.png)
- Map:
  - the world: ![Map](monitor-map-world.png)
  - zoomed in on Europe: ![Europe](monitor-map-europe.png)
  - zoomed in on South America: ![South America](monitor-map-south-america.png)
  - Germany chosen: ![Chosen place](monitor-map-selection.png)
- Settings → Privacy → Monitor: ![Privacy](monitor-privacy.png)
- In Arabic (RTL): ![RTL](monitor-rtl.png)

## Pending

- **Privacy link:** with the window's default size (1180×700 on 1280×800), the "Monitor privacy
  settings" link in the map footer falls in the Dock area, which takes the click. It can still be
  reached through Settings → Privacy.
- **Arabic and other languages:** the Monitor's texts exist in es/en/pt. In Arabic the window
  mirrors correctly, but the texts show in English until the language pack translates them.
- **Sensors:** the VM has no CPU `hwmon` or fans, so temperature and fan show "—". Still needs
  testing on real hardware (the code looks for `coretemp`, `k10temp`, `zenpower`, `cpu_thermal`
  and `pwmfan`).
- **Disk temperatures on real hardware:** the code paths and the sysfs layouts are tested on fake
  trees (`build/disktemp-test`), and on VM 119 every disk correctly says "No sensor" (QEMU's SATA disk
  has neither SCT nor attribute 194, so `drivetemp` does not bind to it). A real NVMe drive (a PC, or
  a Pi 5 with an M.2 HAT) and a real SATA disk with `drivetemp` loaded are still to be checked, and
  with them the "too hot" notice.
- **Phase 2:** what's in the "Phase 2" section.

## Attributions

- **Countries by IP:** DB-IP Lite Country © db-ip.com, CC BY 4.0 license
  (<https://creativecommons.org/licenses/by/4.0/>).
  - The attribution is shown in About LordOS, at the foot of the map and in
    `/usr/share/doc/lordos/copyright`.
  - Source used: `dbip-country-lite-2026-10`.
- **Country outlines and names:** Natural Earth (<https://www.naturalearthdata.com>), public
  domain.
