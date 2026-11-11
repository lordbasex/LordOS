LordOS no tiene cuentas, ni publicidad, ni rastreo, ni en el sitio ni en la distribución. Nadie vende ni comparte tus datos. El único dato personal que guardamos es la dirección IP en los registros de solicitudes del sitio, que usamos para estadísticas de visitas que solo ve el equipo de LordOS. Esta política tiene dos partes: el **sitio lordos.org** y la **distribución LordOS** que instalás en tu computadora.

## Parte 1: el sitio

### Qué guarda tu navegador

El sitio es un conjunto de páginas estáticas. **No usa cookies** y guarda solo dos valores en el `localStorage` de tu navegador, en tu dispositivo:

| Clave | Qué contiene | Para qué |
|---|---|---|
| `lordos-theme` | `light` o `dark` (nada si usás el automático) | para mostrar el tema que elegiste |
| `lordos-lang-seen` | `1` | para que la sugerencia "esta página también está en tu idioma" aparezca una sola vez |

Nunca salen de tu navegador y nosotros no podemos leerlos. Podés borrarlos cuando quieras desde la configuración de datos de sitios de tu navegador. Mirá el [Cookies y estadísticas](/legal/cookies-and-statistics/).

### Sin servicios de analítica, sin publicidad, sin código de terceros

El sitio no carga ningún servicio de analítica, ni publicidad, ni widgets de redes sociales, ni fuentes o scripts de otros servidores: todo lo que necesita se sirve desde lordos.org.

### Registros del servidor

El sitio está alojado en Amazon Web Services (S3 y CloudFront). Como en cualquier sitio, tu navegador envía tu dirección IP y los datos habituales de la solicitud (la página pedida, la hora, el agente de usuario del navegador) para recibir la página. Nuestro proveedor de alojamiento puede guardar registros estándar de solicitudes de la CDN por seguridad, durante un tiempo limitado. A partir de estos registros contamos las visitas para saber cómo crece el proyecto: páginas vistas, visitantes, países, sistemas (Windows, macOS, Linux…) y dispositivos, que solo ve el equipo de LordOS, con registros que incluyen tu dirección IP y se borran a los 13 meses. No los usamos para identificar ni perfilar a quienes nos visitan. Los detalles están en [Cookies y estadísticas](/legal/cookies-and-statistics/).

### Descargas en GitHub

