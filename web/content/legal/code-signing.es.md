LordOS firma lo que publica para que puedas comprobar que un archivo viene del proyecto y que nadie lo cambió en el camino. Esta página explica qué se firma, con qué claves y cómo verificarlo vos mismo. Para avisarnos de un problema con una firma, mirá la [Política de seguridad](/legal/security/).

## Archivos de cada versión

Cada archivo de una versión en GitHub (la imagen ISO, la imagen para Raspberry Pi 5 y el paquete `.deb`) figura con su SHA-256 en un archivo llamado `SHA256SUMS`. `SHA256SUMS` está firmado con la **clave de versiones de LordOS**:

- `SHA256SUMS.sig` es el base64 de la firma **Ed25519** de 64 bytes sobre los bytes exactos de `SHA256SUMS`.
- Clave pública: [lordos-release.pub.pem](/lordos-release.pub.pem), también en el repositorio como [`tools/release/lordos-release.pub.pem`](https://github.com/lordbasex/LordOS/blob/main/tools/release/lordos-release.pub.pem).
- Huella (SHA-256 de la clave pública en DER):
  `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

El script de instalación para Debian (`lordos.org/install.sh`) lleva adentro la misma clave pública. Verifica la firma antes de descargar el `.deb`, después verifica el `.deb` contra `SHA256SUMS`, y no instala nada si alguna de las dos verificaciones falla.

Para verificar una descarga a mano, con OpenSSL 3, en la carpeta donde guardaste los archivos:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

El primer comando decodifica la firma, el segundo tiene que mostrar `Signature Verified Successfully` y el tercero tiene que mostrar `OK` para cada archivo que descargaste. Para verificar la clave en sí, compará su huella con la de arriba:

```
openssl pkey -pubin -in lordos-release.pub.pem -outform DER | sha256sum
```

## Apps de LordOS (.lpk) y la Tienda

Las apps nativas de LordOS vienen como paquetes `.lpk`. Un `.lpk` firmado lleva la lista de todos sus archivos con su SHA-256 y una firma **Ed25519** sobre esa lista. LordOS la verifica con su propio código en C al instalar la app y cada vez que la carga, así que un archivo cambiado se detecta.

La Tienda y el diálogo de instalación muestran uno de estos niveles de confianza:

| Nivel | Cuándo |
|---|---|
| **LordOS** | firmado con la clave oficial de LordOS, que viene compilada en el sistema (ningún archivo de tu carpeta personal puede agregar una clave oficial) |
| **Autor verificado** | firmado con una clave que figura en el catálogo firmado de LordOS |
| **Firmado por <nombre>** | firmado con una clave que LordOS no conoce |
| **Sin firma** | no tiene firma: la Tienda te avisa que la instales solo si confiás en quien te la dio |

Otras reglas te protegen después de instalar:

- **Misma clave para las actualizaciones.** Una versión nueva tiene que estar firmada con la misma clave que la instalada; una actualización firmada con otra clave se rechaza.
- **Catálogo firmado y revocación.** El catálogo de LordOS está firmado con la clave de LordOS y trae las claves de los autores verificados y una **lista de revocación** de claves y paquetes. Un paquete revocado no se instala, y uno ya instalado se desactiva con un mensaje claro.
- **Modificada por vos.** Si editás una app instalada, LordOS la marca como modificada, te muestra qué archivos cambiaron y puede restaurar la original. Si compartís tu versión, se firma con tu propia clave y le da el crédito al autor original.

## Paquetes de Debian

El sistema base y todos los paquetes de la sección Debian de la Tienda vienen de Debian y los protegen las firmas de Debian:

- El índice de paquetes (`InRelease`) tiene que tener una firma OpenPGP válida de una clave del llavero de esa fuente, que instala el paquete `debian-archive-keyring` de Debian. Los índices vencidos, revocados o repetidos se rechazan.
- Cada lista de paquetes y cada `.deb` se verifican contra el SHA-256 de ese índice firmado antes de guardarlos o pasárselos a `dpkg`.
- LordOS hace estas verificaciones con su propio motor de paquetes, y `apt` sigue funcionando al lado con las mismas claves.

Las descargas van por HTTPS cuando la dirección de la fuente es `https://` (nunca se rebajan a HTTP, ni siquiera por una redirección), y por HTTP en los demás casos, igual que con `apt` y `deb.debian.org`: la seguridad viene de las firmas y los hashes, así que por HTTP común alguien en la red puede ver qué paquetes descargás pero no puede cambiarlos.

## El repositorio de actualizaciones de LordOS

Los sistemas instalados reciben las actualizaciones propias de LordOS desde el repositorio APT de LordOS en `lordos.org/apt`. Su índice está firmado con la **clave del archivo de LordOS**, una clave OpenPGP Ed25519 distinta de la clave de versiones, porque APT necesita OpenPGP:

- Clave pública: [`packaging/keys/lordos-archive-keyring.asc`](https://github.com/lordbasex/LordOS/blob/main/packaging/keys/lordos-archive-keyring.asc) en el repositorio, instalada en cada sistema como `/usr/share/keyrings/lordos-archive-keyring.gpg` por el paquete `lordos`.
- Huella: `1DDA F390 107D F264 FA8A  0918 3972 CC6D E4DA 2336`
- La fuente del repositorio queda limitada a esa clave (`Signed-By`), y su índice y sus paquetes se verifican igual que los de Debian, más arriba.

Para verificar la clave a mano: `gpg --show-keys /usr/share/keyrings/lordos-archive-keyring.gpg` tiene que mostrar la huella de arriba.

## El código propio de LordOS

El escritorio, la pantalla de inicio de sesión y cada app nativa son paquetes de JavaScript. Sus tamaños y su SHA-256 están en `code-manifest.json`, firmado con la **clave de versiones** de arriba. Desde la 0.8.2 `lord-server`, que trae esa clave pública compilada, verifica la firma y el SHA-256 de cada paquete antes de ejecutarlo. Un paquete que no coincide no se ejecuta: Acerca de muestra **Verificado** en un sistema firmado, **Compilación de desarrollo** en uno sin firmar y **Modificado — no se cargó** cuando algo no coincide, y LordOS muestra una pantalla de recuperación en vez de una en blanco cuando no queda nada para cargar.

## Clave del archivo de Raspberry Pi

## Clave del archivo de Raspberry Pi

En la Raspberry Pi 5, el kernel y el firmware vienen del archivo de Raspberry Pi (`archive.raspberrypi.com`), y todo lo demás de Debian:

- Al armar la imagen se toma la clave del paquete `raspberrypi-archive-keyring` del archivo y se compara su **huella** con la que verificamos (`CF8A1AF502A2AA2D763BAE7E82B129927FA3303E`); si no coinciden, el armado se detiene.
- La clave se instala solo para esa fuente (`signed-by=/etc/apt/keyrings/raspberrypi.pgp`), así que no puede respaldar paquetes de ningún otro lado.
- Un **pin** de apt le da al archivo una prioridad baja, salvo para el kernel, el firmware y sus utilidades: ningún otro paquete se puede reemplazar desde ahí.

## Cómo guardamos las claves

- La mitad privada de la **clave de versiones** vive solo en la computadora del mantenedor, legible solo por su dueño, con una copia de respaldo sin conexión. Nunca se guarda en el repositorio, en una máquina de compilación ni en un servidor.
- La mitad privada de la **clave de LordOS** para los paquetes `.lpk` también se guarda fuera del repositorio, en la computadora del mantenedor. Hoy es una clave de desarrollo; la clave definitiva para las versiones públicas la va a reemplazar en el sistema y el cambio se va a anunciar.
- La mitad privada de la **clave del archivo de LordOS** también se guarda solo en la computadora del mantenedor, con una copia de respaldo.
- Las claves públicas se publican en el repositorio y en este sitio, para que cualquiera pueda verificarlas.

## Si una clave se ve comprometida

Si alguna vez sospechamos que se expuso una clave privada, vamos a:

1. dejar de usarla y **revocarla**: para los paquetes `.lpk`, con la lista de revocación del catálogo firmado; para las versiones, sacándola del script de instalación;
2. crear una **clave nueva** y anunciar su huella en lordos.org (en esta página y en la [Política de seguridad](/legal/security/)) y en GitHub;
3. **volver a firmar** las versiones y los paquetes afectados con la clave nueva, y publicar un aviso que explique qué pasó y qué tenés que revisar.

Si la huella que ves no coincide con la de esta página, no instales el archivo. Avisanos por [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es), con "Security" en el asunto.
