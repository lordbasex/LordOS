*Esta página reemplaza al antiguo Aviso de cookies, que estaba en /legal/cookies/.*

## Sin cookies

lordos.org **no usa cookies**: ni de sesión, ni de analítica, ni de publicidad, ni de seguimiento, y tampoco de terceros. Por eso el sitio no muestra un cartel de cookies.

## Sin servicios de analítica

Nada corre en tu navegador para medirte:

- **En el sitio:** ningún servicio de analítica, ningún píxel de seguimiento, ninguna huella del navegador, ningún widget de redes sociales y ninguna fuente o script cargado desde otros servidores.
- **En la distribución LordOS:** sin telemetría, sin reportes de fallas y sin identificadores del equipo, y LordOS no manda nada extra para que lo contemos. Lo que sí contamos, desde el 12 de noviembre de 2026, son los pedidos que su búsqueda de actualizaciones hace igual a `lordos.org/apt` (más abajo). Las conexiones de red que hace LordOS están listadas en la [Política de privacidad](/legal/privacy/).

## Estadísticas de visitas

Para saber cómo crece el proyecto, contamos las visitas a partir de los registros estándar de solicitudes de la CDN del sitio (Amazon CloudFront). Son las solicitudes que tu navegador manda de todas formas para recibir cada página: no agregamos ningún código, cookie ni identificador para obtenerlas.

De cada solicitud guardamos:

- la página o el archivo pedido, y la hora;
- **tu dirección IP**, para contar visitantes distintos;
- el país, que CloudFront calcula por su cuenta a partir de la IP;
- la página desde la que llegaste (el referente), cuando tu navegador la manda;
- el agente de usuario del navegador, para saber el sistema (Windows, macOS, Linux, Android, iOS) y el tipo de dispositivo (computadora, celular, tableta) y para dejar a los bots fuera de los conteos;
- datos técnicos de la respuesta: su código de estado, su tamaño, cuánto tardó y qué ubicación de CloudFront la sirvió.

No usamos estos registros para averiguar quién sos, no los cruzamos con otros datos y no los vendemos ni los compartimos.

- **Quién las ve:** solo el equipo de LordOS, en un panel privado de AWS: visitas por día, semana, mes y año, las páginas más vistas, las visitas por país y por sistema y dispositivo, y, aparte, las computadoras con LordOS que buscan actualizaciones por día, país, arquitectura, versión y paquete. No publicamos estas estadísticas ni las compartimos con nadie.
- **Las búsquedas de actualizaciones de LordOS (desde el 12 de noviembre de 2026):** CloudFront escribe las solicitudes que hace LordOS a `lordos.org/apt` en los mismos registros, con los mismos datos. Hasta el 11 de noviembre de 2026 las dejábamos fuera de todos los conteos. Desde el 12 de noviembre de 2026 contamos, a partir de esos mismos registros, cuántas computadoras distintas buscan actualizaciones cada día, y cuántas descargan cada versión de LordOS y cada paquete (los paquetes propios de LordOS, las voces y las apps de la Tienda), por país, arquitectura (amd64 o arm64) y canal de actualizaciones (el repositorio normal o el de pruebas). La dirección IP se usa solo para no contar dos veces la misma computadora en un día (o en la semana o el mes que abarca un total), y no se combina con nada más. Del agente de usuario solo tomamos qué programa hizo la solicitud (el motor de paquetes de LordOS o apt, y su versión). No guardamos ningún identificador de la computadora ni tuyo, no hay cookies y no se instala nada: son las solicitudes que LordOS hace de todas formas para buscar actualizaciones. Estos registros se guardan los mismos 13 meses.
- **Cómo evitarlo:** desactivá **Buscar actualizaciones automáticamente** en Tienda → Actualizaciones. Cuando buscás actualizaciones o instalás algo a mano, LordOS igual pide el índice de paquetes (y los paquetes), y esas solicitudes se cuentan de la misma forma.
- **Por cuánto tiempo:** estos registros se guardan 13 meses y después se borran.

## Qué guarda el sitio en tu navegador

Se guardan dos preferencias chiquitas en el `localStorage` de tu navegador, solo en tu dispositivo:

| Clave | Valor | Para qué |
|---|---|---|
| `lordos-theme` | `light` o `dark` | recuerda el tema que elegiste con el botón de tema; en automático no se guarda nada y el sitio sigue a tu sistema |
| `lordos-lang-seen` | `1` | muestra una sola vez la sugerencia de leer el sitio en tu idioma |

Nunca se mandan a un servidor, no te identifican y no se comparten con nadie. Si tu navegador bloquea el almacenamiento, el sitio funciona igual; solo se olvida de lo que elegiste. Para borrarlas, eliminá los datos del sitio lordos.org en la configuración de tu navegador; las ventanas privadas las borran al cerrarse.

## Registros del servidor

El sitio se sirve a través de una red de distribución de contenido (CDN). Para entregarte cada página, tu navegador manda tu dirección IP y los datos habituales de la solicitud (la página pedida, la hora, el agente de usuario del navegador). Nuestro proveedor de alojamiento puede guardar los registros estándar de solicitudes de la CDN por seguridad, durante un tiempo limitado. Esos registros del proveedor, con direcciones IP, están separados de las estadísticas de arriba, y no los usamos para identificar ni perfilar a nadie.

## Cantidad de descargas

Los archivos de cada versión están alojados en GitHub Releases. GitHub lleva un contador de descargas por archivo; es público, es un único total por archivo y no dice nada de quién lo descargó. Si alguna vez mencionamos cuántas veces se descargó LordOS, ese número sale de esos contadores.

## Sitios de donación

Los botones de donación te llevan a PayPal o Mercado Pago (y, más adelante, Binance). Esos sitios ponen sus propias cookies, con sus propias políticas, una vez que estás ahí. Nada de ellos se carga en lordos.org antes de que hagas clic.

## Consultas

Mirá la [Política de privacidad](/legal/privacy/), o escribinos por [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es).
