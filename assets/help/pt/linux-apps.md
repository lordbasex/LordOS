---
title: Apps Linux (GNOME e KDE)
section: desktop
order: 15
keywords: wayland, gtk, qt, firefox, apps linux, debian
---
# Apps Linux (GNOME e KDE)

O LordOS tem o seu próprio compositor Wayland, então os apps Linux —GTK (GNOME), Qt (KDE),
Firefox, Electron— abrem como mais uma janela da área de trabalho.

- **Instalar:** pela [Loja](help:store), que traz os apps do Debian com ícones e descrições.
- **Abrir:** Aplicativos, o Dock ou Pesquisar, como qualquer app.
- **Abrir um arquivo com um deles:** em [Arquivos](help:files), botão direito → “Abrir com”.
- **Área de transferência:** o que você copia em um app Linux cola no LordOS e vice-versa, também
  imagens.
- **Teclado e touchpad:** recebem as teclas (as combinações com ❖ continuam sendo do LordOS, exceto no Lord Flow, onde [❖C, ❖V e outras chegam como Ctrl](help:shortcuts)) e os
  gestos de pinça e deslizar.

Os apps que desenham a própria moldura (por exemplo os do GNOME com libadwaita) aparecem sem a
moldura do tema. Por segurança, o LordOS só abre apps instalados (os arquivos `.desktop`), nunca
um comando digitado.

## Moldura e cores

- **Chromium e Firefox** usam a moldura do LordOS (os semáforos do tema). Para voltar à barra de abas
  do próprio navegador: no Chromium, menu ⋮ → Configurações → Aparência → desativar «Usar barra de
  título e bordas do sistema»; no Firefox, menu ☰ → Mais ferramentas → Personalizar barra de
  ferramentas → desmarcar «Barra de título».
- **Brave** (e outros navegadores feitos sobre o Chromium) desenha sua própria barra. Com o tema
  «Clássico» os botões da janela são os dele, não os do LordOS. Configurações → Aparência →
  «Navegadores: abas na barra de título» também altera os perfis do Brave (feche o Brave antes, ou
  vale quando o Brave fechar); no próprio Brave: menu ☰ → Configurações → Aparência → Tema → GTK.
  Para usar a moldura do LordOS: ative ali «Usar barra de título e bordas do sistema».
- **GTK e GNOME** seguem o modo escuro ou claro e a cor de destaque do LordOS ao vivo, e põem os
  botões da janela à esquerda, como os semáforos (à direita no tema Lord Classic). Os apps GTK 3 usam o
  tema «LordOS»; a cor de destaque deles é fixa.
