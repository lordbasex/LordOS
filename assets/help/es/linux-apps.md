---
title: Apps Linux (GNOME y KDE)
section: desktop
order: 15
keywords: wayland, gtk, qt, firefox, apps linux, debian
---
# Apps Linux (GNOME y KDE)

Axia DE tiene su propio compositor Wayland, así que las apps de Linux —GTK (GNOME), Qt (KDE),
Firefox, Electron— se abren como ventanas más del escritorio.

- **Instalarlas:** desde la [Tienda](help:store), que trae las apps de Debian con sus íconos y
  descripciones.
- **Abrirlas:** Aplicaciones, el Dock o Buscar, como cualquier app.
- **Abrir un archivo con una de ellas:** en [Archivos](help:files), clic derecho → «Abrir con».
- **Portapapeles:** lo que copiás en una app Linux se pega en Axia DE y al revés, también imágenes.
- **Teclado y touchpad:** reciben las teclas (las combinaciones con ❖ siguen siendo de Axia DE, salvo en Lord Flow, donde [❖C, ❖V y otras llegan como Ctrl](help:shortcuts)) y
  los gestos de pellizcar y deslizar.

Las apps que dibujan su propio marco (por ejemplo las de GNOME con libadwaita) se muestran sin el
marco del tema. Por seguridad, Axia DE solo abre las apps instaladas (sus archivos `.desktop`), sin
ningún comando escrito a mano.

## Marco y colores

- **Chromium y Firefox** usan el marco de Axia DE (los semáforos del tema). Para volver a la barra de
  pestañas propia del navegador: en Chromium, menú ⋮ → Configuración → Apariencia → desactivar «Usar
  la barra de título y los bordes del sistema»; en Firefox, menú ☰ → Más herramientas → Personalizar
  barra de herramientas → desmarcar «Barra de título».
- **GTK y GNOME** siguen el modo oscuro o claro y el color de acento de Axia DE en vivo, y ponen los
  botones de la ventana a la izquierda, como los semáforos (en el tema Lord Classic, a la derecha). Las
  apps GTK 3 usan el tema «Axia DE»; su color de acento es fijo.
