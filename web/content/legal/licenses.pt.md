## LordOS: a licença MIT

O código próprio do LordOS (o ambiente de desktop, o servidor gráfico e compositor Wayland, os apps nativos, o SDK, os temas, as ferramentas e este site) é publicado sob a **licença MIT**:

> Copyright (c) 2026 iPERFEX e os colaboradores do LordOS.
>
> É concedida permissão, gratuitamente, a qualquer pessoa que obtenha uma cópia deste software e dos arquivos de documentação associados, para lidar com o software sem restrições, incluindo, sem limitação, os direitos de usar, copiar, modificar, mesclar, publicar, distribuir, sublicenciar e/ou vender cópias dele, desde que o aviso de copyright e este aviso de permissão sejam incluídos em todas as cópias ou partes substanciais do software. O software é fornecido "no estado em que se encontra", sem garantia de nenhum tipo.

Isto é um resumo; o que vale é o [texto completo da licença](https://github.com/lordbasex/LordOS/blob/main/LICENSE), em inglês.

A licença MIT cobre o código. Ela não concede direitos sobre o nome LordOS, o logo da coroa nem sobre Lumo, o mascote: veja a [Política de marca](/legal/trademark/).

## Componentes de terceiros incluídos no LordOS

Estes componentes ficam no repositório do LordOS (`third_party/` e `assets/`) e mantêm as suas próprias licenças. A lista completa está em [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md).

| Componente | Usado para | Licença |
|---|---|---|
| QuickJS | motor JavaScript da interface | MIT |
| stb_image, stb_image_write, stb_truetype | decodificação e codificação de imagens, rasterização de fontes | MIT ou domínio público |
| Monocypher 4 | criptografia do cofre de senhas e do chaveiro | CC0-1.0 ou BSD-2-Clause |
| Lista de palavras grande da EFF | gerador de frases-senha | CC BY 3.0 |
| Senhas comuns do SecLists | verificação de senhas fracas | MIT |
| React, react-reconciler, scheduler | o modelo de componentes da interface (incluído na compilação) | MIT |

## Fontes e dados

| Dados | Usado para | Licença |
|---|---|---|
| Plus Jakarta Sans 2.7.1 | o logotipo LordOS, na interface, nos arquivos do logo e neste site | SIL Open Font License 1.1 ([texto da licença](/licenses/PlusJakartaSans-OFL.txt)) |
| Inter | textos da interface | SIL Open Font License 1.1 |
| JetBrains Mono | textos do terminal e de código | SIL Open Font License 1.1 |
| DB-IP Lite Country | consulta offline de IP para país no Monitor | CC BY 4.0, geolocalização de IP por [DB-IP](https://db-ip.com) |
| Natural Earth | o mapa-múndi do Monitor | domínio público |
| Unicode Character Database e CLDR | layout de texto, texto bidirecional, nomes de emoji | Unicode License v3 |

## Pacotes do Debian

O LordOS é construído sobre o Debian. O sistema base, o kernel Linux, o Mesa, o PipeWire, o CUPS, o BlueZ, os firmwares, as fontes, os navegadores pré-instalados e todos os pacotes instalados pela Loja são pacotes do Debian (e, no Raspberry Pi, o kernel e o firmware vêm do repositório do Raspberry Pi). Cada um é distribuído sob a sua própria licença, em geral licenças de software livre como GPL, LGPL, MIT, BSD e Apache; alguns firmwares não são livres, mas podem ser redistribuídos.

Em um sistema instalado, o copyright e a licença de cada pacote estão em `/usr/share/doc/<pacote>/copyright`. O código-fonte dos pacotes do Debian está disponível no próprio Debian ([sources.debian.org](https://sources.debian.org)).

O LordOS se vincula dinamicamente a algumas bibliotecas do sistema: libwayland e libxkbcommon (MIT), Mesa (MIT), OpenSSL 3 (Apache-2.0), libcrypt (LGPL-2.1+), Poppler, cairo, FreeType e fontconfig (GPL-2.0-or-later, LGPL, FTL), libarchive (BSD-2-Clause) e FFmpeg e libva (LGPL-2.1+; a compilação do FFmpeg do Debian é GPL-2.0-or-later). Os auxiliares `lord-doc` e `lord-media` se vinculam ao Poppler e ao FFmpeg do Debian, por isso esses dois binários, como distribuídos, ficam cobertos pela GPL; o código-fonte do LordOS sob a licença MIT, a partir do qual eles são compilados, é compatível com ela.

Desde a 0.8.7 o `lordos` também depende do FreeRDP (Apache-2.0), rclone (MIT), fuse3 (LGPL-2.1, GPL-2.0) e do cliente OpenSSH (BSD). O item Disk Formats da Loja, que não vem pré-instalado, traz ntfs-3g e exfatprogs (GPL-2.0), dosfstools (GPL-3.0) e, em amd64, apfs-dkms (GPL-2.0). Os fundos em movimento são de domínio público, CC0, CC BY e CC BY-SA, com créditos em Ajustes e no repositório deles.

Desde a 0.8.8 o `lordos` também depende do polkit (`polkitd` e `pkexec`, LGPL-2.0 ou posterior), para que os apps Linux que pedem direitos de administrador recebam o aviso de administrador do LordOS, e recomenda o libnss-mdns (LGPL-2.1 ou posterior), que resolve os nomes `.local`. As imagens acrescentam o wireless-regdb (ISC), o banco de dados das regras de Wi-Fi de cada país, e o libnss-mdns; as ISOs também trazem o firmware-brcm80211, firmware de Wi-Fi Broadcom não livre mas redistribuível, da seção `non-free-firmware` do Debian (a imagem para Raspberry Pi já o tinha para o Wi-Fi da placa).

## Código-fonte

O código-fonte do LordOS está em [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). Se você precisar do código-fonte correspondente de um componente GPL que distribuímos e não o encontrar, escreva pelo [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt).
