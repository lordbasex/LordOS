## En pocas palabras

LordOS tiene un escritorio propio, pero abajo sigue siendo Debian. Tenés a mano las decenas de miles de programas del archivo de Debian, y las apps gráficas hechas con GTK o Qt, más navegadores como Chromium y Firefox, se abren como ventanas de LordOS, con el mismo marco, el mismo Dock y el mismo modo claro u oscuro que las apps nativas.

[[shot:0]]

El truco es un idioma en común. Las apps de Linux hablan **Wayland**, un protocolo para decir "acá está mi ventana, acá están mis píxeles, mandame el mouse y el teclado". El servidor gráfico de LordOS también lo habla. Así, una app que no sabe nada de LordOS igual puede vivir en su escritorio, como un viajero que habla el idioma del lugar y no necesita guía.

[[diagram:linux]]

## Cómo se instalan

- **La Tienda** muestra las apps de Debian por categoría, con búsqueda, y te muestra un plan (qué se va a instalar o quitar) antes de hacer nada. También instala las apps `.lpk` propias de LordOS y archivos `.deb` locales.
- **El motor de paquetes propio de LordOS** lee tus fuentes de apt, verifica los índices firmados con su propio código OpenPGP, resuelve las dependencias y controla cada archivo descargado contra el SHA-256 del índice firmado. Recién entonces `dpkg`, la herramienta propia de Debian, desempaqueta y configura el paquete. La base de datos de dpkg la sigue escribiendo solo dpkg, así que `apt` y `dpkg` siguen funcionando normalmente en la Terminal.
- **La Terminal** funciona como en cualquier sistema Debian. Cada pestaña ya tiene las variables de Wayland configuradas, así que arrancar un programa GTK o Qt desde ahí simplemente funciona.

[[shot:1]]

## Cómo se abren

Las apps con un archivo `.desktop` aparecen en el lanzador de apps, en el menú de inicio del tema Lord Classic y en Buscar, con sus íconos. Tocar una pasa por una puerta en `lord-server` que es angosta a propósito:

1. La interfaz solo puede pasar el **id** de un archivo `.desktop` que ya esté instalado en `/usr/share/applications` o `~/.local/share/applications`, nunca un comando.
2. `lord-server` vuelve a leer ese archivo, verifica que sea una aplicación real, visible y que no sea de terminal, y arma el comando a partir de su línea `Exec` según la especificación, sin shell y sin argumentos de quien llama. "Abrir con" en Archivos puede agregar exactamente un archivo, solo si la app declara los tipos de archivo que maneja y el archivo está en tu carpeta personal o en un disco que montaste.
3. La app arranca en su propio cgroup, `app-lordos-<id>.scope`, creado por root, así el Monitor, el firewall por app y sus restricciones saben a qué app pertenece cada proceso. Si le restringiste a la app tus archivos, acá es también donde recibe su propia vista: sus propios espacios de nombres, una carpeta personal vacía con solo sus propias carpetas, y Landlock.

El JavaScript de LordOS no puede escribir archivos `.desktop`, así que la interfaz no puede inventar un comando: solo puede elegir una app que ya está instalada.

## Cómo se ven y se comportan

