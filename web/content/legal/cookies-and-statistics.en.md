*This page replaces the former Cookie notice, which used to be at /legal/cookies/.*

## No cookies

lordos.org sets **no cookies**: no session, analytics, advertising or tracking cookies, and none from third parties. That's why the website shows no cookie banner.

## No analytics services

Nothing runs in your browser to measure you:

- **On the website:** no analytics service, no tracking pixels, no fingerprinting, no social media widgets, and no fonts or scripts loaded from other servers.
- **In the LordOS distribution:** no telemetry, no usage statistics, no crash reports and no device identifiers. The network connections LordOS makes are listed in the [Privacy policy](/legal/privacy/).

## Visit statistics

To know how the project is growing, we count visits from the website's standard CDN request logs (Amazon CloudFront). These are the requests your browser sends anyway to receive each page: we add no code, cookie or identifier to get them.

From each request we keep:

- the page or file asked for, and the time;
- **your IP address**, to count distinct visitors;
- the country, which CloudFront works out from the IP address by itself;
- the page you came from (the referrer), when your browser sends it;
- the browser's user agent, to know the system (Windows, macOS, Linux, Android, iOS) and the kind of device (computer, phone, tablet) and to leave bots out of the counts;
- technical data about the answer: its status code, its size, how long it took and which CloudFront location served it.

We don't use these logs to find out who you are, we don't combine them with any other data and we don't sell or share them.

- **Who sees them:** only the LordOS team, in a private AWS dashboard: visits per day, week, month and year, the most viewed pages, the visits per country and per system and device. We don't publish these statistics or share them with anyone.
- **What we don't count:** the requests LordOS makes to `lordos.org/apt` when it checks for updates. CloudFront writes them to the same logs, with the same data, because it can't keep them apart; we leave them out of every count and they are deleted with the rest.
- **How long:** these logs are kept for 13 months and then deleted.

## What the website keeps in your browser

Two small preferences are saved in your browser's `localStorage`, on your device only:

| Key | Value | Purpose |
|---|---|---|
| `lordos-theme` | `light` or `dark` | remembers the theme you chose with the theme button; with automatic, nothing is saved and the site follows your system |
| `lordos-lang-seen` | `1` | shows the suggestion to read the site in your language only once |

They are never sent to a server, don't identify you and aren't shared with anyone. If your browser blocks storage, the website still works; it just forgets your choices. To remove them, delete the site data for lordos.org in your browser's settings; private windows remove them when you close the window.

## Server logs

The website is served by a content delivery network. To deliver each page, your browser sends your IP address and the usual request data (the page asked for, the time, the browser's user agent). Standard CDN request logs may be kept by our hosting provider for security, retained for a limited time. Those provider logs, with IP addresses, are separate from the statistics above, and we don't use them to identify or profile visitors.

## Download counts

The release files are hosted on GitHub Releases. GitHub keeps a download counter for each release file; it is public, it is a single total per file, and it says nothing about who downloaded it. If we mention how many times LordOS was downloaded, that number comes from those counters.

## Donation sites

The donation buttons take you to PayPal or Mercado Pago (and, later, Binance). Those sites set their own cookies under their own policies once you are there. Nothing from them is loaded on lordos.org before you click.

## Questions

See the [Privacy policy](/legal/privacy/), or write to us through [our contact form](https://www.iperfex.com/contacto/?lang=en).
