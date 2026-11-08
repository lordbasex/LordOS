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
- bibliotecas que se usan solo dentro de procesos ayudantes aislados: FFmpeg (multimedia), Poppler y libarchive (documentos), OpenSSL (TLS para el correo y el escritorio remoto), el núcleo de FreeRDP (RDP en Remote Desk), Monocypher (la criptografía de la bóveda).

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
- **En la 0.8.6, medido en compilaciones previas:** en una Raspberry Pi 5 con un monitor de 1920×1080, la prueba de escritorio de LordBench mueve una ventana con un cuadrado que gira adentro a **59,8–60 cuadros por segundo** (53–58 antes del trabajo sobre el cuadro de una ventana que se arrastra, que deja cada píxel igual), y a unos 31 (30,5–31,1) mientras todos los núcleos están ocupados con una carga de baja prioridad (24–25 antes; `docs/BENCHMARK.md`). Dibujar la ventana que se arrastra en el chip gráfico y pintar el cuadro siguiente mientras el anterior espera la pantalla también se midieron ahí y estaban apagados por defecto en la 0.8.6: lo primero era exacto pero no dio una ganancia medible, lo segundo dio 33–36 cuadros por segundo con carga pero el puntero llegó a la pantalla 17–29 ms más tarde. En la 0.8.7 lo primero cubre todo el arrastre y está encendido por defecto (ver los límites más abajo).

Estos números salen de máquinas virtuales y de una sola placa, no de una variedad de PCs reales. Son un punto de partida, no una promesa para cualquier máquina.

[[shot:2]]

## Escritorios

En la 0.8.7: varios escritorios en Lord Flow y Lord Classic, con una vista general de las ventanas en vivo, una tira de escritorios y arrastre de una ventana de uno a otro. Cada ventana vuelve a su escritorio después de reiniciar, y una ventana de un escritorio oculto nunca tiene el teclado, ni siquiera si pide pantalla completa desde ahí. Probado en una sesión aislada en una máquina virtual, con apps X11 en una sesión completa en otra, y la vista general en el monitor de una Raspberry Pi 5; falta comprobar un idioma de derecha a izquierda en pantalla y gestos reales de trackpad.

## Archivos, servidores y otros discos

Todo en la 0.8.7.

- **Conectarse a un servidor.** Red lista cada computadora como una carpeta, y sus recursos se despliegan ahí mismo; iniciás sesión con usuario y contraseña o como invitado, lo que pasó contra una carpeta compartida real y un servidor de prueba. Ir → Conectarse al servidor acepta direcciones smb, nfs, sftp, ftp, ftps y WebDAV, y todas se abren como carpetas en Archivos, con las claves SSH y los certificados comprobados antes.
- **Pantallas remotas.** Remote Desk, una sola app para RDP y VNC, viene con el sistema y dibuja con el motor de LordOS: sin ventana de cliente, sin sesión X11, sin toolkit. Los dos protocolos se probaron contra una máquina de prueba descartable; RDP a una sesión de escritorio real y las carpetas compartidas de RDP todavía no están verificados.
- **Discos de otros sistemas.** Disk Formats, un solo ítem de la Tienda, abre NTFS, exFAT y FAT en lectura y escritura, HFS+ con journal en solo lectura y, en amd64 como parte opcional, APFS en solo lectura. Son formatos comerciales; los controladores son software libre y sin costo.
- **Tu forma de trabajar.** Elegí, reordená y ajustá las columnas de la lista, agrandá o achicá los iconos y la lista (el control deslizante de la barra de estado, Ctrl y + o −, o Ctrl y la rueda) y cada carpeta recuerda su propio tamaño, usá la Configuración propia de Archivos y abrí Terminal en la carpeta donde estás.

## Teclas, teclado en pantalla y volver a donde estabas

Todo en la 0.8.7, menos los dos puntos marcados 0.8.8.

