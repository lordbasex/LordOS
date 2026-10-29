## En pocas palabras

LordOS es una distribución GNU/Linux basada en Debian 13. Lo que la hace distinta es el escritorio: en lugar de traer GNOME o KDE, LordOS tiene un entorno de escritorio propio, construido desde sus componentes fundamentales sobre Debian y Wayland.

Pensalo como una casa. Debian y el kernel Linux son los cimientos, las cañerías y la instalación eléctrica: se reutilizan, están muy probados y los mantiene una comunidad enorme. LordOS construye las habitaciones donde vivís de verdad: la pantalla que ves, las ventanas que movés, las apps que abrís y los guardias en las puertas.

[[shot:0]]

[[diagram:layers]]

## Qué es propio y qué se reutiliza

Acá conviene ser preciso, porque es fácil exagerar con "propio".

**Escrito para LordOS (en este repositorio):**

- **El servidor gráfico y compositor Wayland** (`lord-server`, en C). Configura los modos de pantalla directamente con la interfaz DRM/KMS del kernel, sin libdrm; lee teclados, mouse, touchpads y pantallas táctiles desde evdev, sin libinput; compone cada ventana; y habla el protocolo Wayland con las apps de Linux.
- **El motor de interfaz.** El escritorio y las apps nativas están escritos en JavaScript con React. Corren sobre QuickJS (un motor de JavaScript chico, incluido como código fuente) dentro de `lord-server`, con un árbol de nodos nativo, un motor de layout flexbox, un rasterizador 2D y un motor de texto escritos en C. Detrás de la interfaz no hay ningún motor de navegador web.
- **Las apps nativas:** Archivos, Ajustes, Terminal, Tienda, Mail, Calendario, Contactos, Recordatorios, Contraseñas, Monitor, Fotos, Reproductor, Visor de PDF, Discos, Copias de seguridad y el resto, todas apps React sobre el mismo motor.
- **Los servicios de seguridad:** el lanzador de la caja de arena de las apps, el broker de permisos, el almacén de permisos por app, el servicio de firewall (`lord-netperm`), la firma de paquetes y la bóveda de contraseñas (`lord-vault`).
- **Servicios del sistema** como el gestor de sesiones y la pantalla de ingreso (`lord-sessiond`), el servicio de red (`lord-netd`) y el de ventiladores (`lord-fand`).

**Reutilizado de Debian y del mundo del software libre:**

- el kernel Linux y los paquetes de Debian (`dpkg` sigue desempaquetando y configurando cada paquete);
- Mesa, para la composición opcional en GPU;
- PipeWire, para el sonido;
- el propio protocolo Wayland, a través de `libwayland-server` y `libwayland-client`;
- `libxkbcommon` para las distribuciones de teclado;
- bibliotecas que se usan solo dentro de procesos ayudantes aislados: FFmpeg (multimedia), Poppler y libarchive (documentos), OpenSSL (TLS para el correo y el escritorio remoto), Monocypher (la criptografía de la bóveda).

La regla que sigue el proyecto está escrita en `docs/ARCHITECTURE.md`: dentro de LordOS no se usan X11, wlroots, libdrm, libinput, WebKit, GTK ni Qt. Las apps GTK y Qt igual funcionan, como clientes Wayland.

## Cómo llega un cuadro a la pantalla

Esto es lo que pasa cuando pasás el mouse por el Dock, paso a paso:

1. El kernel entrega un evento de entrada por evdev. `lord-server` lo lee y busca qué nodo de la interfaz está bajo el puntero.
2. Llama al manejador de eventos de JavaScript. React actualiza su estado y le avisa al motor qué nodos cambiaron: un estilo, un hijo, un texto.
3. El motor recalcula el layout y calcula la región dañada: solo la parte de la pantalla que de verdad cambió.
4. Repinta solo esa región, la copia al buffer trasero, dibuja el cursor y cambia de página en el siguiente refresco vertical.

Si hay una GPU con aceleración 3D, la composición pasa por Mesa (GBM, EGL y GLES2). Si no la hay, o si el único renderizador es uno por software, el trabajo lo hace el camino por CPU. El camino por CPU siempre tiene que seguir funcionando: es una regla firme del código (`LORD_GPU=0` lo fuerza), y la entrada "modo seguro" de grub lo usa.

[[diagram:frame]]

## Dónde se enchufan las apps de Linux

En un escritorio LordOS hay tres tipos de ventanas, y se conectan en capas distintas:

- **Las apps incluidas y el tema** (barra de menú, Dock, Archivos, Ajustes…) corren dentro del contexto de JavaScript del propio `lord-server`, sobre el motor de interfaz. Las excepciones son Contraseñas y, desde 0.8.2, Mail, Calendario, Contactos y Recordatorios, que corren como procesos aislados igual que las apps `.lpk`.
- **Las apps `.lpk` instaladas** corren cada una como un proceso aparte (`lord-server --app <id>`, el mismo binario en "modo app"). Ese proceso tiene su propia copia del motor de interfaz, se encierra en una caja de arena y dibuja en un buffer de memoria compartida, que le pasa al compositor como un cliente Wayland común.
- **Las apps de Linux** (GTK, Qt, Chromium, Firefox…) se conectan al socket Wayland del compositor. Se dibujan solas; el tema les pone su marco de ventana alrededor.

O sea que el compositor es el punto de encuentro. El motor de interfaz lo usan la interfaz y las apps propias de LordOS, no las apps de Linux.

[[shot:3]]

## Liviano por diseño

