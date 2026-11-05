---
title: VPN
section: system
order: 61
keywords: vpn, wireguard, openvpn, ipsec, ikev2, pptp, openconnect, anyconnect, fortinet
---
# VPN

In [Settings → Network](settings:network), the **VPN** row:

- **Import** a configuration: **WireGuard** (`.conf`), **OpenVPN** (`.ovpn`, with its certificates
  next to it), **IPsec/IKEv2**, **PPTP**, and optionally **OpenConnect** (AnyConnect) and
  **Fortinet**, enabled with a button (they install themselves).
- **Connect** and **disconnect** with the switch. You see the status, address and traffic.
- **Full or split tunnel:** depending on the configuration, all traffic goes through the VPN or
  just certain networks. The computer's normal route is never touched: when you disconnect,
  everything goes back to how it was.

Importing or deleting a VPN requires an administrator; connecting and disconnecting don't. Keys and
passwords are stored readable only by root. **PPTP is insecure**: it is there for old networks and
marked as such.

## Fortinet with single sign-on

If your organization signs you in on its own page (single sign-on, often with an authenticator app),
choose **Authentication → Single sign-on** when adding the Fortinet VPN: no user name or password.
When you connect, LordOS opens the organization's sign-in page in your browser and shows "Waiting
for you to sign in on the organization's page…". Sign in there; the VPN connects when you finish.
You have 5 minutes; **Cancel** stops it and **Open sign-in page** opens it again.

## "This server's certificate couldn't be verified"

LordOS shows the certificate's fingerprint, who it was issued to and by. Often the server is sending
an incomplete certificate chain, which other systems complete on their own. Compare the fingerprint
with the one your organization gives you and choose **Trust** (an administrator): LordOS remembers it
for that VPN and connects. If the server later shows a different certificate, LordOS says it changed
and asks again.

To change a saved VPN, turn it off and choose **⋯ → Edit…** in its row. The trash can button deletes it.