- **Atajos por tema.** Un solo despachador corre antes que cualquier oyente de teclas, así que una tecla que toma no le llega a nadie más, y los dos temas tienen un selector de apps con íconos grandes. Sobre una app de Linux el escritorio se guarda solo una lista fija de teclas, elegida por nombre. Ajustes → Teclado lista los atajos del tema en uso.
- **Bloqueo del puntero.** Un bloqueo vale solo con el puntero sobre la ventana y el teclado en su app, y ❖Esc siempre lo libera, así que una ventana de fondo no puede esconder el puntero cuando pasa por encima.
- **Fondos en movimiento.** Bucles cortos para el escritorio y el inicio de sesión, por sección, descargados al elegirlos, con créditos de cada clip. Podés agregar tu propio video de hasta 10 segundos. Necesitan una GPU compatible y se pausan cuando nadie los ve.
- **Tu propia imagen de fondo (0.8.8).** Ajustes → Fondo de pantalla acepta un PNG o JPEG tuyo para el escritorio y la pantalla de inicio de sesión, en todos los temas. Lo convierte un proceso que corre como vos, y la pantalla de inicio de sesión recibe su propia copia en una carpeta del sistema, así que nunca lee tu carpeta personal.
- **Un selector de apps nuevo (0.8.8).** Del 1 al 9 vas directo a una app, las ventanas de la app elegida se ven en vivo en una fila, y Z o la tecla de abajo de Esc las recorre; Ajustes → Teclado elige las teclas, qué apps aparecen, el tamaño y el aspecto.
- **Teclado en pantalla.** Se abre a mano con el botón junto al reloj, para una computadora sin teclado físico, en campos de LordOS, apps de Linux, apps aisladas y en las pantallas de inicio de sesión y de bloqueo. Toma una lista cerrada de teclas, así que no puede ser un atajo, nada guarda lo que se escribe, y su panel sale negro en capturas y grabaciones. No hay registrada ninguna prueba en una pantalla táctil real.
- **Las ventanas se abren donde estaban.** Tamaño, posición, maximizada y pantalla, por app, en los dos temas, para apps de LordOS y de Linux; si una pantalla ya no está o el lugar queda fuera de la pantalla, la ventana se abre en un lugar visible. Sin probar entre dos pantallas.
- **Restaurar la sesión.** Las apps abiertas al cerrar sesión, reiniciar o apagar vuelven con sus ventanas y su escritorio, y después de un corte de luz o un fallo LordOS pregunta primero. Se guardan solo identificadores de apps, nunca rutas de documentos. Probado en una máquina virtual, con reinicios, un apagado real y un apagado forzado.

## En desarrollo para la 0.8.8.6

Todavía no se publicó; el último lanzamiento es la 0.8.8.5.

- **Leer en voz alta.** Voz en toda app con texto, ❖Esc para la selección, un reproductor pequeño con la onda de la voz, Voz en tiempo real y una Voz personal, todo en el equipo y sin internet (mirá *Leer en voz alta* en las funciones).
- **Capturas en todas las pantallas.** ❖⇧3 guarda un archivo por pantalla y ❖⇧4 selecciona en todas a la vez, con lupa y selector de color; T copia el texto del área, que lee un ayudante aislado. Toda captura también queda en el portapapeles.
- **Sacudir el puntero para encontrarlo.** Unos trazos rápidos agrandan el cursor por un momento; viene activado en Ajustes → Mouse y trackpad.
- **Teclado.** Las luces de Bloq Mayús, Bloq Num y Bloq Despl siguen al teclado, y un usuario que nunca eligió una distribución escribe con la del equipo en lugar de una latinoamericana predeterminada.

## Muchas pantallas, texto nítido

El compositor maneja varias pantallas a la vez, extendidas o en espejo, y dibuja cada una con su propia escala: del 100 al 200 % en pasos de 25 %, y en la 0.8.8.6 (en desarrollo) hasta el 300 % para paneles 4K de notebook, mientras el escritorio conserve al menos 1024 × 600 (un panel de 3840 × 2160 al 300 % es un escritorio de 1280 × 720). Texto, vectores e imágenes se rasterizan a la resolución real de cada pantalla, así que un panel 4K al 150 % al lado de uno 1080p al 100 % se ve nítido en los dos. Las apps de Linux reciben un `wl_output` por pantalla y el protocolo de escala fraccionaria, así que GTK 4, Qt 6 y Chromium también se ven nítidos. Desde 0.8.5, Ajustes → Pantallas numera cada pantalla, Identificar muestra ese número en grande en el propio monitor, y una pantalla arrastrada se pega borde con borde sin superponerse. El menú Ventana puede mover una ventana a otra pantalla, y el Dock puede asignar una app a una.

