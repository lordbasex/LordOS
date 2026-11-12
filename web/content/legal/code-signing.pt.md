O LordOS assina o que publica para que você possa confirmar que um arquivo vem do projeto e que ninguém o alterou no caminho. Esta página explica o que é assinado, com quais chaves e como você mesmo pode conferir. Para avisar sobre um problema com uma assinatura, veja a [Política de segurança](/legal/security/).

## Arquivos de cada versão

Cada arquivo de uma versão no GitHub (a imagem ISO, a imagem para Raspberry Pi 5 e o pacote `.deb`) aparece com o seu SHA-256 num arquivo chamado `SHA256SUMS`. O `SHA256SUMS` é assinado com a **chave de versões do LordOS**:

- `SHA256SUMS.sig` é o base64 da assinatura **Ed25519** de 64 bytes sobre os bytes exatos do `SHA256SUMS`.
- Chave pública: [lordos-release.pub.pem](/lordos-release.pub.pem), também no repositório como [`tools/release/lordos-release.pub.pem`](https://github.com/lordbasex/LordOS/blob/main/tools/release/lordos-release.pub.pem).
- Impressão digital (SHA-256 da chave pública em DER):
  `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

O script de instalação para Debian (`lordos.org/install.sh`) traz a mesma chave pública embutida. Ele confere a assinatura antes de baixar o `.deb`, depois confere o `.deb` contra o `SHA256SUMS`, e não instala nada se alguma das verificações falhar.

Para conferir um download manualmente, com o OpenSSL 3, na pasta onde você salvou os arquivos:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

O primeiro comando decodifica a assinatura, o segundo precisa mostrar `Signature Verified Successfully` e o terceiro precisa mostrar `OK` para cada arquivo que você baixou. Para conferir a própria chave, compare a impressão digital dela com a de cima:

```
openssl pkey -pubin -in lordos-release.pub.pem -outform DER | sha256sum
```

## Apps do LordOS (.lpk) e a Loja

Os apps nativos do LordOS vêm como pacotes `.lpk`. Um `.lpk` assinado traz a lista de todos os seus arquivos com o SHA-256 de cada um e uma assinatura **Ed25519** sobre essa lista. O LordOS a confere com o seu próprio código em C ao instalar o app e toda vez que o carrega, então um arquivo alterado é detectado.

A Loja e a janela de instalação mostram um destes níveis de confiança:

| Nível | Quando |
|---|---|
| **LordOS** | assinado com a chave oficial do LordOS, que vem compilada no sistema (nenhum arquivo da sua pasta pessoal pode adicionar uma chave oficial) |
| **Autor verificado** | assinado com uma chave que consta no catálogo assinado do LordOS |
| **Assinado por <nome>** | assinado com uma chave que o LordOS não conhece |
| **Sem assinatura** | não tem assinatura: a Loja avisa para instalar só se você confiar em quem o enviou |

Outras regras protegem você depois da instalação:

- **Mesma chave nas atualizações.** Uma versão nova precisa ser assinada com a mesma chave da instalada; uma atualização assinada com outra chave é recusada.
- **Catálogo assinado e revogação.** O catálogo do LordOS é assinado com a chave do LordOS e traz as chaves dos autores verificados e uma **lista de revogação** de chaves e pacotes. Um pacote revogado não é instalado, e um já instalado é desativado com uma mensagem clara.
- **Modificado por você.** Se você editar um app instalado, o LordOS o marca como modificado, mostra quais arquivos mudaram e pode restaurar o original. Se você compartilhar a sua versão, ela é assinada com a sua própria chave e dá o crédito ao autor original.

## Pacotes do Debian

O sistema base e todos os pacotes da seção Debian da Loja vêm do Debian e são protegidos pelas assinaturas do Debian:

- O índice de pacotes (`InRelease`) precisa ter uma assinatura OpenPGP válida de uma chave do chaveiro daquela fonte, instalado pelo pacote `debian-archive-keyring` do Debian. Índices vencidos, revogados ou repetidos são recusados.
- Cada lista de pacotes e cada `.deb` são conferidos contra o SHA-256 desse índice assinado antes de serem guardados ou passados ao `dpkg`.
- O LordOS faz essas verificações com o seu próprio motor de pacotes, e o `apt` continua funcionando ao lado com as mesmas chaves.

Os downloads usam HTTPS quando o endereço da fonte é `https://` (nunca rebaixados para HTTP, nem por um redirecionamento), e HTTP nos outros casos, como no `apt` com `deb.debian.org`: a segurança vem das assinaturas e dos hashes, então em HTTP comum alguém na rede pode ver quais pacotes você baixa, mas não consegue alterá-los.

## O repositório de atualizações do LordOS

Os sistemas instalados recebem as atualizações do próprio LordOS pelo repositório APT do LordOS em `lordos.org/apt`. O índice dele é assinado com a **chave do arquivo do LordOS**, uma chave OpenPGP Ed25519 separada da chave de versões, porque o APT precisa de OpenPGP:

- Chave pública: [`packaging/keys/lordos-archive-keyring.asc`](https://github.com/lordbasex/LordOS/blob/main/packaging/keys/lordos-archive-keyring.asc) no repositório, instalada em todo sistema como `/usr/share/keyrings/lordos-archive-keyring.gpg` pelo pacote `lordos`.
- Impressão digital: `1DDA F390 107D F264 FA8A  0918 3972 CC6D E4DA 2336`
- A fonte do repositório fica limitada a essa chave (`Signed-By`), e o índice e os pacotes dela são conferidos como os do Debian, acima.

Para conferir a chave à mão: `gpg --show-keys /usr/share/keyrings/lordos-archive-keyring.gpg` deve mostrar a impressão digital acima.

## O código do próprio LordOS

O desktop, a tela de login e cada app nativo são pacotes de JavaScript. Os tamanhos e o SHA-256 deles estão em `code-manifest.json`, assinado com a **chave de versões** acima. Desde a 0.8.2 o `lord-server`, que tem essa chave pública compilada, confere a assinatura e o SHA-256 de cada pacote antes de executá-lo. Um pacote que não confere não é executado: o Sobre mostra **Verificado** num sistema assinado, **Compilação de desenvolvimento** num sem assinatura e **Modificado — não carregado** quando algo não confere, e o LordOS mostra uma tela de recuperação em vez de uma em branco quando não resta nada para carregar.

## Chave do arquivo do Raspberry Pi

## Chave do arquivo do Raspberry Pi

No Raspberry Pi 5, o kernel e o firmware vêm do arquivo do Raspberry Pi (`archive.raspberrypi.com`), e todo o resto do Debian:

- Ao montar a imagem, a chave é tirada do pacote `raspberrypi-archive-keyring` do arquivo e a sua **impressão digital** é comparada com a que verificamos (`CF8A1AF502A2AA2D763BAE7E82B129927FA3303E`); se não coincidirem, a montagem para.
- A chave é instalada só para essa fonte (`signed-by=/etc/apt/keyrings/raspberrypi.pgp`), então ela não pode garantir pacotes de nenhum outro lugar.
- Um **pin** do apt dá ao arquivo uma prioridade baixa, exceto para o kernel, o firmware e seus utilitários: nenhum outro pacote pode ser substituído a partir dele.

## Como guardamos as chaves

- A metade privada da **chave de versões** fica só no computador do mantenedor, legível só pelo dono, com uma cópia de segurança offline. Ela nunca é guardada no repositório, numa máquina de compilação nem num servidor.
- A metade privada da **chave do LordOS** para os pacotes `.lpk` também fica fora do repositório, no computador do mantenedor. Hoje é uma chave de desenvolvimento; a chave definitiva para as versões públicas vai substituí-la no sistema e a troca será anunciada.
- A metade privada da **chave do arquivo do LordOS** também fica só no computador do mantenedor, com uma cópia de segurança.
- As chaves públicas são publicadas no repositório e neste site, para que qualquer pessoa possa conferi-las.

## Se uma chave for comprometida

Se algum dia suspeitarmos que uma chave privada foi exposta, vamos:

1. parar de usá-la e **revogá-la**: para os pacotes `.lpk`, pela lista de revogação do catálogo assinado; para as versões, removendo-a do script de instalação;
2. criar uma **chave nova** e anunciar a impressão digital dela no lordos.org (nesta página e na [Política de segurança](/legal/security/)) e no GitHub;
3. **assinar de novo** as versões e os pacotes afetados com a chave nova, e publicar um aviso explicando o que aconteceu e o que você deve conferir.

Se a impressão digital que você vê não coincidir com a desta página, não instale o arquivo. Avise-nos pelo [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt), com "Security" no assunto.
