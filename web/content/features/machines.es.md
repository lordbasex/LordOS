## En pocas palabras

**Máquinas** (Machines) corre otros sistemas en ventanas de tu escritorio de LordOS, a velocidad nativa con KVM donde el hardware lo permite. Salió en la **0.8.8.5** (alfa): tarjetas de descarga para Debian, Ubuntu, Fedora y Kali que comprueban las firmas de sus fabricantes y pueden instalar sin preguntas, instantáneas, clones, migración desde otras apps de virtualización y, desde la 0.8.8.7, carpetas compartidas.

**En desarrollo después de la 0.8.9.4.** Todo lo que sigue está en el repositorio y todavía no en un lanzamiento; el último lanzamiento es la **0.8.9.4**. Trae máquinas con Windows 11, las herramientas de LordOS para Windows, un portapapeles que solo se mueve cuando pegás, transferencias de archivos rápidas en los dos sentidos, el modo apps, sonido y micrófono, redes de verdad, 3D para máquinas Linux y la imagen a la velocidad completa del invitado.

[[shot:0]]

## Windows 11, desde los servidores de Microsoft

Una tarjeta nueva del asistente Nueva máquina arma una máquina que Windows 11 acepta tal cual, en PC de 64 bits:

- **Dos caminos.** *Tu Windows 11*: Máquinas abre la página de descarga de Microsoft, encuentra la ISO en Descargas cuando está completa y la comprueba contra el SHA-256 que Microsoft publica para cada uno de sus 38 idiomas. *Probar 90 días*: la Enterprise Evaluation de Microsoft, en inglés, español o portugués, descargada del servidor de Microsoft y guardada solo si coincide su hash publicado.
- **LordOS no guarda nada de Microsoft.** LordOS no aloja, no incluye ni activa Windows, y la licencia es asunto tuyo.
- **Lo que pide Windows 11:** UEFI con arranque seguro, un TPM 2.0 propio para cada máquina, al menos 4 GB, 2 procesadores y 64 GB, y disco y red virtio rápidos cuyos drivers el instalador carga solo, sin el paso «Cargar controlador».
- **Instalar sin preguntas.** Tu cuenta local y una contraseña opcional, el nombre del equipo, la edición con tu clave (o la clave que publica Microsoft para instalarla sin activar), sin pedir una cuenta en línea, las opciones de privacidad apagadas, y el idioma, el teclado y la zona horaria de este equipo. Al terminar, Máquinas borra el disco chico que tenía la contraseña y saca los dos discos. O usá el instalador propio de Windows, guiado, con los drivers ya puestos.
- **El TPM va con la máquina**: las instantáneas lo guardan, volver a una lo repone y los clones se lo llevan.

## Las herramientas de LordOS para Windows

Un programa chico que construye LordOS, que se instala solo después de una instalación desatendida (o desde el botón del agente) y corre como vos, nunca como servicio:

- **La resolución sigue a la ventana**, exacta, entre un cuarto y medio segundo después de cada cambio de tamaño.
- **Copiar y pegar** texto, texto con formato (limpio de scripts en este equipo) e imágenes.
- **El modo apps**, **los archivos soltados** y **Abrir con** (más abajo).
- **El puntero lo dibuja LordOS**, así moverlo ya no espera a la máquina. Un interruptor por máquina se lo devuelve al invitado.
- **Un clon recibe su propio nombre de equipo**, que usa desde su próximo reinicio.

## El portapapeles, solo cuando pegás

Una máquina se trata como no confiable: un programa adentro podría leer tu portapapeles o cambiar lo que copiaste.

- **A pedido.** Una máquina solo se entera de *que* hay algo para pegar (texto, texto con formato o una imagen), nunca del contenido, hasta que un programa en ella pega de verdad, y solo mientras la estás usando: su ventana al frente, o una de sus ventanas del modo apps, o dentro de los 3 segundos.
- **Protección en segundo plano.** Una máquina que intenta leer o cambiar tu portapapeles mientras estás en otra cosa queda bloqueada, el intento queda en el registro y recibís un solo aviso por tanda.
- **Preguntar antes de cada pegado.** Elegilo en Configurar → Compartir y una barrita muestra lo que va a cruzar, con *Pegar* y *No pegar*, o *Traer a LordOS* e *Ignorar* para una copia que sale. Pega solo lo que te mostró, y no se puede atravesar con una barra levantada debajo de tu puntero.
- **Los secretos se quedan.** Una contraseña copiada desde un gestor de contraseñas dentro de una máquina no llega a tu historial del portapapeles.

## Archivos para adentro y para afuera

- **Arrastrá archivos a la ventana de una máquina** desde Archivos: llegan a la carpeta Descargas de la persona que inició sesión en el invitado, con una línea de progreso, y reciben su nombre solo cuando coinciden su tamaño y su SHA-256.
- **Una carpeta de transferencia para cada máquina**, `~/Machines/Transfer/<nombre>`, rápida en los dos sentidos. Una máquina Linux la tiene en `/mnt/LordOS` y como carpeta `LordOS` en cada home; una máquina con Windows la recibe como unidad, preparada sola después de una instalación desatendida.
- **Abrir con una máquina.** En Archivos, clic derecho en un documento o un programa → Abrir con → la máquina: arranca o se reanuda si hace falta, el archivo se copia adentro y se abre ahí con su app de siempre, y un programa corre en su propia ventana de LordOS. Podés elegir abrir siempre un tipo de archivo con una máquina.
- **Soltar en una app.** Los archivos soltados en una ventana del modo apps van a esa ventana: a la carpeta que muestra, a una app que acepta archivos o a su portapapeles.