LordOS apunta a máquinas modestas. Algunas mediciones reales de los registros de prueba del proyecto:

- La ISO de la prueba final, en reposo un minuto después de ingresar, en una VM de 4 GB y 4 núcleos: **465 MB de RAM usados** por todo el sistema en la sesión live y **487 MB** en el sistema instalado, con la CPU **99 % ociosa**. El arranque tardó 16,3 s (`docs/ISO.md`, "Final ISO test").
- En una Raspberry Pi 5 (16 GB) en la pantalla de ingreso, sin monitor: `lord-server` con **36 MB de RSS** y 0,6 % de CPU (`docs/RASPBERRY.md`).
- Cuando la máquina no tiene una placa con 3D, `lord-server` ni siquiera carga Mesa y se queda en unos 32 MB de RSS (`docs/GPU.md`).
- Desde 0.8.5 la pantalla de ingreso ya no queda en memoria detrás de tu sesión: se cierra 10 segundos después de que una sesión toma la pantalla y vuelve a arrancar en pocos milisegundos cuando bloqueás o cambiás de usuario. En una VM de VMware todo el sistema pasó de **603 a 548 MB** en uso, y bloquear la pantalla pasó de 903 ms a **141 ms**.
- En placas de video virtuales que completan cada cambio de cuadro al instante (`vmwgfx`, de VMware), 0.8.5 ajusta los cuadros a la frecuencia de la pantalla en vez de dibujar 500–580 por segundo: **59,8 cuadros por segundo** a 60 Hz, percentil 99 de 18,4 ms. El tiempo de cada arranque se muestra en Acerca de LordOS → Tiempo de inicio y se guarda para comparar versiones (`docs/RESULTS.md`).
- **En desarrollo para la 0.8.6:** en una Raspberry Pi 5 con un monitor de 1920×1080, la prueba de escritorio de LordBench mueve una ventana con un cuadrado que gira adentro a **59,8–60 cuadros por segundo** (53–58 antes del trabajo sobre el cuadro de una ventana que se arrastra, que deja cada píxel igual), y a 30 mientras todos los núcleos están ocupados con una carga de baja prioridad (24–25 antes; `docs/BENCHMARK.md`). <!-- PI-RESULT: own-desktop, the Pi 5 with its monitor and the final build: the under-load frame rate with the frame pipeline (and GPU drag if on by default) -->

Estos números salen de máquinas virtuales y de una sola placa, no de una variedad de PCs reales. Son un punto de partida, no una promesa para cualquier máquina.

[[shot:2]]

## Muchas pantallas, texto nítido

El compositor maneja varias pantallas a la vez, extendidas o en espejo, y dibuja cada una con su propia escala: 100, 125, 150, 175 o 200 %. Texto, vectores e imágenes se rasterizan a la resolución real de cada pantalla, así que un panel 4K al 150 % al lado de uno 1080p al 100 % se ve nítido en los dos. Las apps de Linux reciben un `wl_output` por pantalla y el protocolo de escala fraccionaria, así que GTK 4, Qt 6 y Chromium también se ven nítidos. Desde 0.8.5, Ajustes → Pantallas numera cada pantalla, Identificar muestra ese número en grande en el propio monitor, y una pantalla arrastrada se pega borde con borde sin superponerse. El menú Ventana puede mover una ventana a otra pantalla, y el Dock puede asignar una app a una.

[[shot:1]]

## Límites honestos

- **Las GPU reales todavía no están verificadas.** El camino por GPU se probó con el renderizador por software de Mesa en máquinas virtuales. Falta probar en placas AMD y Nvidia reales (`docs/GPU.md`).
- **Las pantallas múltiples se probaron con dos salidas virtuales**, no con monitores reales, paneles de notebook ni hubs DisplayPort (`docs/DISPLAYS.md`).
- **La Raspberry Pi 5 solo se probó con un monitor HDMI.** Desde la 0.8.4.1 la imagen muestra la pantalla de ingreso y el escritorio en un monitor HDMI desde el primer arranque, y LordBench la midió así; dos monitores y 4K todavía no se probaron en la placa (`docs/RASPBERRY.md`).
- **En la Raspberry Pi 5 una app ocupada todavía reduce a la mitad los cuadros por segundo del escritorio:** 30 en vez de 60 en la prueba de escritorio de LordBench mientras todos los núcleos están ocupados. Pintar el cuadro siguiente mientras el anterior espera la pantalla está hecho para la 0.8.6 y todavía no se midió (`docs/GPU.md`). <!-- PI-RESULT: own-desktop limit, remove or reword with the measured number if the frame pipeline brings the Pi 5 under load near 60 fps -->
- **Las apps X11 funcionan con Xwayland desde 0.8.3,** nítidas solo al 100 % de escala y pudiendo verse entre sí, como en cualquier escritorio X11. Arrastrar y soltar entre apps de Linux y los métodos de entrada de LordOS dentro de apps de Linux están pendientes (`docs/WAYLAND.md`).
- **La accesibilidad está mayormente por delante:** hoy hay un ajuste de "reducir movimiento"; un lector de pantalla, el zoom y el alto contraste están en la hoja de ruta.

## Leé el código

- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md): las capas y la política de dependencias
- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c): modos de pantalla con ioctls de DRM, sin libdrm
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c): entrada por evdev
- [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c): composición opcional en GPU con Mesa
- [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c) y [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c): layout y pintado
- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js): el renderizador de React para el motor nativo
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): el compositor Wayland
- [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md), [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md)
