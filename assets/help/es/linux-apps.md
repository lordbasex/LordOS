---
title: Apps Linux (GNOME y KDE)
section: desktop
order: 15
keywords: wayland, gtk, qt, firefox, apps linux, debian
---
# Apps Linux (GNOME y KDE)

LordOS tiene su propio compositor Wayland, así que las apps de Linux —GTK (GNOME), Qt (KDE),
Firefox, Electron— se abren como ventanas más del escritorio.

- **Instalarlas:** desde la [Tienda](help:store), que trae las apps de Debian con sus íconos y
  descripciones.
- **Abrirlas:** Aplicaciones, el Dock o Buscar, como cualquier app.
- **Abrir un archivo con una de ellas:** en [Archivos](help:files), clic derecho → «Abrir con».
- **Portapapeles:** lo que copiás en una app Linux se pega en LordOS y al revés, también imágenes.
- **Teclado y touchpad:** reciben las teclas (las combinaciones con ❖ siguen siendo de LordOS, salvo en Lord Flow, donde [❖C, ❖V y otras llegan como Ctrl](help:shortcuts)) y
  los gestos de pellizcar y deslizar.

Las apps que dibujan su propio marco (por ejemplo las de GNOME con libadwaita) se muestran sin el
marco del tema. Por seguridad, LordOS solo abre las apps instaladas (sus archivos `.desktop`), sin
ningún comando escrito a mano.

## Marco y colores

- **Chromium y Firefox** usan el marco de LordOS (los semáforos del tema). Para volver a la barra de
  pestañas propia del navegador: en Chromium, menú ⋮ → Configuración → Apariencia → desactivar «Usar
  la barra de título y los bordes del sistema»; en Firefox, menú ☰ → Más herramientas → Personalizar
  barra de herramientas → desmarcar «Barra de título».
- **Brave** (y otros navegadores hechos sobre Chromium) dibuja su propia barra. Con su tema «Clásico»
  los botones de ventana son los suyos, no los de LordOS. Configuración → Apariencia → «Navegadores:
  pestañas en la barra de título» también cambia los perfiles de Brave (con Brave cerrado; si está
  abierto, se aplica cuando se cierra); en el propio Brave: menú ☰ → Configuración → Apariencia → Tema → GTK.
  Para usar el marco de LordOS: activar ahí «Usar la barra de título y los bordes del sistema».
- **GTK y GNOME** siguen el modo oscuro o claro y el color de acento de LordOS en vivo, y ponen los
  botones de la ventana a la izquierda, como los semáforos (en el tema Lord Classic, a la derecha). Las
  apps GTK 3 usan el tema «LordOS»; su color de acento es fijo.
