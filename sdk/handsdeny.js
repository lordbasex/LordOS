// handsdeny.js — the surfaces an AI agent (Hands, docs/HANDS.md "Never") never reads, clicks, types into or sees, in one
// place: what changes accounts, passwords, remote access, permissions, security, software or the session is the
// person's own. Tested by server/tests/hands_deny_test.mjs: every key names a real Settings page or app, so a dead key
// fails the build.
//
// Besides these lists, a node is out of an agent's reach when it (or a node it is in) is marked handsDeny (every
// destructive ConfirmDialog, the theme-trust dialog, the power dialog, Settings → AI → Hands, the Hands prompt and
// banner), protected (password fields, protected windows) or in the system band (sdk/layers.js 100 and up).

// Settings' pages (apps/settings/index.jsx SECTIONS): network (Wi-Fi and VPN credentials), sharing (remote desktop, SSH,
// screen-sharing grants), cloud (accounts), privacy (permissions, the firewall and its rules), users (accounts and
// passwords), webapps and flatpak (installing and removing), updates, session.
export const PERSON_ONLY_SETTINGS = Object.freeze(['network', 'sharing', 'cloud', 'privacy', 'users', 'webapps', 'flatpak', 'updates', 'session']);

// Whole apps (apps/<id>): the Store (software), Passwords (the vault and the keyring), Backups (destinations and their
// encryption), Disks and Imager (formatting and writing disks), the installer, the remote desktop client (credentials).
export const PERSON_ONLY_APPS = Object.freeze(['store', 'passwords', 'backup', 'disks', 'imager', 'installer', 'remote-desk']);

export const settingsPageDenied = (id) => PERSON_ONLY_SETTINGS.includes(id);
export const appDenied = (id) => PERSON_ONLY_APPS.includes(String(id || '').split('#')[0]);
