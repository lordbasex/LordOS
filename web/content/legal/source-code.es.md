## Qué ofrecemos

Las imágenes de LordOS (las ISO en vivo para PC, la imagen para máquinas virtuales y la imagen para Raspberry Pi) contienen paquetes de Debian 13 y, en la Raspberry Pi, algunos paquetes del archivo de Raspberry Pi y nuestra propia compilación del kernel Linux de Raspberry Pi. Muchos de esos programas están bajo la Licencia Pública General de GNU (GPL), la LGPL u otra licencia copyleft. Esas licencias te dan derecho al código fuente de los programas, y lo ponemos a tu disposición.

**Oferta por escrito.** Durante al menos tres años después de la última vez que publiquemos una imagen o un paquete, cualquiera que lo tenga puede pedirnos el código fuente completo y correspondiente de cualquier programa GPL, LGPL u otro copyleft que contenga, en forma legible por máquina, como descarga y sin costo (o en un medio físico, por no más de lo que cueste hacer y enviar ese medio). La oferta cubre todas las imágenes y paquetes de LordOS publicados desde la 0.8.0.

## Dónde está el código fuente

Junto a cada imagen, en su página de versión, hay un archivo con el nombre de la imagen terminado en `.sources.tsv` (por ejemplo `lordos-live-0.8.8-amd64.sources.tsv`). Lista cada paquete fuente de esa imagen con su versión exacta, los paquetes binarios que salen de él y un enlace a ese código fuente exacto:

| Origen | Dónde está el código fuente |
|---|---|
| Debian | [snapshot.debian.org](https://snapshot.debian.org), que guarda todas las versiones que Debian publicó: `snapshot.debian.org/package/<fuente>/<versión>/` |
| Archivo de Raspberry Pi (imagen para Raspberry Pi) | nuestra propia copia del código fuente exacto en `lordos.org/apt/pool/rpi/source/<letra>/<fuente>/` (FFmpeg, kms++ y, en desarrollo después de LordOS 0.8.8.8 y publicados con la próxima actualización de nuestro repositorio de paquetes, el kernel y el firmware de Raspberry Pi de la imagen 0.8.8); la lista enlaza su `.dsc`, y los archivos que nombra están al lado. Si no, el archivo de Raspberry Pi |
| LordOS | el [repositorio de LordOS](https://github.com/lordbasex/LordOS) en la etiqueta de la versión. Los ayudantes `lord-doc` y `lord-media` enlazan compilaciones GPL de Poppler y FFmpeg; su código fuente es ese mismo repositorio |
| El kernel de Raspberry Pi | el [árbol de Linux](https://github.com/raspberrypi/linux) de Raspberry Pi en el commit que indica la lista, con nuestra configuración en `tools/rpi/build-kernel.sh` (misma etiqueta) y el `.config` completo en `/boot/config-<versión>` dentro de la imagen. En desarrollo después de LordOS 0.8.8.8, y publicada con la próxima actualización de nuestro repositorio de paquetes, también guardamos una copia propia: el árbol en ese commit, el `.config` exacto y el script de compilación, listados en [lordos.org/apt/pool/rpi/source/l/linux-lordos-rpi-2712/SOURCE](https://lordos.org/apt/pool/rpi/source/l/linux-lordos-rpi-2712/SOURCE) |
| rclone | la versión oficial de rclone, sin cambios: [github.com/rclone/rclone](https://github.com/rclone/rclone) en la etiqueta de esa versión |

En un sistema instalado, la licencia de cada paquete está en `/usr/share/doc/<paquete>/copyright`. Los detalles están en [docs/SOURCE-OFFER.md](https://github.com/lordbasex/LordOS/blob/main/docs/SOURCE-OFFER.md) (en inglés).

## Cómo pedirlo

Si un enlace no funciona o querés que te mandemos el código fuente, abrí un [pedido de código fuente](https://github.com/lordbasex/LordOS/issues/new?template=source-request.yml) en GitHub. Indicá el archivo que recibiste de nosotros (por ejemplo `lordos-live-0.8.8-amd64.iso`) y el programa o los paquetes fuente que querés, o "todo". Si no tenés cuenta de GitHub, usá [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es) con los mismos datos. Te respondemos ahí con un enlace de descarga.

Las imágenes de la 0.8.8 y anteriores se publicaron antes de que existieran estas listas; sus listas se están generando a partir de las imágenes publicadas. Mientras una lista no esté en su página de versión, la oferta de arriba vale igual.