- **El marco.** Las apps que le piden al compositor que dibuje su marco (las apps Qt, foot, Chromium con la barra de título del sistema) reciben el marco del tema. Las apps GTK 3 como Firefox lo reciben por un segundo protocolo de decoración que LordOS también habla. Las apps GTK 4 que dibujan su propia barra la conservan, y LordOS les recorta la sombra.
- **Navegadores con una sola barra.** Desde 0.8.4, Chromium y Firefox preinstalados dibujan una sola barra con sus pestañas y los botones de la ventana adentro, del lado que usa el tema de LordOS (a la izquierda en Lord Flow, a la derecha en Lord Classic), y siguen en vivo un cambio de tema o de modo claro/oscuro. Un interruptor en Ajustes → Apariencia, "Navegadores: pestañas en la barra de título", vuelve a poner los dos bajo el marco de LordOS. El compositor respeta el marco que pida cada app, incluso si cambia con la ventana abierta.
- **Claro y oscuro, en vivo.** LordOS sirve por su cuenta el portal de escritorio "Settings" en el bus de la sesión, así que las apps GTK 4 y libadwaita siguen el modo, el color de acento y el orden de los botones. Las apps GTK 3 lo siguen a través del archivo de claves de GSettings, y LordOS trae un tema GTK que combina. Las dos cambian en vivo cuando cambiás de modo. De la 0.8.8 a la 0.8.8.7, la última versión publicada, las apps GTK 4 quedaban en modo oscuro con acento azul sin importar lo que eligieras; está arreglado en la 0.8.8.8 (en desarrollo).
- **Abrir y guardar con el diálogo de LordOS** (desde la 0.8.8): las apps de Linux que no tienen un diálogo de archivos propio piden uno por el portal estándar FileChooser, y LordOS responde con su propio diálogo, que se ve y funciona como Archivos, dentro de la carpeta personal.
- **Permisos de administrador con el aviso de LordOS** (desde la 0.8.8): una app que los pide con `pkexec`, como un lanzador que se actualiza solo, recibe el aviso de administrador de LordOS, que nombra la app y el comando, en lugar de frenar con el error 127. Los programas que le piden a polkit por D-Bus todavía no encuentran agente.
- **Los clics llegan a la superficie correcta** (desde la 0.8.8): el compositor ahora respeta la región de entrada que fija una app, así que las pestañas, la barra y las páginas de Firefox vuelven a responder al mouse.
- **Un solo portapapeles.** El texto que copiás en una app de Linux se pega en LordOS y al revés; las imágenes siguen disponibles después de cerrar la app de origen y aparecen en el historial del portapapeles (❖V).
- **Seleccionar para copiar, clic del medio para pegar.** Desde 0.8.3 la selección primaria funciona entre apps Wayland, y entre ellas y las apps X11. Desde 0.8.5 la app que copió también ve su propia copia en el portapapeles, como dice el protocolo Wayland. En la 0.8.8.8 (en desarrollo) también participan los campos de texto propios de LordOS y la Terminal.
- **Los videos mantienen la pantalla encendida** (desde 0.8.5): un reproductor de video de Linux puede pedir que la pantalla no se oscurezca ni se bloquee mientras reproduce (`zwp_idle_inhibit_manager_v1`), y los temporizadores de inactividad esperan mientras su ventana está a la vista.
- **Nítidas en cualquier escala.** Cada pantalla es un `wl_output` aparte, y la escala fraccionaria permite que GTK 4, Qt 6 y Chromium se dibujen nítidos al 125–175 %.
- **Gestos del touchpad.** El driver de touchpad propio de LordOS les pasa a las apps de Linux los gestos de pellizcar, deslizar y mantener, así que el zoom con pellizco funciona en los navegadores.
- **Juegos y apps de GPU.** Con una GPU, las apps le pueden pasar a LordOS buffers de GPU directamente (`linux-dmabuf`), usar pantalla completa de verdad, bloquear el puntero y recibir el movimiento crudo del mouse. En la 0.8.7, un bloqueo vale solo con el puntero sobre la ventana y el teclado en su app, y ❖Esc siempre lo libera.
- **Las notificaciones** de las apps de Linux llegan por el servicio estándar de notificaciones, que provee `lord-server`, y se pueden configurar por app en Ajustes.
- **Contraseñas:** el Secret Service estándar lo sirve el llavero de LordOS, así que los navegadores pueden guardar ahí sus secretos sin otro demonio de llavero.
- **Forzar salida** (desde 0.8.5): en Lord Flow, Forzar salida… en el menú de la corona lista las apps abiertas y termina la elegida sin preguntarle. Para una app de Linux termina solo el proceso de esa app; a una app X11 se le pide que se cierre.
- **Apps por defecto sin elegir** (desde 0.8.6): si no se eligió una app para un tipo de archivo, el archivo se abre con la predeterminada del sistema o con la primera app instalada que declara su tipo, que se muestra como "Automática" en Ajustes → Apps por defecto. Las apps elegidas quedan como están.