| Medido en la PC de prueba (archivo de 2 GB) | Por el agente del invitado | Por la carpeta de transferencia |
|---|---|---|
| A Descargas de un invitado Ubuntu | unos 75 s | **17,6 s**, con el hash comprobado |
| A Descargas de un invitado Windows 11 | 78–80 s | **25,6 s**, con el hash comprobado |
| Fuera de la máquina, guardado en su carpeta o unidad LordOS | no se podía | **unos 2,5 s** |

## Modo apps

Cada ventana de la máquina pasa a ser su propia ventana de LordOS, con el título del invitado, el nombre de la máquina y el ícono de su app en el Dock, la barra de tareas y el selector, mientras el escritorio y la barra de tareas del invitado quedan ocultos. Las apps de la Tienda de un invitado Windows tienen cada una su propia entrada y su ícono. Mover, cambiar de tamaño, minimizar o cerrar una ventana hace lo mismo en el invitado. La ventana de la máquina se vuelve un control chico con los programas del invitado, y LordOS solo puede arrancar un programa que el propio invitado listó.

## Sonido, micrófono y redes

- **El sonido** suena en los parlantes de este equipo, con el silencio y el volumen del propio invitado. **El micrófono** se abre solo mientras el ajuste Micrófono de la máquina está encendido, el permiso de micrófono de LordOS deja a Máquinas, el invitado graba y la máquina corre; una luz lo muestra.
- **Cada máquina con su salida y su entrada**, elegidas en Configurar o desde el botón del parlante mientras corre. Un dispositivo desconectado pasa al predeterminado del sistema y vuelve cuando lo enchufás de nuevo.
- **Cuatro modos de red.** *Compartida*, ahora con hasta 32 **puertos redirigidos**, abiertos solo para este equipo por defecto. *En puente*: la máquina se suma a tu red cableada como un dispositivo más y recibe su dirección del DHCP de tu router. *Solo anfitrión*: llega a este equipo y a nada más, sin contraseña de administrador. *Aislada*. Configurar dice qué significa cada uno para el firewall: el tráfico de una máquina en puente no pasa por el firewall de LordOS.

## 3D para máquinas Linux

Con **Gráficos 3D** en Configurar → Pantalla, una máquina Linux dibuja con tu placa de video a través de virgl (OpenGL), en PC de 64 bits. Los cuadros de la máquina llegan a LordOS sin que espere nunca el trabajo gráfico del invitado. Solo esa máquina puede usar el dispositivo de render de la placa, nunca la pantalla en sí.

**Las máquinas con Windows siguen en 2D**: no existe un driver 3D libre y estable para ellas. El 3D para máquinas con Windows está en desarrollo, y todavía no se ofrece nada. Las máquinas también son 2D en la Raspberry Pi y en un escritorio que dibuja sin placa de video, y una máquina 3D en marcha no se puede suspender ni sacar instantáneas con su memoria.

## Una imagen más fluida

LordOS ahora lee los cuadros de cada máquina en su propio bucle y repinta solo la parte que cambió. Medido con un invitado Windows 11 que dibuja 61 cuadros por segundo:

| | Antes | Después |
|---|---|---|
| Cuadros por segundo mostrados | 29,5 | **60,0** |
| Pintado por cuadro | 1,7 ms | **0,1 ms** |
| El servidor gráfico de LordOS, de un núcleo | 54 % | **16–18 %** |
| Mover el puntero | un viaje de ida y vuelta por el invitado, 35–39 ms | **lo dibuja LordOS en el cuadro siguiente** |

El resto del escritorio recibió el mismo cuidado: Ajustes, la Tienda, Archivos y Monitor ya no congelan el puntero mientras trabajan (cambiar un ajuste con tres apps abiertas pasó de 161 a 20 ms, una carpeta de 2.220 archivos abre en 229 ms en vez de casi 2 s), y los interruptores que arrancan algo lento cambian al instante con un indicador de espera.

## También nuevo

- **Orden de arranque**: disco del sistema, CD/DVD y red en el orden que elijas, y un menú de arranque al inicio si lo querés.
- **Resolución de inicio**: *Sigue a la ventana* o un tamaño fijo, que todo sistema toma al arrancar.
- **Un aspecto nuevo**: hojas sobre la ventana, ajustes agrupados y 38 glifos propios de LordOS que siguen tu color de acento, y una vista Almacenamiento de dónde se va tu espacio.
- **La Tienda, ordenada por el origen de las apps**: Debian, Flathub, Apps web y apps de LordOS, y las apps que eligió LordOS.

## Qué se probó y qué no

Comprobado en una PC de 64 bits con KVM, sobre todo con una herramienta de desarrollo que hace el papel del servidor gráfico de LordOS: la instalación desatendida de Windows 11 y sus herramientas (resolución, portapapeles en los dos sentidos, la lista de ventanas, los íconos y las acciones del modo apps), los números de la carpeta de transferencia de arriba, los tres modos de red nuevos contra una LAN real, el sonido y el micrófono por PipeWire, y el 3D con un invitado Ubuntu en una Radeon RX 580.

**Todavía no:** el modo apps dibujando en un monitor real, todos los paneles de Configurar en pantalla, Windows en la Raspberry Pi, el 3D para máquinas con Windows, dispositivos USB pasados a una máquina y arrastrar archivos fuera de una máquina.

El diseño completo, las reglas de seguridad y cada medición están en `docs/VIRTUALIZATION.md` y `docs/MACHINES-GPU.md`.
