## En pocas palabras

Desde **LordOS 0.8.9.4**, las miles de apps de Linux de **Flathub** se instalan en LordOS, y **todo se hace desde la Tienda**. Ahí activás Flathub, recorrés sus apps, ves qué pide cada una y qué hace LordOS al respecto, y la instalás, actualizás o quitás, sin terminal. **Ajustes → Flatpak** muestra cuánto ocupan las apps y quita Flatpak por completo si cambiás de idea.

Lo que lo hace de LordOS: cada app de Flathub corre con **las reglas propias de LordOS**, no solo con las suyas. Tiene su propia caja de arena dentro de la vista propia de LordOS, tu **Firewall** y tus restricciones se le aplican como a cualquier app, y **tu carpeta personal es de solo lectura** para ella salvo su propia carpeta y tus carpetas personales.

[[shot:0]]

Hoy funciona en **PC de 64 bits** (x86-64); la Raspberry Pi viene después.

## Todo desde la Tienda

**Activarlo.** Tienda → **Flathub** → **Activar Flathub**. La Tienda instala `flatpak` desde Debian como cualquier otro paquete (6 paquetes, 11 MB, nada se instala hasta que apretás el botón), agrega Flathub con la clave que trae LordOS y descarga el catálogo.

**Recorrerlo.** Las apps de Flathub aparecen en su propia sección, en Descubrir y en la búsqueda, con los nombres, íconos y categorías del propio Flathub. De entrada la Tienda muestra **solo las apps que Flathub verificó como publicadas por sus propios desarrolladores**; un interruptor muestra el resto, marcadas como «No verificada».

**Primero la tienda, después lo que instalaste** (en desarrollo; la última versión publicada es la 0.8.9.5). La sección Flathub abre en su tienda: el inicio, las colecciones y las categorías. Su segunda columna empieza con **Instaladas**, con cuántas apps de Flathub tenés, y debajo las categorías, como la barra lateral de la Tienda tiene Instaladas entre sus páginas. Instaladas muestra solo tus apps de Flathub, con Actualizar las apps de Flathub, Quitar y Abrir. Tienda → **Instaladas** muestra todas tus apps, vengan de donde vengan, con un filtro por origen (LordOS, Debian, Flathub, apps web).

**Una tienda despejada, con Ajustes junto a Instaladas** (en desarrollo; la última versión publicada es la 0.8.9.5). La sección se llama **Apps Flathub** y abre directo en su banner. Solo verificadas, los rankings de Flathub, Actualizar y Desactivar están en sus **Ajustes**, justo debajo de Instaladas en la segunda columna; un ícono ↻ chico junto a la búsqueda actualiza el catálogo, y el campo de búsqueda ("Buscar apps Flathub") busca solo en Flathub. La búsqueda de Descubrir sigue buscando en todo.

**Populares, Tendencias y la app del día** (desde la 0.8.9.5). El inicio de Flathub en la Tienda abre con la app del día del propio Flathub y filas de Apps de la semana, Populares, Tendencias, Añadidas recientemente y Actualizadas recientemente. LordOS le pide esas listas a `flathub.org` como mucho una vez por día, con un pedido fijo que no dice nada de vos, y guarda solo los identificadores de las apps: los nombres, íconos y descripciones siguen saliendo del catálogo firmado de Flathub, y las reglas de instalación no cambian. Un interruptor en la Tienda lo apaga; sin internet, las filas simplemente no están.

**Antes de instalar**, la página de cada app dice quién la publica, su licencia y **lo que declara**: internet, sonido, tus archivos, dispositivos, otras apps en el bus. Al lado de cada cosa, lo que hace LordOS al respecto: lo que podés bloquear en el Firewall, lo que LordOS limita y lo que nunca da.

[[shot:1]]

[[shot:2]]

**Instalar, actualizar, quitar.** Un botón para cada cosa, con el progreso en la página. Quitar una app también quita los runtimes que ninguna otra app usa.

**Las apps de Chromium y Electron** (desde la 0.8.9.5) se instalan como cualquier otra. En la 0.8.9.4 estaban marcadas como «todavía no compatibles»; ahora las cajas de arena internas que necesitan sus ayudantes se sirven dentro de la propia caja de arena de la app.

## Ajustes → Flatpak

Ajustes tiene una página **Flatpak** propia: si Flatpak está instalado, si Flathub se muestra en la Tienda, cuántas apps de Flathub hay y cuánto ocupan ellas, sus runtimes y sus propios datos, y un botón para abrir Flathub en la Tienda.

**Quitar Flatpak por completo** se lleva todas las apps de Flathub, los runtimes que usaban, el propio Flathub y el paquete `flatpak`, con los paquetes listados antes de que pase nada y la contraseña de administrador pedida, y opcionalmente manda los datos propios de las apps a la Papelera. Después no queda nada de Flatpak en la computadora.

[[shot:3]]

[[shot:4]]

## En tu escritorio, como cualquier app

Las apps de Flathub aparecen en la búsqueda, en el lanzador y en **Abrir con** de Archivos, y las podés fijar en el **Dock**. Tienen el marco de ventana de LordOS, su barra de menú y sus notificaciones con el nombre y el ícono de la propia app.

