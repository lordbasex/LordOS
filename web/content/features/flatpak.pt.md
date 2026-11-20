## Em poucas palavras

Desde o **LordOS 0.8.9.4**, os milhares de apps Linux do **Flathub** se instalam no LordOS, e **tudo é feito pela Loja**. Lá você ativa o Flathub, percorre os apps, vê o que cada um pede e o que o LordOS faz a respeito, e o instala, atualiza ou remove, sem terminal. **Configurações → Flatpak** mostra quanto os apps ocupam e remove o Flatpak por completo se você mudar de ideia.

O que o torna do LordOS: cada app do Flathub roda com **as regras próprias do LordOS**, não só com as dele. Ele ganha um sandbox próprio dentro da visão própria do LordOS, o seu **Firewall** e as suas restrições se aplicam a ele como a qualquer app, e **a sua pasta pessoal é somente leitura** para ele, exceto a pasta do próprio app e as suas pastas pessoais.

[[shot:0]]

Hoje funciona em **PCs de 64 bits** (x86-64); o Raspberry Pi vem depois.

## Tudo pela Loja

**Ativar.** Loja → **Flathub** → **Ativar o Flathub**. A Loja instala o `flatpak` do Debian como qualquer outro pacote (6 pacotes, 11 MB, nada é instalado até você apertar o botão), adiciona o Flathub com a chave que o LordOS traz e baixa o catálogo.

**Percorrer.** Os apps do Flathub aparecem na própria seção, em Descobrir e na busca, com os nomes, ícones e categorias do próprio Flathub. Por padrão a Loja mostra **só os apps que o Flathub verificou como publicados pelos próprios desenvolvedores**; um interruptor mostra o resto, marcados como “Não verificado”.

**Antes de instalar**, a página de cada app diz quem o publica, a licença e **o que ele declara**: internet, som, os seus arquivos, dispositivos, outros apps no barramento. Ao lado de cada item, o que o LordOS faz a respeito: o que você pode bloquear no Firewall, o que o LordOS limita e o que ele nunca concede.

[[shot:1]]

[[shot:2]]

**Instalar, atualizar, remover.** Um botão para cada coisa, com o progresso na página. Remover um app também remove os runtimes que nenhum outro app usa.

Os apps **ainda não suportados no LordOS** dizem isso antes de você tentar: os feitos com Chromium ou Electron são marcados e o botão Instalar fica desligado.

[[shot:3]]

## Configurações → Flatpak

As Configurações têm uma página **Flatpak** própria: se o Flatpak está instalado, se o Flathub aparece na Loja, quantos apps do Flathub há e quanto ocupam eles, os runtimes e os dados dos próprios apps, e um botão para abrir o Flathub na Loja.

**Remover o Flatpak por completo** leva todos os apps do Flathub, os runtimes que eles usavam, o próprio Flathub e o pacote `flatpak`, com os pacotes listados antes de qualquer coisa e a senha de administrador pedida, e opcionalmente manda os dados dos próprios apps para o Lixo. Depois, nada do Flatpak fica no computador.

[[shot:4]]

[[shot:5]]

## Na sua área de trabalho, como qualquer app

Os apps do Flathub aparecem na busca, no lançador e no **Abrir com** do Arquivos, e você pode fixá-los no **Dock**. Eles têm a moldura de janela do LordOS, a barra de menu e as notificações com o nome e o ícone do próprio app.

[[shot:6]]

[[shot:7]]

Quando você abre um arquivo com um app do Flathub, **ele recebe esse arquivo**, nada mais da sua home.

[[shot:8]]

GTK 4 e GTK 3, Qt e KDE, apps só de X11 e o sandbox próprio do Firefox funcionam:

[[shot:9]]

[[shot:10]]

[[shot:11]]

## Sob o seu Firewall e as suas regras

Cada app do Flathub roda no **seu próprio escopo do root** (`flatpak:<id>` no Firewall e no Monitor), então nada do que ele inicia consegue sair dali. Isso dá a ele tudo o que os apps Linux têm:

- **Sem internet, só rede local, regras por domínio, listas de bloqueio**, aplicadas pelo kernel, com a própria linha em Monitor → Rede.
- **Sem câmera, sem microfone, sem USB, sem Bluetooth** e **sem acesso aos seus arquivos**, em Configurações → Privacidade.
- **Sem notificações**, se você quiser silêncio.

Os downloads do próprio Flathub (o catálogo, as instalações e as atualizações) também são uma linha própria, **Flathub**, que você pode ver e bloquear.

[[shot:12]]

[[shot:13]]

## O que o LordOS nunca concede

Declare o que declarar um app, ou o que um arquivo de override pedir depois:

- **A sua pasta pessoal é somente leitura** para ele, exceto a pasta do próprio app (`~/.var/app/<id>`) e as suas pastas pessoais (Documentos, Downloads, Imagens…). Nenhum arquivo de inicialização, chave ou entrada de início automático pode ser escrito, criado ou colocado no lugar renomeando.
- **As pastas próprias do LordOS ficam ocultas**: as suas configurações, as regras do Firewall, as permissões, os lançadores e o início automático.
- **O barramento de sessão só pelo proxy de filtragem do Flatpak**, e nunca os nomes que rodam comandos fora do sandbox (`org.freedesktop.Flatpak`, systemd) ou guardam as suas senhas (o Secret Service), nem qualquer curinga que os cubra.
- **Sem barramento do sistema**, e nunca os serviços próprios do LordOS para se passar por eles.
- **Sem X11** para um app que você restringiu (apps X11 podem espiar uns aos outros).

## As decisões por trás

| Decisão | Por quê |
|---|---|
| Cada app na visão e no escopo próprios do LordOS | Para que o seu Firewall, as suas restrições e o Monitor vejam o app como ele é. Um `flatpak run` comum se tira sozinho de qualquer escopo que o LordOS lhe dê. |
| Home somente leitura, declare o que declarar o app | Um arquivo de inicialização ou uma entrada de início automático escritos por um app rodam fora de qualquer sandbox no seu próximo login. |
| Apps verificados por padrão | O Flathub confere que um app verificado vem do próprio desenvolvedor. O resto está a um interruptor de distância, marcado. |
| Instalado por usuário, nunca para todo o sistema | Sem root nem polkit: o auxiliar de root do Flatpak fica mascarado. |
| O portal próprio do Flatpak e o seu auxiliar de sessão mascarados | O que eles iniciam roda fora do sandbox do app. O LordOS serve ele mesmo os sub-sandboxes de que os apps do GNOME precisam, dentro do sandbox do mesmo app. |
| O catálogo é lido num sandbox próprio | O catálogo do Flathub é interpretado por um auxiliar pequeno sem rede nem arquivos, nunca pela própria área de trabalho. |
| Uma linha própria no Firewall para o Flathub | Os downloads ficam visíveis, e você pode pará-los como qualquer app. |
| Apps Electron e Chromium marcados | Os auxiliares deles precisam de uma forma de compartilhar o sandbox que o LordOS ainda não oferece. Melhor marcados que quebrados. |
| Nada em segundo plano | Nenhum serviço do Flatpak roda se não houver um app aberto; a parte própria do LordOS só existe enquanto o `flatpak` está instalado. |
| x86-64 primeiro | Testado ali; o Raspberry Pi vem depois. |

## O que foi testado

Numa máquina de teste em 20 de novembro de 2026, um **plano de testes automatizado** de 31 verificações roda toda vez (`server/tests/fwflatpak/flatpak.sh`): a linha do Flathub no Firewall, a identidade de cada app, o que nunca é concedido mesmo com um override, sem arquivos, sem internet, os sub-sandboxes, X11 com e sem restrição, e a restrição do microfone entre dois apps. Os apps testados à mão: **Editor de Texto do GNOME, Decibels, Shortwave, Meld, KCalc, Firefox e GNU Emacs**. Uma revisão de segurança de toda a mudança encontrou quatro problemas, todos corrigidos antes da versão.

**Ainda não:** apps baseados em Electron e Chromium; o Spotify só inicia com as próprias flags de Wayland (escolha de quem o desenvolve, não do LordOS); o Raspberry Pi.

O design completo, cada decisão e cada resultado dos testes estão em `docs/FLATPAK.md`.