[[shot:1]]

## Límites honestos

- **Las GPU reales de PC todavía no están verificadas.** El camino por GPU corre en la V3D de la Raspberry Pi 5 y en la GPU virtual de VMware (SVGA3D), y se probó con el renderizador por software de Mesa en máquinas virtuales. Falta probar en placas AMD y Nvidia reales (`docs/GPU.md`).
- **GPU drag, encendido por defecto en la 0.8.7, está verificado solo en V3D y SVGA3D.** En una Raspberry Pi 5, en arrastres reales, reduce a la mitad el CPU de la sesión sin carga y lleva una carga de memoria de 29 a 35 cuadros por segundo. En cualquier otra GPU solo lo protege su autoprueba, y `LORD_GPU_DRAG=0` lo apaga (`docs/GPU.md`).
- **Las pantallas múltiples se probaron con dos salidas virtuales**, no con monitores reales, paneles de notebook ni hubs DisplayPort (`docs/DISPLAYS.md`). En el desarrollo de la 0.8.8.6, una PC con una Radeon RX 580 y tres monitores trajo dos arreglos (el tamaño del cursor por hardware y la pantalla de inicio de sesión en la pantalla principal elegida); otras placas, paneles de notebook y hubs no se probaron.
- **La Raspberry Pi 5 solo se probó con un monitor HDMI.** Desde la 0.8.4.1 la imagen muestra la pantalla de ingreso y el escritorio en un monitor HDMI desde el primer arranque, y LordBench la midió así; dos monitores y 4K todavía no se probaron en la placa (`docs/RASPBERRY.md`).
- **En la Raspberry Pi 5 una app ocupada todavía reduce a la mitad los cuadros por segundo del escritorio:** unos 31 en vez de 60 en la prueba de escritorio de LordBench mientras todos los núcleos están ocupados. Con esa carga el chip gráfico tarda 14–16 ms por cuadro en vez de 1,5, sin bajar su frecuencia ni por temperatura; en la 0.8.7, una medición puso la causa en la memoria: 28 ms por cuadro con cuatro núcleos leyendo memoria, 1,4 ms con cuatro núcleos solo girando. Cuánto de eso puede evitar el compositor sigue abierto (`docs/GPU.md`).
- **Los fondos en movimiento copian cada cuadro en su camino a la pantalla.** En la Raspberry Pi 5, en la pantalla de inicio de sesión, a 1080p y 60 cuadros por segundo, `lord-media` usó el 41 % de un núcleo y `lord-server` el 147 %; un camino sin esa copia está planeado para una versión posterior. El decodificador por hardware de la Pi 5 maneja solo HEVC, así que los clips H.264, VP9 y AV1 no se reproducen ahí.
- **Cambiar de tema cierra y vuelve a abrir las ventanas de LordOS,** y el texto sin guardar se pierde, sin aviso.
- **Las apps X11 funcionan con Xwayland desde 0.8.3,** nítidas solo al 100 % de escala y pudiendo verse entre sí, como en cualquier escritorio X11. Arrastrar y soltar entre apps de Linux y los métodos de entrada de LordOS dentro de apps de Linux están pendientes (`docs/WAYLAND.md`).
- **La accesibilidad está mayormente por delante:** hoy hay un ajuste de "reducir movimiento", un teclado en pantalla y, en la 0.8.8.6 (en desarrollo), leer en voz alta y Voz en tiempo real; un lector de pantalla, el zoom y el alto contraste están en la hoja de ruta.

## Leé el código

- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md): las capas y la política de dependencias
- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c): modos de pantalla con ioctls de DRM, sin libdrm
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c): entrada por evdev
- [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c): composición opcional en GPU con Mesa
- [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c) y [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c): layout y pintado
- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js): el renderizador de React para el motor nativo
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): el compositor Wayland
- [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md), [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md)
