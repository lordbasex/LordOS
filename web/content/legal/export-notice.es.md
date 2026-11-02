LordOS contiene software criptográfico. Esta página lo enumera y explica, en términos generales, por qué eso puede importar donde vivís. Es información general, no asesoramiento legal.

## Criptografía en LordOS

**El código propio de LordOS** (licencia MIT) implementa:

- **Monocypher 4** (incluido en `third_party/`, CC0-1.0 o BSD-2-Clause): XChaCha20-Poly1305, Argon2id y BLAKE2b, que usan la bóveda de contraseñas, el llavero y las copias de seguridad cifradas;
- SHA-256, SHA-512, Ed25519 y verificación de firmas RSA: para verificar las listas de paquetes firmadas de Debian, firmar y verificar paquetes `.lpk` y verificar las firmas de las versiones;
- AES-128/256, SHA-1, HMAC y HKDF: para leer archivos de KeePass (KDBX), el protocolo Secret Service, las llaves de seguridad FIDO2 (con P-256) y los códigos de dos factores (TOTP);
- X25519 y PBKDF2-HMAC-SHA1: las claves de WireGuard y la contraseña WPA2 de las redes Wi-Fi;
- NTLMv2 y la firma de SMB 2 para las carpetas compartidas, y el DES que el protocolo VNC exige para su contraseña.

**Bibliotecas y programas de Debian** que usa LordOS:

- **OpenSSL 3** (Apache-2.0), enlazado solo por `lord-sessiond` (el TLS del escritorio remoto), `lord-maild` (TLS para IMAP, SMTP, CalDAV y CardDAV), `lord-fetch` (las descargas de las listas de bloqueo) y `lord-tls` (el HTTPS de la Tienda);
- **OpenVPN**, **strongSwan** (IPsec/IKEv2), **OpenConnect** y **openfortivpn**, que LordOS inicia para las VPN que configurás (OpenVPN viene instalado; los demás son paquetes de Debian que sumás cuando los necesitás), y el WireGuard del kernel Linux, que LordOS configura por netlink;
- **FreeRDP** para RDP en Remote Desk, y **rclone** y el cliente OpenSSH para conectarse a servidores desde Archivos;
- **wpa_supplicant** para el Wi-Fi, y **OpenSSH** en la imagen para Raspberry Pi 5 (apagado de fábrica);
- el kernel Linux, la verificación de firmas de apt, los navegadores web y otros paquetes de Debian, cada uno con su propia criptografía.

La lista completa de componentes incluidos y sus licencias está en [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md) y en la página de [Licencias](/legal/licenses/).

## Código fuente disponible públicamente

LordOS es software libre. Su código fuente completo, incluido el de cada versión, es público en [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). Los componentes de Debian los publica Debian en [sources.debian.org](https://sources.debian.org).

## Restricciones de exportación, importación y uso

Algunos países restringen la exportación, la reexportación, la importación o el uso de software de cifrado. Solo como información general:

- muchos países siguen el **Arreglo de Wassenaar**, que en general trata el software de cifrado disponible públicamente de forma distinta a los productos comerciales;
- en Estados Unidos, las Export Administration Regulations (EAR) clasifican el software de cifrado en la ECCN 5D002, pero el código fuente de cifrado de código abierto disponible públicamente en general no está sujeto a las EAR (15 CFR §734.3(b)(3) y §742.15(b)), y desde hace tiempo se le aplicó un trato similar a través de la License Exception TSU.

Estas reglas cambian y no cubren todos los países ni todos los usos. **Vos sos responsable** de conocer y cumplir las leyes que se te aplican antes de descargar, usar, importar, exportar o redistribuir LordOS. Si tenés dudas, consultá a un abogado de tu jurisdicción.

## Sin garantía y sin asesoramiento legal

Este aviso no es asesoramiento legal y no asegura que LordOS se pueda exportar a un país determinado, importar en él o usar allí. LordOS se ofrece "tal cual", como explican los [Términos de uso](/legal/terms/). Consultas: [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es).
