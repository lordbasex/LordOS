# Axia DE sobre wlroots

## Decisão

O `axia-server` tem um compositor Wayland escrito do zero (`server/src/wl`, `kms.c`, `input.c`, `tp/`, `platform/seat.c`, cerca de 10 mil linhas). Ele implementa à mão uns 25 protocolos e deixa de fora vários que os apps atuais usam: `xdg-activation`, `layer-shell`, `session-lock`, `ext-image-copy`, `linux-drm-syncobj`, além de partes do protocolo de subsuperfícies (`set_sync`/`set_desync` e a ordem de empilhamento são ignorados). O plano é trocar essa camada pelo **wlroots 0.20** (pacote `wlroots` do Fedora 44) e manter todo o resto: o motor de UI, o shell em React, os apps nativos e os auxiliares.

A aparência não depende dessa troca. Blur, sombras e cantos arredondados são desenhados pelo motor de UI (`server/src/canvas.c`, `server/src/ui/paint.c`) sobre o que o shell pinta; as janelas dos apps são nós `NODE_SURFACE` da árvore de UI, com moldura e ordem decididas pelo React. O wlroots entra embaixo disso: vídeo (DRM/KMS e sessão), entrada (libinput), protocolos, buffers e foco.

## Prova de conceito

`server/poc/axia-wlr.c` (`make -C server build/axia-wlr`, fora do `all`, nunca instalado) é um compositor wlroots em que o motor de UI pinta a tela inteira, como o `engine-test` faz: papel de parede, barra de menus e Dock com o blur da DE, e cada janela `xdg-shell` como uma View com moldura, sombra e um `NODE_SURFACE`. As funções `wl_paint` e `wl_paint_popups` (as mesmas que o motor chama no `axia-server`) leem a textura de cada superfície pelo wlroots (`wlr_texture_read_pixels`) e a copiam no canvas; o quadro vai para a saída como uma textura. Usa `xdg-decoration` em modo servidor e `xdg-activation`.

```sh
sudo dnf install wlroots          # a única dependência que o Fedora Workstation não traz
axia-wlr -s firefox               # num console de texto (DRM), ou dentro de outra sessão Wayland
```

Ctrl+Alt+Backspace encerra. Fontes e papel de parede vêm de `AXIA_ROOT` (padrão `/usr/libexec/axia`).

### Resultado (2 de novembro de 2026, VM com a ISO Fedora 44 da Axia)

- O motor desenha igual: papel de parede, barra e Dock translúcidos com blur, janela com cantos, sombra e semáforos.
- O Firefox abre sem barra de título própria: aceita a decoração pelo servidor.
- Cliques, teclado, a lista de sugestões da barra de endereço (popup) e arrastar a janela funcionam. Testado: abrir aba com o "+", digitar na barra de endereço.
- Um cuidado encontrado no caminho: o Firefox desenha o conteúdo numa subsuperfície e espera o `frame_done` dela antes do quadro seguinte. Todo commit de qualquer superfície precisa agendar um quadro; sem isso a janela congela, embora receba a entrada.

Limites da prova de conceito: só CPU, uma saída em escala 1, sem QuickJS nem o shell React, sem Xwayland, sem redimensionar janelas.

## Plano de migração

1. **Backend wlroots no `axia-server`**, ao lado do atual (`WLR=1`): `wl_display`, backend, renderer e alocador do wlroots; o laço de eventos do servidor passa a ser o do `wl_display`. Saída única e caminho de CPU, como na prova de conceito.
2. **Protocolos e janelas**: a API de `server/src/wl/wl.h` (cerca de 60 funções usadas pelo resto do servidor) reimplementada sobre `wlr_xdg_shell`, `wlr_subcompositor`, `wlr_seat`, `wlr_data_device`, `wlr_primary_selection`, `wlr_xwayland`, `wlr_pointer_constraints`, `wlr_relative_pointer`, `wlr_text_input_v3` e os demais que o `wl.c` já anuncia.
3. **Entrada**: `wlr_cursor` e libinput no lugar de `input.c` e do driver próprio de touchpad (`tp/`); os gestos do shell passam a vir de `wlr_pointer_gestures`.
4. **Várias telas e escala**: `wlr_output_layout` e `wlr_output_management` no lugar de `outputs.c`/`kms.c`.
5. **GPU**: compor com o renderer do wlroots (texturas dos apps sem cópia pela CPU). O blur atrás das janelas dos apps, hoje pendente, pode vir da scenefx (pacote `scenefx` do Fedora, feita para o wlroots 0.20).
6. **Remoção** de `server/src/wl`, `kms.c`, `input.c`, `tp/` e `platform/seat.c` quando houver paridade; a suíte de renderização (`engine-test`, hashes de arrasto) continua valendo, porque o motor não muda.

## Referências

- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) e o exemplo `tinywl` do mesmo repositório.
- [scenefx](https://github.com/wlrfx/scenefx), a cena com efeitos do SwayFX.
