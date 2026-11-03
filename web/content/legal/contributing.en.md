LordOS is free software and contributions are welcome: code, documentation, translations, tests on real hardware, and good bug reports. This page explains how to take part and the few rules every contribution follows.

## Ways to take part

The repository is public at [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). You can:

- try LordOS and [report problems](/legal/issues/) on [GitHub Issues](https://github.com/lordbasex/LordOS/issues), with the output of `lordos-issue` (from 0.8.8 on: run it in a terminal; it writes `~/lordos-issue-<date>.txt` with names and addresses replaced, and sends nothing);
- test it on your hardware and tell us what works and what doesn't;
- **watch** or **star** the repository to hear about releases.

## Fork and pull request

1. **Fork** the repository on GitHub and create a branch for your change.
2. Make your change, with tests where it makes sense, and run the test suites (see below).
3. Commit with a sign-off (`git commit -s`, explained below).
4. Open a **pull request against `main`**, explaining what it changes and why, and linking the issue it fixes if there is one.
5. A maintainer reviews it. We may ask for changes; once it is ready, it is merged.

For larger changes (a new app, a new system service, a new dependency), open an issue first so we can agree on the design before you spend time on it.

## License of contributions

LordOS uses **inbound = outbound** licensing: by contributing, you license your contribution under the same **MIT license** as the rest of the project ([LICENSE](https://github.com/lordbasex/LordOS/blob/main/LICENSE)). You keep the copyright of your work; there is no copyright assignment and no separate contributor agreement to sign.

## Developer Certificate of Origin (sign-off)

Every commit must be signed off. The sign-off is a line at the end of the commit message:

```
Signed-off-by: Your Name <you@example.com>
```

`git commit -s` adds it for you, with the name and email of your Git configuration. By adding it, you certify the [Developer Certificate of Origin 1.1](https://developercertificate.org/), which says, in short, that:

- you wrote the contribution, or have the right to submit it under the project's open-source license;
- if it is based on earlier work, that work is covered by a compatible open-source license that allows you to submit it;
- you understand that the contribution and your sign-off are public and kept permanently in the project's history.

Use your real name (or the name you are publicly known by). Pull requests with commits that are not signed off can't be merged.

## Project rules

- **English only.** All code, identifiers, code comments, commit messages and Markdown documentation are written in English. User-facing text is the exception: it lives in the language catalogs (`assets/locales/<lang>/`) and goes through translation, with English as the default.
- **Tests must pass.** Run `make -C server test` for the self-contained suites (and `make -C server test-full` when you touch the helpers or the sandbox), and `node tools/i18n-check.mjs` when you touch user-facing text. Add a test for every bug you fix and every feature you add, where possible.
- **Clean C.** Readable, modular C11 that builds without warnings under `-Wall -Wextra`. Respect the dependency policy in [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md).
- **Don't claim untested compatibility.** If you say something works on a piece of hardware or with an app, you tested it.
- **Brand rules.** Follow the [brand kit](https://github.com/lordbasex/LordOS/blob/main/docs/LORDOS-BRAND.md) for the logo, colours and the spelling "LordOS". Use generic names for features, and keep other companies' brand and product names out of public text; `node tools/brand-check.mjs` checks this.
- **No secrets.** Never commit private keys, passwords or personal data.

## Translations

LordOS ships in four languages: English, Spanish, Portuguese and French (since 0.8.8). Other languages are added as **language packs** built from the translation catalogs; see [docs/LANGUAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/LANGUAGES.md). Corrections to the existing translations are welcome too.

## Code of conduct

Everyone who takes part in LordOS follows our [Code of conduct](/legal/code-of-conduct/). Security problems are never reported in public: see the [Security policy](/legal/security/).
