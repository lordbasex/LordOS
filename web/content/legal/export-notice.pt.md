O LordOS contém software criptográfico. Esta página o relaciona e explica, em termos gerais, por que isso pode importar onde você vive. É informação geral, não aconselhamento jurídico.

## Criptografia no LordOS

**O código próprio do LordOS** (licença MIT) implementa:

- **Monocypher 4** (incluído em `third_party/`, CC0-1.0 ou BSD-2-Clause): XChaCha20-Poly1305, Argon2id e BLAKE2b, usados pelo cofre de senhas, pelo chaveiro e pelos backups criptografados;
- SHA-256, SHA-512, Ed25519 e verificação de assinaturas RSA: para conferir as listas de pacotes assinadas do Debian, assinar e conferir pacotes `.lpk` e conferir as assinaturas das versões;
- AES-128/256, SHA-1, HMAC e HKDF: para ler arquivos do KeePass (KDBX), o protocolo Secret Service, as chaves de segurança FIDO2 (com P-256) e os códigos de dois fatores (TOTP);
- X25519 e PBKDF2-HMAC-SHA1: as chaves do WireGuard e a senha WPA2 das redes Wi-Fi;
- NTLMv2 e a assinatura de SMB 2 para as pastas compartilhadas, e o DES que o protocolo VNC exige para a senha.

**Bibliotecas e programas do Debian** que o LordOS usa:

- **OpenSSL 3** (Apache-2.0), vinculado só pelo `lord-sessiond` (o TLS do desktop remoto), pelo `lord-maild` (TLS para IMAP, SMTP, CalDAV e CardDAV), pelo `lord-fetch` (downloads das listas de bloqueio) e pelo `lord-tls` (o HTTPS da Loja);
- **OpenVPN**, **strongSwan** (IPsec/IKEv2), **OpenConnect** e **openfortivpn**, que o LordOS inicia para as VPNs que você configura (o OpenVPN vem instalado; os outros são pacotes do Debian que você adiciona quando precisar), e o WireGuard do kernel Linux, que o LordOS configura via netlink;
- **FreeRDP** para RDP no Remote Desk, e **rclone** e o cliente OpenSSH para conectar a servidores pelo Arquivos;
- **wpa_supplicant** para o Wi-Fi, e **OpenSSH** na imagem para Raspberry Pi 5 (desligado por padrão);
- o kernel Linux, a verificação de assinaturas do apt, os navegadores web e outros pacotes do Debian, cada um com sua própria criptografia.

A lista completa de componentes incluídos e suas licenças está no [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md) e na página de [Licenças](/legal/licenses/).

## Código-fonte disponível publicamente

O LordOS é software livre. O código-fonte completo, incluindo o de cada versão, é público em [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). Os componentes do Debian são publicados pelo Debian em [sources.debian.org](https://sources.debian.org).

## Restrições de exportação, importação e uso

Alguns países restringem a exportação, a reexportação, a importação ou o uso de software de criptografia. Apenas como informação geral:

- muitos países seguem o **Acordo de Wassenaar**, que em geral trata o software de criptografia disponível publicamente de forma diferente dos produtos comerciais;
- nos Estados Unidos, as Export Administration Regulations (EAR) classificam o software de criptografia na ECCN 5D002, mas o código-fonte de criptografia de código aberto disponível publicamente em geral não está sujeito às EAR (15 CFR §734.3(b)(3) e §742.15(b)), e há tempos um tratamento semelhante se aplicou pela License Exception TSU.

Essas regras mudam e não cobrem todos os países nem todos os usos. **Você é responsável** por conhecer e cumprir as leis que se aplicam a você antes de baixar, usar, importar, exportar ou redistribuir o LordOS. Em caso de dúvida, consulte um advogado da sua jurisdição.

## Sem garantia e sem aconselhamento jurídico

Este aviso não é aconselhamento jurídico e não assegura que o LordOS possa ser exportado para um país específico, importado nele ou usado lá. O LordOS é fornecido "no estado em que se encontra", como explicam os [Termos de uso](/legal/terms/). Dúvidas: [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt).