La imagen ISO, la imagen para Raspberry Pi, el paquete `.deb` y sus firmas están alojados en GitHub Releases. Cuando los descargás, o cuando el script de instalación para Debian los busca, GitHub recibe tu solicitud según la [declaración de privacidad de GitHub](https://docs.github.com/es/site-policy/privacy-policies/github-general-privacy-statement). GitHub cuenta cuántas veces se descarga cada archivo de una versión y muestra esos totales en público; son números agregados, sin información sobre quién descargó. Son las únicas cifras de descargas que miramos.

### Donaciones

Los botones de donación te llevan a PayPal o a Mercado Pago (y, más adelante, a Binance). El pago se hace por completo en sus sitios: nunca vemos ni guardamos los datos de tu tarjeta o de tu cuenta. El procesador nos informa el monto, la fecha y el nombre o el email que le diste, que guardamos solo para nuestra contabilidad. Se aplican sus propias políticas: [PayPal](https://www.paypal.com/myaccount/privacy/privacyhub), [Mercado Pago](https://www.mercadopago.com.ar/privacidad). Mirá los [Términos de las donaciones](/legal/donations/).

### Formulario de contacto

Si nos escribís por [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es), en el sitio de iPERFEX, guardamos tu mensaje y los datos de contacto que nos des solo para responderte y hacer el seguimiento del tema.

## Parte 2: la distribución LordOS

### Sin telemetría

LordOS **no envía telemetría**: ni estadísticas de uso, ni informes de fallos, ni identificadores del dispositivo, ni comprobaciones que "llamen a casa", ni inicio de sesión en una cuenta. No se nos envía nada sobre vos ni sobre tu computadora.

### A qué se conecta LordOS de forma predeterminada

Estas son las únicas conexiones de red que hace una instalación nueva de LordOS sin que vos pidas algo:

| Conexión | Cuándo | Para qué | Cómo desactivarla |
|---|---|---|---|
| **DHCP y DNS** en tu red local | cuando hay un cable de red o una red Wi-Fi conectada | para obtener una dirección y resolver nombres; el DNS va a los servidores que te da tu red o los que configures | desconectate, o configurá una dirección y un DNS manuales en Ajustes → Red |
| **Hora de red (NTP)** con `systemd-timesyncd`, contra el grupo público de servidores NTP de Debian | cuando la computadora está conectada | para mantener la hora correcta | Ajustes → Fecha y hora → desactivá "Ajustar la fecha y la hora automáticamente" |
| **mDNS** (`avahi-daemon`, en las ISO y en la imagen para Raspberry Pi, con `libnss-mdns` para que se resuelvan los nombres `.local`) | solo en la red local | anuncia el nombre de la computadora para que otros dispositivos de tu red la encuentren, y responde consultas de nombres locales | `sudo systemctl disable --now avahi-daemon` |

La zona horaria nunca se deduce a partir de la red. Nada de esta tabla llega al proyecto LordOS.

### Solo cuando lo pedís

Estas conexiones se hacen solo después de una acción tuya:

- **Tienda y actualizaciones:** las listas de paquetes y los paquetes vienen de los servidores espejo de Debian que tengas en tus fuentes de apt (`deb.debian.org` y `security.debian.org` por defecto; en la Raspberry Pi, el kernel y el firmware vienen de `archive.raspberrypi.com`). La Tienda los actualiza solo cuando tocás **Buscar actualizaciones** (en la Tienda o en Ajustes → Actualizaciones) o instalás algo, y verifica cada lista con las firmas de Debian. Las actualizaciones de LordOS vienen de su repositorio firmado en `lordos.org/apt`, que se consulta al mismo tiempo. Desde 0.8.5 ese repositorio también trae el banner destacado de la Tienda (solo se toma de una fuente cuyo índice firmado dice que es de LordOS, y se controla contra sus hashes) y la lista de apps de Tienda → Inteligencia Artificial; las fuentes `https://` se descargan por HTTPS. Las apps `.lpk` propias de LordOS se instalan desde archivos que vos elegís; no hay un catálogo de apps `.lpk` en línea. La CDN del sitio registra estas solicitudes como cualquier otra, con tu dirección IP, durante 13 meses; no se usan para estadísticas (mirá [Cookies y estadísticas](/legal/cookies-and-statistics/)).
- **Apps desde los repositorios de sus fabricantes** (desde 0.8.5): instalar una app de Tienda → Inteligencia Artificial o Tienda → Comerciales cuyo fabricante tiene su propio repositorio primero te dice de quién es el repositorio que agrega y la huella de su clave, y después lo agrega, así que desde ese momento la Tienda y apt también descargan las listas de paquetes de ese fabricante. Quitar la app quita ese repositorio, salvo que lo use otra app instalada. A qué se conecta la app misma depende de su fabricante.
- **Paquetes de idioma:** agregar un idioma en Ajustes puede instalar sus diccionarios y fuentes desde los mismos servidores espejo de Debian.
- **Impresoras y escáneres:** Ajustes → Impresoras y escáneres le pregunta a la red local (mDNS) qué impresoras y escáneres hay, y habla solo con direcciones locales.
- **Mail, Calendario y Contactos:** se conectan solo a los servidores de las cuentas que agregues (IMAP, SMTP, CalDAV, CardDAV). Las imágenes remotas de los mensajes quedan bloqueadas hasta que tocás **Cargar imágenes**, y los píxeles de rastreo se quitan.
- **Listas de bloqueo** (desde 0.8.5, todas apagadas por defecto): cada lista que encendés en Ajustes → Privacidad → Listas de bloqueo, o que agregás por su dirección https, se descarga por HTTPS una vez por día desde la dirección https de quien la publica, con un descargador aislado. Con el pedido no se envía nada sobre vos más allá de lo que lleva cualquier descarga.
- **Archivos → Red y servidores** (desde 0.8.7): Archivos → Red está apagado hasta que abrís esa página. Entonces hace una pasada acotada por la red local, con mDNS y probando los puertos SMB y NFS en las redes privadas propias de esta computadora, y habla solo con direcciones locales. Conectarse al servidor y Remote Desk se conectan solo a los servidores que indicás.
- **Fondos en movimiento** (desde 0.8.7): un clip se descarga del repositorio del proyecto en `raw.githubusercontent.com` cuando lo elegís, y se comprueba contra un SHA-256 del paquete. GitHub ve ese pedido como ve cualquier descarga.
- **Reportes de hardware** (desde 0.8.7): LordOS no envía nada por su cuenta. Compartir en GitHub guarda un reporte y una página en tu computadora; si enviás el formulario de GitHub, con tu propia cuenta, el reporte queda público ahí. No tiene campos para números de serie, direcciones ni nombres de equipo, de usuario o de red.
- **Wi-Fi, Bluetooth y VPN:** solo las redes, los dispositivos y los servidores VPN que elijas.
- **El escritorio remoto** está **desactivado** por defecto (puerto 5673 cerrado). Solo escucha después de que lo activás en Ajustes → Compartir. En la Raspberry Pi, el escritorio remoto y el servidor SSH también quedan desactivados salvo que pongas un archivo interruptor `remote-desktop` o `ssh` en la partición de arranque.
- **Navegadores web y otras apps:** Firefox ESR, Chromium y cualquier app de Linux que instales son programas aparte, de sus propios proyectos. Hacen las conexiones que permita su configuración, según sus propias políticas de privacidad. Las apps `.lpk` aisladas llegan a la red solo si les das el permiso, y el firewall por app muestra y controla a qué se conecta cada una.
- **El script de instalación para Debian** (`curl -fsSL https://lordos.org/install.sh | sh`) se conecta solo a `api.github.com` (para encontrar la última versión), a `github.com` (para descargarla junto con su firma) y a tus servidores espejo de apt. No envía telemetría.

### Lo que queda en tu computadora

- **El mapa del mundo de Monitor** usa una base de datos de IP a país sin conexión (DB-IP Lite) que viene con LordOS. Averiguar adónde va una conexión nunca le pregunta a un servidor.
- **El mapa de almacenamiento de Limpieza** guarda nombres y tamaños de carpetas entre sesiones solo en esta computadora, legible solo por tu usuario. Lo podés apagar en los Ajustes de Limpieza o borrar en su Actividad.
- **Contraseñas:** la bóveda está cifrada en tu disco y su proceso no tiene acceso a la red. La comprobación de "contraseñas filtradas" es solo sin conexión.
- **Los ajustes, el correo, los archivos y los registros** quedan en tu computadora. Los registros no se suben a ningún lado.

## Parte 3: tus derechos, contacto y cambios

### Tus derechos

La distribución no recolecta datos personales. El sitio guarda las direcciones IP en sus registros de solicitudes durante 13 meses, como se explica arriba. Sobre lo que tengamos (esos registros, un email que nos mandaste, el registro de una donación), podés pedir verlo, corregirlo o borrarlo, salvo los registros que debamos conservar por ley. Si estás en Argentina, la Ley 25.326 de Protección de Datos Personales te da estos derechos, y podés presentar un reclamo ante la Agencia de Acceso a la Información Pública (AAIP). Si estás en la Unión Europea, en Brasil o en otro país con leyes similares (RGPD, LGPD), tenés los derechos que te dan esas leyes.

### Contacto

Consultas o pedidos sobre privacidad: [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es).

### Cambios

Si LordOS o el sitio empiezan a hacer un tipo de conexión nuevo, o si esta política cambia de cualquier otra forma, primero vamos a actualizar esta página y a cambiar la fecha al principio.
