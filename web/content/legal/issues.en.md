Found something that doesn't work, or have an idea that would make LordOS better? This page explains where to tell us and what to include, so we can act on it quickly.

## Where to report

- **Right now** the LordOS repository on GitHub is private, so bug reports and feature requests go through [our contact form](https://www.iperfex.com/contacto/?lang=en). Start the subject with "Bug" or "Feature request".
- **Once the repository is public**, use [GitHub Issues](https://github.com/lordbasex/LordOS/issues). Choose the bug report or feature request template; blank issues are turned off so that every report has the information we need.

Before you write, check the [FAQ](/faq/) and, once they are public, the existing issues: someone may have reported it already.

## Security problems are different

**Never report a vulnerability in a public issue.** Follow the [Security policy](/legal/security/) and use [our contact form](https://www.iperfex.com/contacto/?lang=en) with "Security" in the subject. Public issues about security problems will be hidden and handled privately.

## What to include in a bug report

- **The LordOS version**, as shown in the About LordOS app (for example "0.8.6 · Beta").
- **How you installed it:** the ISO (live or installed), the Raspberry Pi 5 image, or the `.deb` on Debian 13.
- **Your hardware:** the computer or board model, the graphics card, the amount of memory, and any device involved (printer, touchpad, Wi-Fi adapter, monitor).
- **Steps to reproduce:** what you did, one step per line, starting from a fresh start if you can.
- **What you expected** and **what happened instead**, including the exact text of any error message.
- **Logs:** open the Logs app, find the entries from the moment of the problem and copy them into the report. Screenshots or a short screen recording help too.

One problem per report, please. If you find several, send several reports.

## What not to include

Logs and screenshots can contain private information. Before you send them, remove or blur:

- passwords, passphrases, recovery codes, 2FA codes and private keys;
- personal data: names, addresses, phone numbers, email addresses, account names;
- the content of private mail, documents or messages;
- IP addresses, Wi-Fi network names and VPN details you don't want to share.

We'll never ask you for a password.

## Feature requests

Tell us what you are trying to do and why, not only the solution you have in mind: the problem helps us find the right design. Say whether it is something you need for daily use and whether other desktops or apps solve it in a way you like (describe the behaviour; there is no need to name products). We can't promise to build every request, but we read them all and they shape the roadmap.

## What happens next

We are a small team, so these are goals, not guarantees:

- we acknowledge reports sent through the contact form within about **5 business days**;
- we triage each report: we try to reproduce it, ask for more information if needed, and give it labels and a priority;
- fixes ship in the next release that can include them, and the release notes on [GitHub Releases](https://github.com/lordbasex/LordOS/releases) mention them.

## Labels

Once issues are public, these are the labels we use:

| Label | Meaning |
|---|---|
| `bug` | something doesn't work as documented |
| `enhancement` | a new feature or an improvement |
| `documentation` | docs, help pages or website text |
| `hardware` | depends on a specific computer, board or device |
| `needs-info` | we need more details from the reporter to continue |
| `good first issue` | a good starting point for new contributors |
| `duplicate` | already reported elsewhere (we link the original) |
| `wontfix` | we decided not to change it, with the reason explained |

Want to fix it yourself? See [Contributing](/legal/contributing/).
