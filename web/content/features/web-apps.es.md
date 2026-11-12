## En pocas palabras

El **LordOS Web Runtime** es un entorno de ejecución nativo para aplicaciones web, escrito en C y hecho sobre WPE WebKit, que instala y ejecuta apps web y PWA como apps de LordOS, sin un navegador externo. Tu chat, tu correo o tu pizarra dejan de ser una pestaña más: tienen su propia ventana con el marco de LordOS, su propio ícono en el Dock con el contador de no leídos, sus notificaciones con el nombre y el sonido de la app, su barra de menú y, si querés, siguen funcionando en segundo plano detrás de un ícono junto al reloj.

No es un cliente nativo de ningún servicio. **LordOS no cambia los sitios ni sus PWA; los ejecuta tal como los sirven sus sitios.** La página de la ventana es la interfaz web del propio servicio, sin cambios, la misma que muestra un navegador. LordOS pone la ventana, la integración con el escritorio y la seguridad alrededor. Donde a WPE WebKit le falta algo que un escritorio necesita (el portapapeles, arrastrar y soltar, los menús Edición y del clic derecho, claro y oscuro), `lord-web` agrega pequeños scripts propios y documentados (los que manejan tu portapapeles y lo que soltás hablan con LordOS solo desde un mundo aislado que la página no ve), sin tocar el código ni el contenido del sitio.

[[shot:6]]

Es nuevo en **LordOS 0.8.8.8**, publicada el 12 de noviembre de 2026, el último lanzamiento. Vive en un paquete opcional, `lordos-web`: sin él, el resto de LordOS funciona igual, y la Tienda lo instala por vos con tu primera app web.

[[figure:web-runtime]]

## Qué lo hace distinto

| | Un sitio en una ventana del escritorio | Una PWA instalada desde un navegador | LordOS Web Runtime |
|---|---|---|---|
| Qué ejecuta la página | Una vista web simple en una ventana | El motor y el perfil de ese navegador | Un proceso `lord-web` propio por app y perfil, sobre el WPE WebKit de Debian |
| Cuentas | Normalmente una | Los perfiles de ese navegador | Todos los perfiles que quieras, cada uno con sus cookies y sus datos |
| Caja de arena del motor | Depende del envoltorio | La de ese navegador | La de WebKit (bubblewrap + seccomp), siempre activa |
| Permisos | Depende del envoltorio | Los guarda el navegador | Se preguntan una vez por app y quedan en Ajustes → Apps web |
| Firewall y límites | — | — | La fila propia de la app en el firewall, límites de memoria y CPU |
| Escritorio | Una ventana | Algo de integración | Ícono y contador en el Dock, notificaciones con el sonido de la app, barra de menú, menú Edición, menú del clic derecho de LordOS, bandeja, claro y oscuro |

La columna del medio describe cómo funcionan en general esas apps, no un producto en particular; LordOS no cambia cómo un navegador ejecuta sus propias PWA.

## Cómo conseguir apps web: la sección Apps web de la Tienda

La Tienda tiene una sección **Apps web**, un banner y un estante en Descubrir, y las apps web aparecen en la búsqueda y en su categoría. Cada tarjeta dice "App web", así las distinguís de las apps de LordOS y de las apps de Linux.

[[shot:0]]