[[shot:5]]

[[shot:6]]

Cuando abrís un archivo con una app de Flathub, **recibe ese archivo**, nada más de tu home.

[[shot:7]]

GTK 4 y GTK 3, Qt y KDE, las apps solo de X11 y la caja de arena propia de Firefox funcionan:

[[shot:8]]

[[shot:9]]

[[shot:10]]

## Bajo tu Firewall y tus reglas

Cada app de Flathub corre en **su propio scope de root** (`flatpak:<id>` en el Firewall y en Monitor), así que nada de lo que arranca puede salir de ahí. Eso le da todo lo que tienen las apps de Linux:

- **Sin internet, solo red local, reglas por dominio, listas de bloqueo**, aplicadas por el kernel, con su propia fila en Monitor → Red.
- **Sin cámara, sin micrófono, sin USB, sin Bluetooth** y **sin acceso a tus archivos**, en Ajustes → Privacidad.
- **Sin notificaciones**, si querés silencio.

Las descargas del propio Flathub (el catálogo, las instalaciones y las actualizaciones) también son una fila propia, **Flathub**, que podés ver y bloquear.

[[shot:11]]

[[shot:12]]

## Lo que LordOS nunca da

Declare lo que declare una app, o lo que pida después un archivo de override:

- **Tu carpeta personal es de solo lectura** para ella, salvo su propia carpeta (`~/.var/app/<id>`) y tus carpetas personales (Documentos, Descargas, Imágenes…). Ningún archivo de inicio, clave ni entrada de autoarranque se puede escribir, crear ni poner en su lugar renombrando.
- **Las carpetas propias de LordOS están ocultas**: tus ajustes, las reglas del Firewall, los permisos, los lanzadores y el autoarranque.
- **El bus de sesión solo a través del proxy filtrante de Flatpak**, y nunca los nombres que corren comandos fuera de la caja de arena (`org.freedesktop.Flatpak`, systemd) o guardan tus contraseñas (el Secret Service), ni ningún comodín que los cubra.
- **Sin bus del sistema**, y nunca los servicios propios de LordOS para hacerse pasar por ellos.
- **Sin X11** para una app que restringiste (las apps de X11 se pueden espiar entre sí).

## Las decisiones detrás

| Decisión | Por qué |
|---|---|
| Cada app en la vista y el scope propios de LordOS | Para que tu Firewall, tus restricciones y Monitor vean la app como lo que es. Un `flatpak run` común se saca solo de cualquier scope que le dé LordOS. |
| Home de solo lectura, declare lo que declare la app | Un archivo de inicio o una entrada de autoarranque escritos por una app corren fuera de toda caja de arena en tu próximo inicio de sesión. |
| Apps verificadas de entrada | Flathub comprueba que una app verificada viene de su propio desarrollador. El resto está a un interruptor de distancia, marcado. |
| Instalado por usuario, nunca para todo el sistema | Sin root ni polkit: el ayudante de root de Flatpak está enmascarado. |
| El portal propio de Flatpak y su ayudante de sesión enmascarados | Lo que arrancan corre fuera de la caja de arena de la app. LordOS sirve él mismo los sub-sandbox que necesitan las apps de GNOME, dentro de la caja de arena de la misma app. |
| El catálogo se lee en una caja de arena propia | El catálogo de Flathub lo interpreta un ayudante chico sin red ni archivos, nunca el propio escritorio. |
| Una fila propia en el Firewall para Flathub | Las descargas se ven, y las podés frenar como a cualquier app. |
| Los ayudantes de Chromium y Electron, en la caja de arena de la app | Sus cajas de arena internas las arranca un ayudante chico dentro de la vista de la app, con los mismos límites de la app y ninguno menos (desde la 0.8.9.5). |
| Nada en segundo plano | Ningún servicio de Flatpak corre si no hay una app abierta; la parte propia de LordOS existe solo mientras `flatpak` está instalado. |
| Primero x86-64 | Ahí está probado; la Raspberry Pi viene después. |

## Qué se probó

En una máquina de prueba el 20 de noviembre de 2026, un **plan de pruebas automatizado** de 31 verificaciones corre cada vez (`server/tests/fwflatpak/flatpak.sh`): la fila de Flathub en el Firewall, la identidad de cada app, lo que nunca se da aun con un override, sin archivos, sin internet, los sub-sandbox, X11 con y sin restricción, y la restricción del micrófono entre dos apps. Las apps probadas a mano: **Editor de texto de GNOME, Decibels, Shortwave, Meld, KCalc, Firefox y GNU Emacs**. Una revisión de seguridad de todo el cambio encontró cuatro problemas, todos corregidos antes de la versión.

**Desde la 0.8.9.5:** las apps basadas en Chromium y Electron, probadas el 22 de noviembre de 2026 con Whatsie (Qt WebEngine), Chromium y FreeTube (Electron); el video con DRM (Widevine) no está disponible para ellas. **Todavía no:** Spotify arranca solo con sus propios flags de Wayland (decisión de quien la desarrolla, no de LordOS); la Raspberry Pi.

El diseño completo, cada decisión y cada resultado de las pruebas están en `docs/FLATPAK.md`.
