LordOS contains cryptographic software. This page lists it and explains, in general terms, why that can matter where you live. It is general information, not legal advice.

## Cryptography in LordOS

**LordOS's own code** (MIT license) implements:

- **Monocypher 4** (vendored in `third_party/`, CC0-1.0 or BSD-2-Clause): XChaCha20-Poly1305, Argon2id and BLAKE2b, used by the password vault and keyring and by encrypted backups;
- SHA-256, SHA-512, Ed25519 and RSA signature verification: checking Debian's signed package lists, signing and checking `.lpk` packages, and checking release signatures;
- AES-128/256, SHA-1, HMAC and HKDF: reading KeePass (KDBX) files, the Secret Service protocol, FIDO2 security keys (with P-256) and two-factor codes (TOTP);
- X25519 and PBKDF2-HMAC-SHA1: WireGuard keys and the WPA2 password of Wi-Fi networks;
- NTLMv2 and SMB 2 signing for shared folders, and the DES that VNC's password check requires by protocol.

**Libraries and programs from Debian** that LordOS uses:

- **OpenSSL 3** (Apache-2.0), linked only by `lord-sessiond` (the TLS of remote desktop), `lord-maild` (TLS for IMAP, SMTP, CalDAV and CardDAV), `lord-fetch` (blocklist downloads) and `lord-tls` (the Store's HTTPS);
- **OpenVPN**, **strongSwan** (IPsec/IKEv2), **OpenConnect** and **openfortivpn**, which LordOS starts for the VPNs you set up (OpenVPN is preinstalled; the others are Debian packages you add when you need them), and the Linux kernel's WireGuard, which LordOS configures over netlink;
- **FreeRDP** for RDP in Remote Desk, and **rclone** and the OpenSSH client for connecting to servers from Files;
- **wpa_supplicant** for Wi-Fi, and **OpenSSH** in the Raspberry Pi 5 image (off by default);
- the Linux kernel, apt's signature verification, the web browsers and other Debian packages, each with its own cryptography.

The complete list of bundled components and their licenses is in [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md) and on the [Licenses](/legal/licenses/) page.

## Publicly available source code

LordOS is free software. Its full source code, including the source of every release, is public at [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). The Debian components are published by Debian at [sources.debian.org](https://sources.debian.org).

## Export, import and use restrictions

Some countries restrict the export, re-export, import or use of encryption software. As general information only:

- many countries follow the **Wassenaar Arrangement**, which generally treats publicly available encryption software differently from commercial products;
- in the United States, the Export Administration Regulations (EAR) classify encryption software under ECCN 5D002, but publicly available open-source encryption source code is generally not subject to the EAR (15 CFR §734.3(b)(3) and §742.15(b)), and similar treatment has long applied through License Exception TSU.

These rules change, and they don't cover every country or every use. **You are responsible** for knowing and complying with the laws that apply to you before you download, use, import, export or redistribute LordOS. If in doubt, ask a lawyer in your jurisdiction.

## No warranty and no legal advice

This notice is not legal advice and gives no assurance that LordOS may be exported to, imported into or used in any particular country. LordOS is provided "as is", as explained in the [Terms of use](/legal/terms/). Questions: [our contact form](https://www.iperfex.com/contacto/?lang=en).
