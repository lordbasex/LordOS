LordOS has no accounts, no advertising and no tracking, on the website or in the distribution. Nobody sells or shares your data. The only personal data we keep is the IP address in the website's request logs, used for visit statistics and, since 12 November 2026, to count the LordOS computers that check for updates; only the LordOS team sees them. This policy has two parts: the **lordos.org website**, and the **LordOS distribution** you install on your computer.

## Part 1: the website

### What your browser keeps

The website is a set of static pages. It sets **no cookies** and keeps only two values in your browser's `localStorage`, on your device:

| Key | What it holds | Why |
|---|---|---|
| `lordos-theme` | `light` or `dark` (nothing when you use automatic) | to show the theme you chose |
| `lordos-lang-seen` | `1` | so the "this page is also in your language" suggestion appears only once |

They never leave your browser and we can't read them. Clear them at any time from your browser's site data settings. See the [Cookies and statistics](/legal/cookies-and-statistics/).

### No analytics services, no ads, no third-party code

The website loads no analytics service, no advertising, no social media widgets and no fonts or scripts from other servers: everything it needs is served from lordos.org.

### Server logs

The website is hosted on Amazon Web Services (S3 and CloudFront). As with any website, your browser sends your IP address and the usual request data (the page asked for, the time, the browser's user agent) to deliver the page. Standard CDN request logs may be kept by our hosting provider for security, retained for a limited time. We count visits from these logs to know how the project grows: page views, visitors, countries, systems (Windows, macOS, Linux…) and devices, seen only by the LordOS team, from logs that include your IP address and are deleted after 13 months. We don't use them to identify or profile visitors. The details are in [Cookies and statistics](/legal/cookies-and-statistics/).

### Downloads on GitHub

The ISO image, the Raspberry Pi image, the `.deb` package and their signatures are hosted on GitHub Releases. When you download them, or when the Debian installer script fetches them, GitHub receives your request under [GitHub's privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement). GitHub counts how many times each release file is downloaded and shows those totals publicly; they are aggregate numbers with no information about who downloaded. They are the only download figures we look at.

### Donations

Donation buttons take you to PayPal or Mercado Pago (and, later, Binance). The payment happens entirely on their sites: we never see or store your card or account details. The processor tells us the amount, the date and the name or email you gave it, which we keep only for our accounting. Their own policies apply: [PayPal](https://www.paypal.com/myaccount/privacy/privacyhub), [Mercado Pago](https://www.mercadopago.com.ar/privacidad). See the [Donation terms](/legal/donations/).

### Contact form

If you write to us through [our contact form](https://www.iperfex.com/contacto/?lang=en), on the iPERFEX website, we keep your message and the contact details you give only to answer you and to follow up on the matter.

## Part 2: the LordOS distribution

### No telemetry

LordOS sends **no telemetry**: no usage statistics, no crash reports, no device identifiers, no "phone home" checks and no account sign-in. LordOS sends nothing extra about you or your computer. Its update check does ask `lordos.org/apt` for the package lists, and since 12 November 2026 we count those requests (how many computers check each day and what they download, with no identifier of the computer or of you): see [Cookies and statistics](/legal/cookies-and-statistics/).

### What LordOS connects to by default

These are the only network connections a fresh LordOS installation makes without you asking for something:

| Connection | When | Why | How to turn it off |
|---|---|---|---|
| **DHCP and DNS** on your local network | when a network cable or Wi-Fi network is connected | to get an address and resolve names; DNS goes to the servers your network gives you or that you set | disconnect, or set a manual address and DNS in Settings → Network |
| **Network time (NTP)** through `systemd-timesyncd`, to Debian's public NTP pool | when the computer is online | to keep the clock right | Settings → Date & Time → automatic time off |
| **mDNS** (`avahi-daemon`, on the ISOs and the Raspberry Pi image, with `libnss-mdns` so `.local` names resolve) | on the local network only | announces the computer's name so other devices on your network can find it, and answers local name queries | `sudo systemctl disable --now avahi-daemon` |
| **Update check** (Store → Updates → **Check for updates automatically**, on by default) | once a day, in your desktop session, the first time a few minutes after you log in | downloads the signed package lists from the Debian mirrors in your apt sources and from LordOS's repository at `lordos.org/apt`, and tells you when updates are available; it never installs anything. Since 12 November 2026 its requests to `lordos.org/apt` are counted, with no identifier (see [Cookies and statistics](/legal/cookies-and-statistics/)) | turn off **Check for updates automatically** in Store → Updates. With no readable package list at all (a fresh installation, or an update that changed the list's format), LordOS still downloads the lists once so the Store can work |

The time zone is never guessed from the network. Apart from the update check's requests to `lordos.org/apt` (described under Store and updates below), nothing in this table goes to the LordOS project.

### Only when you ask

These connections happen only after an action of yours:

- **Store and updates:** the package lists and packages come from the Debian mirrors in your apt sources (`deb.debian.org` and `security.debian.org` by default; on the Raspberry Pi, the kernel and firmware come from `archive.raspberrypi.com`). Besides the daily update check in the table above, the Store refreshes them when you press **Check for updates** (in the Store or in Settings → Updates) or install something, and checks every list with Debian's signatures. LordOS's own updates come from LordOS's signed repository at `lordos.org/apt`, fetched at the same time. Since 0.8.5 that repository also carries the Store's featured banner (taken only from a source whose signed index says it is LordOS's, and checked against its hashes) and the list of apps on Store → Artificial Intelligence; `https://` sources are downloaded over HTTPS. LordOS's own `.lpk` apps are installed from files you choose; there is no online catalog of `.lpk` apps. The website's CDN logs these requests like any other, with your IP address, for 13 months; since 12 November 2026 we use them to count how many computers check for updates each day and what they download, never to identify anyone (see [Cookies and statistics](/legal/cookies-and-statistics/)).
- **Apps from their makers' repositories** (since 0.8.5): installing an app from Store → Artificial Intelligence or Store → Commercial whose maker has its own repository first tells you whose repository it adds and its key fingerprint, then adds it, so from then on the Store and apt also fetch that maker's package lists. Removing the app removes that repository unless another installed app uses it. What the app itself connects to is up to its maker.
- **Language packages:** adding a language in Settings can install its dictionaries and fonts from the same Debian mirrors.
- **Printers and scanners:** Settings → Printers & Scanners asks the local network (mDNS) which printers and scanners are there, and talks only to local addresses.
- **Mail, Calendar and Contacts:** they connect only to the servers of the accounts you add (IMAP, SMTP, CalDAV, CardDAV). Remote images in messages are blocked until you press **Load Images**, and tracking pixels are removed.
- **Blocklists** (since 0.8.5, all off by default): each list you turn on in Settings → Privacy → Blocklists, or add by its https address, is downloaded over HTTPS once a day from its publisher's https address, by a sandboxed downloader. Nothing about you is sent with the request beyond what any download carries.
- **Files → Network and servers** (since 0.8.7): Files → Network is off until you open that page. Then it makes one bounded pass over the local network, with mDNS and by trying the SMB and NFS ports on this computer's own private links, and talks only to local addresses. Connect to Server and Remote Desk connect only to the servers you name.
- **Moving wallpapers** (since 0.8.7): a clip is downloaded from the project's repository on `raw.githubusercontent.com` when you choose it, and checked against a SHA-256 in the package. GitHub sees that request as it sees any download.
- **Hardware reports** (since 0.8.7): LordOS sends nothing by itself. Share on GitHub saves a report and a page on your computer; if you submit GitHub's form, with your own account, the report is public there. It has no field for serial numbers, addresses or host, user or network names.
- **Cloud:** Settings → Cloud connects, through rclone, only to the providers of the accounts you add.
- **Web apps** (since 0.8.8.8): once the web engine (`lordos-web`) is installed, browsing the Store's Web apps section fetches each app's icon from that app's own site, one at a time, and Settings → Web apps → Refresh icon fetches it again. An installed web app talks to its own service, under that service's privacy policy; links outside it open in your browser.
- **Wi-Fi, Bluetooth and VPNs:** only the networks, devices and VPN servers you choose.
- **Remote desktop** is **off** by default (port 5673 closed). It listens only after you turn it on in Settings → Sharing. On the Raspberry Pi, remote desktop and the SSH server also stay off unless you put a `remote-desktop` or `ssh` switch file on the boot partition.
- **Web browsers and other apps:** Firefox ESR, Chromium and any Linux app you install are separate programs from their own projects. They make the connections their settings allow, under their own privacy policies. Isolated `.lpk` apps reach the network only if you grant them the permission, and the per-app firewall shows and controls what each app connects to.
- **The Debian installer script** (`curl -fsSL https://lordos.org/install.sh | sh`) contacts only `api.github.com` (to find the latest release), `github.com` (to download it and its signature) and your apt mirrors. It sends no telemetry.

### What stays on your computer

- **Monitor's world map** uses an offline IP-to-country database (DB-IP Lite) shipped with LordOS. Looking up where a connection goes never asks a server.
- **Cleaner's storage map** keeps folder names and sizes between sessions on this computer only, readable by your user alone. You can turn it off in Cleaner's Settings or clear it in its Activity.
- **Passwords:** the vault is encrypted on your disk and its process has no network access. The "breached password" check is offline only.
- **Settings, mail, files and logs** stay on your computer. Logs are not uploaded anywhere.

## Part 3: your rights, contact and changes

### Your rights

The distribution collects no personal data. The website keeps IP addresses in its request logs for 13 months, as described above. For anything we hold (those logs, an email you sent us, a donation record), you can ask to see it, correct it or delete it, subject to the records we must keep by law. If you are in Argentina, Law 25.326 on personal data protection gives you these rights, and you can complain to the Agency of Access to Public Information (AAIP). If you are in the European Union, Brazil or another country with similar laws (GDPR, LGPD), you have the rights those laws give you.

### Contact

Privacy questions or requests: [our contact form](https://www.iperfex.com/contacto/?lang=en).

### Changes

If LordOS or the website starts making a new kind of connection, or this policy changes in any other way, we will update this page first and change the date at the top.
