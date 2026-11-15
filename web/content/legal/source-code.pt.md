## O que oferecemos

As imagens do LordOS (as ISOs ao vivo para PC, a imagem para máquinas virtuais e a imagem para Raspberry Pi) contêm pacotes do Debian 13 e, no Raspberry Pi, alguns pacotes do arquivo do Raspberry Pi e a nossa própria compilação do kernel Linux do Raspberry Pi. Muitos desses programas estão sob a Licença Pública Geral GNU (GPL), a LGPL ou outra licença copyleft. Essas licenças dão a você direito ao código-fonte dos programas, e nós o disponibilizamos.

**Oferta por escrito.** Por pelo menos três anos depois da última vez que publicarmos uma imagem ou um pacote, qualquer pessoa que o tenha pode nos pedir o código-fonte completo e correspondente de qualquer programa GPL, LGPL ou outro copyleft que ele contenha, em forma legível por máquina, como download e sem custo (ou em uma mídia física, por no máximo o que custar fazer e enviar essa mídia). A oferta cobre todas as imagens e pacotes do LordOS publicados desde a 0.8.0.

## Onde está o código-fonte

Ao lado de cada imagem, na página da versão, há um arquivo com o nome da imagem terminado em `.sources.tsv` (por exemplo `lordos-live-0.8.8-amd64.sources.tsv`). Ele lista cada pacote-fonte dessa imagem com a versão exata, os pacotes binários gerados a partir dele e um link para esse código-fonte exato:

| Origem | Onde está o código-fonte |
|---|---|
| Debian | [snapshot.debian.org](https://snapshot.debian.org), que guarda todas as versões que o Debian já publicou: `snapshot.debian.org/package/<fonte>/<versão>/` |
| Arquivo do Raspberry Pi (imagem para Raspberry Pi) | a nossa própria cópia do código-fonte exato em [lordos.org/apt/pool/rpi/source](https://lordos.org/apt/pool/rpi/source/) (FFmpeg, kms++); senão, o arquivo do Raspberry Pi |
| LordOS | o [repositório do LordOS](https://github.com/lordbasex/LordOS) na tag da versão. Os auxiliares `lord-doc` e `lord-media` usam compilações GPL do Poppler e do FFmpeg; o código-fonte deles é esse mesmo repositório |
| O kernel do Raspberry Pi | a [árvore do Linux](https://github.com/raspberrypi/linux) do Raspberry Pi no commit indicado na lista, com a nossa configuração em `tools/rpi/build-kernel.sh` (mesma tag) e o `.config` completo em `/boot/config-<versão>` dentro da imagem |
| rclone | a versão oficial do rclone, sem alterações: [github.com/rclone/rclone](https://github.com/rclone/rclone) na tag dessa versão |

Em um sistema instalado, a licença de cada pacote está em `/usr/share/doc/<pacote>/copyright`. Os detalhes estão em [docs/SOURCE-OFFER.md](https://github.com/lordbasex/LordOS/blob/main/docs/SOURCE-OFFER.md) (em inglês).

## Como pedir

Se um link não funcionar ou você quiser que enviemos o código-fonte, abra um [pedido de código-fonte](https://github.com/lordbasex/LordOS/issues/new?template=source-request.yml) no GitHub. Indique o arquivo que você recebeu de nós (por exemplo `lordos-live-0.8.8-amd64.iso`) e o programa ou os pacotes-fonte que deseja, ou "tudo". Se você não tiver conta no GitHub, use o [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt) com as mesmas informações. Respondemos por lá com um link de download.

As imagens da 0.8.8 e anteriores foram publicadas antes de essas listas existirem; as listas delas estão sendo geradas a partir das imagens publicadas. Enquanto uma lista não estiver na página da versão, a oferta acima vale da mesma forma.