[[shot:2]]

[[shot:3]]

## En desarrollo para la 0.8.8.8

Todavía sin publicar; la última versión publicada es la 0.8.8.7.

- **Arrastrar y soltar.** Arrastrá archivos desde Archivos a una app de Linux (recibe una copia; apretá Shift para moverlos), desde una app de Linux a una carpeta de Archivos, o entre apps de Linux. Solo las apps que no están en una caja de arena pueden pasarle archivos a LordOS. Las apps X11 (por Xwayland) también participan, ya construido pero todavía sin verificar en nuestra máquina de pruebas.
- **Archivos copiados.** Copiá en Archivos y pegá en un gestor de archivos de Linux, o al revés. Los archivos copiados en otra app se pegan solo después de que Archivos te pregunte, con Cancelar como opción predeterminada.
- **Tu método de entrada dentro de las apps de Linux.** Los métodos de entrada de coreano, japonés y chino de LordOS ahora escriben en las apps GTK y Qt, con la lista de candidatos en el cursor, y nunca en campos de contraseña.
- **Íconos de bandeja.** Las apps de Linux que ponen un ícono en la bandeja lo muestran junto a los ítems de estado de LordOS, con su menú. Un ícono que existe solo en SVG también se dibuja, en el color de la barra (verificado con el applet de volumen).
- **Lo que suena y las teclas multimedia.** Los navegadores y los reproductores de video y de música aparecen en el reproductor del Centro de control con su título, su carátula y sus controles, y las teclas de reproducir/pausar, siguiente y anterior les llegan.
- **Diálogos y enlaces.** El diálogo de una app de Linux se abre encima de su ventana y se mueve con ella, y un enlace abierto desde LordOS trae al frente el navegador que ya está abierto. Una ventana no puede tomar el foco por su cuenta.
- **Compartir una pantalla o una ventana.** Una videollamada en un navegador puede compartir una pantalla o una sola ventana: el selector propio de LordOS muestra las pantallas y las ventanas con una imagen de cada una y el nombre de la app que pide, no se comparte nada hasta que apretás Compartir, y el Detener del indicador de pantalla compartida lo termina. Verificado en nuestra máquina de pruebas con un navegador. Contraseñas y las otras ventanas protegidas salen en negro.
- **Apps que toman los atajos.** Una máquina virtual, un escritorio remoto o un juego pueden tomar ❖, ⌥Tab y el resto mientras están al frente, después de que lo permitas una vez; ❖Esc siempre te los devuelve. El aviso nombra a la app por lo que LordOS verificó que es, no por lo que dice ser.
- **Atajos globales.** El pulsar para hablar de una app de llamadas o los atajos de un grabador funcionan mientras otra app está al frente. Vos elegís las teclas en la hoja de LordOS, y las cambiás u olvidás en Ajustes → Teclado.
- **Apps X11.** Participan de arrastrar y soltar, y se ven nítidas en pantallas HiDPI porque se escalan solas (Ajustes → Pantallas vuelve a la forma vieja, borrosa).
- **Apps de terminal.** Programas como htop se abren en una pestaña de la Terminal desde el lanzador de apps.
- **Apps web.** La sección Apps web de la Tienda instala apps web, cada una en una ventana y un proceso propios con sus propias cuentas, a través del paquete opcional `lordos-web`. Las apps web siguen el modo claro u oscuro del escritorio, en vivo, a través del `matchMedia` de la página; una página que elige sus colores solo con media queries de CSS queda clara (elegí Oscuro en sus propios ajustes). Probado en nuestra máquina de pruebas, en claro y en oscuro.

Los atajos globales, arrastrar con apps X11, las apps X11 nítidas y las apps de terminal están construidos y probados, pero todavía no se verificaron en nuestra máquina de pruebas.

## Qué protocolos Wayland hay

`wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (uno por pantalla), `xdg_wm_base` con menús emergentes, `wl_seat` (puntero, teclado, táctil), `wl_data_device_manager` (portapapeles), `zxdg_decoration_manager_v1`, el protocolo de decoración del servidor de KDE, `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1`, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1`, `zwp_primary_selection_device_manager_v1` (desde 0.8.3) y `zwp_idle_inhibit_manager_v1` (desde 0.8.5) y, en la 0.8.7, `zwp_text_input_manager_v3`, para el teclado en pantalla (en la 0.8.8.8, en desarrollo, también para los métodos de entrada de LordOS). En la 0.8.8.8 (en desarrollo) el compositor también ofrece `xdg_activation_v1`, `zxdg_exporter_v2` y `zxdg_importer_v2`, y `xdg_wm_dialog_v1`, y `wl_data_device` suma arrastrar y soltar; más adelante en la 0.8.8.8 también `zwp_keyboard_shortcuts_inhibit_manager_v1` y `wp_security_context_manager_v1`.

**Comprobalo desde afuera** (desde 0.8.5): el **Banco de pruebas** (`lordos-testbench`) es una ventana con una carta de ajuste que revisa el servidor gráfico como lo ve cualquier app: los tiempos de los cuadros y cuándo llegan de verdad a la pantalla, cada tecla del teclado, el mouse y la rueda, y los protocolos Wayland que ofrece el sistema.

Ausente a propósito: cualquier protocolo que le permita a una app capturar la pantalla o las ventanas de otras apps. En la 0.8.8.8 (en desarrollo) una app solo puede pedirlo por el portal ScreenCast, y recibe solo la pantalla o la ventana que elegís, mientras el indicador de pantalla compartida lo muestra.

[[shot:4]]

## Las preguntas del lector escéptico

**¿Las apps de Linux están en una caja de arena?** No de forma predeterminada. Esto es importante: corren como tu usuario, como en cualquier sistema Debian, y pueden leer tu carpeta personal. Lo que LordOS suma es un cgroup por app, la atribución en el Monitor, el firewall por app que aplica el kernel (permitir, denegar, solo LAN, sin conexiones entrantes, reglas por dominio o dirección) y restricciones que podés fijar por app: sin cámara, sin USB y sin sockets Bluetooth (en el kernel), sin micrófono (la guardia de PipeWire de LordOS más los nodos de captura de la placa de sonido), sin notificaciones y "sin acceso a tus archivos", que se aplica desde el próximo inicio. No cubren el portapapeles, el Bluetooth a través de BlueZ en el bus del sistema ni los programas iniciados fuera del lanzador de apps. Solo las apps `.lpk` propias de LordOS tienen la caja de arena completa.

**¿Una app de Linux puede ver otras ventanas o grabar la pantalla?** No a través de LordOS. El compositor solo le da a una app sus propias superficies y no ofrece ningún protocolo de captura de pantalla, las apps X11 solo ven otras apps X11 (nunca las apps Wayland ni a LordOS), hasta la 0.8.8.7 no hay portal de captura de pantalla (en la 0.8.8.8, en desarrollo, una app puede pedirla, y recibe solo lo que elegís en el selector propio de LordOS, nunca si le restringiste capturar la pantalla), y a una app restringida también se le niega el dispositivo de framebuffer, así que las ventanas protegidas como Contraseñas también quedan a salvo de ellas.

**¿Y las apps que solo usan X11?** Desde LordOS 0.8.3 Beta se abren con Xwayland, que arranca solo cuando una se conecta, con el marco de ventana del tema, su ícono en el Dock y el portapapeles compartido. Tienen la misma identidad por app, el mismo firewall y la misma atribución en el Monitor que las apps Wayland; las apps con restricciones de dispositivos o de archivos no reciben pantalla X. Se probaron XTerm, una app Java Swing y un juego SDL.

**¿Y Flatpak?** Hoy LordOS no lo usa ni lo integra. No está planeado para ninguna versión: para mantener el sistema estable no se está construyendo, y en `docs/FLATPAK.md` queda una revisión del diseño para el futuro, con lo que las apps de Flatpak necesitarían para seguir el modelo de LordOS (su propia identidad en el firewall y en el Monitor, sus permisos en Ajustes → Privacidad y los avisos propios de LordOS).