- **Un catálogo de 37 apps viene con LordOS**: chat, correo y calendario, oficina, notas y tareas, música y video, gráficos. Cada una pasó el control automático de LordOS el 11 de noviembre de 2026 (cargó y dio su nombre). Para ser honestos con dónde estamos: **por ahora solo una está marcada como probada en LordOS**, una app de chat que el mantenedor usa con dos cuentas. Las demás dicen "Sin probar todavía" hasta que alguien las pruebe, y la Tienda te avisa antes de instalar una.
- **Cualquier sitio, desde su dirección.** Escribí una dirección en *Agregar desde una dirección* y apretá Revisar. LordOS abre la página aparte, en una sesión descartable sin ninguno de tus datos, lee el manifiesto de app web y el ícono del sitio, y te muestra la dirección real. Podés cambiarle el nombre. **No se instala nada hasta que apretás Instalar.** Solo se aceptan direcciones seguras (`https`), y el control no se deja llevar de un sitio público a tu propia red.
- **La primera app web instala el motor.** La primera vez, la Tienda te dice que el motor web de LordOS hace falta una sola vez, muestra la descarga y el espacio en disco del paquete `lordos-web`, pide la contraseña de administrador, lo instala y después instala tu app, sin otro clic. Desde la segunda app no hay paso del motor.

[[shot:1]]

*Los nombres, logos e íconos de productos y servicios del catálogo son marcas de sus respectivos titulares y se usan solo para identificar los servicios. LordOS no está afiliado a ellos ni cuenta con su respaldo o patrocinio; "probada en LordOS" solo quiere decir que el proyecto LordOS comprobó que el sitio funciona en el LordOS Web Runtime.*

[[shot:2]]

Cada app web tiene su página en la Tienda, como cualquier otra app: siempre al día (es la web), el día en que LordOS la probó, lo que ocupa en el disco, su memoria (la cifra típica medida, o la real mientras corre), quién la hace, el sitio verificado, el motor, los permisos que puede pedir, sus perfiles, el modo en segundo plano y sus límites conocidos.

[[shot:3]]

[[shot:4]]

## Perfiles: varias cuentas a la vez

Un **perfil** es una cuenta aparte de la misma app: la personal y la del trabajo, o dos números del mismo chat. Cada perfil tiene su propia ventana, sus propias cookies, sus propios datos guardados y su propio inicio de sesión; nada de un perfil lo puede leer otro. Los podés abrir al mismo tiempo.

- Agregá, renombrá o quitá perfiles en **Ajustes → Apps web**. Al quitar uno, te pregunta si se borran también sus datos.
- Cada perfil puede tener un **color**. Con más de un perfil, el marco de la ventana muestra una franja fina de ese color, el título y el selector de ventanas dicen "App — Perfil", y el punto del perfil aparece sobre el ícono en las ventanas, las notificaciones y Ajustes.
- El Dock abre el primer perfil; los demás están en el menú del clic derecho del ícono ("Nueva ventana: Trabajo"). También le podés dar a cada perfil **su propio ícono en el Dock**.

[[shot:7]]

## Privacidad y seguridad

- **Un proceso por app y perfil.** Cada perfil de cada app corre en su propio proceso `lord-web`, y solo `lord-server` lo arranca, desde una ruta fija, con una app y un perfil que revisó antes. Un cuelgue o un error afecta solo a esa ventana.
- **La caja de arena de WebKit nunca se apaga.** Los procesos de la página y de red corren dentro de la caja de arena propia de WebKit (bubblewrap y seccomp). `lord-web` se niega a arrancar si alguien intenta apagarla.
- **Su propia fila en el firewall y sus límites.** Cada app web tiene su propia identidad en el firewall de LordOS y su propia fila en el Monitor, así que la podés permitir o bloquear como a cualquier app. Sus perfiles comparten límites de memoria y CPU a la medida de tu computadora (memoria hasta tres cuartos de la total, entre 1 y 16 GB, y la mitad de los núcleos, al menos dos).
- **Permisos preguntados una vez por app.** Las notificaciones, la cámara, el micrófono, compartir pantalla y tu ubicación se preguntan la primera vez que la app los necesita, y tu respuesta queda en **Ajustes → Apps web**, donde la podés cambiar (Permitir, Preguntar, Denegar). Solo las páginas propias de la app los reciben: una página de inicio de sesión de otro sitio, o un marco de otro origen, queda denegada y nunca se pregunta por ella.
- **Ninguna API de LordOS llega a la página.** Una página no recibe `lord.*`, ni el broker, ni scripts con privilegios.
- **Copiar y pegar.** Lo que copia una página llega a tu portapapeles solo desde las páginas propias de la app, y solo justo después de tu propia tecla, clic o toque en su ventana. LordOS lee tu portapapeles para la página solo cuando pegás.
- **Archivos que soltás o pegás.** Una página recibe solo archivos de tus lugares (tu carpeta personal, tus discos, `/tmp`), nunca de una carpeta oculta ni de un archivo oculto (así `~/.ssh` o `~/.config` nunca se mandan), y nunca a través de un enlace puesto en la ruta; como mucho 20 archivos de hasta 64 MB cada uno. Un arrastre o una copia que viene de una app contenida (una app aislada de LordOS o un Flatpak) le da a una app web solo texto, nunca archivos.

