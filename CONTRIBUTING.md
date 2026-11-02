# Contributing to LordOS

Thank you for helping. The full guide is at
**[lordos.org/legal/contributing](https://lordos.org/legal/contributing/)**; this is the short
version.

## Reporting a bug

**Security problems are not reported in issues**: see [SECURITY.md](SECURITY.md). For anything else, open an
[issue](https://github.com/lordbasex/LordOS/issues/new/choose) and include what is below. It makes the difference between a
problem we can fix and one we can only guess at.

### 1. Collect the report

Open Terminal and run:

```sh
lordos-issue
```

It only reads, sends nothing, and saves `~/lordos-issue-<date>.txt` with your LordOS version, hardware, displays, input
devices, the session's log and the errors of this boot. Addresses, your user and computer names and home paths are replaced.
**Read the file before posting it**, since issues are public. Run `sudo lordos-issue` to add the complete kernel log. Paste it in the
**Logs** box of the form.

If the problem stops you from using the desktop, press **Ctrl+Alt+F2** to get a text console, sign in and run it there. On the live
system it works the same way. If `lordos-issue` isn't there (before 0.8.7.1), send the output of these:

```sh
lordos-fetch --share --color=never
grep -E '^N:' /proc/bus/input/devices
journalctl -b -p warning --no-pager | tail -n 80
tail -n 60 ~/.cache/lordos/session.log
```

### 2. Say what happened

- The LordOS version and how you installed it (ISO, Raspberry Pi image, or on Debian).
- The computer's model.
- The exact steps, what you expected, and what happened instead. A photo of the screen helps when nothing else can be collected.

### 3. Share how your hardware runs

If everything works, a hardware report helps too: **System Information → Report → Share on GitHub**.

## Ideas

Use the feature form, and say what problem it solves. Check [the open issues](https://github.com/lordbasex/LordOS/issues) first.

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
  `assets/locales/{en,es,pt,fr}/`, never hard-coded.
- **Tests must pass:** `make -C server test` (and `make -C server test-full` when you touch the
  helpers or the sandbox), plus `node tools/i18n-check.mjs`, which also runs
  `tools/brand-check.mjs` and `tools/err-check.mjs`.
- **Follow AGENTS.md** for the architecture and code rules of each part.
- **Brand rules:** use the LordOS name, the crown logo and Lumo as described in
  [docs/LORDOS-BRAND.md](docs/LORDOS-BRAND.md), and don't use third-party brand or product names in
  the interface.
- Be kind: everyone here follows the [code of conduct](CODE_OF_CONDUCT.md).
