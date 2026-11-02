# Mail, Calendar, Contacts and Reminders (M9)

LordOS ships four native apps for personal information: **Mail** (a three-pane client in the
spirit of classic desktop mail clients), **Calendar**, **Contacts** and **Reminders**. They are
ordinary LordOS apps (React on our engine, `apps/mail`, `apps/calendar`, `apps/contacts`,
`apps/reminders`). All the network protocols and every parser of data that comes from outside
run in one sandboxed helper process, **`lord-maild`**.

| Piece | Status |
|---|---|
| `lord-maild`: IMAP (IDLE, CONDSTORE, MOVE, UIDPLUS, SPECIAL-USE), SMTP submission (465 / 587 STARTTLS), CalDAV and CardDAV | **done**, tested against Dovecot 2.4, Postfix and Radicale 3.5 (VM 123) |
| Sandbox: shared helper sandbox (`sandbox_apply_helper`), **no sockets at all**: connections come from `lord-server`, only to configured servers | **done** |
| TLS with OpenSSL, verification against the system CA store and the host name; bad certificates shown, never accepted silently | **done** (local CA in the VM; a mismatched certificate shows the warning) |
| Local cache (Maildir-like) and offline index; 10,000-message folder opens from the cache | **done** (list of 10,000: ~20 ms per window, see Measurements) |
| Conversations (References / In-Reply-To / subject), unified inbox, flags, 6 color categories | **done** |
| HTML mail rendered by our own sanitizer into native views; remote images blocked; tracking pixels removed; deceptive links flagged | **done** (sanitizer fuzzed, 30,953 checks) |
| Attachments (open with the app Files would use, save to Downloads), inline `cid:` and `data:` images | **done** |
| Compose: rich text (markdown marks + live preview), markdown, plain text; signatures; drafts on the server; send later | **done** |
| Invitations (.ics in mail): Accept / Maybe / Decline, event added to Calendar, iTIP REPLY sent to the organizer | **done** (REPLY delivered to the organizer's mailbox) |
| Calendar: day / week / month, several calendars with colors, local and CalDAV, recurrence, alarms | **done** |
| Reminders: lists, due dates, priorities, flags, Today / Scheduled / All / Flagged / Completed, VTODO over CalDAV | **done** |
| Contacts: local and CardDAV, groups, photo, the usual vCard fields; Mail autocompletes from it | **done** |
| Alarms of events and to-dos fire as notifications with the apps closed | **done** (seen in the VM with every app closed) |
| Accounts: any IMAP/SMTP; built-in settings for popular providers (Gmail, Yahoo and others); app passwords | **done** (built-in providers not tested: no real accounts were used) |
| OAuth2 (XOAUTH2) for Gmail and other large providers | **prepared, disabled**: needs your client ID (see [OAuth](#oauth-needs-your-client-id)) |

![Mail: conversation](mail-threads.png)

## Processes and trust

```
 lord-server --app mail | calendar | contacts | reminders      (0.8.2: each its own sandboxed process,
   apps/<id> (React), sdk/mail.js, calendar.js, contacts.js     no network, no access to the cache)
                 │ lord.mail.request / poll (ui/js_brokermail.c) over the broker: BOP_MAIL
                 ▼
 lord-server ── server/src/sec/brokermail.c (per-app allow list, fds from the system dialog only)
             ── server/src/ui/js_mail.c (the session's lord.mail: notifications, alarms, need-auth)
             └─ server/src/mail/mailc.c: starts lord-maild, its channel thread, the CONNECTOR
                 │ socketpair (fd 3, same framing as lord-doc: header + JSON + SCM_RIGHTS)
 lord-maild  ── sandboxed: TLS, IMAP, SMTP, HTTP/WebDAV, MIME, HTML, iCalendar, vCard, images
```

- **The windows run in their own process (0.8.2).** Mail, Calendar, Contacts and Reminders are
  system packages that run as `lord-server --app <id>` with Landlock, seccomp, no network and their
  own cgroup scope; they reach lord-maild only through `lord-server` (`BOP_MAIL`), each with its own
  allow list, its own replies and images, and files only as fds picked in the system dialog. The
  session keeps new-mail notifications, alarms and passwords on demand. See
  [PRIVACY.md, "System apps in their own process"](PRIVACY.md#system-apps-in-their-own-process-082).
- **lord-maild** is started by `lord-server` with a fixed path (`<lord-server dir>/lord-maild`,
  owned by root or the user, not writable by others), no arguments, a minimal environment and the
  channel on fd 3. Before reading anything from the network it loads the CA bundle and the time
  zone, receives the cache folder as an open fd, and locks itself in with
  `sandbox_apply_helper()` (`server/src/sec/sandbox.c`): Landlock (read-only `/usr`, the
  zoneinfo; write access only below the cache folder), seccomp without `socket`, `connect`,
  `bind`, `execve` or `fork` (threads only, TSYNC), no capabilities, cgroup scope
  `lordos-maild.scope`. It dies with `lord-server` (`PR_SET_PDEATHSIG`) and is not dumpable.
- **No socket of its own.** `lord-maild` cannot create sockets. To reach a server it sends
  `MR_CONNECT {host, port}`; `lord-server` (`mailc.c`) checks the endpoint against the accounts
  file and, if allowed, resolves it, connects (10 s timeout) on a thread of its own and passes the
  connected socket back (`MQ_SOCK`). (0.8.5) The socket itself is made and connected by Mail's connector
  (`lord-server --net-connector mail`, in `lordos-net-mail.scope`), so Mail and the calendar sync
  are their own app for the firewall and Monitor → Network ("Mail and calendars", `lordos:mail`;
  `FIREWALL.md`, "LordOS components in their own scopes"); the endpoint check above is unchanged. The allowed endpoints are exactly: each account's IMAP and
  SMTP `host:port`, each DAV URL's `host:port`, and — for CalDAV/CardDAV discovery redirects inside
  the same provider (for example `pNN-caldav.icloud.com`) — other hosts of the same domain on port 443.
  So a malicious message that takes over `lord-maild` still cannot talk to anything else.
- **Remote images** are the only other endpoints, and only after the user presses *Load Images*
  (or trusts the sender): `lord.mail.allowImages(urls)` grants those `http(s)` hosts on ports 80/443
  for ten minutes, and the connector then refuses every resolved address that is not public
  (loopback, RFC 1918, CGNAT, link-local and cloud metadata, multicast, reserved, ULA, and the same
  ranges behind v4-mapped, NAT64 and 6to4 addresses), checked on the address it actually connects
  to (no DNS rebinding). Redirects (at most 3) go through the same check; each image is capped at
  5 MB and 20 s. Tested by `build/mailc-test`.
- **OpenSSL** is linked only by `lord-maild`, `lord-sessiond` and, since 0.8.5, `lord-fetch` (the blocklist
  downloader) and `lord-tls` (the package engine's HTTPS); see `ARCHITECTURE.md`.
  `lord-server` never links it.
- **The compositor never waits**: requests are asynchronous (`lord.mail.poll()`), and inside
  `lord-maild` network work runs on account threads, never on the thread that reads the channel.
- **Images are decoded in the sandbox** (stb_image in `lord-maild`, PNG/JPEG/GIF/BMP) and come
  back as sealed memfds of ARGB pixels, like `lord-doc`'s; `lord-server` only copies pixels.

### Passwords and the keyring

Passwords never touch a file. When you add an account, Mail stores the password in the system
keyring (Passwords, `keyring.set('mail', { service: 'mail:<id>', account: user })`, see
`docs/PASSWORDS.md`) and hands it to `lord-maild`, which keeps it in memory only. When
`lord-maild` starts and needs it, it asks (`need-auth` event) and the SDK reads it from the
keyring. If the keyring is locked or there is none (a session without a login password, such as
the test VM's autologin), the account works for the session and Mail shows ⚠ next to it to type
the password again.

The accounts file `~/.config/lordos/mail/accounts.json` holds servers, user names, signatures and
**trusted certificate fingerprints**, never passwords (`saveConfig` refuses a `password` field).
Because it decides which servers `lord-maild` may reach, `lord.fs` cannot write it
(`fsops_protected`): only `lord.mail.saveConfig` can, after validating it. The cache
`~/.local/share/lordos/mail` is protected the same way.

### Certificates

Every TLS connection is verified against the system CA store (loaded once, before the sandbox)
and the server's host name (`SSL_set1_host`). A certificate that does not verify is **never**
accepted silently: the handshake fails and Mail shows a warning with the problem, subject, issuer,
expiry and SHA-256 fingerprint. Only if the user then presses *Trust This Certificate* is that
exact fingerprint stored for that account (and for that server role: IMAP, SMTP or DAV); any
other certificate still fails.

## Mail

### Layout

Three panes: favorites (unified inbox, unread, flagged), accounts with their folders (special
folders named in your language and ordered: Inbox, Drafts, Sent, Archive, Junk, Trash), and the
color categories | the message list with sender, subject, two lines of preview, date, unread dot,
thread count, attachment and flag marks | the reading pane, with the whole conversation (also the
replies you sent, from Sent) — earlier messages collapsed, the selected one open. The toolbar has
New, Reply, Reply All, Forward, Archive, Delete, Flag and Category; search sits above the list.
Keyboard: ❖N new, ❖R reply, ❖⇧R reply all, ❖J forward, ❖E archive, ⌦ delete, ↑/↓ next message.

### Sync

Per account, one worker thread: connect (implicit TLS on 993 or STARTTLS on 143; plaintext is
refused), `AUTHENTICATE PLAIN` (or `LOGIN`), `ENABLE CONDSTORE`, `LIST … RETURN (SPECIAL-USE)`,
then each folder, inbox first:

1. `SELECT` with `CONDSTORE`: UIDVALIDITY, UIDNEXT, HIGHESTMODSEQ. A new UIDVALIDITY clears that
   folder's cache. If nothing changed, nothing else is sent.
2. `UID SEARCH ALL` → messages gone from the server leave the cache.
3. New messages, **newest first**, in chunks of 250: flags, size, the header fields shown in the
   list and the first 2 KB of the body for the preview. The list updates after each chunk, and a
   waiting request (opening a message) goes ahead of the rest.
4. Flags of the cached messages: `CHANGEDSINCE` with CONDSTORE, otherwise all flags (or the last
   2,000 for very large folders).
5. The bodies of the 40 newest inbox messages are downloaded for offline reading and search; any
   other body is downloaded when opened.

Then the worker waits in **IDLE** on the inbox (refreshed every 25 minutes; without IDLE it polls
every 5 minutes). A new message interrupts the IDLE, the inbox syncs, and new unread messages
become a notification ("2 new messages", with Open). UI requests (open, flags, move, delete,
drafts) interrupt the IDLE through an eventfd. Every 15 minutes all folders sync again. Gmail's
"All Mail" is not synced in the background.

Flags map to IMAP: `\Seen`, `\Flagged`, `\Answered`, `$Forwarded`, `$Junk`, and the six
categories to `$label1`…`$label6` (the keywords Thunderbird uses for its first five tags), so
other clients see them. Archive moves to the `\Archive` folder (created as "Archive" if missing);
Delete moves to Trash (or expunges inside Trash). `UID MOVE` when available, else COPY + STORE
`\Deleted` + `UID EXPUNGE`.

### Conversations

A message's conversation key is the first Message-ID of `References` (the thread root), else
`In-Reply-To`, else its own id; a reply without those headers falls back to its subject without
"Re:/Fwd:/RV:/AW:…". The list groups by that key (per account), shows the newest message with the
count, and the reading pane shows the whole conversation from every folder except Trash and Junk.

### Local cache and offline search

```
~/.local/share/lordos/mail/
  <account>/folders              folder list
  <account>/f/<hash>/state       UIDVALIDITY UIDNEXT HIGHESTMODSEQ
  <account>/f/<hash>/index       one line per message (headers for the list, flags, preview)
  <account>/f/<hash>/m/<uid>     the raw message (one file each, Maildir-like)
  <account>/f/<hash>/t/<uid>     its text, for the search index
  att/…  img/…  outbox/…  pim/…  attachments opened, remote images loaded, "send later", calendars/contacts
```

Everything is in memory while `lord-maild` runs. The **search index** is an inverted index of
folded words (lower case, no accents): subject, people, preview and, for downloaded messages, the
body; every word of the query matches by prefix ("presu" finds "presupuesto"), and results come
sorted by date. It is rebuilt from the cache in milliseconds at start and never needs the network.

### HTML without executing anything

LordOS has no web engine (WebKit/Chromium are not allowed), and Mail does not need one. The
HTML part goes through **our sanitizer** (`server/src/mail/html.c`): a tolerant tokenizer that
builds a small tree in JSON — blocks, layout tables (rows, cells, widths, colors, padding,
alignment), text runs (bold, monospace, color, size, background, underline/strike), images and
links — and `apps/mail/HtmlView.jsx` draws it with View/Text/Image. Common newsletter markup
(nested tables for layout, inline styles, `bgcolor`, `align`, `<font>`, lists, quotes) renders
close to the intent; anything else degrades to text. Always dropped: `script`, `style`, `iframe`,
`object`, `embed`, `form` controls, `svg`, `math`, media, comments, `<!DOCTYPE>`, every `on*`
attribute, CSS `url()`/`expression()`, links that are not `http`, `https` or `mailto` (`javascript:`
with any spacing or entities included), relative/`file:` images, and everything under
`display:none`, `visibility:hidden`, `font-size:0` or the "hidden preheader" tricks. Messages with
their own colors are shown on their own light "paper" (also in dark mode), others follow the theme.

- **Remote images** are not requested when the message opens: a banner says how many were
  blocked, with *Load Images* (this message) and *Always from this sender* (stored in the accounts
  file). 1×1/2×2 or zero-sized images (tracking pixels) are counted and never shown.
- **Deceptive links**: when the visible text looks like an address (`www.bank.com`, `https://…`,
  `bank.com/login`) and its domain is not the link's, or the link has `user@` in it, an IP address
  or a punycode host the text does not, the link is drawn in red with ⚠, a banner counts them, and
  clicking shows the real destination. Links never open a browser (there is none in Mail): the
  address can be copied. `mailto:` opens a compose window.
- There is no browser fallback: if HTML is missing or empty, the plain text part is shown, with
  links clickable and quoted lines dimmed.

### Compose

To/Cc/Bcc fields with chips and autocomplete (Contacts first, then people you exchanged mail
with), subject, and the body in one of three formats: **rich text** (the formatting buttons —
bold, italic, strike, lists, quote, link, code — add markdown marks and a live preview shows the
result), **markdown**, or **plain text**. Rich text and markdown go out as `text/plain` plus a
simple `text/html` that Mail builds itself from the parsed markdown (nothing typed is passed
through as HTML). The editor is the Notes editor (monospaced). Signatures (markdown) per account
are added below the cursor. Replies quote the original; Forward brings its attachments.

- **Attachments**: the file sheet lists the home folder; `lord-server` opens the files (same
  rules as `lord.fs`) and passes the fds; `lord-maild` reads them (25 MB each, 35 MB per message).
- **Drafts** are saved to the server's Drafts folder (❖S, and every 30 s while there are changes;
  the previous version is replaced), and the draft is removed when the message is sent.
- **Send later**: in 1 hour, tomorrow at 8:00, Monday at 8:00. The message is composed at once and
  kept in `outbox/`; the sender thread sends it when it is due, with Mail closed too.
- Sent messages are appended to Sent unless the server files them itself (Gmail and some other large providers). The
  original gets `\Answered` or `$Forwarded`.

Attachment names chosen by the sender are reduced to a safe basename everywhere (RFC 2231/2047
decoded first, then: last path component, no leading dots, no control, bidi-override or
FAT-reserved characters, at most 200 bytes); saving also checks the final path is inside
Downloads.

### Accounts and settings by provider

*Add Account* asks for a name, the address and the password, then fills the servers from our own
table (`sdk/mail.js`, `PROVIDERS`, in the spirit of Mozilla's ISPDB; nothing is looked up online):

| Provider | IMAP | SMTP | Notes |
|---|---|---|---|
| Gmail | imap.gmail.com:993 SSL | smtp.gmail.com:465 SSL | needs an **app password** (2-Step Verification on); server files sent mail; CalDAV needs OAuth |
| Outlook.com / Hotmail / Live | outlook.office365.com:993 SSL | smtp-mail.outlook.com:587 STARTTLS | app password; the provider has been retiring password sign-in for IMAP: OAuth may be required |
| Yahoo | imap.mail.yahoo.com:993 SSL | smtp.mail.yahoo.com:465 SSL | app password |
| iCloud | imap.mail.me.com:993 SSL | smtp.mail.me.com:587 STARTTLS | app-specific password; also offers iCloud calendars and contacts (caldav.icloud.com, contacts.icloud.com) |
| anything else | imap.&lt;domain&gt;:993 | smtp.&lt;domain&gt;:465 | a guess; *Set the servers by hand* shows every field |

None of the four providers was tested here (no real accounts were used, by design); the settings
are the providers' published ones. A CalDAV/CardDAV account (Nextcloud, Radicale, Fastmail…)
is added with its URL; discovery follows `current-user-principal`, `calendar-home-set` /
`addressbook-home-set` and `/.well-known/caldav`.

### OAuth (needs your client ID)

Gmail and other large providers prefer OAuth2 (XOAUTH2). They only hand out tokens to
applications registered in their consoles, so this is **prepared but disabled**:

- `sdk/mail.js` → `OAUTH_PROVIDERS.google` / `.microsoft`: endpoints and scopes are filled in;
  `enabled: false`, `clientId: ''`. The setup sheet shows *Sign in with Gmail* greyed out with
  "Needs your client ID".
- `lord-maild` accepts `"auth": "oauth2"` in an account and answers `oauth-disabled`.

To enable it: register an OAuth client (Google Cloud Console → APIs & Services → Credentials,
type "Desktop app", scopes `https://mail.google.com/` and the calendar/contacts ones; or Azure →
App registrations, "Mobile and desktop applications", redirect `http://localhost`, delegated
permissions `IMAP.AccessAsUser.All`, `SMTP.Send`, `offline_access`), put the client ID in
`OAUTH_PROVIDERS`, and implement the remaining pieces: the authorization-code flow with PKCE
(a loopback redirect served by `lord-server`), storing the refresh token in the keyring, and
`AUTHENTICATE XOAUTH2` in `imap.c`/`smtp.c` (the place is marked `oauth-disabled`).

## Calendar, Reminders and Contacts

Kept by `lord-maild` too (`server/src/mail/pim.c`), so they work with every app closed and are
synced by the same sandboxed process. Local collections exist from the first run (a calendar, a
reminders list, an address book); CalDAV/CardDAV accounts add the server's collections.

- **Storage**: `pim/colls` (collections), `pim/<id>/meta` (file, UID, href, ETag, dirty) and one
  `.ics` / `.vcf` per item.
- **Sync** (every 15 minutes, at start, and a few seconds after a local change): discovery,
  `PROPFIND` of the homes (names, colors, supported components, CTag), local changes first
  (`PUT` with `If-Match` / `If-None-Match: *`, `DELETE` with `If-Match`; on `412` the server's
  version wins and is refetched; a change the server refuses for good — `400`, `403`, `415` — is
  dropped in favor of the server's copy, with a `dav-rejected` event), then the ETag list and a
  `calendar-multiget` / `addressbook-multiget` of what changed; deletions on the server delete
  locally.
- **iCalendar** (`ical.c`): VEVENT/VTODO/VALARM, DATE and DATE-TIME (UTC, `TZID` through the system
  tz database with our TZif reader, non-IANA zone names mapped, floating = local), `RRULE` with
  DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL, BYDAY (with ordinals ±1..53), BYMONTHDAY,
  BYMONTH; `EXDATE`; `RECURRENCE-ID` overrides. Events we write carry a `VTIMEZONE` built from
  the zone's own rule.
- **Invitations**: a `text/calendar` (or `.ics`) part shows a card with Accept / Maybe / Decline.
  The event goes to the calendar that already has it, else the first visible one, with your
  participation status, and an iTIP `REPLY` goes to the organizer (as a normal message, not kept
  in Sent). `REPLY` and `CANCEL` messages are shown as such.
- **Reminders**: VTODO with due date (all-day or with time), priority (high 1, medium 5, low 9),
  flag (`X-LORDOS-FLAGGED`), completion, subtasks (`RELATED-TO`); smart lists Today (due today or
  overdue), Scheduled, All, Flagged, Completed.
- **Contacts** (`vcard.c`): vCard 2.1/3.0/4.0 in (with quoted-printable and other charsets), 3.0
  out: name parts, nickname, organization and department, title, e-mails, phones, addresses, URLs,
  IM, birthday, notes, categories, photo (resized to 320 px JPEG in the sandbox) and groups
  (`X-ADDRESSBOOKSERVER-KIND:group` and vCard 4 `KIND`/`MEMBER`).

### Alarms with the apps closed

Like the Clock's alarms (`sdk/alarms.js`), they live in the SDK, not in an app window:
`startShell` calls `startReminders()`, which asks `lord-maild` for the alarms of the next 36 hours
(event `VALARM`s, and to-dos at their due time — 9:00 for all-day ones), sets one timer to the next
one (in pieces of at most a minute when it is close, because the monotonic clock stops during
suspend), rings a sticky notification (Snooze 10 min, *Mark as done* for to-dos, Open), and
remembers what rang in `settings.pimAlarms.fired`. An alarm missed by less than 10 minutes still
rings. It re-reads the list whenever calendar data changes.

## Programming interface

`lord.mail` (`server/src/ui/js_mail.c`): `available()`, `start()`, `request(json, [paths])` → id,
`poll()`, `pending()`, `config()`, `saveConfig(json)`, `allowImages(urls)`, `bind(node, image)`,
`release(image)`, `cacheDir()`. Every request is a JSON object with an `op`; files are picked by the
user and opened by `lord-server`.

SDK (`@lordos/sdk`):

- `sdk/mail.js`: `mailRequest(op, args, { files })`, `onMailEvent(fn)`, `mailConfig`,
  `mailAccounts.add/update/remove/setPassword/trustCertificate`, `mailStatus` (store),
  `autoconfig(email)`, `OAUTH_PROVIDERS`, `startMail()`.
- `sdk/calendar.js`: `calendars.list/create/update/remove`, `calendarEvents.range/save/remove/importFile`,
  `reminders.lists/items/save/remove/toggle`, `usePim()`, `usePimQuery()`, `startReminders()`.
- `sdk/contacts.js`: `addressBooks.*`, `contacts.list/save/remove/importFile/photo/autocomplete`,
  `contactName`, `contactInitials`, `<ContactPhoto>`.
- `sdk/filepicker.jsx`: `<FilePicker>` (choose files from the home folder in apps that run in the
  theme's context).

Operations of `lord-maild` (`server/src/mail/maild.c`): `status`, `auth`, `folders`, `counts`,
`list`, `thread`, `search`, `open`, `attachment`, `image`, `invite`, `invite-respond`, `flag`,
`move`, `delete`, `create-folder`, `send` (also `{test:true}` to check SMTP), `draft`, `sync`,
`autocomplete`, `alarms`, and `cal-*`, `ev-*`, `todo-*`, `book-*`, `contact-*`.

## Tests

`make -C server test` runs, among the rest:

- `build/mail-test` (30,953 checks): the IMAP response parser (literals, bracketed sections,
  modified UTF-7, UID sets), MIME (nested multipart, base64 / quoted-printable, RFC 2047 words,
  RFC 2231 continuations, address lists with groups and comments, dates, 25 charsets including
  KOI8-R, ISO-8859-15 and UTF-16), the HTML sanitizer against hostile input (script, event
  handlers, `javascript:` with tabs and entities, hidden text, tracking pixels, deceptive links,
  400 KB of nested tables, 400 KB of `<`), iCalendar (time zones across a DST change, RRULE with
  COUNT/EXDATE/BYDAY ordinals, non-IANA zone names, REPLY, a round trip; hostile RRULEs with
  `2147483647MO` and friends), vCard (2.1 QP, groups, photo round trip), the XML reader
  (namespaces, CDATA, DOCTYPE refused, depth limit), hostile attachment names — and for each
  parser a deterministic mutation fuzzer (thousands of mutated inputs).
- `build/mailnet-test` (138 checks, needs the OpenSSL headers): HTTP chunk sizes and
  Content-Length (overflow, garbage, negative), header limits, IMAP literals near `SIZE_MAX`,
  every literal size around the buffer-growth boundaries, endless lines — over a socketpair.
- `build/mailc-test` (69 checks): the connector's endpoint policy and the public-address check.
- `make -C server mail-asan`: the same suites under AddressSanitizer and UBSan.
- `build/maild-cli '{"op":"status"}' --wait 5` (not part of `test`) drives `lord-maild` from a
  terminal like `lord-server` does; with another `HOME` it uses another cache.

## Measurements (VM 123, 4 vCPU, local Dovecot)

| What | Result |
|---|---|
| First sync of a 10,000-message folder (headers + previews) | ~2.1 s, list usable after the first chunk (~0.1 s) |
| Opening that folder from the cache (`list`, 60 rows) | ~20 ms (the CLI polls every 20 ms: the real number is lower) |
| Any window of it (offset 5,000) | ~20 ms |
| Search over all accounts (`factura valentina`, prefix match) | ~20 ms |
| `lord-maild` start with the 10,000-message cache (index + search index) | ~0.1 s |
| `lord-maild` memory with that cache | ~25 MB RSS |
| Scrolling the 10,000-row list | only visible rows exist; rows load in windows of ~40 |

## Testing on the VM

Local servers only, inside VM 123 (`docs/VM-SETUP.md`, step 24): Dovecot (IMAP, users
`ana@lordos.test` and `beto@lordos.test`), Postfix (submission 587 STARTTLS and 465, delivery to
Dovecot over LMTP, nothing relayed outside), Radicale (CalDAV/CardDAV with TLS), all behind a
test CA added to the VM's CA store, and test data from scratch scripts. No real account was used.

## Not done / not tested

- The four built-in providers and Google CalDAV: settings
  from the providers' documentation, not tried. OAuth: prepared, disabled.
- Editing **one occurrence** of a repeating event (changes apply to the series; deleting one
  occurrence works). Editing a series rewrites it without its per-occurrence overrides.
- New calendars and address books are created locally (no `MKCALENDAR`/`MKCOL` on the server).
- The rich-text editor is markup with a live preview, not a WYSIWYG editor; the engine has no
  italic face (italic is shown with a tint) — the sent HTML is real `<i>`/`<b>`.
- No S/MIME or OpenPGP; no QRESYNC (expunges are found with `UID SEARCH ALL`); no POP3; no
  sieve/filters; no full-text search on the server (the local index covers what is cached).
- Charsets: the 8-bit families, UTF-16/32; CJK multibyte charsets (GB2312, Shift_JIS, EUC-KR,
  Big5, ISO-2022-JP) are shown as replacement characters unless they are valid UTF-8.
- Cache folders are emptied but not removed when an account goes away (the shared sandbox gives
  helpers no right to remove directories).

## Screenshots

| | |
|---|---|
| ![HTML blocked](mail-html-images-blocked.png) | ![HTML loaded](mail-html-images-loaded.png) |
| A newsletter: remote images blocked, one tracking pixel removed | After *Load Images* (decoded in the sandbox) |
| ![conversation](mail-threads.png) | ![compose](mail-compose.png) |
| A conversation, with the reply from Sent | Compose in rich text with live preview |
| ![invitation](mail-invitation.png) | ![deceptive link](mail-deceptive-link.png) |
| An invitation, accepted (REPLY sent to the organizer) | A deceptive link and its real destination |
| ![account](mail-account-setup-servers.png) | ![10,000](mail-10000-messages.png) |
| Account setup: Gmail found, app password, OAuth needs a client ID | A 10,000-message folder from the cache |
| ![week](calendar-week.png) | ![month](calendar-month.png) |
| Calendar, week (CalDAV and local) | Calendar, month |
| ![contacts](contacts.png) | ![reminders](reminders.png) |
| Contacts (CardDAV, photo decoded in the sandbox) | Reminders: Today |
| ![alarm](reminders-alert.png) | ![light](mail-light.png) |
| A reminder rings with every app closed | Mail in light mode |
| ![calendar light](calendar-light.png) | ![contacts light](contacts-light.png) |
| Calendar in light mode | Contacts in light mode |
| ![reminders light](reminders-light.png) | ![account 1](mail-account-setup.png) |
| Reminders in light mode | Add Account |
