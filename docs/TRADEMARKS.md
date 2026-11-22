# Trademarks and the systems' logos

LordOS names other systems in text where it has to say what something downloads or runs (Machines' catalog, the New Machine
assistant). It ships **no third-party logo**: not in the repository, the packages, the site, screenshots, the paper or any
marketing. Machines draws its own marks for every system (`apps/machines/ui.jsx` `OsMark`, `assets/machines/marks`).

## Official logos, opt-in (approved by the maintainer 2026-11-22)

Machines settings → General → "Official logos of the systems" is off by default and per person. Turning it on asks for
confirmation ("The logos are downloaded from each project's official site and stay on this computer only. The marks belong
to their owners."); then the logos of the systems listed below are downloaded from each owner's own site, checked against
the SHA-256 LordOS pinned, re-encoded in lord-media's sandbox and kept only in that person's
`~/.cache/lordos/machines/logos/`. Off hides them at once; "Remove downloaded logos" deletes them. Wherever a logo appears
nothing suggests a partnership; About carries the notice: "Product names and logos are trademarks of their respective
owners; LordOS is not affiliated with or endorsed by them. Logos are only downloaded on request, from each owner's site, and
shown on this computer." Screenshots and tests of LordOS are taken with the switch off. How it works:
docs/VIRTUALIZATION.md 5.6.2.

**Rule for the list:** a project is listed only if its official trademark or logo policy explicitly allows a third party to
display the logo to refer to or identify the project's software. A policy that restricts logo use, asks for permission or
approval, or leaves it ambiguous keeps the project out (it keeps LordOS's drawn mark). Only a PNG the project itself
publishes on its own https site is listed (an SVG-only logo stays out). Microsoft/Windows is always out.

| Project | Policy | Checked | Decision | Why |
|---|---|---|---|---|
| Debian | https://www.debian.org/trademark (Trademark Policy v2.0, "Guidelines for Using Logos"), https://www.debian.org/logos/ | 2026-11-22 | **Listed**: `https://www.debian.org/logos/openlogo-nd-100.png`, SHA-256 `92aad0be9b2be94420cfb6c1daf5985338e402110353b4fa051fb429f4cde2b8`, 1444 bytes, 101 × 124 | The Open Use Logo is under LGPL-3.0-or-later or CC BY-SA 3.0, and the policy allows it "as part of a page describing your products or services" to identify Debian (nominative use). The Restricted Use logo is not used. |
| Ubuntu (Canonical) | https://canonical.com/legal/intellectual-property-policy | 2026-11-22 | Out | The trademarks, word and logo, "can only be used with Canonical's permission"; use of the logo needs written permission. |
| Fedora (Red Hat) | https://docs.fedoraproject.org/en-US/legal/trademarks/ (source: gitlab.com/fedora/legal/fedora-legal-docs) | 2026-11-22 | Out | Use without permission is limited to listed cases "only in connection with promoting the Fedora Project", with the brand standards strictly observed; nothing explicitly allows showing the logo inside other software. |
| Kali Linux (OffSec) | https://www.kali.org/docs/policy/trademark/ | 2026-11-22 | Out | The policy encourages use to identify OffSec's products, but forbids putting the marks on "anything that you produce commercially" without written permission; that ambiguity keeps it out. |
| Linux Mint | https://linuxmint.com/faq.php ("Can I use the Linux Mint logo…") | 2026-11-22 | Out | The FAQ allows showing the logo without implying affiliation, but the project publishes its logo only as SVG (its one PNG is a 22 × 22 site icon). |
| Windows (Microsoft) | — | — | Always out | Machines keeps its own mark. |

## Profiles and systems people share

A `.lordprofile` or an added system may carry `logo: { url, sha256 }` (https, PNG). That logo is its author's
responsibility: LordOS doesn't host or redistribute it, an exported profile holds only the address and the hash, never the
image, and the import summary says so. This version doesn't download those logos.