**¿Y apps como Chrome o Spotify?** (desde 0.8.5) Tienda → Comerciales instala apps gratis cuyo código fuente no es público desde los repositorios firmados de sus fabricantes, con la clave que fija LordOS, y lo avisa antes de instalar nada.

**¿Los paquetes llegan por HTTPS?** Desde LordOS 0.8.5, sí para las fuentes `https://`: un helper chico y aislado, `lord-tls`, verifica el certificado y el nombre del servidor y cifra la descarga, así nadie en la red puede ver qué paquetes descargás, y una descarga segura nunca se manda de vuelta a HTTP sin cifrar. Hasta la 0.8.4.1 llegaban por `http://`. En los dos casos la seguridad viene de las firmas y los hashes: cada índice tiene que llevar una firma válida de una clave de confianza, no puede estar vencido ni ser más viejo que el que ya está guardado, y cada archivo se controla contra su SHA-256 antes de usarse, así que nadie en la red puede modificarlos.

**¿Qué es la categoría Inteligencia Artificial?** (desde 0.8.5) Una página de la Tienda con una lista corta de herramientas de IA elegidas por LordOS: agentes que programan (Claude Code, Codex, goose, opencode), editores (Visual Studio Code, Cursor), la app de escritorio de Claude, llama.cpp para correr modelos de lenguaje en tu equipo y whisper.cpp para pasar voz a texto. Las apps cuyo fabricante tiene un repositorio firmado se instalan desde ahí: la Tienda te dice primero qué repositorio agrega, y compara la clave del fabricante con la huella que fija LordOS. Las demás vienen del repositorio firmado de LordOS, compiladas por LordOS desde fuentes verificadas o copiadas después de comprobar la firma del fabricante. llama.cpp baja un modelo la primera vez que lo ejecutás (`llama-server -hf <usuario>/<modelo>-GGUF`); corre en la CPU, y en una GPU por Vulkan, que todavía no se probó en hardware real.

## Límites honestos

- **Las apps X11 se ven entre sí,** como en cualquier escritorio X11; se ven nítidas solo al 100 % de escala (en la 0.8.8.8, en desarrollo, se escalan solas y se ven nítidas, salvo una app que no lea ninguna configuración de DPI).
- **No están en la 0.8.8.7, la última versión publicada:** arrastrar y soltar entre apps, los métodos de entrada de LordOS (coreano, japonés, chino) dentro de las apps de Linux, y los pedidos de activación de apps. Los tres están en desarrollo para la 0.8.8.8 (más arriba), donde arrastrar con apps X11 ya está construido pero todavía sin verificar en nuestra máquina de pruebas.
- **Íconos solo en SVG:** hasta la 0.8.8.7 LordOS no dibuja SVG y usa los íconos PNG que publica Debian para la mayoría de las apps, o la inicial; en la 0.8.8.8 (en desarrollo) `lord-media` dibuja los íconos SVG en su caja de arena.
- **Las apps de terminal** con archivo `.desktop` no aparecen en el lanzador de apps hasta la 0.8.8.7; arrancalas desde la Terminal. En la 0.8.8.8 (en desarrollo) se abren en una pestaña de la Terminal.
- **Las apps GTK 3 mantienen su propio acento azul;** la lectura del esquema de colores desde el portal en Qt 6 no se probó.

## Leé el código

- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): el compositor Wayland y sus protocolos
- [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c): el lanzador de `.desktop`
- [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c): el portal de configuración para GTK y libadwaita, y el portal FileChooser
- [sdk/wayland.js](https://github.com/lordbasex/LordOS/blob/main/sdk/wayland.js): las ventanas de Linux en el gestor de ventanas
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg): el motor de paquetes Debian
- [apps/store](https://github.com/lordbasex/LordOS/tree/main/apps/store): la Tienda
- [docs/WAYLAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/WAYLAND.md), [docs/PACKAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/PACKAGES.md), [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md)
