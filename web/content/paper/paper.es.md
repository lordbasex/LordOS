## Resumen

LordOS es una distribución GNU/Linux libre basada en Debian 13 que trae un entorno de escritorio propio, construido desde sus componentes fundamentales sobre Debian y Wayland. En lugar de GNOME o KDE, tiene su propio servidor gráfico y compositor Wayland escritos en C (`lord-server`), su propio motor de interfaz (React corriendo sobre el motor de JavaScript QuickJS, con layout, pintado y texto en código C nativo), sus propias apps nativas y sus propios servicios de seguridad: un lanzador de apps en caja de arena, un broker de permisos, un registro de permisos por app, un firewall por app que aplica el kernel, firma de paquetes y una bóveda de contraseñas cifrada. Reutiliza lo que está maduro y muy revisado: el kernel Linux, Debian y sus paquetes, Mesa, PipeWire, el protocolo Wayland, Xwayland para las apps X11 y unas pocas bibliotecas (FFmpeg, Poppler, libarchive, OpenSSL, libsrtp2, usrsctp, Monocypher) que quedan encerradas en pequeños procesos ayudantes aislados o en el gestor de sesiones.

Este documento describe el diseño y el estado actual de la **0.8.5 Beta**, publicada el 27 de octubre de 2026, para un lector técnico que quiere verificar el proyecto en lugar de creer en él a ciegas. **La 0.8.5 Beta es el último lanzamiento**; la 0.8.0 (18 de octubre) fue el primer lanzamiento público, seguida por la 0.8.1, la 0.8.2, la 0.8.3, la 0.8.4, la 0.8.4.1 (solo paquetes) y la 0.8.5, cada una listada en `CHANGELOG.md`. Cada afirmación remite a un archivo del repositorio público. Los números son los medidos en los registros de prueba del proyecto, e indican dónde se midieron. Lo que está diseñado pero no construido aparece marcado como tal. Desde la 0.8.5, muchas de las afirmaciones también se pueden verificar en el propio equipo del lector con las Verificaciones de Información del sistema y el Banco de pruebas (§11), y `docs/PAPER-VALIDATION.md` lista cada afirmación de este documento con cómo se verifica.

## 1. Visión

### 1.1 El problema

Un escritorio Linux moderno se arma con muchos proyectos independientes: un servidor gráfico o compositor, un toolkit, un gestor de sesiones, un demonio de configuración, un llavero, un gestor de red, una interfaz de firewall y las apps. Cada uno es bueno en lo suyo, pero las costuras entre ellos son donde las cosas se vuelven lentas, inconsistentes o inseguras. Tres costuras en particular motivaron LordOS:

- **Peso.** Un escritorio que dibuja su interfaz con un motor web, o que carga varios toolkits a la vez, necesita cientos de megabytes antes de que el usuario abra nada. LordOS quiere andar cómodo en máquinas modestas y en una Raspberry Pi 5.
- **Confianza en las apps.** En un escritorio Linux tradicional, cada programa que corrés puede leer toda tu carpeta personal, grabar el micrófono, abrir cualquier conexión de red y leer el portapapeles. El aislamiento existe, pero suele ser un agregado que el escritorio mismo no entiende.
- **Visibilidad.** Rara vez ves qué app está hablando con qué servidor, y menos todavía podés decir "esta app no puede usar internet" de una forma que la app no pueda esquivar.

### 1.2 Principios

El proyecto sigue una lista corta de reglas, escritas en `AGENTS.md` y `docs/ARCHITECTURE.md`:

1. **Ser dueños de lo que el usuario toca; reutilizar los cimientos.** El servidor gráfico, el compositor, el motor de interfaz, las apps y los servicios de seguridad son código de LordOS. El kernel, los paquetes de Debian, Mesa, PipeWire y el protocolo Wayland se reutilizan.
2. **Una política de dependencias estricta.** Dentro de LordOS no se usan X11, wlroots, libdrm, libinput, WebKit, GTK ni Qt. Las bibliotecas que leen datos no confiables las enlazan solo ayudantes aislados, nunca el compositor.
3. **La interfaz no puede ejecutar comandos.** El JavaScript de LordOS no tiene ninguna API para ejecutar programas arbitrarios. Las excepciones son pocas, angostas, y cada una está aprobada y documentada en `AGENTS.md`: la shell de inicio de la Terminal; lanzar una app `.desktop` instalada (opcionalmente con un archivo que abrió el usuario, o con una de las acciones con nombre de la propia app); arrancar una app `.lpk` instalada en su caja de arena; Xwayland, que arranca el propio `lord-server` con una línea de comandos fija solo cuando se conecta una app X11; los conectores de red de los componentes y el descargador de listas de bloqueo, que vuelven a ejecutar los binarios propios de LordOS con argumentos fijos; y el hijo de descarga de la Tienda, que se ubica a sí mismo en su propio scope. Ninguna toma texto de la interfaz.
4. **Root solo a través de puertas cerradas.** Todo lo que necesita root pasa por una tabla fija de acciones validadas o por un servicio pequeño y dedicado con un protocolo cerrado.
5. **El camino por CPU siempre tiene que funcionar.** La composición en GPU es una optimización, nunca un requisito.
6. **El compositor nunca espera.** Ayudantes, apps y servicios hablan con `lord-server` por sockets no bloqueantes; una app colgada se cuelga solo a sí misma.
7. **Verificar en un sistema real.** Cada hito se comprueba en una máquina virtual con capturas reales antes de marcarlo como terminado, y no se declara compatibilidad que no se probó.

### Archivos fuente

- [AGENTS.md](https://github.com/lordbasex/LordOS/blob/main/AGENTS.md)
- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md)

## 2. Arquitectura

### 2.1 Capas

[[diagram:layers]]

De abajo hacia arriba:

- **El kernel Linux** provee DRM/KMS para las pantallas, evdev para la entrada y las funciones de seguridad sobre las que se apoya LordOS: Landlock, seccomp-bpf, cgroups v2 y eBPF. En PC es el kernel de Debian; en la Raspberry Pi 5 es la compilación de LordOS del kernel de Raspberry Pi con Landlock activado (§7.4).
- **Debian 13** provee el espacio de usuario: `dpkg` y el archivo de paquetes, systemd, Mesa, PipeWire, `libwayland`, `libxkbcommon`, CUPS, BlueZ y el resto.
- **`lord-server`** es el servidor gráfico y compositor Wayland. Maneja las pantallas y la entrada directamente, compone y sirve el protocolo Wayland a las apps de Linux.
- **El motor de interfaz** vive dentro de `lord-server`: QuickJS corre la interfaz en React, y código C nativo hace el layout, el pintado y el texto.
- **Las apps** son de tres tipos: las apps incluidas y el tema, que corren en el contexto de JavaScript de `lord-server`; las apps `.lpk` instaladas, cada una en su propio proceso aislado con su propia instancia del motor; y las apps de Linux, que se conectan al compositor como clientes Wayland comunes.

### 2.2 Procesos y privilegios

[[diagram:processes]]

Un LordOS en marcha tiene pocos procesos, con roles bien separados:

| Proceso | Corre como | Rol |
|---|---|---|
| `lord-sessiond` | root | pantalla de inicio de sesión, sesiones, prestar la pantalla y la entrada a la sesión visible, escritorio remoto |
| `lord-netd` | root | la red del sistema: DHCP, Wi-Fi, VPN; solo las acciones `net-*` |
| `lord-netperm` | root (capacidades limitadas) | reglas de red por app, y las restricciones de dispositivos y Bluetooth de las apps de Linux, aplicadas con programas eBPF de cgroup y de LSM |
| `lord-fand` | root (sin capacidades) | control de ventiladores según la temperatura |
| `lord-server` | el usuario | uno por sesión: compositor, motor de interfaz, broker |
| `lord-server --app <id>` | el usuario, aislado | uno por app `.lpk` en ejecución, y las apps del sistema que corren aisladas: Contraseñas, Mail, Calendario, Contactos, Recordatorios y la Prueba del aislamiento de Información del sistema |
| `lord-doc`, `lord-media`, `lord-maild`, `lord-vault`, `lord-fetch` | el usuario, aislados | ayudantes que procesan datos no confiables o secretos (`lord-fetch` descarga las listas de bloqueo del firewall) |
| `lord-tls` | `nobody`, aislado, en el scope de descarga de la Tienda | HTTPS para las descargas del motor de paquetes (0.8.5): verifica el certificado y el nombre del servidor, y después solo pasa bytes |
| `lord-server --net-connector mail\|print\|lists` | el usuario, en su propio scope | abre sockets de red para un componente de LordOS, para que el firewall pueda verlo y bloquearlo por separado |
| `Xwayland` | el usuario, scope y espacio de nombres de red propios | solo mientras hay una app X11 conectada, arrancado por `lord-server` con una línea de comandos fija |
| pantalla de inicio de sesión | `lordos-greeter` | dibuja la interfaz de inicio; nunca verifica contraseñas por su cuenta |

Cada proceso root tiene una interfaz angosta y documentada. La §6.3 las describe.

### 2.3 Qué es propio y qué se reutiliza

| Propio (en este repositorio) | Reutilizado |
|---|---|
| Servidor gráfico y compositor Wayland (C) | Kernel Linux |
| Motor de interfaz: nodos nativos, layout flexbox, rasterizador, shaping de texto, bidi, métodos de entrada | Debian 13 y sus paquetes; `dpkg` |
| Renderizador de React (configuración de host para `react-reconciler`) | React 19.3.1, `react-reconciler` 0.34 (empaquetados en el tema) |
| Apps nativas y temas | QuickJS 2025-04-26 (incluido como código fuente) |
| Caja de arena, broker, permisos, firma, servicio de firewall, bóveda | Mesa (opcional), PipeWire, `libwayland`, `libxkbcommon` |
| Gestor de sesiones, servicio de red, servicio de ventiladores | systemd, systemd-resolved, BlueZ, CUPS, wpa_supplicant, OpenVPN, el demonio de strongSwan |
| Motor de paquetes Debian (OpenPGP, xz, HTTP y HTTPS, resolución de dependencias) | FFmpeg, Poppler, libarchive, OpenSSL, Monocypher (solo en ayudantes) |
| Driver de touchpad, emulador de terminal, intérprete de WebAssembly | stb (fuentes e imágenes), fuentes bajo la OFL |
| Escritorio remoto por WebRTC: ICE-lite, establecimiento de DTLS-SRTP, empaquetado RTP, control de ancho de banda (la codificación, en `lord-media`) | libsrtp2 y usrsctp (en el puente de `lord-sessiond`), libx264, libvpx, Opus y VA-API (en `lord-media`) |
| El recolector de Información del sistema, `lordos-sysreport`, `lordos-fetch`, el Banco de pruebas | Xwayland (el paquete de Debian, a pedido), smartmontools, `pci.ids` y `usb.ids` |

LordOS habla con varios servicios de Debian por sus propios protocolos, con clientes propios: D-Bus (BlueZ, perfiles de energía, fecha y hora), IPP (CUPS), el protocolo de PulseAudio (PipeWire) y el Secret Service. No se usan libdbus, sd-bus, libcups, libpulse, libbpf ni libapt.

### 2.4 Servicios del sistema

- **Red (`lord-netd`).** La red pertenece al sistema, no a una sesión. `lord-sessiond` arranca `lord-netd` al iniciar, con una ruta fija y sin argumentos, así la máquina tiene dirección antes de que alguien inicie sesión. Solo ejecuta las acciones cerradas `net-*`: su propio cliente DHCP y código netlink, Wi-Fi a través del `wpa_supplicant` de Debian, WireGuard con el código netlink propio de LordOS, OpenVPN, IPsec/IKEv2 a través del `charon` de strongSwan manejado por VICI con un cliente propio de LordOS y, opcionalmente, túneles OpenConnect, PPTP y compatibles con Fortinet. Habla solo con los ayudantes root de las sesiones, por un socket que acepta únicamente uid 0.
- **Ventiladores (`lord-fand`).** Un servicio root que lee la temperatura de la CPU cada 2 s y escribe el ciclo de trabajo de los ventiladores según un perfil, desde temprano en el arranque. Maneja solo los ventiladores que encontró su propio escaneo: el conector de ventilador de la Raspberry Pi 5 y controladores de placa madre de una lista permitida. Nunca escribe ventiladores de notebooks, GPU ni desconocidos. Al detenerse, o después de un fallo, restaura la configuración original del kernel. No ejecuta programas, no tiene red, ni capacidades, ni nodos de dispositivo.
- **Configuración de hardware.** Pantallas (modos desde KMS con nombres EDID, una reversión de 15 segundos con "¿mantener esta configuración?", persistencia por pantalla, rotación, Luz nocturna), Bluetooth a través de BlueZ, batería y perfiles de energía, inactividad y tapa, fecha y hora, notificaciones para apps de Linux y apps predeterminadas. Los cambios llegan como uevents del kernel y señales de D-Bus; nada consulta en bucle. En una Raspberry Pi 5, Ajustes → Firmware de la placa muestra y actualiza el cargador de arranque que está en la EEPROM de la placa mediante dos acciones de administración cerradas (0.8.4.1).
- **Actualizaciones.** Los sistemas instalados se actualizan en el lugar desde el repositorio APT firmado de LordOS en lordos.org, desde Tienda → Actualizaciones o con `apt`; los cambios que son más que archivos nuevos se ejecutan como migraciones numeradas e idempotentes (0.8.1). El motor de paquetes de la Tienda respeta las prioridades de apt (`/etc/apt/preferences.d`, 0.8.5), así que en la imagen para Raspberry Pi "Actualizar todo" elige las mismas versiones que `apt` y el kernel fijado sigue bloqueado, y acepta fuentes escritas como `Architectures: amd64,arm64`, como apt. La actualización de la 0.8.4.1 o la 0.8.4 a la 0.8.5 no ejecuta ninguna migración; se probó con el paquete del release compilado como 0.8.5~rc2 en una máquina VMware con la 0.8.4.1 y en una Raspberry Pi 5 con la 0.8.4, las dos con `apt` y después un reinicio.

### Archivos fuente

