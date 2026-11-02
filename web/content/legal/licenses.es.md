## LordOS: la licencia MIT

El código propio de LordOS (el entorno de escritorio, el servidor gráfico y compositor Wayland, las apps nativas, el SDK, los temas, las herramientas y este sitio) se publica bajo la **licencia MIT**:

> Copyright (c) 2026 iPERFEX y los colaboradores de LordOS.
>
> Se concede permiso, sin cargo, a cualquier persona que obtenga una copia de este software y de los archivos de documentación asociados, para usar el software sin restricciones, incluidos sin limitación los derechos de usar, copiar, modificar, fusionar, publicar, distribuir, sublicenciar y/o vender copias, con la condición de incluir el aviso de copyright y este aviso de permiso en todas las copias o partes sustanciales del software. El software se ofrece "tal cual", sin garantía de ningún tipo.

Esto es un resumen; el que se aplica es el [texto completo de la licencia](https://github.com/lordbasex/LordOS/blob/main/LICENSE) (en inglés).

La licencia MIT cubre el código. No da derechos sobre el nombre LordOS, el logo de la corona ni Lumo, la mascota: mirá la [Política de marca](/legal/trademark/).

## Componentes de terceros incluidos en LordOS

Estos componentes están en el repositorio de LordOS (`third_party/` y `assets/`) y mantienen sus propias licencias. La lista completa está en [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md).

| Componente | Para qué se usa | Licencia |
|---|---|---|
| QuickJS | motor de JavaScript de la interfaz | MIT |
| stb_image, stb_image_write, stb_truetype | decodificación y codificación de imágenes, rasterizado de fuentes | MIT o dominio público |
| Monocypher 4 | criptografía de la bóveda de contraseñas y del llavero | CC0-1.0 o BSD-2-Clause |
| EFF large wordlist | generador de frases de contraseña | CC BY 3.0 |
| Contraseñas comunes de SecLists | comprobación de contraseñas débiles | MIT |
| React, react-reconciler, scheduler | el modelo de componentes de la interfaz (incluido al compilar) | MIT |

## Fuentes y datos

| Datos | Para qué se usan | Licencia |
|---|---|---|
| Plus Jakarta Sans 2.7.1 | el logotipo de LordOS, en la interfaz, en los archivos del logo y en este sitio | SIL Open Font License 1.1 ([texto de la licencia](/licenses/PlusJakartaSans-OFL.txt)) |
| Inter | texto de la interfaz | SIL Open Font License 1.1 |
| JetBrains Mono | texto de la terminal y del código | SIL Open Font License 1.1 |
| DB-IP Lite Country | búsqueda de IP a país sin conexión en Monitor | CC BY 4.0, geolocalización IP de [DB-IP](https://db-ip.com) |
| Natural Earth | el mapa del mundo de Monitor | dominio público |
| Unicode Character Database y CLDR | composición de texto, texto bidireccional, nombres de emoji | Unicode License v3 |

## Paquetes de Debian

LordOS está construido sobre Debian. El sistema base, el kernel Linux, Mesa, PipeWire, CUPS, BlueZ, el firmware, las fuentes, los navegadores preinstalados y todos los paquetes que se instalan desde la Tienda son paquetes de Debian (y, en la Raspberry Pi, el kernel y el firmware vienen del repositorio de Raspberry Pi). Cada uno se distribuye bajo su propia licencia, en su mayoría licencias de software libre como la GPL, la LGPL, la MIT, la BSD y la Apache; parte del firmware es no libre y redistribuible.

En un sistema instalado, el copyright y la licencia de cada paquete están en `/usr/share/doc/<package>/copyright`. El código fuente de los paquetes de Debian está disponible en Debian ([sources.debian.org](https://sources.debian.org)).

LordOS enlaza dinámicamente algunas bibliotecas del sistema: libwayland y libxkbcommon (MIT), Mesa (MIT), OpenSSL 3 (Apache-2.0), libcrypt (LGPL-2.1+), Poppler, cairo, FreeType y fontconfig (GPL-2.0-or-later, LGPL, FTL), libarchive (BSD-2-Clause) y FFmpeg y libva (LGPL-2.1+; la compilación de FFmpeg de Debian es GPL-2.0-or-later). Los procesos auxiliares `lord-doc` y `lord-media` enlazan Poppler y el FFmpeg de Debian, así que esos dos binarios, tal como se distribuyen, quedan cubiertos por la GPL; el código fuente de LordOS con licencia MIT del que se compilan es compatible con ella.

Desde la 0.8.7 `lordos` también depende de FreeRDP (Apache-2.0), rclone (MIT), fuse3 (LGPL-2.1, GPL-2.0) y el cliente OpenSSH (BSD). El ítem Disk Formats de la Tienda, que no viene preinstalado, trae ntfs-3g y exfatprogs (GPL-2.0), dosfstools (GPL-3.0) y, en amd64, apfs-dkms (GPL-2.0). Los fondos en movimiento son de dominio público, CC0, CC BY y CC BY-SA, con créditos en Ajustes y en su repositorio.

## Código fuente

El código fuente de LordOS está en [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). Si necesitás el código fuente correspondiente de un componente GPL que distribuimos y no lo encontrás, escribinos por [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es).
