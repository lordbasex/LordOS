# Contributing to LordOS

Thank you for helping. The full guide is at
**[lordos.org/legal/contributing](https://lordos.org/legal/contributing/)**; this is the short
version.

## Before the repository is public

The repository is private during the early betas. Watch it on GitHub and follow
[lordos.org](https://lordos.org) to know when it opens. Until then, send feedback through the
[contact form](https://www.iperfex.com/contacto/?lang=en).

## Bugs and ideas

See [lordos.org/legal/issues](https://lordos.org/legal/issues/). Use the issue forms; never post
passwords or personal data. **Security problems are not reported in issues**: see
[SECURITY.md](SECURITY.md).

## Pull requests

1. Fork the repository and create a branch from `main`.
2. Keep each pull request to one change, with tests when it changes behaviour.
3. Sign off every commit (see below) and open the pull request against `main`.

## License and sign-off (DCO)

LordOS is released under the [MIT license](LICENSE). Contributions are accepted under the same
license (inbound = outbound): by contributing, you agree that your work is released under MIT.

We use the [Developer Certificate of Origin](https://developercertificate.org) (DCO): by signing
off a commit you certify that you wrote it, or otherwise have the right to submit it under the
project's license. Every commit must carry a `Signed-off-by` line with your real name and email,
which `git commit -s` adds:

```
Signed-off-by: Your Name <you@example.org>
```

Pull requests with unsigned commits can't be merged.

## Rules

- **English only** in code, comments, commit messages and documentation. Interface text goes in
  `assets/locales/{en,es,pt}/`, never hard-coded.
- **Tests must pass:** `make -C server test` (and `make -C server test-full` when you touch the
  helpers or the sandbox), plus `node tools/i18n-check.mjs`, which also runs
  `tools/brand-check.mjs` and `tools/err-check.mjs`.
- **Follow AGENTS.md** for the architecture and code rules of each part.
- **Brand rules:** Axia DE does not use the LordOS name, crown logo or Lumo (the LordOS trademark
  policy reserves them; "based on LordOS" is fine), and don't use third-party brand or product names in
  the interface.
- Be kind: everyone here follows the [code of conduct](CODE_OF_CONDUCT.md).