## Editar todos los días, como en un navegador

A WPE WebKit le faltan varias cosas que un navegador hace por vos, así que `lord-web` las agrega:

- **Arrastrar para seleccionar texto**, doble clic para una palabra, triple clic para un párrafo, Shift+clic para extender.
- **Copiar y pegar en los dos sentidos**, texto en cualquier idioma, **imágenes** (una imagen que copiaste se pega en un chat; una página puede copiar una imagen a tu portapapeles) y **archivos** (copiás en Archivos y pegás en la app web).
- **Arrastrar archivos** desde Archivos a la página, como en un navegador: también funcionan las páginas de chat que muestran una zona para soltar cuando llega un archivo.
- **El menú Edición** en la barra de menú: Deshacer, Rehacer, Cortar, Copiar, Pegar y Seleccionar todo. Ajustes… en el menú de la app abre los ajustes de esa app, y Ayuda abre el sitio en tu navegador.
- **El menú del clic derecho propio de LordOS**: Cortar, Copiar, Pegar, Seleccionar todo, Copiar dirección del enlace, Abrir enlace en el navegador y Copiar imagen, según lo que haya bajo el puntero. Una página con su propio menú (una pizarra, un editor) lo conserva.
- Recargá con Ctrl+R o F5, andá para atrás y para adelante con Alt+Izquierda/Derecha o los botones laterales del mouse, y hacé zoom con Ctrl+más, menos y 0. Los enlaces a otros sitios se abren en tu navegador; las descargas van a tu carpeta Descargas con una notificación.

[[shot:8]]

[[shot:9]]

## Claro y oscuro

Las apps web siguen el modo claro u oscuro del escritorio, en vivo: cambiá el Modo oscuro en el Centro de control y una app web abierta cambia al instante. Una página que le pregunta al navegador `matchMedia('(prefers-color-scheme: dark)')`, como hacen muchas apps con un tema "Sistema", recibe la respuesta de LordOS. En modo oscuro tampoco hay un destello blanco mientras carga una página.

**El límite honesto:** WPE WebKit 2.48 no le da a LordOS ninguna forma de fijar el esquema de colores de la página, así que una página que elige sus colores solo con CSS (reglas `@media (prefers-color-scheme: dark)`) queda clara hasta que un WebKit más nuevo lo permita. Esos sitios suelen tener un ajuste de tema propio: elegí Oscuro ahí. La página de la app en la Tienda lo dice cuando se sabe.

[[shot:12]]

## Rendimiento

- **Las ventanas ocultas se pausan.** Una ventana minimizada, en otro escritorio, cerrada al segundo plano o tapada por completo por otra ventana le avisa a su página que está oculta: la página deja de dibujar, sus animaciones se pausan y sus temporizadores corren como mucho una vez por segundo, mientras su red, su audio y sus notificaciones siguen. Medido en nuestra máquina virtual de desarrollo con una página de prueba exigente: **de 115 % de una CPU a 0,4 %** minimizada, y el compositor de 5 % a 0,3 %. Al volver a mostrarse, dibuja al instante.
- **Presión de memoria.** WebKit suelta las cachés que puede reconstruir cuando una página crece (cerca de un tercio de tu memoria, hasta 1 GB), y también las imágenes decodificadas y el código compilado pasada la mitad (cerca de 1,5 GB), revisado cada 30 segundos; un modelo de caché más liviano no deja ningún proceso web ocioso.
- **Límites a la medida de tu computadora**, como arriba, compartidos por los perfiles de la app.
- **El Monitor muestra la memoria real** de cada app web, contada una sola vez por memoria proporcional (PSS) entre sus procesos. Una cuenta de la app de chat probada usó unos 810 MB en la primera medición.

