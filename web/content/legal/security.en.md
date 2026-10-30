We take the security of LordOS seriously and we're grateful to anyone who helps us find and fix problems. This page explains how to report a vulnerability and what you can expect from us.

## How to report a vulnerability

Write to us through **[our contact form](https://www.iperfex.com/contacto/?lang=en)**, with "Security" in the subject. Once the repository is public, GitHub private vulnerability reporting will be available too. Please don't open a public issue, post on social media or publish details until a fix is out.

Include, as far as you can:

- the LordOS version (shown in the About LordOS app, for example "0.8.6 · Beta") and whether it is the ISO, the Raspberry Pi image or the Debian package;
- the component affected (for example the compositor, the app sandbox, the per-app firewall, remote desktop, the password vault, the Store, the Debian installer script or the website);
- the steps to reproduce it, a proof of concept if you have one, and the impact you see;
- how you'd like to be credited, if at all.

You can write in English, Spanish or Portuguese.

## Scope

**In scope:**

- LordOS's own code: `lord-server` (the display server and Wayland compositor), `lord-sessiond` and the login screen, the app sandbox and permission broker, `lord-netperm`, `lord-vault`, the mail, document and media helpers, the package engine, the native apps and the SDK;
- the release files and their signatures, and the Debian installer script (`lordos.org/install.sh`);
- the configuration LordOS ships for the system (for example services it enables);
- the lordos.org website.

**Out of scope** (report these to their own projects):

- vulnerabilities in Debian packages, the Linux kernel, Mesa, browsers or other third-party software that also affect plain Debian ([Debian's security team](https://www.debian.org/security/)); tell us too if LordOS makes it worse;
- third-party `.lpk` apps (report to their author; tell us if one escapes the sandbox);
- attacks that need physical access to an unlocked computer, social engineering, or denial of service by flooding;
- reports from automated scanners without a demonstrated impact, and missing best-practice headers with no security consequence.

## Safe harbour

If you act in good faith and follow this policy, we will consider your research authorised, we won't take legal action against you, and we'll say so if anyone else does. Good faith means that you:

- test only on your own computers, virtual machines and accounts;
- don't access, change or delete other people's data, and stop as soon as you reach any;
- don't degrade the website or other users' systems;
- give us a reasonable time to fix the problem before disclosing it.

## What to expect

We are a small team, so these are our goals, not guarantees:

| Step | Target |
|---|---|
| We acknowledge your report | within 3 business days |
| We confirm the problem and tell you how serious we think it is | within 10 business days |
| We fix critical and high-severity problems | as fast as possible, usually within 30 days |
| Coordinated public disclosure | when the fix is released, or after 90 days, whichever comes first, unless we agree otherwise |

We'll keep you informed, credit you in the release notes if you want, and publish an advisory for fixed vulnerabilities. We don't offer paid bug bounties at this time.

## Verifying releases

Every release file (the ISO, the Raspberry Pi image and the `.deb`) is listed with its SHA-256 in `SHA256SUMS`, and `SHA256SUMS` is signed with the LordOS release key (Ed25519). The Debian installer script refuses to install when the signature is missing or invalid.

- Public key: [lordos-release.pub.pem](/lordos-release.pub.pem)
- Fingerprint (SHA-256 of the DER public key):
  `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

To check a download by hand:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

If the fingerprint you see doesn't match the one above, don't install the file and tell us. How we sign releases, apps and packages, and how we protect the keys, is explained in [Code signing](/legal/code-signing/).

## security.txt

This policy is also published in machine-readable form at [/.well-known/security.txt](/.well-known/security.txt) (RFC 9116).