- [server/src/main.c](https://github.com/lordbasex/LordOS/blob/main/server/src/main.c)
- [server/src/admin.c](https://github.com/lordbasex/LordOS/blob/main/server/src/admin.c)
- [server/src/net/netd.c](https://github.com/lordbasex/LordOS/blob/main/server/src/net/netd.c)
- [server/src/fan/fand.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fan/fand.c)
- [server/src/hw](https://github.com/lordbasex/LordOS/tree/main/server/src/hw)

## 3. El compositor

### 3.1 Pantallas sin libdrm

`lord-server` configura los modos de pantalla con los ioctls de DRM del kernel directamente (`kms.c`): enumera conectores, CRTC y planos, reserva buffers y cambia de página en el refresco vertical. Elige la placa que tiene conectores, no simplemente `card0`, algo importante en placas donde el controlador de pantalla y la GPU son dispositivos separados (en la Raspberry Pi 5, `vc4` y `v3d`).

### 3.2 Dos formas de armar un cuadro

- **Camino por CPU.** La interfaz y las ventanas de las apps se copian a un lienzo, y se presenta un buffer "dumb" con un cambio de página. No necesita nada más que el kernel. Es el predeterminado cuando no hay una placa con 3D, y `LORD_GPU=0` lo fuerza (el modo seguro del menú de arranque lo usa). Desde la 0.8.5, los cuadros pesados se pintan en un grupo de hasta 8 hilos (`par.c`), solo mientras pintar lleva más del 70 % de un cuadro a la frecuencia objetivo (en la 0.8.6, en desarrollo, también mientras el cuadro entero lleva más del 60 %): el desenfoque del fondo por fases y las primitivas locales a cada fila repartidas en bandas de filas, con los mismos píxeles que con un solo hilo (§7.2).
- **Camino por GPU.** GBM, EGL y GLES2 de Mesa (`server/src/gpu/gpu.c`). La interfaz pasa a ser una textura, y en cada cuadro se sube solo la región que cambió. Las ventanas de las apps no se copian al lienzo de la interfaz: la interfaz deja un hueco transparente donde va cada ventana, y las texturas de las ventanas se dibujan debajo, así una app que solo cambia su contenido no provoca un repintado de la interfaz. Las apps pueden entregar buffers de GPU (`linux-dmabuf`) sin copias, y una app a pantalla completa con un buffer adecuado se muestra directamente (scanout directo). El cursor usa el plano de cursor por hardware.

El camino se elige al arrancar: una placa sin 3D (simpledrm, bochs, virtio-gpu sin virgl y otras) usa la CPU sin siquiera cargar Mesa; un renderizador OpenGL por software también vuelve a la CPU, porque componer con él cuesta más que el camino por CPU propio de LordOS. Si un cuadro por GPU falla, la GPU se apaga y el camino por CPU toma el control.

**Estado:** el camino por GPU se probó con el renderizador por software de Mesa en máquinas virtuales. Falta probarlo en placas AMD y Nvidia reales.

### 3.3 Ventanas y entrada

- **Gestión de ventanas en C.** Mover y redimensionar corre en C, con un evento por cuadro y sin JavaScript, y la ventana que se mueve se dibuja desde su propia capa. Los cuatro bordes y las esquinas redimensionan, con cursores propios de LordOS.
- **Entrada sin libinput.** Teclados, mouses y tabletas se leen desde evdev. Las distribuciones de teclado vienen de `libxkbcommon` (todas las distribuciones XKB, variantes y compose).
- **Touchpad y pantalla táctil.** LordOS tiene su propio driver multitáctil (`server/src/tp`): aceleración del puntero, tocar para hacer clic y clic con dos dedos, rechazo de la palma, desplazamiento cinético y natural, pellizco y deslizamientos con tres dedos. Los gestos llegan tanto a la interfaz de LordOS como a las apps de Linux (`zwp_pointer_gestures_v1`). Se prueba con gestos grabados en `make test`.

### 3.4 Compatibilidad con Wayland

El compositor (`server/src/wl/wl.c`) usa solo `libwayland-server` y código generado a partir del XML de los protocolos. Ofrece: `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (uno por pantalla), `xdg_wm_base` con popups, `wl_seat` (puntero, teclado, táctil), `wl_data_device_manager` (un portapapeles compartido con LordOS), `zwp_primary_selection_device_manager_v1` (pegar con el clic del medio, 0.8.3), `zxdg_decoration_manager_v1`, el protocolo de decoración del servidor de KDE (para que las apps GTK 3 como Firefox tengan el marco del tema), `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1` v4, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1` y `zwp_idle_inhibit_manager_v1` (un reproductor de video mantiene la pantalla encendida, 0.8.5). El compositor respeta el marco que pide una app, también cuando cambia con la ventana abierta (0.8.4).

**Verificado desde afuera.** El Banco de pruebas (`lordos-testbench`, 0.8.5) es un cliente Wayland común que se enlaza a cada uno de estos protocolos en la versión anunciada y lo ejercita: los formatos de shm, las salidas y sus modos, xdg-output, la escala fraccionaria, una subsuperficie con viewport, un popup, los estados maximizado y pantalla completa, el feedback de dmabuf, idas y vueltas del portapapeles y de la selección primaria, idle-inhibit, la forma del cursor, el puntero relativo y los gestos. Su primera corrida encontró que el compositor no le volvía a enviar una selección nueva al cliente que la había fijado, como exige el protocolo, y que faltaba idle-inhibit; las dos cosas se corrigieron el mismo día, y ahora la corrida da 16 pasaron, 0 fallaron en la VM 116 (`docs/PAPER-VALIDATION.md`).

Las ventanas de Linux entran al gestor de ventanas del SDK como cualquier otra, con título, foco, maximizar, minimizar e ícono en el Dock. Las apps que piden decoración del lado del servidor reciben el marco del tema; las que dibujan el suyo (GTK 4) lo conservan, y se recorta su sombra. Las apps GTK 4 y libadwaita siguen el modo claro u oscuro, el color de acento y el orden de los botones a través de una implementación de `org.freedesktop.portal.Settings` que `lord-server` sirve él mismo en el bus de sesión; GTK 3 los sigue a través del archivo de claves de GSettings.

**Apps X11 (0.8.3).** Cuando una app que solo habla X11 se conecta al socket X de la sesión, `lord-server` arranca el `/usr/bin/Xwayland` de Debian con una línea de comandos fija, en modo rootless, en su propio scope y su propio espacio de nombres de red, con `no_new_privs`; el compositor es el gestor de ventanas X (`server/src/wl/xwm.c`), así que las ventanas X11 tienen el mismo marco, la misma entrada en el Dock, la misma identidad por app, las mismas reglas de firewall y las mismas restricciones que las de Wayland, y el portapapeles funciona en los dos sentidos. Las apps `.lpk` aisladas y las apps de Linux con restricciones de dispositivos no reciben `DISPLAY`. Tiene que estar instalado el paquete `xwayland` de Debian.

Todavía quedan pendientes: arrastrar y soltar, los métodos de entrada de LordOS dentro de apps de Linux (`text-input` v3), la activación (`xdg-activation`), la sincronización explícita, los buffers de un solo píxel, el control de tearing, el tipo de contenido, las tabletas digitalizadoras y los íconos de app en SVG. No se ofrece a las apps ningún protocolo de captura de pantalla.

### 3.5 Varias pantallas y HiDPI

Todas las pantallas conectadas se manejan a la vez, extendidas o en espejo, en un único espacio lógico, con una disposición que se recuerda por identidad EDID y manejo de conexión en caliente. Cada pantalla se pinta a su propia escala (100, 125, 150, 175 o 200 %), así el texto y los vectores se rasterizan a su resolución real. Las apps de Linux reciben un `wl_output` por pantalla y escalado fraccional. Se probó con dos salidas virtuales (1280×800 y 2560×1600) en los dos caminos; no se probaron monitores reales, paneles de notebook ni hubs DisplayPort.

Desde la 0.8.5, Ajustes → Pantallas numera cada pantalla (la principal es la 1) e **Identificar** muestra ese número grande en cada monitor; al arrastrar una pantalla en la disposición se pega borde con borde y se alinea con las otras, soltarla nunca deja dos pantallas superpuestas ni separadas, y el servidor guarda una posición que encaja exacto. Una ventana se puede mandar a otra pantalla desde el menú Ventana, y una app se puede asignar a una pantalla desde su ítem del Dock, recordada por el nombre del conector (HDMI-A-1, DP-2…) para distinguir dos monitores del mismo modelo (verificado en la VM 118). La pantalla de inicio de sesión y de bloqueo va solo en la pantalla principal, con el fondo en todas, y sigue a las pantallas que se conectan o desconectan (VM 118, dos pantallas).

### 3.6 Sesiones e inicio de sesión

[[diagram:sessions]]

`lord-sessiond` es el gestor de sesiones propio de LordOS:

- **La pantalla de inicio de sesión** es un `lord-server` que corre como el usuario sin privilegios `lordos-greeter`, con un tema especial. Envía los pedidos de inicio de sesión a `lord-sessiond`; **las contraseñas siempre las verifica el proceso root** contra `/etc/shadow`, nunca el JavaScript de la pantalla de inicio. Los intentos fallidos se responden a un tiempo fijo de 1,25 s, así que el tiempo no revela nada. Desde la 0.8.5 sigue los eventos de pantalla del kernel, así que un monitor conectado después del arranque la muestra, y ya no queda corriendo detrás de la sesión del usuario: se cierra 10 segundos después de que una sesión toma la pantalla (unos 53 MB propios, 78 MB contando las páginas compartidas, medidos en una VM de VMware; el sistema completo pasó de 603 a 548 MB en uso) y vuelve a arrancar en unos pocos milisegundos al bloquear, cambiar de usuario o cerrar sesión. Al bloquear, la pantalla primero se oscurece y recién entonces arranca la pantalla de inicio, así el escritorio desbloqueado nunca queda a la vista mientras tanto.
- **Primero el bus de sesión del usuario (0.8.5).** `lord-sessiond` arranca `user@<uid>` y espera el socket del bus de sesión del usuario (hasta 5 s) antes de arrancar la sesión. Con el inicio de sesión automático arrancaba unos milisegundos demasiado pronto, así que el Secret Service de `lord-vault` y el servidor de notificaciones nunca llegaban al bus (§6.10).
- **Una sesión por usuario**, y todas pueden vivir a la vez (cambio rápido de usuario). Cada una es un `lord-server` que arranca como root y suelta los privilegios enseguida.
- **Préstamo de la pantalla.** `lord-sessiond` abre la placa gráfica y los dispositivos de entrada como root y se los presta a la sesión que está en el monitor, por un socket Unix. Al cambiar, los revoca: `EVIOCREVOKE` hace que los descriptores de entrada de la sesión anterior dejen de funcionar, y `DROP_MASTER` le quita el control de la pantalla, antes de darle algo a la sesión siguiente. Desde la 0.8.5 el acceso se sigue cortando en el acto, pero los dispositivos se cierran en un hilo de vida corta, porque el último cierre de un mouse USB espera a que terminen sus transferencias: del bloqueo a la pantalla de inicio de sesión pasó de 903 ms a 141 ms en una VM de VMware Fusion, y un traspaso lento queda registrado con el tiempo de cada paso. La sesión visible sigue componiendo en la GPU sin ser nunca root. Una sesión fuera del monitor dibuja en memoria, y solo si alguien la está mirando de forma remota.
- **Bloquear saca de verdad la sesión del monitor**; no es una capa dibujada encima. Si la sesión del usuario se cuelga, la pantalla de inicio sigue funcionando.
- **Arranque.** Un tema de GRUB de LordOS dibuja el menú de arranque, y un tema de Plymouth de LordOS dibuja la pantalla de carga (la corona con una barra de progreso). El traspaso no tiene ningún cuadro negro ni texto de consola: Plymouth deja de dibujar y suelta el control de la pantalla mientras su último cuadro queda visible, `lord-sessiond` toma el control de la pantalla y pone el primer cuadro real de la pantalla de inicio de sesión, y recién entonces Plymouth termina, dejando la pantalla de carga hasta que se reemplaza. Desde la 0.8.5 la pantalla de carga muestra el tiempo desde que arrancó, Acerca de LordOS → Tiempo de inicio muestra cuánto tardó este arranque desde el inicio del kernel hasta la pantalla de inicio de sesión (o el escritorio, con inicio de sesión automático), y cada arranque se registra con la versión y la compilación de LordOS (`journalctl -b -u lordos | grep "first frame"`) y se guarda en `/var/lib/lordos/boot-times.jsonl` (los últimos 200 arranques).

**Escritorio remoto.** El mismo servicio ofrece el escritorio a un navegador en `https://<dirección>:5673` (5673 deletrea "LORD" en un teclado de teléfono). Viene **desactivado por defecto**, se activa en Ajustes → Compartir y de forma predeterminada acepta solo la red local (0.8.3; se puede elegir una lista de redes permitidas o cualquier dirección, y las interfaces VPN propias de LordOS cuentan como locales). Usa TLS 1.2 o superior con un certificado P-256 autofirmado cuya huella muestra Ajustes para comparar, una política de seguridad de contenido estricta, controles de mismo origen y una cookie de sesión `Secure`, `HttpOnly` y `SameSite=Strict`; la página de inicio nunca lista las cuentas de usuario. **Tres inicios de sesión fallidos bloquean la dirección de origen** por 15 minutos, después por 1 hora, después por 24 horas (IPv6 por /64), y eso lo persiste solo `lord-sessiond`, con listas manuales de bloqueo y de permitidos; los fallos se responden a un tiempo fijo de 1,25 s. Todo lo que llega de la red pasa por parsers estrictos con pruebas unitarias y una corrida de fuzzing bajo ASan y UBSan (0.8.3).

Desde la 0.8.4 la pantalla viaja como **video WebRTC**: H.264 (VP8 cuando el navegador no tiene H.264) con el sonido de la sesión en Opus, codificado una sola vez por sesión por `lord-media` (VA-API en la placa gráfica cuando puede, x264 si no) para todos los espectadores, con las zonas quietas perfectas al píxel gracias a mosaicos sin pérdida. Teclado, mouse y portapapeles van por un canal de datos WebRTC. Además de UDP, el video se conecta por TCP en el mismo puerto 5673, así que alcanza con un firewall o una VPN que deje pasar solo ese puerto; la página se reconecta sola después de un corte, y dice por qué cuando de verdad no puede conectarse. Se pueden agregar servidores STUN y un relé TURN (UDP, TCP o TLS) para otras redes. El proceso que atiende una conexión suelta root apenas sabe de quién es la sesión y se queda solo con cuatro descriptores. Desde la 0.8.5 el video sigue la frecuencia de refresco de la pantalla del navegador (60, 120, 144 Hz…) en lugar de 30 fijos, el ancho de banda sube en segundos en lugar de minutos, y una barra superior muestra los cuadros por segundo, el codificador y si la pantalla se compone en la GPU o en la CPU (§7.2). Reiniciar y Apagar se ofrecen solo en la sesión que está en el monitor. El acceso desde internet no está previsto; se recomienda una VPN.

**Inicio de sesión remoto (SSH).** Ajustes → Compartir también activa el servidor OpenSSH de Debian (0.8.1) mediante acciones de administración cerradas. Desde la 0.8.5 una **opción avanzada, apagada por defecto, deja que root entre por SSH**, solo con clave pública o con clave o contraseña, para pruebas y administración. Solo los administradores pueden cambiarla, y cada cambio lleva la contraseña del propio administrador, verificada como root contra `/etc/shadow`. Los modos escriben solo texto fijo en `/etc/ssh/sshd_config.d/05-lordos-root.conf` (que se lee antes del `10-lordos.conf` que trae el paquete, que mantiene `PermitRootLogin no`), comprobado con `sshd -t`. El único texto del usuario que se escribe es una línea de clave pública validada en C (un tipo conocido, un blob que coincide con él, un comentario imprimible, sin opciones) en `/root/.ssh/authorized_keys`; las claves se listan con sus huellas SHA-256, y la contraseña de root va a `chpasswd` por la entrada estándar. Mientras está activa, la página muestra una advertencia. Pruebas: `build/ssh-root-test` (17 verificaciones, las mismas huellas que `ssh-keygen -l`) y una corrida de punta a punta como root en la VM 116 (contraseña equivocada rechazada, una clave agregada y aceptada desde otra computadora, un duplicado rechazado, la clave quitada, el modo apagado, `ssh root@` rechazado).

### Archivos fuente

- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c), [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c), [server/src/outputs.c](https://github.com/lordbasex/LordOS/blob/main/server/src/outputs.c)
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c), [server/src/tp](https://github.com/lordbasex/LordOS/tree/main/server/src/tp), [server/src/ui/grab.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/grab.c)
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c), [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c)
- [server/src/remote/sessiond.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/sessiond.c), [server/src/remote/monitor.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/monitor.c), [server/src/remote/sess.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/sess.c)
- [tools/boot/plymouth](https://github.com/lordbasex/LordOS/tree/main/tools/boot/plymouth), [tools/iso/grub](https://github.com/lordbasex/LordOS/tree/main/tools/iso/grub), [docs/SESSIONS.md](https://github.com/lordbasex/LordOS/blob/main/docs/SESSIONS.md)

## 4. El motor de interfaz

### 4.1 No es un navegador

La interfaz de LordOS está escrita en JavaScript con React, pero no hay ningún motor web debajo. WebKit o Chromium costarían de 100 a 300 MB de RAM por proceso y no serían código de LordOS. En cambio, el tema y las apps usan un renderizador de React (`runtime/index.js`) que crea y actualiza **nodos nativos** en C: `View`, `Text`, `Image`, más superficies para ventanas Wayland y multimedia. Los estilos son un subconjunto de CSS (flexbox, colores, bordes redondeados y por lado, sombras, desenfoque, transformaciones, transiciones) que aplica código C controlado por el proyecto. El modelo se parece a cómo un framework móvil renderiza React a vistas nativas, salvo que acá el lado "nativo" es propio de LordOS.

### 4.2 Piezas

- **JavaScript:** QuickJS 2025-04-26, un motor ES2023 pequeño en C, incluido como código fuente y embebido en `lord-server`.
- **React:** React 19.3.1 con `react-reconciler` 0.34, empaquetados con esbuild dentro de cada tema. El tema predeterminado con todas las apps pesa alrededor de 1,8 MB (540 KB comprimido) en la 0.8.5.
- **Layout:** implementación propia de flexbox (`layout.c`), con ajuste de línea, bordes lógicos de inicio y fin, y espejado de derecha a izquierda.
- **Pintado:** rasterizador 2D propio con rectángulos redondeados con antialiasing, sombras en caché, desenfoque a resolución reducida y seguimiento de daño: solo se repinta la región que cambió. En ARM, bucles NEON opcionales aceleran las copias y las máscaras, verificados bit a bit contra el código C.
- **Texto:** los glifos se rasterizan con `stb_truetype`. El shaping es código OpenType propio de LordOS (sustituciones GSUB y posicionamiento GPOS, emoji a color), con bidi Unicode completo, unión de letras árabes, shaping índico y el Universal Shaping Engine (409 de 409 oraciones de prueba idénticas a HarfBuzz en `tests/hb_compare.py`), corte de línea CJK, separación de palabras en tailandés y fuentes de respaldo por escritura. `lord-server` no enlaza HarfBuzz, FreeType, fontconfig, fribidi ni ICU.
- **Métodos de entrada:** coreano (hangul), japonés (kana y SKK) y chino (pinyin), en `server/src/ime.c`, sin ibus ni fcitx.

[[diagram:frame]]

### 4.3 Cómo llega el JavaScript al sistema

El lado JavaScript llama a servicios nativos a través de las APIs `lord.*` implementadas en C (`server/src/ui/js_*.c`): `lord.fs` (archivos), `lord.settings`, `lord.net`, `lord.pkg`, `lord.media`, `lord.mail`, `lord.vault`, `lord.fw`, `lord.monitor`, `lord.fan` y otras. Cada llamada valida sus argumentos en C. Las reglas son estrictas:

- `lord-server` corre sin root, y **ninguna API ejecuta comandos arbitrarios**.
- `lord.fs` escribe solo en la carpeta personal y en los montajes propios del usuario, no tiene un "borrar" general (solo mover a la Papelera) y rechaza las carpetas que el sistema lee para lanzar o instalar algo o que guardan decisiones de seguridad: `~/.local/share/applications`, `~/.config/autostart`, la carpeta de apps instaladas, `settings.json`, el archivo de permisos, las reglas del firewall, las listas de bloqueo, el archivo de cuentas de correo y la carpeta de la bóveda. Desde la 0.8.5 sus llamadas de lectura alcanzan todo el sistema en solo lectura, como el usuario (Archivos → Este equipo; `/proc`, `/sys`, `/dev` y `/run` del kernel quedan afuera, y los permisos propios del sistema deciden el resto); la escritura nunca se amplió, y las apps aisladas conservan solo su propia carpeta de datos.
- Todo lo que necesita root pasa por `admin.c`: una tabla cerrada de acciones con argumentos validados del lado root, rutas de programa fijas y configuración generada por LordOS, nunca texto del usuario pasado a una shell. Las contraseñas pasan por la entrada estándar, nunca por la línea de comandos.
- El trabajo largo nunca bloquea un cuadro: ayudantes y servicios se consultan desde el bucle principal sin esperar.

### 4.4 Idiomas

La interfaz viene en inglés (el predeterminado), español y portugués, y se suman más idiomas como paquetes de idioma sin cambiar código. El texto pasa por `t('namespace.key')` del SDK, con plurales y formatos regionales. Toda la interfaz se espeja para los idiomas de derecha a izquierda, y la región (`LANG`/`LC_*`) se pasa a las apps de Linux. `node tools/i18n-check.mjs` verifica que cada clave exista en todos los idiomas y que no aparezcan marcas de terceros en la interfaz.

### Archivos fuente

- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js), [sdk/index.js](https://github.com/lordbasex/LordOS/blob/main/sdk/index.js)
- [server/src/ui/js.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js.c), [server/src/ui/node.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/node.c), [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c), [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c)
- [server/src/ui/shape.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/shape.c), [server/src/ui/otf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/otf.c), [server/src/ui/bidi.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/bidi.c), [server/src/ime.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ime.c)
- [server/src/ui/js_sys.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_sys.c), [server/src/fsops.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fsops.c)

## 5. Aplicaciones

### 5.1 Apps nativas

LordOS trae treinta y una apps nativas, todas apps React sobre el mismo motor y SDK: Archivos, Ajustes, Terminal, Tienda, Editor de texto, Notas, Calculadora, Monitor, Visor de PDF, Fuentes, Emoji y símbolos, Escáner, Fotos, Reproductor, Capturas, Cámara, Grabadora, Contraseñas, Discos, Copias de seguridad, Registros, Reloj, Ayuda, Mail, Calendario, Contactos, Recordatorios, LordOS Imager (0.8.4), Información del sistema (0.8.5), Acerca de y el instalador. Desde la 0.8.5 el manifiesto de cada app lleva su propia versión (todas empiezan en 0.8.5; la compilación rechaza una app sin una versión válida), que se muestra en Tienda → Instalado y en la ventana Acerca de de la app, así más adelante las apps se pueden publicar aparte de LordOS. Algunas para destacar:

- **Archivos:** desde la 0.8.5 la barra lateral tiene **Este equipo**, para recorrer todo el sistema en solo lectura como el usuario (deciden los permisos propios del sistema; `/proc`, `/sys`, `/dev` y `/run` se muestran con un candado y no se abren), y cada disco con un sistema de archivos: uno que no está montado aparece atenuado y un clic lo monta y lo abre (un disco interno pide un administrador, como en Discos), y uno extraíble se puede expulsar. Las carpetas en las que el usuario no puede escribir no muestran acciones que las cambiarían (§4.3).

- **Terminal:** emulador propio compatible con xterm, en C, con pestañas, paneles divididos, perfiles, imágenes en línea y Sixel, y sesiones que sobreviven a un reinicio de `lord-server`.
- **Monitor:** CPU, memoria, discos (con temperatura y salud SMART, 0.8.2), procesos (detener un proceso, Obtener información, 0.8.1), refrigeración y conexiones de red por app en un mapa del mundo, con búsqueda de país desde una base de datos local. La segunda generación de Monitor → Red (0.8.5) es un árbol desde cada app hacia los sitios, direcciones y puertos con los que habla, con los bytes enviados y recibidos, un interruptor permitir/bloquear en cada destino, una búsqueda con chips `app:`, `domain:`, `ip:`, `port:`, `proto:` y `country:`, y un historial de tráfico de una hora a 30 días. Lista los sockets TCP, UDP, ICMP (ping), IP crudos y de paquetes bajo la app que los abrió (un `ping` corrido en la Terminal cuenta bajo la Terminal), y avisa cuando un programa puede ver o falsificar tráfico por debajo de TCP y UDP. Un único botón con menú agrupa la vista por app, dominio, dirección, puerto o país y la filtra por dirección del tráfico, protocolo y estado de la regla, y lo recuerda por usuario. En el mapa cada app tiene su propio color, los arcos hacia un país siguen visibles, puntos en movimiento muestran la dirección de cualquier tráfico (naranja de salida, verde de entrada), y el mapa sigue la búsqueda y los filtros. Su hilo de muestreo existe solo mientras la app está abierta.
- **Información del sistema** (0.8.5, Acerca de LordOS → Informe del sistema…): cada pieza de hardware y cómo la ve LordOS; la placa gráfica mostrada capa por capa (driver, salida de pantalla, nodo de render, contexto de GPU, composición, scanout directo) con el motivo exacto cuando la pantalla se compone en la CPU; un panel de Compatibilidad; los módulos de memoria desde las tablas del firmware, mediante una acción de administración de solo lectura que nunca lee números de serie; una pestaña Informe con el texto exacto antes de copiarlo o guardarlo (no se envía nada a ningún lado); y las Verificaciones, con **Probar** para la Prueba del aislamiento (§6.4, §11.4). `lordos-sysreport report` imprime el mismo informe en una terminal (Markdown o `--json`, sin números de serie, direcciones ni nombres), y `lordos-fetch` muestra la corona de LordOS junto a un resumen del equipo y de cómo se aíslan las apps (`--share` omite los nombres de usuario y de equipo).
- **Banco de pruebas** (`lordos-testbench`, 0.8.5): una ventana de carta de ajuste, un cliente Wayland común, que verifica el servidor gráfico desde afuera: los tiempos de cuadro y la presentación, cada tecla, el mouse y la rueda, los protocolos Wayland ofrecidos (§3.4), una escena 3D que dice si la dibuja una GPU real o el procesador, video generado a 24, 30 o 60 cuadros por segundo y el bloqueo del puntero. Las teclas que LordOS se reserva aparecen marcadas; su último informe aparece en Información del sistema → Verificaciones.
- **LordOS Imager** (0.8.4): escribe una imagen de LordOS con la firma verificada en una tarjeta SD o un pendrive USB, nunca en un disco interno, la vuelve a leer para comparar y prepara el primer arranque de la Raspberry Pi 5 (nombre, idioma, teclado, una cuenta con solo el hash de la contraseña, Wi-Fi con solo la clave derivada, SSH, escritorio remoto, tema y refrigeración). La misma app existe para macOS, Linux y Windows.
- **Discos:** lector SMART propio, el informe SMART completo con smartmontools, formateo y pendrives de arranque verificados con SHA-256.
- **Copias de seguridad:** instantáneas con enlaces duros y cifrado opcional, que corren con prioridad baja.
- **El Dock** (0.8.1) lo ordena el usuario, con apps fijadas y las acciones propias de las apps; el tema predeterminado sumó acomodo de ventanas y una vista de tareas, y el tema **Lord Classic** pasó a ser una segunda apariencia completa (0.8.3, §8).

### 5.2 Apps `.lpk` instalables

Las apps nativas de terceros se empaquetan como `.lpk`: un tar comprimido con gzip con un manifiesto, el `app.js` empaquetado, un ícono obligatorio y traducciones y recursos opcionales. Una app no trae React ni el SDK; usa los del sistema, y eso la mantiene chica (la app de ejemplo Fractal pesa 5 KB). Se instalan sin root en `~/.local/share/lordos/apps/<id>/`. El instalador rechaza enlaces, dispositivos, rutas absolutas, `..`, caracteres raros, más de 2.000 archivos o 64 MB, y reemplaza la versión anterior de forma atómica. Cada una corre aislada (§6.4).

### 5.3 La Tienda y los paquetes de Debian

La Tienda (rediseñada en la 0.8.5) tiene una barra lateral en tres grupos (Descubrir, Para este equipo, Instalado y Actualizaciones; Inteligencia Artificial y las diez categorías de Debian con sus cantidades; las apps de LordOS) y muestra las apps como tarjetas en una grilla que llena la ventana: ícono, nombre, categoría, dos líneas de resumen, el tamaño de la descarga y un botón Obtener, Abrir o Ver. Descubrir abre con un banner destacado, después el estante de IA y un estante por categoría; la página de una app tiene una fila de datos (versión, tamaño, categoría, distribución). Muestra las apps de Debian por categoría con búsqueda, las apps `.lpk` y los archivos `.deb` locales, sugerencias para el hardware de este equipo (firmware faltante, drivers de impresoras y escáneres, herramientas para máquinas invitadas, 0.8.2; con su propia página, Para este equipo, desde la 0.8.5) y las actualizaciones propias de LordOS (0.8.1), y siempre muestra un plan antes de instalar o quitar. Los paquetes de Debian los maneja **el motor de paquetes propio de LordOS** (`server/src/pkg`): lee las fuentes de apt, verifica las firmas de `InRelease` con su propio código OpenPGP (RSA y Ed25519), rechaza índices vencidos, repetidos, de otra suite o con datos agregados al final, verifica cada índice y cada `.deb` contra los valores SHA-256 del índice firmado, y resuelve dependencias (planes idénticos a los de apt en 12 casos de prueba). Solo el último paso es de Debian: `dpkg` desempaqueta y configura, y sigue siendo el único que escribe su base de datos, así `apt` sigue funcionando. Desde la 0.8.5, las fuentes `https://` se descargan por HTTPS a través de `lord-tls`, un helper aislado que verifica la cadena de certificados y el nombre del servidor y solo pasa bytes (§6); antes se descargaban por `http://`. La integridad sigue descansando en las firmas y los hashes en los dos casos; HTTPS suma privacidad (los nombres de los paquetes descargados ya no quedan visibles en la red) y rechaza un servidor que no es quien dice ser, y una descarga segura nunca se redirige a HTTP sin cifrar. A `lord-tls` lo ejecuta el hijo de descarga (que corre como `nobody` en su propio scope de descarga) con una ruta fija, sin argumentos, con un entorno vacío y el socket ya conectado; aplica la caja de arena de los ayudantes antes de leer nada, usa TLS 1.2 o superior con SNI, nunca interpreta HTTP y nunca decide sobre los paquetes. La actualización y las instalaciones de la Tienda lo usaron en la VM 116 y en una máquina VMware. Desde la 0.8.5 el motor además envía un `+` en el nombre de un paquete como `%2B`, como apt, así paquetes como `kms++-utils` se descargan desde la CDN de lordos.org.

**Inteligencia Artificial (0.8.5).** Una categoría de la Tienda armada desde un catálogo curado en el repositorio (`apt/catalog.json`, con las claves de los fabricantes en `apt/keys/` y las recetas de compilación de LordOS en `apt/recipes/`): nueve apps para amd64 y arm64, cada una con su tipo, fabricante, licencia, resumen y una fuente fijada; una app aparece atenuada en una arquitectura que no soporta. Claude Code, Claude Desktop, Visual Studio Code y Cursor se instalan desde el repositorio APT firmado de su propio fabricante ("direct"; `apt/build.sh check` verifica la huella fijada de la clave, el `InRelease` firmado, el hash del índice y el paquete de cada arquitectura): la Tienda primero dice de quién es el repositorio que agrega y la huella de su clave, y después una acción de administrador (`pkg-source-add`) lo escribe con la clave del fabricante, que tiene que tener exactamente la huella fijada en el catálogo, y trae las listas; de la interfaz solo llega el id de la app, nunca una URL, una clave, un nombre de archivo ni un campo. El archivo `.sources` (con `Signed-By` esa clave) y el llavero son archivos de formato fijo llamados `lordos-<id>`, o con los nombres propios del fabricante cuando su paquete siempre escribe su propia fuente (el `cursor.sources` de Cursor, porque una segunda entrada para el mismo repositorio con otro `Signed-By` hace que apt no lea ninguna fuente); `pkg-source-remove` quita el repositorio junto con la app, salvo que lo use otra app instalada del catálogo. Los paquetes de los fabricantes nunca se modifican; lo que hacen sus scripts de mantenedor al instalarse (VS Code, Claude Desktop y Cursor agregan su propia fuente de APT) está documentado en `docs/UPDATES.md`. llama.cpp 0.7.0, whisper.cpp 1.9.5 y codex-cli 0.164.0 los compila LordOS desde fuentes oficiales verificadas en contenedores `debian:trixie` sin red (codex-cli es el binario estático de OpenAI, con su firma de sigstore verificada con cosign), y goose 1.56.0 y opencode 1.19.4 son los paquetes de sus fabricantes copiados después de comprobar un SHA-256 fijado; los cinco se publican en el repositorio firmado de LordOS, armado y firmado en la máquina del mantenedor. Una app que compila LordOS y que las listas locales todavía no tienen ofrece volver a traerlas. llama.cpp se compila con todas las variantes de CPU (la mejor se elige al ejecutarse) y un backend Vulkan que se carga solo si hay un dispositivo Vulkan, y baja los modelos de Hugging Face la primera vez que se ejecuta (`-hf`); no trae ningún modelo incluido. Probado en una máquina VMware: llama.cpp instalado desde la Tienda y `llama-server -hf ggml-org/Qwen3-0.6B-GGUF:Q8_0` respondió por su API HTTP en la CPU a unos 33 tokens/s (sin dispositivo Vulkan); en una GPU todavía no se probó en hardware real (solo se verificaron el camino por CPU y la carga del backend con Vulkan por software). En la VM 116 se agregaron las fuentes de Cursor y de VS Code, se instaló Cursor (su propio script reescribió el mismo archivo), `apt update` y la actualización propia de la Tienda siguieron limpios, y se quitó.

**Comerciales (0.8.5).** Apps gratis sin código fuente público, de los repositorios APT firmados de sus fabricantes con la clave que fija LordOS: Chrome (amd64 y arm64), Edge y Spotify (amd64). La página dice, antes de instalar nada, que LordOS no puede revisar ni corregir lo que hacen. Sus paquetes escriben su propia fuente de APT, así que el catálogo nombra los archivos que escriben Chrome y Edge (`google-chrome.sources`, `microsoft-edge.sources` y sus llaveros) y LordOS escribe exactamente esos; para Spotify, que agrega `spotify.list` salvo que ese archivo exista, un campo del catálogo, `optout`, hace que `pkg-source-add` lo cree vacío (solo si falta; `pkg-source-remove` lo borra solo mientras siga vacío). Probado en una máquina VMware con la acción de administración y el motor de paquetes: las tres fuentes agregadas, las tres apps instaladas (Chrome 156.0.8078.47, Edge 156.0.4314.50, Spotify 1.2.95), `apt update` sin errores ni advertencias y la actualización del motor limpia después, y la ventana de Chrome abriéndose en la sesión. El paquete de Spotify además copia su clave en `/etc/apt/trusted.gpg.d/`, en la que apt confía para cualquier fuente sin `Signed-By` (las fuentes propias de Debian tienen uno). Steam necesita multiarch i386, que el motor de paquetes todavía no hace.

**El banner destacado (0.8.5).** El banner de Descubrir puede cambiar sin un lanzamiento: `apt/featured/featured.json` (un objeto JSON de esquema 1 de 64 KB como máximo) y `banner.png` (un PNG de 2 MB como máximo) los verifica `apt-repo.sh` y se publican firmados con los índices (`main/featured/`). El motor de paquetes los toma solo de una fuente cuyo Release firmado dice `Origin: LordOS`, verifica sus hashes, interpreta el JSON y la firma del PNG, y los guarda en `/var/lib/lordos/pkg/`; la interfaz los lee con `lord.pkg.featured()`, en solo lectura. Mientras la imagen no está, la Tienda dibuja un esqueleto del banner (que late salvo que Reducir movimiento esté activado), y un repositorio sin banner muestra en su lugar una app del catálogo. Probado en la VM 116 contra lordos.org/apt-staging.

### 5.4 Apps de Linux

Las apps de Linux con un archivo `.desktop` aparecen en el lanzador de apps. El lanzamiento pasa por `lord.apps.launch(id)`: la interfaz solo puede nombrar un archivo `.desktop` instalado; `lord-server` lo vuelve a leer, lo verifica, arma `argv` a partir de su línea `Exec` según la especificación y lo ejecuta con `execve`, sin shell ni argumentos del llamador, dentro de su propio scope de cgroup que pertenece a root. Chromium y Firefox vienen preinstalados en la ISO y en la imagen para Raspberry Pi; desde la 0.8.4 dibujan una sola barra con sus pestañas y los botones de ventana de LordOS (un interruptor en Ajustes → Apariencia devuelve el marco de LordOS), y una extensión opcional mantiene fluido el video sin decodificación por hardware (0.8.2). Las apps que solo hablan X11 corren a través de Xwayland (§3.4). **LordOS no aísla las apps de Linux de forma predeterminada**: corren como el usuario, como en cualquier Debian. Tienen atribución, el firewall por app y las restricciones por app que fije el dueño (§6.6), incluida una vista opcional "sin acceso a tus archivos" que aplica el lanzador. Desde la 0.8.5 un archivo `.desktop` cuyo nombre tiene un espacio u otro carácter raro (el de LordOS Imager es `LordOS Imager.desktop`) recibe un id con esos caracteres cambiados por `-` y se lanza desde su archivo real, y **Forzar salida…** (el menú de la corona de Lord Flow) termina solo el proceso propio de una app de Linux o aislada, a través de `lord.wayland.kill`, que acepta solo un cliente Wayland del mismo usuario (a una app X11, en cambio, se le pide que se cierre).

### 5.5 WebAssembly

LordOS incluye su propio intérprete de WebAssembly en C (`server/src/wasm`), expuesto como el objeto global estándar `WebAssembly`, así una app nativa puede traer módulos compilados desde C, C++, Rust o Zig. Cubre WebAssembly 2.0 sin SIMD (el MVP más extensión de signo, conversiones saturadas, multi-valor, memoria masiva y tipos de referencia), pasa 26.290 de 26.290 aserciones de la suite oficial de pruebas en la VM, y convierte cada error en un trap en lugar de un fallo. En el ejemplo del proyecto corrió de 3 a 5 veces más rápido que el mismo código en JavaScript.

### 5.6 Instalar LordOS

- **ISO live** para PC de 64 bits, con un instalador gráfico (UEFI y BIOS).
- **Imagen para Raspberry Pi 5** para tarjeta SD o NVMe (§7.4), escrita y preparada con LordOS Imager, que además escribe un archivo de primera ejecución que la placa aplica una sola vez en su primer arranque (validado de forma estricta; no se aplica nada si alguna parte está mal).
- **Sobre un Debian 13 existente**: `curl -fsSL https://lordos.org/install.sh | sh`. El script pregunta antes de cambiar nada, descarga `SHA256SUMS` y verifica su firma Ed25519 con la clave de releases embebida en el script, se niega a seguir sin una firma válida, verifica el SHA-256 del `.deb`, lo instala con apt y configura el sistema como la ISO. Los archivos del release se publican en GitHub Releases.

### Archivos fuente

- [apps](https://github.com/lordbasex/LordOS/tree/main/apps), [server/src/lpk.c](https://github.com/lordbasex/LordOS/blob/main/server/src/lpk.c), [tools/lpk.mjs](https://github.com/lordbasex/LordOS/blob/main/tools/lpk.mjs)
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg), [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c)
- [server/src/wasm/wasm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wasm/wasm.c), [server/src/ui/js_wasm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_wasm.c)
- [tools/install/install.sh](https://github.com/lordbasex/LordOS/blob/main/tools/install/install.sh)

## 6. Seguridad

### 6.1 Modelo de amenazas

LordOS está diseñado contra estas amenazas:

- **Una app instalada maliciosa o comprometida** que quiere los archivos, las claves, el micrófono, la cámara, el portapapeles o la red del usuario.
- **Documentos y mensajes maliciosos**: PDF, archivos comprimidos, fuentes, imágenes, video y correo armados para explotar un lector.
- **Una app de Linux que quiere sacar datos**, o escaparse de las reglas que se le pusieron.
- **Un atacante en la red local** que intenta usar el escritorio remoto o alterar descargas.
- **Paquetes y releases alterados.**

Fuera del modelo, por ahora: un kernel o una cuenta root comprometidos; ataques físicos; canales laterales; y apps de Linux que el dueño no restringió, que corren como el usuario, como en cualquier Debian, y pueden leer sus archivos. A las apps de Linux se las limita solo con las restricciones que el dueño les fija (§6.6), y solo cuando se abren desde el lanzador de apps.

### 6.2 Mínimo privilegio

- `lord-server` corre como el usuario. En una sesión administrada nunca tiene los dispositivos de pantalla ni de entrada por sí mismo: `lord-sessiond` se los presta y se los revoca.
- Las bibliotecas que leen datos no confiables las enlazan solo ayudantes: FFmpeg, libva y los codificadores de video, `lord-media`; Poppler y libarchive, `lord-doc`; OpenSSL, `lord-maild`, `lord-fetch`, `lord-tls` (HTTPS para las descargas del motor de paquetes: verifica el certificado y el nombre del servidor y solo pasa bytes; sobre los paquetes sigue decidiendo el índice firmado) y `lord-sessiond`; libsrtp2 y usrsctp, `lord-sessiond`, que las usa solo en el puente que corre como el usuario después del inicio de sesión; Monocypher, `lord-vault`.
- Los ayudantes reciben archivos y dispositivos como descriptores abiertos por `lord-server`; nunca abren rutas.
- El compositor nunca espera a un ayudante ni a una app, así un fallo o un cuelgue queda contenido.

### 6.3 Los procesos root y sus interfaces cerradas

| Proceso | Acepta | Nunca hace |
|---|---|---|
| ayudante `admin.c` | una tabla fija de acciones, con argumentos validados como root | acciones genéricas, texto del usuario en una shell |
| `lord-sessiond` | pedidos de inicio de sesión desde la pantalla de inicio; JSON de sesión corto (≤ 4 KB) | dejar que la pantalla de inicio verifique contraseñas; leer píxeles como root |
| su hijo de ubicación en cgroups | `enter <scope> <limits>` solo para el proceso que llama, desde la hoja `server` de una sesión, corriendo un binario del directorio de `lord-server` | ubicar procesos arbitrarios; devolverle el árbol al usuario |
| `lord-netd` | acciones `net-*` solo de pares root | acciones que no son de red; ejecutar nada que venga de una sesión |
| `lord-netperm` | `RELOAD` sin datos | ejecutar programas; recibir argumentos; abrir sockets de internet |
| `lord-fand` | `STATUS` para cualquiera; `PROFILE`, `CURVE`, `FORCE` para administradores | nombrar rutas o dispositivos; ejecutar programas; usar la red |
| el hijo de descarga de la Tienda | mover su propio pid a su scope de descarga de root, desde la hoja `server` de su sesión, y después bajar a `nobody` | ubicar cualquier otro proceso |

En la 0.8.5 la tabla del ayudante de administración sumó solo acciones cerradas del mismo tipo: `pkg-source-add` y `pkg-source-remove <id>` (§5.3: de la interfaz solo llega un id del catálogo; la entrada, la huella de su clave y cada campo se verifican como root contra el catálogo de root que trae el paquete lordos), `ssh-root`, `ssh-root-key`, `ssh-root-password` y `ssh-root-status`, de solo lectura (§3.6: solo administradores, cada cambio con la contraseña del propio administrador verificada como root), y `hw-memory-modules`, de solo lectura (Información del sistema: los dispositivos de memoria SMBIOS, interpretados de forma estricta en C, nunca números de serie, etiquetas de inventario ni números de pieza).

### 6.4 Qué está aislado, y la caja de arena de las apps

No todos los programas de LordOS están aislados. Esto es exactamente lo que se aplica a cada tipo:

| Tipo | Proceso | Caja de arena | Scope de cgroup | Broker y permisos | Firewall y restricciones (§6.6) |
|---|---|---|---|---|---|
| Apps `.lpk`, y Contraseñas, Mail, Calendario, Contactos y Recordatorios (0.8.2) | propio (`lord-server --app <id>`) | Landlock, seccomp, sin capacidades | propio, de root, con límites | cada llamada se verifica en C; las cuatro apps de PIM tienen solo sus propios pedidos de correo, de una lista permitida | reglas de red y las ocho restricciones que no son de red, en el broker |
| Las demás apps incluidas | dentro de `lord-server` | no (son parte del escritorio) | el de `lord-server` | sin broker; cámara, micrófono y pantalla se verifican en C | como "LordOS": reglas de red; cámara, micrófono, pantalla y notificaciones |
| Apps de Linux desde el lanzador de apps | propio | ninguna de forma predeterminada; con "sin acceso a tus archivos", espacios de nombres de usuario, montaje y PID propios más Landlock, desde el próximo inicio | propio, de root | no | reglas de red; cámara, micrófono, USB, sockets Bluetooth, archivos y notificaciones; no el portapapeles |
| Programas de la Terminal | hijos de la shell de la Terminal | no | el scope de componente propio de la Terminal (0.8.5) | no | como "Terminal": reglas de red, con los nombres de los programas en el Monitor |
| Apps X11 | bajo Xwayland | como su entrada del lanzador | propio, como las apps Wayland | no | lo mismo que las apps de Linux |

Un programa iniciado de otra forma, fuera del lanzador de apps, no está en ningún scope de app y no recibe reglas por app. No se usan Flatpak ni bubblewrap.

Cada app `.lpk`, y la app Contraseñas, corre como `lord-server --app <id>`: el mismo binario en "modo app", lanzado por `lord-server` con un id de paquete validado como único argumento. El proceso se conecta al compositor como cliente Wayland y abre el socket del broker, después aplica la caja de arena, y recién entonces carga el código de la app (`server/src/sec/sandbox.c`):

- **Landlock** (ABI hasta 6): acceso de solo lectura al paquete, al runtime compartido de las apps, a las fuentes y a las bibliotecas; lectura y escritura solo en `~/.local/share/lordos/appdata/<id>`; acotamiento de sockets Unix abstractos y de señales.
- **seccomp-bpf**: una lista de llamadas al sistema permitidas en todos los hilos (TSYNC), sin `ptrace`, `mount`, `kexec`, `bpf`, `perf_event_open`, `keyctl`, carga de módulos ni `execve`. Se rechazan los sockets Unix nuevos y `socketpair`, porque Landlock hasta la ABI 6 no filtra `connect` a sockets Unix con nombre; así la app queda lejos del bus de sesión, del servicio de impresión y del socket de Wayland.
- **Sin privilegios**: `no_new_privs`, un conjunto de capacidades vacío y el conjunto límite descartado.
- **cgroup v2**: `app-lordos-lpk-<id>.scope`, creado por root, con `memory.high` 192 MB, `memory.max` 512 MB y un núcleo de CPU.
- **Red**: sin el permiso `network` (declarado **y** permitido), seccomp rechaza los sockets de internet y Landlock rechaza connect y bind de TCP. No se usan espacios de nombres de red, porque crear uno necesita privilegios que `lord-server` no tiene.

Si falta una función del kernel, la app arranca igual y el registro dice qué parte falta. La app de prueba maliciosa a propósito (`tests/lpk-evil`) fracasa en todo lo que intenta; `build/sandbox-test` pasa 14 de 14 verificaciones para apps y 14 de 14 con red. Medido en una VM: unos 13,7 MB de RSS por app aislada, un arranque mediano de 106 ms y 0,89 ms para aplicar la caja de arena.

**En el propio equipo del usuario (0.8.5).** Información del sistema → Verificaciones → **Probar** arranca el paquete oculto del sistema `sysprobe` como arranca cualquier app aislada; en modo app, solo para ese paquete, `server/src/app/probe.c` toma la misma caja de arena e intenta lo que tiene que fallar: la carpeta personal, `/etc/passwd`, el `cgroup.procs` del cgroup raíz, un socket de internet y uno Unix, `ptrace`, un espacio de nombres de usuario, `mount` y `exec` (su propia carpeta de datos tiene que funcionar, como control). Un hijo creado con fork antes de la caja de arena toma el perfil de los ayudantes de decodificación e intenta un socket, `exec`, `fork` y archivos; y la prueba le pide a su broker lo que se le tiene que rechazar (una ruta fuera de su carpeta, el portapapeles sin permiso, la llave de seguridad, el correo). `lord-server` agrega lo que solo él puede leer: `NoNewPrivs`, `Seccomp` y `CapEff` del proceso y el `memory.max` de su cgroup. En la VM 116 se rechazaron todos los intentos (9 + 4 + 4), con seccomp en modo 2, sin privilegios nuevos y sin capacidades. La prueba nunca recibe un permiso ni una autorización de red.

### 6.5 El broker, los permisos y el diálogo de archivos

[[diagram:permission]]

La app aislada habla con `lord-server` solo por un socket `SOCK_SEQPACKET`. Cada pedido se verifica en C: acceso a archivos solo dentro de la carpeta de datos propia de la app; lectura del portapapeles solo con el permiso `clipboard`; pedidos de permiso respondidos según la decisión guardada o, si no hay, con un **aviso dibujado por el sistema** (*Permitir / No permitir*); cámara y micrófono solo a través de `lord-server`.

- **Las decisiones** se guardan por usuario en `~/.config/lordos/permisos.json`, fuera del alcance de la app. La consulta es O(1).
- **El diálogo de archivos es el powerbox propio de LordOS** (`sdk/powerbox.jsx`), no `xdg-desktop-portal`: lo dibuja `lord-server`, que abre el archivo elegido y pasa solo ese descriptor. La única interfaz de portal de escritorio que implementa LordOS es `org.freedesktop.portal.Settings`, para la apariencia de GTK.
- **Micrófono:** `lord-server` es dueño del stream de PipeWire y le da a la app un anillo compartido de solo lectura.
- **Cámara:** `lord-server` abre el dispositivo y se lo da solo a `lord-media`; la app recibe una vista de solo lectura de los cuadros decodificados y tiene que confirmar cada uno, y `lord-server` valida cada confirmación.
- **Revocación:** denegar la cámara o el micrófono en Ajustes los corta al instante (se mata el `lord-media` de la app; se cierra el stream de PipeWire). El permiso `network` se decide al lanzar la app.
- **Llavero:** los pedidos al llavero de una app aislada llevan un dueño que `lord-server` fija a partir del id verificado de la app.

**Las restricciones les ganan a los permisos.** Una restricción del firewall (§6.6) gana sobre cualquier decisión guardada: el pedido se rechaza sin aviso, aunque la app haya tenido permiso antes, y restringir una app en marcha le quita la cámara y el micrófono al instante, como revocar.

**Límites.** No hay "permitir una vez". Algunos permisos declarables (Bluetooth, lanzar apps, notificaciones) todavía no tienen llamada en el broker, así que las apps aisladas directamente no pueden usar esas capacidades; escuchar en un puerto depende del permiso de red. Una app de Linux que corre como el usuario podría editar el archivo de permisos, salvo que esté restringida con "sin acceso a tus archivos".

### 6.6 El firewall por app, sus restricciones y el Monitor

[[diagram:firewall]]

`lord-netperm` aplica las reglas por app en el kernel. Las apps de Linux abiertas desde el lanzador de apps corren en `app-lordos-<id>.scope`, las apps `.lpk` en `app-lordos-lpk-<id>.scope`, y el resto de la sesión cuenta como `lordos`. Todos los scopes pertenecen a root. El servicio carga siete programas de red (`connect4/6`, `sendmsg4/6`, `bind4/6`, `sock_create`), un programa de dispositivos (`cgroup/dev`) y un programa LSM por cgroup sobre `socket_create` en el cgroup de `lordos.service`, identifica la app de cada proceso por su ancestro de cgroup y devuelve permitir o denegar (`EPERM`). Los programas están compilados dentro del binario y se cargan con un cargador propio de LordOS sobre la llamada al sistema `bpf()`, que además lee el BTF del kernel para encontrar el gancho LSM; no se usan libbpf ni bcc. Si el kernel rechaza el programa de dispositivos o el LSM, los programas de red siguen funcionando y el servicio lo avisa.

- **Las reglas** admiten dominios exactos, comodines, redes IPv4/IPv6, `lan`, `internet`, puertos, protocolos y dirección. Primero van las restricciones (sin internet, sin LAN, sin red, sin entrantes); después gana la regla más específica, y denegar le gana a permitir a igual especificidad; después las apps de confianza; después el modo (silencioso, el predeterminado, o apagado). `npcompile.c` precalcula todo esto en mapas hash y de coincidencia por prefijo más largo. Una prueba corre el código de decisión del kernel contra el decisor de referencia sobre 480.000 conexiones aleatorias: 0 diferencias.
- **Los nombres de dominio** vienen solo del resolvedor del sistema: la interfaz de monitoreo de systemd-resolved o, sin él, las respuestas del puerto 53 de los servidores configurados, leídas por un socket de paquetes que solo recibe. Nunca se confía en respuestas que una app recibió por sus propios sockets. Por eso, una app que resuelve nombres por su cuenta con DNS cifrado (DNS sobre HTTPS dentro de un navegador, por ejemplo) no queda alcanzada por las reglas de dominio: una regla "permitir solo example.com" la sigue bloqueando, pero una regla "bloquear example.com" no la frena. Las reglas por dirección y por red, el valor predeterminado de la app y sus restricciones siguen valiendo, y Ajustes lo dice debajo de las reglas.
- **Las reglas en la interfaz.** Ajustes → Privacidad → Reglas de red por app muestra el valor predeterminado de cada app ("Permitir todo" o "Bloquear todo salvo las reglas") y sus reglas en el orden en que ganan, con cuándo coincidió cada una por última vez; la hoja para agregar una regla revisa lo que se escribe mientras se escribe. En Monitor → Red, "Permitir / Bloquear este dominio" sobre un destino de una app abre la misma hoja, completada con el nombre que dio el resolvedor del sistema para esa dirección.
- **Escapes.** El árbol de cgroups pertenece a root, así que una app no puede moverse sola. Un proceso que le pide al gestor de systemd del usuario una unidad nueva (`systemd-run --user`) termina bajo `user@<uid>.service`; `lord-netperm` engancha los mismos programas ahí y somete esos procesos a la unión más estricta de las apps restringidas en ejecución de ese usuario. Probado: todos los intentos de escape denegados, los otros usuarios sin afectar.
- **Costo**: alrededor de 0,5 µs por connect de UDP, TCP dentro del ruido, sin cambio en el rendimiento, porque nada corre por paquete.
- **El Monitor** muestra las conexiones de cada app (desde `sock_diag`, más los sockets ICMP, crudos y de paquetes desde `/proc/net`) y los destinos que el firewall informó hace poco, los bytes, los destinos por país desde una base de datos local y un interruptor de Regla por app. Ninguna dirección sale de la máquina para consultas: sin DNS inverso, sin geolocalización en línea.
- **Conexiones abiertas (0.8.3).** Las reglas se aplican a las conexiones ya abiertas, no solo a las nuevas: los programas también descartan los paquetes que una regla ahora deniega, y `lord-netperm` destruye esos sockets por `sock_diag` (`SOCK_DESTROY`), así que una app bloqueada ve terminar sus conexiones en una fracción de segundo (0,04–0,37 s para TCP y 0,08 s para QUIC en una Pi 5 con el kernel de LordOS).
- **Los componentes propios de LordOS (0.8.5).** La sincronización de correo y calendario, la Tienda y las actualizaciones, las impresoras y los escáneres, la Terminal y las actualizaciones de las listas de bloqueo corren cada uno en un scope propio, así que cada uno se puede ver y bloquear por separado; sus sockets los abre un pequeño conector que `lord-server` vuelve a ejecutar con argumentos fijos, que entra al scope del componente y devuelve solo un socket conectado. Un conector (`lord-server --net-connector mail|print|lists`) tiene una lista de argumentos y un entorno fijos, `no_new_privs`, no se puede volcar, responde solo pedidos de tamaño fijo (familia, tipo, dirección, puerto), sin nombres, rutas ni ejecución, y falla cerrado: si no puede entrar a su scope termina y el pedido falla. El hijo de descarga de la Tienda entra a su propio scope con el mismo criterio antes de bajar a `nobody` (§6.3). La resolución de nombres, la sincronización de hora, el servicio de red y los túneles VPN se muestran y nunca se bloquean; bloquear la Tienda pide confirmación antes, porque se detienen las actualizaciones de seguridad.
- **Listas de bloqueo y "denegar en silencio" (0.8.5).** Listas públicas de sitios de publicidad, rastreadores, malware y ataques (cada una con su fuente y su licencia) se descargan por HTTPS una vez por día con `lord-fetch`, un ayudante sin sockets propios y con el certificado verificado contra las autoridades del sistema, y cada línea se vuelve a validar; una regla que el usuario escribe para un sitio siempre gana sobre una lista. Un modo nuevo del firewall, "denegar en silencio", deja conectarse solo lo que una regla permite y registra el resto, mientras los servicios propios del equipo siguen funcionando. Un ítem opcional de la barra de menú muestra las tasas de tráfico y lo que se denegó hace poco.

**Restricciones más allá de la red.** Además de las cuatro restricciones de red (sin internet, sin LAN, sin red, sin conexiones entrantes), el dueño puede restringirle a una app la cámara, el micrófono, la pantalla, el USB, los archivos, el portapapeles, el Bluetooth y las notificaciones. Cada una se aplica donde se entrega el recurso, se verifica contra el scope de la app y nunca contra un nombre que la app se da a sí misma, y gana sobre cualquier permiso otorgado en `permisos.json`.

[[diagram:restrictions]]

| Restricción | Apps `.lpk` y Contraseñas | Apps incluidas ("LordOS") | Apps de Linux desde el lanzador de apps |
|---|---|---|---|
| cámara | broker (`brokercam.c`) | `js_media.c` | `cgroup/dev` sobre los nodos V4L2, y la guardia de PipeWire |
| micrófono | broker | `js_media.c` | la guardia de PipeWire, y `cgroup/dev` sobre los nodos de captura ALSA |
| pantalla | broker (sin API de captura en modo app) | `js_media.c` | por diseño: sin protocolo de captura, sin servidor X, sin portal de captura de pantalla; `/dev/fb*` denegado |
| USB | broker (la llave de seguridad de Contraseñas) | no se ofrece | `cgroup/dev`: nodos USB directos, hidraw y serie USB |
| archivos | broker (selector de archivos, guardados de la cámara) | no se ofrece | el lanzador: espacios de nombres propios, un home en tmpfs y Landlock, desde el próximo inicio |
| portapapeles | broker | no se ofrece | no disponible |
| Bluetooth | broker (sin API de Bluetooth en modo app) | no se ofrece | programa BPF LSM sobre `socket_create`: sin sockets `AF_BLUETOOTH` |
| notificaciones | el permiso `notifications` | `sdk/notify.js` | `notifyd`, por la identidad verificada de quien llama |

Para las apps aisladas, el broker (`sec/restrict.c`) es el único punto de aplicación: un pedido restringido se rechaza sin aviso, y restringir una app en marcha le quita la cámara y el micrófono al instante (`tests/broker_restrict_test.c` cubre cada restricción, con y sin ella). La fila "LordOS" es el escritorio mismo (Archivos, copiar y pegar, Discos, los ajustes de Bluetooth), así que no ofrece archivos, USB, portapapeles ni Bluetooth; en su lugar se restringen apps individuales. Para las apps de Linux:

- **Dispositivos.** En cgroup v2 el controlador de dispositivos es un programa BPF. El de `lord-netperm` rechaza `open()` y `mknod()` de las clases de dispositivo que la app tiene restringidas (V4L2, USB directo, hidraw, serie USB, nodos de captura ALSA, `/dev/fb*`) y permite todo lo demás. Se aplica en el próximo `open`; un descriptor ya abierto sigue sirviendo.
- **Sockets Bluetooth.** El gancho de sockets inet nunca ve `AF_BLUETOOTH`, así que un programa LSM por cgroup sobre `socket_create` los rechaza.
- **Archivos.** Justo antes de `execve`, el hijo del lanzador entra en sus propios espacios de nombres de usuario, montaje y PID, sin privilegios. La carpeta personal es un tmpfs vacío que tiene solo las carpetas propias de la app (derivadas de su id de escritorio, nunca de campos que controla un archivo `.desktop`) y el archivo con el que se abrió; la carpeta de runtime tiene solo los sockets de Wayland y de sonido, así que el bus de sesión y el systemd del usuario quedan fuera de alcance; después Landlock limita el resto. Se aplica desde el próximo inicio. Las apps GTK, Firefox y Chromium corren sin sus integraciones de D-Bus.
- **Notificaciones.** `notifyd` le pide al bus el PID de quien llama (solo cuenta una respuesta que mandó el propio bus), encuentra su scope de app y descarta la notificación de una app restringida; un error o una respuesta tardía también la descartan (falla cerrado).
- **Micrófono y cámara por PipeWire.** La guardia de PipeWire (`sec/pwguard.c`), el cliente de PipeWire propio de `lord-server`, identifica a cada cliente por el PID que PipeWire tomó de las credenciales del socket y, para una app restringida, oculta los nodos de captura y destruye sus streams de captura en menos de un segundo, incluida una grabación ya en curso. Mientras corre una app restringida, un cliente cuya identidad no se puede establecer se trata como restringido.

**Límites.** El modo "preguntar" está diseñado pero hoy se comporta como silencioso. ICMP se bloquea solo con "sin red"; las conexiones a un puerto ya abierto no se verifican. Las restricciones para apps de Linux no cubren:

- BlueZ por D-Bus: una app con el Bluetooth restringido todavía puede pedirle a `bluetoothd`, por el bus del sistema, que busque, empareje y conecte;
- el bus del sistema, que sigue al alcance incluso con "sin acceso a tus archivos";
- el portapapeles;
- las apps iniciadas fuera del lanzador de apps, que no están en ningún scope de app;
- los efectos secundarios de la unión de `user@`: mientras corre una app restringida, los servicios propios del usuario quedan sujetos a la misma unión (el audio de llamadas de unos auriculares, que PipeWire abre en cada llamada, se corta durante ese tiempo);
- una ventana de pocos milisegundos en la que un cliente nuevo de PipeWire de una app restringida tiene todos los permisos, o en la que un stream de captura puede recibir audio antes de ser destruido.

### 6.7 Ayudantes que procesan datos no confiables

[[diagram:helpers]]

`lord-doc` (PDF, archivos comprimidos, fuentes, imágenes del escáner), `lord-media` (video, audio, fotos, cámara), `lord-maild` (correo, calendarios, contactos), `lord-vault` (secretos) y, desde la 0.8.5, `lord-fetch` (las listas de bloqueo del firewall) y `lord-tls` (HTTPS para el motor de paquetes) comparten una caja de arena endurecida, `sandbox_apply_helper()`:

- sin `socket`, `socketpair`, `connect`, `bind`, `listen`, `accept`; sin `execve`; `clone` solo para hilos; seccomp en todos los hilos, y si seccomp no se puede aplicar el ayudante termina;
- archivos solo a través de descriptores que pasa `lord-server`; ioctls solo por clase (DRM, V4L2, DMA-BUF) donde hace falta;
- resultados devueltos en memoria compartida sellada, validada por `lord-server`.

`lord-media` ni siquiera recibe el socket de PipeWire; el audio pasa por un anillo compartido cuyo stream es de `lord-server`. `lord-maild` no puede crear sockets: se lo pide a `lord-server`, que conecta solo con los servidores del archivo de cuentas (y, después de que el usuario pide cargar imágenes, solo con direcciones públicas en los puertos 80/443 durante diez minutos) y le entrega el socket conectado. `lord-fetch` y `lord-tls` tampoco tienen sockets propios: cada uno recibe un socket ya conectado (por el conector `lists`, o por el hijo de descarga de la Tienda), verifica el certificado del servidor contra las autoridades del sistema y su nombre con TLS 1.2 o superior, y eso nunca se relaja; `lord-fetch` hace un único GET de HTTP de 16 MB como máximo, interpreta la lista de forma estricta y le devuelve las redirecciones a `lord-server` (tres como máximo, solo https), que vuelve a validar cada línea. `build/sandbox-test helper` pasa 17 de 17 verificaciones.

### 6.8 Firma de paquetes y niveles de confianza

Los paquetes `.lpk` llevan un manifiesto con el SHA-256 de cada archivo y una firma Ed25519 sobre él. SHA-256 y Ed25519 son código propio de LordOS (Ed25519 siguiendo la aritmética de TweetNaCl), probado contra los vectores del NIST y del RFC 8032 (55 de 55). La verificación ocurre en C al instalar y al cargar. La Tienda muestra el nivel de confianza: **LordOS** (clave oficial, compilada dentro del binario para que ningún archivo pueda agregar otra), **Autor verificado** (listado en el catálogo firmado), **Firmado por <nombre>**, o **Sin firmar** con una advertencia. Las actualizaciones tienen que estar firmadas con la misma clave. Un catálogo firmado lleva una lista de revocación. Si editás una app instalada, LordOS la marca "Modificada por vos", puede restaurar la original o compartir tu versión firmada con tu propia clave, con crédito al original.

**Estado:** los paquetes `.lpk` oficiales todavía se firman con una clave de desarrollo, hasta que la reemplace la clave final. Todavía no está construido un repositorio firmado de apps de LordOS para las apps `.lpk`; el propio LordOS se actualiza desde un repositorio APT firmado (§6.9).

### 6.9 Firma de releases

Cada archivo de release (el `.deb`, la ISO y la imagen para Raspberry Pi) aparece con su SHA-256 en `SHA256SUMS`, y `SHA256SUMS` está firmado con la clave de releases de LordOS (Ed25519). La clave pública es `tools/release/lordos-release.pub.pem`, también embebida en el instalador para Debian. Su huella (SHA-256 de la clave pública en DER) es:

`c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

La clave privada vive solo en la máquina del mantenedor, nunca en el repositorio ni en una máquina de compilación. Cada release trae además una lista de materiales de software (SBOM) SPDX 2.3 por arquitectura, listada en `SHA256SUMS` y por lo tanto cubierta por la firma, y sus notas salen de `CHANGELOG.md`, escrito en el formato Keep a Changelog. La §11 muestra cómo verificar una descarga.

- **El repositorio APT (0.8.1).** `lordos.org/apt` es un repositorio APT firmado que se arma y se firma solo en la máquina del mantenedor; los sistemas instalados se actualizan desde ahí. En la Raspberry Pi 5 además replica el actualizador del cargador de arranque y los pocos paquetes de Raspberry Pi que necesita, copiados solo después de que su firma y el SHA-256 de cada archivo coinciden con una lista fijada (0.8.4.1).
- **El código propio de LordOS se verifica antes de correr (0.8.2).** Cada bundle de JavaScript que LordOS corre como código propio (los temas, el host de apps, las apps del sistema) se carga solo a través de `codesig_load`, que lo verifica contra `code-manifest.json` y su firma Ed25519 con la clave de releases. Un bundle modificado no se carga y el usuario ve por qué; las Verificaciones de Información del sistema lo informan (§11.4). Las compilaciones de desarrollo sin un manifiesto firmado lo dicen.
- **Las apps del sistema no se pueden quitar** desde la Tienda (0.8.2).

### 6.10 La bóveda

`lord-vault` guarda las contraseñas y el llavero de la sesión en archivos `.lvault`: una clave de bóveda aleatoria de 32 bytes, envuelta una vez por cada método de desbloqueo (contraseña con Argon2id con un costo calibrado por máquina, FIDO2 `hmac-secret` con el cliente CTAP2 propio de LordOS, archivo llave, contraseña más archivo llave, clave de recuperación); contenido sellado con XChaCha20-Poly1305 con todo el encabezado como datos asociados; adjuntos sellados por separado; guardado atómico con copia de respaldo. Se detecta cada byte invertido y cada truncamiento (1.165 de 1.165 en la prueba). Los secretos viven en un área de memoria con `mlock`, excluida de los volcados de memoria y borrada al liberarse, en un proceso que no se puede volcar. La ventana de Contraseñas corre en su propio proceso aislado, así los valores revelados nunca entran al JavaScript del escritorio. El llavero se abre al iniciar sesión con la contraseña que verificó `lord-sessiond`, y atiende a las apps de Linux mediante la implementación propia de LordOS del Secret Service. Desde la 0.8.5 la sesión arranca solo cuando ya existe el bus de sesión del usuario (§3.6), así que `org.freedesktop.secrets` y `org.freedesktop.Notifications` pertenecen a la sesión de LordOS desde el primer segundo; y cuando una app arrastra gnome-keyring, un drop-in que trae el paquete lordos (`gnome-keyring-daemon.service.d/lordos.conf`) deja solo su componente PKCS#11, así el llavero de LordOS conserva el Secret Service y no aparece una segunda ventana de "elegí una contraseña para el llavero nuevo". Probado en la máquina VMware después de un reinicio con inicio de sesión automático: los dos nombres pertenecen a la sesión de LordOS, y Chrome pregunta a través del aviso propio del llavero de LordOS. Cuando varias apps preguntan a la vez, una sola respuesta en el aviso del llavero ahora les llega a todas. Las llaves de seguridad de hardware reales todavía no se probaron.

### 6.11 Ventanas protegidas

Una ventana marcada como protegida (Contraseñas lo está) se pinta en negro en todos los caminos que copian píxeles hacia afuera: capturas, grabaciones, miniaturas, el escritorio remoto, y los menús y la pantalla completa de Wayland. El monitor real la muestra normalmente. Las apps de Linux, para empezar, no tienen ningún protocolo de captura de pantalla. Desde la 0.8.2, las teclas que se escriben en los diálogos propios de LordOS (el llavero, las contraseñas de administrador) nunca llegan a la app de Linux que está al frente.

### 6.12 Divulgación y estándares

Las vulnerabilidades se informan como describe `SECURITY.md`, con un contacto legible por máquinas en `https://lordos.org/.well-known/security.txt` (RFC 9116). Los procesos de LordOS están **alineados con, no certificados en**, ISO/IEC 29147 e ISO/IEC 30111 (divulgación y manejo de vulnerabilidades), ISO/IEC 5230 (cumplimiento de licencias de OpenChain: el inventario en `THIRD_PARTY.md`, los avisos de licencia y el SBOM) e ISO/IEC 18974 (garantía de seguridad de OpenChain: el SBOM más las actualizaciones de seguridad de Debian para los paquetes de los que depende LordOS). lordos.org publica las páginas legales y de confianza (política de seguridad, firma de código, licencias, privacidad, ciclo de soporte, estándares) y dice lo que todavía falta: sin autocertificación formal, sin cruce automático del SBOM con bases de vulnerabilidades, sin SBOM de las imágenes completas y sin auditoría de terceros.

### Archivos fuente

- [server/src/sec/sandbox.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sandbox.c), [server/src/sec/brokersrv.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokersrv.c), [server/src/sec/perms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/perms.c), [server/src/sec/cgroup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/cgroup.c)
- [server/src/netperm/np.bpf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/np.bpf.c), [server/src/netperm/netperm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/netperm.c), [server/src/fw/rules.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fw/rules.c)
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c), [server/src/sec/pwguard.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/pwguard.c), [server/src/hw/notifyd.c](https://github.com/lordbasex/LordOS/blob/main/server/src/hw/notifyd.c), [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c), [server/tests/broker_restrict_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/broker_restrict_test.c)
- [server/src/sec/sign.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sign.c), [server/src/pkg/crypto.c](https://github.com/lordbasex/LordOS/blob/main/server/src/pkg/crypto.c), [tools/release/sign-release.sh](https://github.com/lordbasex/LordOS/blob/main/tools/release/sign-release.sh), [tools/release/make-sbom.sh](https://github.com/lordbasex/LordOS/blob/main/tools/release/make-sbom.sh)
- [server/src/vault/vault.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vault.c), [server/src/vault/vfile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vfile.c)
- [server/src/media/capture.c](https://github.com/lordbasex/LordOS/blob/main/server/src/media/capture.c)
- [SECURITY.md](https://github.com/lordbasex/LordOS/blob/main/SECURITY.md), [docs/COMPLIANCE.md](https://github.com/lordbasex/LordOS/blob/main/docs/COMPLIANCE.md), [CHANGELOG.md](https://github.com/lordbasex/LordOS/blob/main/CHANGELOG.md)

## 7. Rendimiento

Todas las cifras de abajo las midió el proyecto y están documentadas en los archivos citados. La mayoría vienen de máquinas virtuales; las mediciones en hardware real se indican como tales.

### 7.1 Memoria y reposo

**Dos magnitudes distintas.** Las primeras filas miden **el sistema completo**: la columna "used" de `free`, que es todo lo que ocupan el kernel, los servicios de Debian (systemd, journald, udev, la red, PipeWire, CUPS…), los servicios root de LordOS y la sesión del usuario, sin la caché de páginas que el kernel puede devolver. Las otras filas son la **memoria residente (RSS) de un proceso**, como la informan `ps` o `/proc/<pid>/status`. Un RSS no es el consumo del sistema completo, y los RSS no se pueden sumar: las páginas de las bibliotecas compartidas se cuentan en cada proceso que las mapea. Para comparar LordOS con otro escritorio hay que comparar el "used" de `free` en la misma máquina, después del mismo tiempo en reposo y con las mismas apps abiertas; el reparto justo de las páginas compartidas por proceso es el PSS (`/proc/<pid>/smaps_rollup`). Cómo repetir ambas mediciones, y los resultados de cada versión, están en §11.5.

| Medición | Valor | Dónde |
|---|---|---|
| Sistema completo, ISO live, en reposo después de un minuto (4 GB, 4 núcleos) | 465 MB usados, CPU 99 % ociosa | VM 117, `docs/ISO.md` |
| Sistema completo, instalado desde la ISO, en reposo | 487 MB usados, CPU 99 % ociosa | VM 120, `docs/ISO.md` |
| Arranque, ISO live con UEFI | 16,3 s (4,9 s kernel + 11,4 s espacio de usuario) | VM 117, `docs/ISO.md` |
| `lord-server` sin placa 3D (Mesa sin cargar) | unos 32 MB de RSS | `docs/GPU.md` |
| Una sesión de usuario (`lord-server`), pruebas de escritorio remoto | unos 50 MB; pantalla de inicio 41 MB | VM 118, `docs/REMOTE.md` |
| Una app `.lpk` aislada | 13,7 MB de RSS | VM 122, `docs/PRIVACY.md` |
| Sistema completo, 0.8.5~rc1 actualizado desde 0.8.4.1, un minuto después de iniciar sesión (VM de VMware, 4,7 GB, GPU virtual) | 606–622 MB usados; el PSS de todos los procesos suma 316 MB, de los que 110 MB son el `lord-server` de la sesión y 78 MB el de la pantalla de inicio (en esa compilación quedaba para cambiar de usuario y bloquear) | `results/0.8.5~rc1-beta/vmware-fusion` |
| La pantalla de inicio cerrada después de iniciar sesión (corrección de la 0.8.5, el mismo tipo de VM de VMware) | sistema completo 603 → 548 MB usados | `CHANGELOG.md`, 0.8.5 |
| Sistema completo, 0.8.5-beta actualizado desde 0.8.4.1, unos cinco minutos después de un reinicio con inicio de sesión automático (la misma VM de VMware, composición en la GPU con SVGA3D; Chrome, VS Code, Spotify y llama.cpp instalados, sin correr) | 592 MB usados | `results/0.8.5-beta/vmware-fusion` |
| Arranque, la misma máquina con 0.8.5-beta | primer cuadro en el monitor 7,4 s después del inicio del kernel (`systemd-analyze`: 3,3 s kernel + 4,3 s espacio de usuario) | `results/0.8.5-beta/vmware-fusion` |

### 7.2 Dibujo

- **Vblank por software (0.8.5).** Una placa virtual que completa los cambios de imagen al instante (`vmwgfx` de VMware) hacía que el compositor y sus apps dibujaran 500–580 cuadros por segundo en una pantalla de 60 Hz; LordOS ahora lo detecta y marca el ritmo del refresco con una grilla fija: 59,8 cuadros por segundo, p99 de 18,4 ms, ningún cuadro fuera de tiempo (`results/0.8.5~rc1-beta/vmware-fusion`). En la 0.8.5-beta publicada, el Banco de pruebas con su vista 3D en la misma máquina midió 59,83 cuadros por segundo sobre 1.796 cuadros, p99 de 17,9 ms, un cuadro fuera de tiempo, 1.795 presentados (`results/0.8.5-beta/vmware-fusion`). La misma grilla también marca el ritmo de las sesiones sin monitor, así que esperar en milisegundos enteros ya no convierte 60 cuadros por segundo en 57.
- **Arrastrar por el camino de GPU (corrección de la 0.8.5).** Una ventana que se arrastra se pinta desde una capa en caché; en el camino de GPU cada píxel totalmente transparente de esa capa se tomaba por el hueco de una ventana de Linux, y ahora solo los píxeles dentro de una superficie de app. En la 0.8.5-beta todavía podían verse pequeños cuadrados negros en las esquinas redondeadas de una ventana que se arrastra, en los caminos de CPU y de GPU: al restaurar las esquinas, la capa tomaba el píxel guardado como opaco. En la 0.8.6 (en desarrollo) mezcla los dos píxeles con su alfa, así la capa queda transparente fuera de la curva; una prueba compara cada esquina con un repintado completo en los dos caminos (§10.2).
- **Un cuadro de arrastre más barato (0.8.6, en desarrollo).** El cuadro de una ventana que se arrastra ahora pinta solo la parte de su capa que cambió y solo la parte cambiada de lo que está debajo, con los mismos píxeles: tienen que coincidir 2.417 hashes de cuadros registrados por camino, y el único cambio aprobado deja las esquinas redondeadas bajo la ventana un nivel más cerca de un repintado completo. En el camino de GPU la propia ventana puede componerla la GPU con la aritmética exacta de la CPU, después de una autoprueba de cada combinación de píxeles ("GPU drag"), y el cuadro siguiente puede pintarse mientras el cambio de página anterior está pendiente; las dos cosas se midieron en la Raspberry Pi 5 y están apagadas por defecto. Los resultados en la Raspberry Pi 5 están en §7.4.
- Arrastrar una ventana en una pantalla de 1280×800 por el camino de CPU: unos 6,7–6,9 ms de trabajo por cuadro, alrededor de 120 cuadros por segundo (VM 118, `docs/DISPLAYS.md`).
- La misma ventana en una pantalla de 2560×1600 al 150 %: unos 5,1 ms por cuadro.
- Video: pintar directamente desde el cuadro decodificado cuando solo cambió un video opaco bajó `lord-server` del 42 % al 13 % de un núcleo para un video 1080p en ventana, y del 92 % al 15 % a pantalla completa (`docs/ROADMAP.md`, multimedia).
- El Monitor cerrado no cuesta nada: su hilo de muestreo no existe hasta que se abre (`docs/MONITOR.md`).
- **El camino por CPU en la 0.8.5** (VM 116, sin GPU, pantalla compuesta en la CPU, vista por el escritorio remoto desde un navegador con una pantalla de 60 Hz; `docs/REMOTE.md`, "Measurements (0.8.5, CPU reference)"): el desenfoque del fondo de una ventana (radio 20) pasó de 27,7 ms a 6,0 ms con un hilo y a 2,7 ms con ocho, con píxeles idénticos; una escena de prueba de 1280×800, de unos 18 a 55–58 cuadros por segundo; una sesión de 1920×1080 que reproduce un clip de 60 fps, de 45 (un núcleo al 100 %) a 55–58 cuadros por segundo con el grupo de hilos. Un video de 1080p a 24 fps en el navegador se reprodujo en tiempo real sin que el reproductor descartara ningún cuadro durante 17 s. El codificador H.264 por software tarda 12–13 ms por cuadro a 1280–1568 px de ancho, así que llega como máximo a unos 75–80 cuadros por segundo. AV1 a 60 fps sigue siendo el caso difícil sin GPU: el navegador lo decodifica por software en la misma máquina que compone la pantalla.

### 7.3 GPU

En las VM solo había un renderizador por software. Con él, el camino de CPU es más barato que la composición "por GPU", y por eso LordOS usa el camino de CPU cuando el único renderizador es por software. El beneficio real del camino por GPU (composición y presentación casi gratis, pantalla completa sin copias) se espera en placas reales, pero **todavía no se midió en hardware AMD ni Nvidia real**. Desde la 0.8.5 dos herramientas lo informan en cualquier máquina: Información del sistema muestra cada capa de la GPU y el motivo exacto cuando la pantalla se compone en la CPU, y el Banco de pruebas dibuja una escena 3D con EGL y dice si el renderizador es una GPU de hardware, una virtual o por software (en la VM 116: `llvmpipe`, por software), con su tiempo de dibujo y si sus cuadros llegan a la pantalla sin copia.

### 7.4 La Raspberry Pi 5

La Raspberry Pi 5 es la única placa soportada. Resultados en una placa real (16 GB, tarjeta SD, sin monitor salvo que se indique), de `docs/RASPBERRY.md`:

| Medición | Valor |
|---|---|
| Compilar todos los binarios de LordOS en la placa | 1 min 02 s, 0 advertencias |
| `make -C server test` | 23 pasaron, 0 fallaron, 0 omitidas |
| Copias y máscaras NEON vs. C puro | 9,90 → 4,33 ms y 11,09 → 5,49 ms por pantalla (2,0–2,3×), idénticas bit a bit |
| En reposo en la pantalla de inicio, sin monitor | `lord-server` 36 MB de RSS, 0,6 % de CPU; sistema 450 MB de 16 GB; 38–43 °C, sin limitación térmica |
| Armar la imagen SD en la placa | 8 min 18 s; 503 MB comprimida |
| Compilar el kernel de LordOS en la placa | unos 50 min por variante |

**El kernel de LordOS.** El kernel propio de Raspberry Pi no compila Landlock. Sin él, la prueba hostil del ayudante de documentos falla 5 de sus 25 verificaciones de sistema de archivos y la prueba de la caja de arena no puede correr. LordOS compila el kernel de Raspberry Pi desde su propio código fuente y configuración predeterminada, cambiando solo tres opciones (Landlock activado, la lista de LSM, el sufijo de versión), y lo fija para que un kernel más nuevo del archivo no pueda reemplazarlo. La imagen también activa el cgroup de memoria, que Raspberry Pi apaga al arrancar.

**Ventiladores.** En la Pi 5 dentro de un gabinete de prueba, `lord-fand` mantuvo el ventilador apagado a 50–53 °C durante una carga de 4 hilos (el gabinete de aluminio funciona como disipador), lo llevó al 55 % (4.500 RPM) y a velocidad máxima (7.400 RPM) con curvas de prueba, y restauró la configuración propia del kernel después de `SIGTERM` y de `kill -9` (`docs/FANS.md`).

**Monitor e imagen.** Desde la 0.8.4.1 la imagen para Raspberry Pi 5 muestra el escritorio en un monitor HDMI en el primer arranque (la línea del driver de pantalla en `config.txt` ahora va después de las líneas de autodetección del firmware, y LordOS espera hasta 30 s al driver), pone la hora desde la red y trae Chromium y Firefox ESR. Desde la 0.8.4 el kernel de LordOS para la Pi (`6.18.57-lordos.3-rpi-2712`) además destruye las conexiones de una app bloqueada. **Medido con monitor (LordBench, 0.8.6 en desarrollo):** la imagen 0.8.5 arrancada con un monitor de 1920×1080 a 60 Hz en cualquiera de los dos puertos HDMI llega a la pantalla de inicio de sesión 5,99 s después de que arranca el kernel (systemd: 2,1 s de kernel y 4,2 s del resto). Con composición por GPU (V3D), una ventana movida por el compositor se mantiene en 59,4 cuadros por segundo, con un cuadro armado en 10,9 ms (mediana) y 13,0 ms (percentil 99) y el 1,0 % de cuadros tarde; con una carga en todos los núcleos a la menor prioridad baja a 24 cuadros por segundo con el 96,5 % tarde, un problema que encontró LordBench: el cgroup del compositor pesaba lo mismo que el de la carga, así que la baja prioridad de la carga no contaba. Darle al compositor diez veces el peso de CPU de una app subió la misma prueba, sin el monitor, de 43–45 a 56–57 cuadros por segundo (31–33 % → 7–8 % tarde) y llevó un movimiento del puntero a la pantalla en 22 ms en vez de 32 ms (mediana); con el monitor solo se midió junto con la prioridad de tiempo real que sigue. Además, el hilo principal del compositor y sus hilos de pintado ahora corren con la menor prioridad de tiempo real, que lord-sessiond permite y lord-server abandona antes de lanzar nada, así que ninguna app la hereda; en la VM 116 bajo carga eso llevó los cuadros tarde de 2,7–2,9 % a 0,28 % y el 1 % más lento de los movimientos del puntero de 31–32 a 25 ms, como sin carga, con la carga recibiendo el mismo tiempo de CPU. En la Pi 5 con su monitor los dos cambios llevaron la prueba con carga de 24 a 29–30 cuadros por segundo y el compositor ya no espera por la CPU, pero con la carga un cuadro tarda 15–16 ms en armarse en vez de 7, así que no llega al refresco de 60 Hz; esa causa todavía se está buscando. La escena de LordBench tenía un cuadrado que debía girar y estaba quieto, porque se le daba una clave de estilo que el motor no tiene; ahora gira, y eso hace la prueba más pesada: con él, ese código daba 53–58 cuadros por segundo sin carga y 24–25 con la carga. En desarrollo para la 0.8.6, el cuadro de una ventana que se arrastra pinta solo lo que cambió (la parte cambiada de la capa de la ventana y de lo que está debajo), con los mismos píxeles, comprobado cuadro por cuadro contra hashes registrados; con eso la misma prueba da 59,8–60 cuadros por segundo sin carga (percentil 99 de 11,3–11,6 ms) y 30 con la carga, donde un cuadro se arma en 12,9 ms (mediana) en vez de unos 20. Con la carga casi todos los cuadros siguen llegando tarde. Una medición final de la versión en desarrollo (2026-10-29) midió los dos cambios que faltaban. Componer en la GPU la ventana que se arrastra pasa su autoprueba en la GPU de la Pi y dio exactamente los píxeles de la CPU en los 138 cuadros comparados, pero sin una ganancia medible (30,8–31,6 cuadros por segundo con la carga, contra 30,5–31,1 sin ella). Pintar el cuadro siguiente mientras el anterior espera el cambio de página dio 33–36 cuadros por segundo con la carga, pero un movimiento del puntero llegó a la pantalla 17 ms más tarde sin carga (42 en vez de 25,5 ms, mediana) y 29 ms más tarde con la carga (78 en vez de 49 ms). Los dos quedan en el código y están apagados por defecto. Con los valores por defecto finales la Pi 5 muestra 59,9 cuadros por segundo sin carga y todavía pasa de 60 a unos 31 (30,5–31,1) con una carga en todos los núcleos. Lo que queda es el tiempo de la GPU por cuadro, que pasa de 1,5 ms sin carga a 14–16 ms con la carga, mientras los relojes y la temperatura (36,7–47,2 °C) no muestran ninguna reducción de frecuencia ni por temperatura; la causa probable es la competencia por la memoria entre la GPU y los núcleos ocupados, todavía no demostrada. La alarma de bajo voltaje de la placa está presente y marcó 0 con la fuente oficial. En 3 minutos de carga en todos los núcleos con el ventilador la placa mantuvo 5,9 GFLOP/s con 55,6 °C como máximo, y decodifica video 1080p por software a 258 (H.264), 246 (HEVC), 204 (VP9) y 159 (AV1) cuadros por segundo.

### 7.5 PC modernas

Todavía no hay mediciones en PC reales en el repositorio. La ISO se prueba en máquinas virtuales (UEFI y BIOS); está planeado probarla en una máquina AMD real desde el USB live. LordBench (0.8.6) está diseñado para medir de forma repetible la memoria y la CPU en reposo, el tiempo de arranque, los tiempos de cuadro, el video y el inicio de apps, y los informes de hardware de la comunidad (0.8.7), para recolectar esas mediciones de máquinas reales (`docs/BENCHMARK.md`). En desarrollo para la 0.8.6 (sin publicar; la última versión es la 0.8.5-beta), su primera parte ya está construida: un ayudante aislado, `lord-bench`, que mide la CPU, la memoria, la GPU fuera de pantalla, el almacenamiento y una carga sostenida, se detiene cerca del límite de temperatura o cuando el procesador baja la velocidad, y registra las condiciones de cada prueba, que Información del sistema → Rendimiento muestra como «×N» de máquinas de referencia; también mide la decodificación de video por software y los codificadores del escritorio remoto, los tiempos de cuadro del escritorio y el tiempo de un movimiento del puntero hasta la pantalla, solo y bajo una carga de baja prioridad, y el tiempo de arranque en varios reinicios; también corre desde una terminal (`lordos-sysreport bench`). También en desarrollo, mide el trabajo de la propia interfaz de LordOS con el código real del motor (el diseño y dibujo de una escena de escritorio fija en full HD, y la composición tipográfica de párrafos en latín, árabe, devanagari, tailandés y chino con fuentes incluidas en el ayudante, que no abre archivos; en la máquina virtual de referencia 37 cuadros por segundo en un núcleo y 40 con los hilos de dibujo, 619 pasadas de composición por segundo en un núcleo y 3.595 en ocho; en la Raspberry Pi 5, en dos pruebas rápidas, 39 y 45 cuadros por segundo y 630 y 2.520 pasadas de composición por segundo en uno y cuatro núcleos), comparado con una referencia solo si se midió con la misma versión de LordOS, y `lord-bench` comprueba su propio aislamiento antes de cualquier trabajo: seis cosas que no debe poder hacer (abrir un socket local o una conexión de red, ejecutar un programa, iniciar un proceso, crear un archivo, leer un archivo del sistema) fueron todas rechazadas en esa máquina, y una prueba en la que alguna pase se marca como inestable. En una Raspberry Pi vigila el firmware de la placa durante toda la prueba: la alarma de bajo voltaje, el único indicador que expone el kernel de LordOS para la Pi, los indicadores completos de limitación del firmware donde un kernel más viejo los tiene, y los 80 °C, donde el firmware empieza a bajar la velocidad del procesador, que detienen una prueba de pico como limitada; la sección Raspberry Pi de Información del sistema lee la misma alarma y los mismos indicadores y deja afuera lo que el kernel no muestra. Esto se probó con archivos de sistema simulados y en la Pi 5, donde la alarma está presente y marca 0 con la fuente oficial; no se provocó un bajo voltaje real. Las referencias son una máquina virtual y una Raspberry Pi 5 (§7.4); la decodificación de video por hardware y una referencia de PC real todavía no están construidas y pasaron a la 0.8.7.

### Archivos fuente

- [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md), [docs/RASPBERRY.md](https://github.com/lordbasex/LordOS/blob/main/docs/RASPBERRY.md), [docs/FANS.md](https://github.com/lordbasex/LordOS/blob/main/docs/FANS.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md)
- [tools/rpi/build-kernel.sh](https://github.com/lordbasex/LordOS/blob/main/tools/rpi/build-kernel.sh), [tools/rpi/build-img.sh](https://github.com/lordbasex/LordOS/blob/main/tools/rpi/build-img.sh)
- [server/src/canvas.c](https://github.com/lordbasex/LordOS/blob/main/server/src/canvas.c)

## 8. Experiencia visual

- **Una guía de diseño.** Las apps siguen una guía común (`docs/DESIGN.md`): una grilla de espaciado de 8 px, una escala tipográfica chica, alturas de controles consistentes y una sola acción con el color de acento por vista. El SDK exporta los tokens (`spacing`, `typeScale`, `controlSize`), y cada app pasó por rondas de pulido documentadas con capturas de antes y después en claro, oscuro, de derecha a izquierda y en ventanas chicas.
- **Temas.** Un tema es un escritorio completo escrito en React: barra de menú, Dock, lanzador de apps, Centro de control y marco de ventana, más sus tokens. LordOS trae **Lord Flow**, con barra de menú y Dock, y desde la 0.8.3 **Lord Classic** como segunda apariencia completa: barra de tareas, menú de inicio con búsqueda, bandeja, ajustes rápidos, panel de notificaciones, acomodo de ventanas con distribuciones y su propio estilo para cada app. Los dos tienen acomodo de ventanas y vista de tareas. Los temas cambian en vivo; la configuración se conserva. En la 0.8.5 los dos temas tomaron estos nombres (antes "Predeterminado" y "Clásico"; la elección del usuario se conserva), Ajustes → Apariencia → Estilo del escritorio pasó a ser el único lugar que lista todos los temas instalados, y si un tema se rechaza, el último tema que cargó queda como respaldo.
- **Menús y ventanas (0.8.5).** Un menú Ventana más completo en los dos temas: Minimizar, Zoom, Rellenar, Centrar; Mover y redimensionar a mitades, cuartos y tercios, o disposiciones de dos o cuatro ventanas; Mover a otra pantalla; Traer todo al frente; Volver al tamaño anterior; y las ventanas abiertas. En Lord Flow, un clic en el nombre de la app en la barra de menú abre su menú de app (Acerca de la app con su propia versión, sus Ajustes, Ocultar, Ocultar otros, Mostrar todo, Salir), y el menú de la corona suma la Tienda con la cantidad de actualizaciones pendientes, Elementos recientes y Forzar salida… (§5.4). Lord Classic, que no tiene barra de menú, muestra los menús propios de una app en una franja debajo de su barra de título. Un ítem del Dock puede abrir su app al iniciar sesión y, con varias pantallas, asignarse a una (§3.5). Las apps transparentes como la Terminal reciben un borde y una línea de barra de título opacos.
- **Claro y oscuro, acentos.** Los dos modos y el color de acento se aplican en vivo a las apps nativas, a las apps aisladas y, a través del portal y de GSettings, a las apps GTK.
- **De derecha a izquierda.** Toda la interfaz se espeja para el árabe, el hebreo y escrituras similares, incluidas las apps aisladas, que siguen en vivo el idioma de la sesión.
- **Lumo.** La mascota de LordOS es Lumo, un pequeño zorro celestial, que aparece en pantallas de bienvenida, estados vacíos y material de marca, con un conjunto fijo de poses definido en la guía de marca.
- **Accesibilidad hoy:** una opción "Reducir movimiento" que apaga las animaciones decorativas, atajos de teclado en todo el sistema y pantallas escalables hasta el 200 %. Un lector de pantalla, zoom y alto contraste están en la hoja de ruta y **todavía no están construidos**.

### Archivos fuente

- [docs/DESIGN.md](https://github.com/lordbasex/LordOS/blob/main/docs/DESIGN.md), [docs/LORDOS-BRAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/LORDOS-BRAND.md)
- [themes](https://github.com/lordbasex/LordOS/tree/main/themes), [sdk/controls.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/controls.jsx), [sdk/i18n.js](https://github.com/lordbasex/LordOS/blob/main/sdk/i18n.js)

## 9. Extensibilidad

- **El SDK** (`@lordos/sdk`) es lo único que importan las apps: ventanas, configuración, tokens del tema, controles, archivos, multimedia, notificaciones, llavero, impresión y más. Las apps no importan nada de los temas, así cualquier app funciona en cualquier tema.
- **Escribir una app** es una carpeta con `manifest.json` e `index.jsx`. `node tools/lpk.mjs <folder>` la empaqueta sin React ni el SDK, agrega el ícono y las traducciones, y la firma si se le da una clave; `keygen`, `sign`, `verify` y `catalog` cubren el resto. Las apps pueden incluir módulos de WebAssembly y dibujar píxeles crudos con `<Image pixels>`.
- **Los temas** siguen un contrato fijo (`Desktop`, `MenuBar`, `Dock`, `Launcher`, `ControlCenter`, `WindowFrame`, tokens), documentado en `themes/README.md`, con el tema Lord Classic como plantilla. Desde la 0.8.5 `docs/THEMES-GUIDE.md` explica, para un desarrollador de React nuevo en LordOS, las partes de un tema (theme.json, tokens, el shell, los marcos de ventana, los menús, las capas), derecha a izquierda, claro y oscuro, varias pantallas y las reglas de rendimiento, con una lista de verificación, y `examples/theme-minimal/` es un tema chico que funciona, para empezar. Un tema que LordOS no firmó todavía no carga en una compilación de release: no hay un formato de paquete de temas ni una decisión de confianza del usuario.
- **La interoperabilidad con Linux** pasa por estándares: Wayland, el portal de configuración, el Secret Service, el servicio de notificaciones, las apps predeterminadas de `mimeapps.list` y los archivos `.desktop`.
- **Automatización:** hoy no hay ninguna API de scripting ni de automatización más allá del SDK. Desde la 0.8.5 la Tienda instala apps de IA (§5.3), pero son apps de Linux comunes, con las mismas reglas que cualquier otra; las integraciones de asistentes de IA con el escritorio mismo (por ejemplo a través del Model Context Protocol) **no existen en LordOS**; son solo una posible dirección futura y tendrían que respetar la misma regla de que el JavaScript no puede ejecutar comandos.

### Archivos fuente

- [sdk](https://github.com/lordbasex/LordOS/tree/main/sdk), [apps/README.md](https://github.com/lordbasex/LordOS/blob/main/apps/README.md), [themes/README.md](https://github.com/lordbasex/LordOS/blob/main/themes/README.md)
- [docs/NATIVE-APPS.md](https://github.com/lordbasex/LordOS/blob/main/docs/NATIVE-APPS.md), [docs/WASM.md](https://github.com/lordbasex/LordOS/blob/main/docs/WASM.md), [examples/apps/fractal](https://github.com/lordbasex/LordOS/tree/main/examples/apps/fractal)

## 10. Hoja de ruta y limitaciones

### 10.1 Dónde estamos

La 0.8.0 Beta fue el primer lanzamiento público (18 de octubre de 2026); la 0.7.0 fue una compilación interna y nunca se publicó. Los hitos M1 a M9 están completos: el servidor gráfico, el motor de interfaz, el SDK y los temas, la terminal, los idiomas y las escrituras complejas, la compatibilidad con apps de Linux, los paquetes y los servicios del sistema, las sesiones, el aislamiento y el firewall, y las suites de apps de M9 (documentos e impresión, multimedia, apps del sistema, contraseñas, configuración de hardware, correo y calendario). Desde entonces: 0.8.1 (actualizaciones desde la Tienda, el Dock, SSH), 0.8.2 (código del sistema verificado, apps de PIM aisladas, salud de los discos, historial de tráfico), 0.8.3 (apps X11, Lord Classic, endurecimiento del escritorio remoto, corte de conexiones abiertas), 0.8.4 (escritorio remoto como video WebRTC con sonido, LordOS Imager), 0.8.4.1 (cargador de arranque de la Raspberry Pi desde Ajustes, imagen para la Pi con HDMI corregido) y 0.8.5 Beta, publicada el 27 de octubre de 2026 (Información del sistema con Verificaciones y la Prueba del aislamiento, el Banco de pruebas, resultados versionados, la segunda generación de Monitor → Red, listas de bloqueo, el modo "denegar en silencio", componentes en sus propios scopes, Archivos → Este equipo, un camino por CPU más rápido, cuadros al ritmo del refresco en placas virtuales, HTTPS en el motor de paquetes, una Tienda rediseñada con las categorías Inteligencia Artificial y Comerciales, cuyos repositorios de fabricante se instalan con la clave que fija LordOS, y un banner destacado firmado, root por SSH como opción avanzada, un menú Ventana más completo, la pantalla de inicio cerrada después de iniciar sesión, la sesión arrancada cuando ya existe su bus de sesión). El Changelog de más abajo lista cada lanzamiento.

### 10.2 Limitaciones conocidas

- Hardware real: no se probaron GPU AMD y Nvidia, monitores reales en PC, notebooks (batería, brillo, tapa), controladores Bluetooth, impresoras y escáneres, llaves FIDO2 ni proveedores de correo reales.
- La Raspberry Pi 5 es la única placa soportada, con la compilación de LordOS de su kernel con Landlock; su imagen 0.8.5 se arrancó y se usó con un monitor de 1920×1080 en los dos puertos HDMI; otros monitores y resoluciones no se probaron.
- Las apps de Linux no están aisladas de forma predeterminada. Las apps X11 necesitan el paquete `xwayland` de Debian.
- Firewall: sin modo "preguntar"; las reglas por dominio no ven a las apps que resuelven nombres con su propio DNS cifrado.
- Las restricciones para apps de Linux no cubren BlueZ por D-Bus, el bus del sistema, el portapapeles ni las apps iniciadas fuera del lanzador de apps; la unión de `user@` también sujeta a los servicios propios del usuario mientras corre una app restringida; un cliente nuevo de PipeWire tiene una ventana de pocos milisegundos antes de que la guardia lo restrinja (§6.6).
- La clave de firma de `.lpk` todavía es una clave de desarrollo, y todavía no hay un repositorio firmado para las apps `.lpk`.
- Permisos: no hay "permitir una vez"; no se muestra el uso de cámara y micrófono por parte de apps de Linux.
- Hay que desactivar Secure Boot; la firma con un shim está pendiente.
- El motor de paquetes todavía no hace multiarch i386, así que Steam no se ofrece.
- Apps de IA en una GPU: el backend Vulkan de llama.cpp no se probó en hardware AMD, NVIDIA ni Intel real.
- Arrastrar una ventana en la 0.8.5-beta: pueden verse pequeños cuadrados negros en sus esquinas redondeadas mientras se mueve, en el monitor y en los cuadros del escritorio remoto (corregido para la 0.8.6, en desarrollo, §7.2).
- En la Raspberry Pi 5 una app que ocupa todos los núcleos todavía reduce a la mitad la frecuencia de cuadros del escritorio (unos 31 en vez de 60 cuadros por segundo en la prueba de LordBench): el tiempo de la GPU por cuadro crece con la carga, y los dos cambios medidos para eso en la 0.8.6 (en desarrollo) dieron muy poco y están apagados por defecto (§7.4).
- Arranque con BIOS heredado: un rectángulo negro (el área de terminal de GRUB mientras carga el kernel y el initrd) se ve unos segundos entre el menú de arranque y la pantalla de carga.
- Wayland: sin arrastrar y soltar, sin métodos de entrada de LordOS dentro de apps de Linux (`text-input`), sin `xdg-activation`, sincronización explícita, control de tearing ni tabletas (§3.4).
- Sin GPU, el video AV1 a 60 fps es pesado, porque el navegador lo decodifica por software en la misma máquina.
- La accesibilidad (lector de pantalla, zoom, alto contraste) no está construida.

### 10.3 Lo que viene

Lo próximo, según lo planeado en `docs/ROADMAP.md` y `docs/BENCHMARK.md`: **0.8.6, LordBench**, mediciones repetibles de CPU, memoria, GPU, el compositor, video y almacenamiento; **0.8.7, informes de hardware de la comunidad**, publicados desde Información del sistema con el consentimiento del usuario; y una conexión rechazada por el firewall por app, verificada en el propio equipo del usuario. Xwayland y el tema Lord Classic, planeados aquí para la 1.0 en la 0.8.0, salieron en la 0.8.3. Flatpak y Flathub no están planeados para ninguna versión: queda una revisión de diseño en `docs/FLATPAK.md` para el futuro, pero para mantener la estabilidad del sistema no se construye.

Después de la 1.0: una app de virtualización de escritorio construida sobre KVM y QEMU; un contenedor Android con LineageOS y apps desde la Tienda de LordOS; el modo "preguntar" del firewall; accesibilidad; y un video de demostración completo.

### Archivos fuente

- [docs/ROADMAP.md](https://github.com/lordbasex/LordOS/blob/main/docs/ROADMAP.md), [CHANGELOG.md](https://github.com/lordbasex/LordOS/blob/main/CHANGELOG.md)

## 11. Cómo verificar el proyecto por tu cuenta

### 11.1 Compilalo

En una máquina de desarrollo con Node.js, empaquetá los temas, el SDK y las apps:

```
npm install && npm run build
```

En Debian 13, compilá los binarios y corré las pruebas:

```
sudo apt-get install -y --no-install-recommends build-essential
make -C server -j4
make -C server test        # every self-contained suite
make -C server test-full   # adds the helpers, the sandbox and the HarfBuzz comparison
```

`make test` falla si falla cualquier suite; una suite cuya herramienta o datos no están instalados informa "skipped" en lugar de pasar en silencio. Las pruebas de la caja de arena necesitan un kernel con Landlock. La ISO y la imagen para Raspberry Pi se arman como se describe en `docs/ISO.md` y `docs/RASPBERRY.md`. En la VM 116 con el árbol de la 0.8.5: 23 pasaron, 0 fallaron, 10 omitidas (las suites de Node.js, que corren en la máquina de desarrollo).

### 11.2 Qué cubren las pruebas

- **Terminal y texto:** el emulador VT, el shaping OpenType contra HarfBuzz, el bidi Unicode contra el archivo de prueba oficial, los métodos de entrada y la separación de palabras en tailandés contra libthai.
- **Seguridad:** la caja de arena (app, red, ayudante), la firma (vectores del NIST y del RFC 8032), el estado de los paquetes, las reglas de rutas de archivos comprimidos contra archivos maliciosos, las protecciones de `lord.fs`, las reglas del firewall (con 20.000 mutaciones de JSON y 30.000 operaciones aleatorias contra una referencia lineal), el lector DNS (200.000 paquetes aleatorios), el compilador de mapas del kernel (480.000 conexiones) y la prueba de integración con el kernel (18 de 18 en una VM).
- **Lectores:** correo, MIME, sanitizador de HTML, iCalendar, vCard y XML (30.953 verificaciones con fuzzing por mutación), el formato de la bóveda, los importadores, QR, CBOR, la serialización de D-Bus, el lector de la base de países, el journal, TZif e IPP.
- **Motor y hardware:** layout y pintado, NEON vs. C, el driver de touchpad con gestos grabados, ventiladores sobre árboles sysfs falsos, la disposición de pantallas, el sonido y la suite de la especificación de WebAssembly. Desde la 0.8.6 (en desarrollo; la última versión publicada es la 0.8.5 Beta), cada cuadro de escenas aleatorias, y de una escena de Monitor con una Terminal translúcida y desenfocada minimizada al Dock, se compara con un repintado completo a través de una copia de la contabilidad de daño y doble búfer del compositor; esa comparación encontró y corrigió siete errores de repintado parcial, y la misma prueba sobre un escritorio en marcha en una máquina virtual (la escena de la 0.8.2, 22 ciclos de minimizar y restaurar, 54 comparaciones por corrida, antes y después, con un solo rectángulo dañado y con varios) no encontró ninguna zona desactualizada. Una ventana con desenfoque tapada por completo por otra opaca ya no se vuelve a pintar con cada cambio cerca de ella: una Terminal translúcida detrás del mapa en movimiento de Monitor hacía que cada cuadro volviera a pintar el 46 % de la pantalla, ahora del 11 al 15 % con un solo rectángulo dañado y del 3 al 4 % con varios en esa máquina virtual, comprobado de la misma forma, sin ninguna zona desactualizada.
- **Información del sistema (0.8.5):** el recolector sobre cinco máquinas falsas (un escritorio AMD, una notebook Intel, un escritorio Nvidia, una Raspberry Pi 5, una máquina virtual) llenas de números de serie, UUID, direcciones MAC y nombres que no deben llegar a un informe (no llega ninguno, 460 verificaciones), el lector de memoria SMBIOS contra tablas armadas a propósito y malformadas, y la evaluación de las Verificaciones y de la Prueba del aislamiento (56 verificaciones).

En la compilación de la ISO final: `make -C server test` informó 23 pasaron, 0 fallaron, 1 omitida (faltaban los datos de bidi).

### 11.3 Verificá un release

Descargá `SHA256SUMS`, `SHA256SUMS.sig`, los archivos del release y la clave pública (`tools/release/lordos-release.pub.pem`). Comprobá que la huella de la clave sea `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e` y después, con OpenSSL 3:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

El SBOM de cada arquitectura (`lordos_<version>_<arch>.spdx.json`) también está en `SHA256SUMS`, así que la misma verificación lo cubre.

### 11.4 Verificá un sistema en marcha

- `systemctl status lord-netperm lord-fand lordos` muestra los servicios; la prueba de la ISO confirmó los siete programas de red enganchados. Si el kernel rechaza el programa de dispositivos o el LSM, `lord-netperm` informa `"devices":false` o `"bluetooth":false`.
- `cat /proc/<pid>/status | grep -E 'Seccomp|NoNewPrivs'` sobre un proceso `lord-server --app` muestra la caja de arena.
- `systemd-cgls` muestra el árbol `lordos-session-<uid>`, de root, con un scope por app.
- Escritorio remoto: `ss -ltn` no muestra nada en el puerto 5673 hasta que lo activás en Ajustes → Compartir.
- **Información del sistema → Verificaciones (0.8.5)** hace estas verificaciones por vos y dice de cada una si se encontró configurada o si de verdad se probó: el servidor gráfico sin root, Landlock, el firewall por app con sus piezas del kernel, cada bundle del código propio de LordOS verificado en esta sesión, el puerto del escritorio remoto cerrado cuando está apagado (leído de `/proc/net`), el árbol de cgroups de la sesión de propiedad de root, la composición en la GPU, y **Probar** para la Prueba del aislamiento (§6.4).
- **`lordos-testbench --auto`** imprime un informe JSON con la lista de verificación de Wayland, los tiempos de cuadro y de presentación, el renderizador 3D y la latencia de la entrada a la pantalla; `lordos-sysreport report --json` imprime el informe de hardware sin números de serie, direcciones ni nombres.

### 11.5 Resultados versionados

Cada versión publica sus mediciones como archivos en `results/<versión>/<máquina>/` (y adjuntos al release de GitHub): el informe del hardware, el informe del banco de pruebas, la memoria del sistema completo con el PSS de cada proceso y el tiempo de arranque, cada carpeta con sus condiciones y los comandos exactos que la produjeron (`tools/results/collect.sh`, `docs/RESULTS.md`). Desde 0.8.6 se suman los resultados de LordBench. Un número de este documento que tenga ese archivo se puede comparar con él y repetir.

## 12. Licencia y créditos

El código de LordOS se publica bajo la **licencia MIT**. Los componentes de terceros conservan sus propias licencias, listadas en `THIRD_PARTY.md`: QuickJS y stb (incluidos como código fuente), Monocypher (CC0/BSD-2), la lista de palabras de la EFF (CC BY 3.0 US), la lista de contraseñas comunes (MIT), los datos de países de DB-IP Lite (CC BY 4.0), Natural Earth (dominio público) y las fuentes Inter, JetBrains Mono y Plus Jakarta Sans (OFL). Cada release trae un SBOM SPDX de lo que el paquete incluye y de lo que depende (`docs/COMPLIANCE.md`).

LordOS lo crea el grupo **iPERFEX**: **Pablo Miranda** y **Federico Pereira**.

## 13. Glosario

- **Broker:** el único socket por el que una app aislada le pide a `lord-server` cualquier cosa que toque las cosas del usuario; cada pedido se verifica en C.
- **cgroup (v2):** un mecanismo del kernel que agrupa procesos para contabilizar y limitar recursos. LordOS le da a cada app su propio grupo, un "scope", y lo usa para identificar la app en el firewall.
- **Compositor:** el programa que combina las ventanas de todas las apps en la imagen final de la pantalla.
- **DRM/KMS:** las interfaces del kernel para los dispositivos gráficos y para configurar los modos de pantalla.
- **eBPF:** programas pequeños y verificados que corren dentro del kernel; LordOS los engancha a cgroups para decidir sobre las conexiones de red, la apertura de dispositivos y los sockets Bluetooth.
- **evdev:** la interfaz del kernel para los dispositivos de entrada.
- **LSM (BPF LSM):** los ganchos de seguridad del kernel; con el LSM `bpf`, un programa chico enganchado por cgroup puede rechazar una operación, como crear un socket Bluetooth.
- **Espacio de nombres (namespace):** una función del kernel que le da a un proceso su propia vista de algo (usuarios, montajes, ids de proceso); LordOS usa espacios sin privilegios para las apps de Linux a las que se les restringen los archivos del usuario.
- **Ayudante:** un proceso chico de LordOS que maneja datos no confiables o secretos en una caja de arena más estricta (`lord-doc`, `lord-media`, `lord-maild`, `lord-vault`, `lord-fetch`, `lord-tls`).
- **Landlock:** una función del kernel que permite que un proceso sin privilegios restrinja qué archivos y puertos de red puede usar él, y sus hijos.
- **`.lpk`:** el formato de paquete de LordOS para apps nativas instalables.
- **`.lvault`:** el formato de archivo de bóveda cifrada de LordOS.
- **Guardia de PipeWire:** el cliente de PipeWire propio de `lord-server` que les oculta los dispositivos de captura a las apps de Linux restringidas y frena sus streams de captura.
- **Powerbox:** un diálogo de archivos dibujado por el sistema que le da a la app solo el archivo que eligió el usuario.
- **QuickJS:** un motor de JavaScript chico escrito en C.
- **SBOM:** una lista de materiales de software, el listado legible por máquina de los componentes que contiene un release, en formato SPDX.
- **Scope:** un cgroup para una app, como `app-lordos-lpk-<id>.scope`.
- **seccomp-bpf:** un filtro del kernel que restringe qué llamadas al sistema puede hacer un proceso.
- **Secret Service:** el estándar de D-Bus de freedesktop que usan las apps de Linux para guardar secretos.
- **Wayland:** el protocolo entre las apps gráficas y el compositor.

## 14. Dónde leer más

| Capítulo | Documentos en `docs/` |
|---|---|
| 1. Visión | ARCHITECTURE.md, LORDOS-BRAND.md |
| 2. Arquitectura | ARCHITECTURE.md, NETWORK.md, FANS.md, SETTINGS.md, SYSTEM.md |
| 3. Compositor | GPU.md, DISPLAYS.md, WINDOWS.md, TRACKPAD.md, WAYLAND.md, SESSIONS.md, REMOTE.md |
| 4. Motor de interfaz | ARCHITECTURE.md, LANGUAGES.md, TERMINAL.md |
| 5. Aplicaciones | NATIVE-APPS.md, PACKAGES.md, UPDATES.md, WASM.md, FILES.md, MEDIA.md, DOCUMENTS.md, MAIL.md, ISO.md, INSTALL-DEBIAN.md |
| 6. Seguridad | PRIVACY.md, FIREWALL.md, MONITOR.md, SIGNING.md, PASSWORDS.md, MEDIA.md, REMOTE.md, COMPLIANCE.md, y `SECURITY.md` |
| 7. Rendimiento | ISO.md, RASPBERRY.md, FANS.md, DISPLAYS.md, GPU.md, REMOTE.md, BENCHMARK.md |
| 8. Experiencia visual | DESIGN.md, LORDOS-BRAND.md, LANGUAGES.md, DOCK.md |
| 9. Extensibilidad | NATIVE-APPS.md, WASM.md, THEMES-GUIDE.md, y `apps/README.md`, `themes/README.md` |
| 10. Hoja de ruta | ROADMAP.md, RELEASING.md, UPDATES.md y `CHANGELOG.md` |
| 11. Verificación | PAPER-VALIDATION.md, RESULTS.md, BENCHMARK.md, IMAGER.md |

## Changelog

Cada lanzamiento, con sus notas completas en GitHub (salen de `CHANGELOG.md`, en el formato Keep a Changelog). Este documento se actualiza con cada cambio.

| Versión | Fecha | Qué trajo |
|---|---|---|
| **0.8.5 Beta** | 2026-10-27 | Información del sistema (hardware, la GPU por capas, Compatibilidad, Informe, Verificaciones y la Prueba del aislamiento), `lordos-sysreport`, `lordos-fetch`, el Banco de pruebas, resultados versionados, la segunda generación de Monitor → Red (un árbol por app, destino y puerto; sockets ICMP, crudos y de paquetes; un color por app), los componentes de LordOS en sus propios scopes, listas de bloqueo descargadas por `lord-fetch`, el modo "denegar en silencio", Archivos → Este equipo y cada disco, la Tienda rediseñada con las categorías Inteligencia Artificial y Comerciales y un banner destacado firmado, descargas por HTTPS a través de `lord-tls`, root por SSH como opción avanzada, un camino por CPU en hasta 8 hilos, cuadros al ritmo del refresco en placas virtuales, video del escritorio remoto a la frecuencia de refresco del espectador, un menú Ventana más completo y Forzar salida, números de pantalla y pantallas que se pegan al arrastrarlas, la pantalla de inicio cerrada después de iniciar sesión, un bloqueo más rápido, la corrección del bus de sesión y del llavero, idle-inhibit y el portapapeles devuelto a su dueño. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.5-beta) |
| **0.8.4.1 Beta** | 2026-10-25 | Raspberry Pi 5: Ajustes → Firmware de la placa (el cargador de arranque), su actualizador en el repositorio firmado de LordOS, una imagen nueva con el HDMI corregido en el primer arranque, hora de red, navegadores y los paquetes del sistema de la ISO. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.4.1-beta) |
| **0.8.4 Beta** | 2026-10-19 | Escritorio remoto como video WebRTC con sonido, por UDP o TCP en su propio puerto, que se reconecta solo, con STUN y TURN; LordOS Imager (en LordOS y para macOS, Linux y Windows) con el archivo de primera ejecución de la Pi; navegadores con una sola barra superior; conexiones de las apps bloqueadas cerradas en la Pi 5. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.4-beta) |
| **0.8.3 Beta** | 2026-10-19 | Apps X11 a través de Xwayland; la selección primaria; Lord Classic como segunda apariencia completa; acomodo de ventanas y la vista de tareas; listas de permitidos y de bloqueo del escritorio remoto, un bloqueo después de 3 inicios de sesión fallidos y una revisión de seguridad; reglas del firewall aplicadas a las conexiones abiertas. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.3-beta) |
| **0.8.2 Beta** | 2026-10-19 | LordOS verifica su propio código antes de correrlo; Mail, Calendario, Contactos y Recordatorios aislados; temperatura y salud SMART de los discos; sugerencias para este equipo; historial de tráfico; interruptores por destino; video fluido en el navegador sin decodificación por hardware. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.2-beta) |
| **0.8.1 Beta** | 2026-10-19 | Actualizaciones desde la Tienda y un repositorio APT firmado, migraciones entre versiones, un Dock que ordenás a tu gusto, inicio de sesión remoto (SSH), Reiniciar y Apagar en el menú de la corona, detener procesos desde el Monitor, Bluetooth en la imagen para la Pi 5. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.1-beta) |
| **0.8.0 Beta** | 2026-10-18 | El primer lanzamiento público: los hitos M1 a M9. [Notas del release](https://github.com/lordbasex/LordOS/releases/tag/v0.8.0-beta) |