## Ajustes → Apps web

Cada app web instalada tiene su página: su sitio y de dónde vino, sus perfiles (abrir, renombrar, color, quitar, con los datos que guarda cada uno en el disco), sus permisos, *Seguir funcionando en segundo plano* (cerrar la ventana la deja funcionando, así siguen llegando sus notificaciones), *Mostrar en la barra de menús* (un ícono junto al reloj con Abrir y Salir), *Mostrar los no leídos en el Dock*, *Cada perfil con su propio ícono en el Dock*, el sonido de las notificaciones (Automático, que pone el de LordOS solo cuando el sitio no pone uno propio; el del sitio; el de LordOS; o ninguno), el ícono (el del propio sitio o el mosaico con letra de LordOS) y Desinstalar.

[[shot:10]]

[[shot:11]]

## Límites conocidos

- **Sin probar todavía:** 36 de las 37 apps del catálogo todavía no están marcadas como probadas en LordOS. Cargaron, pero puede que algunas funciones no anden.
- **Sin notificaciones push con la app cerrada.** WPE no tiene servicio de push en Linux, así que las notificaciones llegan solo mientras la app corre: dejala en segundo plano para eso.
- **Sin video ni música con DRM** (Widevine no está disponible para WPE en Linux), así que los catálogos protegidos de los servicios de música y video no se reproducen.
- **Las llamadas** dependen del WebRTC de WPE WebKit y del servicio; puede que todavía no anden y no se probaron.
- **El modo oscuro solo por CSS** queda claro, como arriba.
- **Deshacer** funciona solo en páginas con su propio historial (un editor enriquecido); un campo de texto simple no puede deshacer.
- **El contador de no leídos** sale del título de la página ("(3) …"), la única fuente que da WPE; no se muestra la foto de quien manda la notificación.
- Algunos servicios se pasan a otra dirección después de iniciar sesión; la página funciona, pero sus enlaces pueden abrirse entonces en tu navegador.
- *Agregar desde una dirección* revisa a dónde lleva un sitio en cada paso, pero un sitio igual podría responderle una dirección a ese control y otra a WebKit (DNS rebinding). Corre en una sesión descartable vacía, sin ninguno de tus datos, y solo lee un nombre y un ícono.

## Leé el código

- [server/src/web](https://github.com/lordbasex/LordOS/tree/main/server/src/web): `lord-web`, el LordOS Web Runtime
- [server/src/ui/js_webapps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_webapps.c): el administrador en `lord-server`
- [assets/webapps/catalog.json](https://github.com/lordbasex/LordOS/blob/main/assets/webapps/catalog.json): el catálogo
- [apps/store/webapps.js](https://github.com/lordbasex/LordOS/blob/main/apps/store/webapps.js) y [apps/settings/WebApps.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/settings/WebApps.jsx): la Tienda y Ajustes
- [docs/WEBAPPS.md](https://github.com/lordbasex/LordOS/blob/main/docs/WEBAPPS.md): todo el diseño, con cada control

*WhatsApp, Telegram, Discord y los demás nombres de productos, logos e íconos que se muestran son marcas de sus respectivos titulares. Se usan solo para identificar los servicios; LordOS no está afiliado a ellos ni cuenta con su respaldo o patrocinio. Mirá la [política de marca](/legal/trademark/).*
