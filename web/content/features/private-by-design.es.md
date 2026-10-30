## En pocas palabras

Una app instalable de LordOS (un paquete `.lpk`) no recibe las llaves de tu casa. Corre en una habitación propia, con la puerta cerrada desde afuera. Cuando necesita algo, se lo pide al sistema por una sola ventanilla, el **broker**. El sistema revisa lo que decidiste, y si todavía no decidiste, te pregunta con un aviso que dibuja el sistema, no la app.

[[shot:0]]

Tres ideas hacen casi todo el trabajo:

- **Aislamiento.** Cada app `.lpk` es su propio proceso, encerrado con funciones del kernel Linux antes de que corra una sola línea de su código.
- **Mediación.** Todo lo que toca tus cosas (archivos, cámara, micrófono, portapapeles, contraseñas) pasa por el broker en `lord-server`, que revisa el permiso en cada pedido.
- **Tu decisión, guardada donde la app no llega.** Las decisiones viven en `~/.config/lordos/permisos.json`, que las apps aisladas no pueden ver ni escribir.

[[diagram:permission]]

## ¿Qué apps están aisladas, exactamente?

Es la primera pregunta que tiene que hacerse un lector cuidadoso, así que acá va la respuesta precisa:

| Tipo de app | Proceso propio | Caja de arena (Landlock + seccomp) | cgroup por app | Broker y permisos | Firewall por app |
|---|---|---|---|---|---|
| Apps `.lpk` instaladas | sí | sí | sí, con límites de memoria y CPU | sí | sí |
| Contraseñas, Mail, Calendario, Contactos y Recordatorios (apps incluidas con proceso propio; las últimas cuatro desde 0.8.2) | sí | sí | sí | sí | sí |
| Otras apps incluidas (Archivos, Ajustes, Terminal…) | no, corren dentro de `lord-server` | no | no (cuentan como "LordOS") | su uso de cámara, micrófono y pantalla se revisa en C | sí, como "LordOS" |
| Apps de Linux abiertas desde el lanzador de apps (apt, la Tienda) | sí, un proceso común | **no** de forma predeterminada; con "sin acceso a tus archivos", espacios de nombres propios y Landlock | sí, `app-lordos-<id>.scope`, sin límites | **no** | **sí**, más las restricciones que fijes (cámara, micrófono, USB, Bluetooth, archivos, notificaciones) |
| Programas iniciados desde la Terminal | sí | no | comparten el lugar de la Terminal en el árbol | no | como "Terminal", con sus propias reglas (desde 0.8.5; antes, como "LordOS") |

O sea: **LordOS no encierra a las apps de Linux en una caja de arena de forma predeterminada.** Corren como tu usuario, exactamente como en cualquier sistema Debian, y pueden leer tu carpeta personal. Lo que LordOS les suma es atribución (el Monitor sabe qué app hizo cada conexión), el firewall por app que aplica el kernel y las restricciones que elijas para una app: sin cámara, sin micrófono, sin USB, sin sockets Bluetooth, sin notificaciones y "sin acceso a tus archivos", que desde su próximo inicio abre la app en sus propios espacios de nombres, con una carpeta personal vacía que solo tiene sus propias carpetas. LordOS no usa ni integra Flatpak, y no se probó.

Las apps incluidas son parte del código propio del sistema y se revisan junto con él; la mayoría no tiene el aislamiento por proceso que tienen los paquetes de terceros. Las excepciones son Contraseñas, porque guarda tus secretos, y, desde 0.8.2, Mail, Calendario, Contactos y Recordatorios, porque leen lo que llega de internet: cada una corre como su propio proceso aislado y llega al correo y a tu llavero solo con pedidos al broker permitidos para esa app y ninguna otra.

## Cómo funciona la caja de arena

La caja de arena es código propio de LordOS (`server/src/sec/sandbox.c`), no bubblewrap, Flatpak ni firejail. Usa directamente funciones del kernel, y se aplica **después** de que la app se conecta al compositor y al broker, y **antes** de cargar cualquier línea de código de la app.

[[diagram:sandbox]]

- **Landlock** limita los archivos que el proceso puede tocar: lectura sola de su propio paquete, del entorno compartido de las apps, de las fuentes y las bibliotecas; lectura y escritura solo en `~/.local/share/lordos/appdata/<id>`. Nada más de tu carpeta personal, y nada de `/dev`.
- **seccomp-bpf** es una lista de llamadas al sistema permitidas, aplicada a cada hilo. Llamadas como `ptrace`, `mount`, `bpf`, `execve`, `keyctl` y la carga de módulos directamente no están. La app tampoco puede abrir sockets Unix nuevos, así que no puede llegar por su cuenta al bus de la sesión, al servicio de impresión ni al socket Wayland.
- **Sin privilegios.** Se activa `no_new_privs` y se quitan todas las capacidades.
- **Un cgroup propio**, `app-lordos-lpk-<id>.scope`, con un límite blando de memoria de 192 MB, uno duro de 512 MB y como máximo un núcleo de CPU. El scope lo crea y lo tiene root (`lord-sessiond`), así que la app no se puede mudar a otro lado.
- **Red solo con permiso.** Sin el permiso `network`, seccomp se niega a crear sockets de internet y Landlock niega el connect y el bind de TCP. Con el permiso (declarado en el manifiesto **y** permitido por vos), se habilitan, y el firewall por app sigue aplicando.

**¿Se usan namespaces?** No para las apps `.lpk`. `lord-server` corre sin root, y crear un namespace de red necesita privilegios que no tiene. El diseño usa Landlock, seccomp y cgroups en su lugar, y deja la app en el namespace de red de la máquina bajo las reglas de arriba. El único lugar donde LordOS usa namespaces es una app de Linux restringida con "sin acceso a tus archivos": arranca en sus propios namespaces de usuario, montaje y PID, sin privilegios, armados por código propio de LordOS en el lanzador, así que no llega a los sockets de la sesión.

El proyecto mantiene una app de prueba maliciosa a propósito (`tests/lpk-evil`). No declara permisos e intenta de todo al arrancar: leer `~/.ssh/id_rsa` y `settings.json`, escribir fuera de su carpeta, leer el portapapeles sin avisar, abrir `/etc/shadow`, abrir un socket, ejecutar `/bin/sh`, `ptrace`, abrir la placa de video. Todos los intentos quedan bloqueados. La batería de pruebas de la caja de arena pasa 14 de 14 controles para apps, 14 de 14 con red y 17 de 17 para ayudantes.

[[shot:1]]

## Qué ves y qué decidís

- **El aviso.** La primera vez que una app pide algo, el sistema muestra: *«App» quiere usar el micrófono — Permitir / No permitir*. La respuesta queda guardada para esa app.
- **Ajustes → Privacidad** lista cada permiso con las apps que lo pidieron y un control Permitir / Preguntar / Denegar.
- **Indicadores.** Un punto verde en la barra de menú mientras se usa la cámara, uno naranja para el micrófono. Si lo tocás, te muestra qué app es.

Permisos que una app puede declarar: archivos, portapapeles, notificaciones, red, cámara, micrófono, pantalla, escuchar conexiones, USB, Bluetooth y apps. Una app no puede obtener un permiso que no declaró.

[[shot:4]]

## Las preguntas del lector escéptico

**¿Se puede saltear el aviso?** La app aislada no. El aviso lo dibuja la sesión, fuera de la ventana de la app. La respuesta llega directo a `lord-server`, y el broker lee en C la decisión guardada en cada pedido. El archivo de decisiones está fuera de lo que Landlock le deja ver a la app, y `lord.fs` se niega a escribirlo. Una app podría dibujar un diálogo falso dentro de su propia ventana, pero tocarlo no cambia nada. El hueco honesto: una app de Linux, que de forma predeterminada no está en caja de arena y corre como vos, podría editar `permisos.json` como cualquier otro archivo de tu carpeta personal, salvo que esté restringida con "sin acceso a tus archivos".

**¿Tengo "Permitir una vez" o "Permitir siempre"?** Hoy el aviso ofrece Permitir y No permitir, y la decisión se recuerda hasta que la cambies en Ajustes. Todavía no hay "permitir una vez" para los permisos de las apps.

**¿La app recibe solo el archivo que elijo?** Sí. Para abrir o guardar, una app aislada llama al diálogo de archivos del sistema. `lord-server` lo dibuja (el "powerbox"), abre con tus permisos el archivo que elegiste y le pasa a la app **solo ese descriptor de archivo**. La app nunca ve la carpeta ni el resto del disco. No hace falta un permiso extra, porque elegir el archivo ya es el consentimiento.

[[shot:2]]

**¿Cómo se median la cámara y el micrófono?** La app nunca abre el dispositivo.
- *Micrófono:* `lord-server` abre el flujo de captura de PipeWire y escribe el audio en un anillo compartido; la app recibe una copia **de solo lectura**.
- *Cámara:* `lord-server` abre `/dev/videoN` y se lo pasa solo al ayudante multimedia aislado, `lord-media`, que decodifica los cuadros a un anillo compartido sellado. La app recibe un mapeo de solo lectura y tiene que confirmar cada cuadro; `lord-server` valida cada confirmación (generación, posición, ritmo). Las fotos y los videos van a un archivo que elegís en el diálogo de guardar del sistema.

[[diagram:camera]]

**¿Se pueden revocar permisos mientras la app corre?** Sí, para la cámara y el micrófono: pasar cualquiera de los dos a Preguntar o Denegar en Ajustes lo corta al instante. Para la cámara, `lord-server` mata el proceso `lord-media` de la app (probado: desaparece en menos de 0,5 s); para el micrófono, cierra el flujo de PipeWire. Controles como el del portapapeles se aplican en el próximo pedido. El permiso `network` se decide cuando arranca la app, así que un cambio vale desde el próximo inicio; para cortarle la red al instante a una app que está corriendo, usá el interruptor del firewall en el Monitor.

[[shot:3]]

**¿Puedo asegurarme de que una app nunca reciba la cámara, pida lo que pida?** Sí: restringila en Ajustes → Privacidad → Red de las apps. Una restricción del firewall les gana a los permisos: la app recibe un rechazo sin aviso aunque antes la hayas permitido, y restringir una app en marcha le quita la cámara y el micrófono al instante. Para las apps aisladas, el broker aplica las ocho restricciones (cámara, micrófono, pantalla, USB, archivos, portapapeles, Bluetooth, notificaciones).

**¿Y las apps que necesitan más privilegios?** No hay forma de que una app `.lpk` obtenga root ni capacidades extra. Todo lo que en LordOS necesita root pasa por una lista cerrada de acciones validadas (`server/src/admin.c`), y JavaScript no puede ejecutar comandos arbitrarios. Las apps de Linux se comportan como en cualquier sistema Debian: si un programa necesita `sudo`, lo corrés vos en la Terminal.

## LordOS verifica su propio código antes de ejecutarlo

Desde 0.8.2, cada paquete de código que LordOS ejecuta como propio (los temas con el shell y cada app nativa, la pantalla de inicio de sesión, el anfitrión de las apps aisladas y las apps del sistema que corren como proceso propio) figura con su SHA-256 en un manifiesto firmado con la clave de versiones de LordOS (Ed25519). `lord-server` verifica la firma con la clave pública compilada adentro, y el hash de cada paquete con la misma lectura con la que lo carga, así que no hay un momento en el que se pueda cambiar el archivo. Un paquete que no coincide no se ejecuta: el escritorio pasa a otro tema y lo avisa, o muestra una pantalla de recuperación que explica cómo repararlo, nunca una pantalla en blanco. Acerca de LordOS muestra "Verificado", o "Compilación de desarrollo" para un árbol de desarrollo sin firmar. Esto protege de los cambios hechos por procesos que no son root y de la modificación sin conexión; root todavía puede reemplazar el propio `lord-server`, algo que una fase posterior (fs-verity, imágenes A/B) debe cubrir.

## Comprobalo en tu propia computadora (desde 0.8.5)

Desde 0.8.5, **Información del sistema** (Acerca de LordOS → Informe del sistema…) tiene una pestaña Verificaciones que pone a prueba estas afirmaciones en la computadora que tenés adelante: el servidor gráfico sin root, Landlock, el firewall por app, el código verificado, el escritorio remoto cerrado cuando está apagado, los scopes de las apps en manos del sistema. Cada verificación dice si el mecanismo solo se encontró *configurado* o si de verdad se *probó*. **Probar** inicia la Prueba del aislamiento, una app de prueba oculta que arranca exactamente como cualquier app aislada e intenta, dentro de la misma caja de arena, lo que no debe poder hacer: tus archivos y los del sistema, sockets, `ptrace`, un espacio de nombres de usuario, `mount`, `exec`, salir de su scope. También intenta lo que la caja de arena de los decodificadores debe rechazar, y le pide al broker cosas que debe negar. La prueba nunca recibe un permiso ni acceso a la red, y no se envía nada a ningún lado.

[[shot:6]]

## Los ayudantes que leen datos no confiables

Los archivos que vienen de afuera son la puerta de entrada clásica. LordOS los abre en procesos chicos y separados que están todavía más encerrados que las apps: `lord-doc` (PDF, archivos comprimidos, fuentes), `lord-media` (video, música, fotos, cámara), `lord-maild` (correo, calendarios, contactos) y `lord-vault` (contraseñas). No tienen red, no pueden crear sockets, no pueden ejecutar programas y solo pueden crear hilos, no procesos. No abren rutas: `lord-server` abre los archivos y se los pasa como descriptores. Si seccomp no se puede aplicar, el ayudante termina en lugar de correr sin encierro. Un PDF malicioso puede tumbar a `lord-doc`, pero no al escritorio. Desde la 0.8.5 `lord-fetch` (las listas de bloqueo del firewall) y `lord-tls` (el HTTPS de la Tienda) están encerrados de la misma forma y tampoco pueden crear sockets: solo reciben una conexión que `lord-server` o la Tienda abrió por ellos. El `lord-bench` de LordBench (desde la 0.8.6) corre sus pruebas bajo el mismo encierro.

## Ventanas protegidas

Una ventana se puede marcar como protegida. La de Contraseñas lo está. Sus píxeles salen en negro en las capturas de LordOS, las grabaciones de pantalla, las miniaturas y el escritorio remoto, mientras el monitor real la muestra normal. Las apps de Linux no pueden capturar la pantalla a través de LordOS: el compositor no les ofrece ningún protocolo de captura, las apps X11 (con Xwayland desde 0.8.3) solo se ven entre ellas, nunca a las apps Wayland, la pantalla de LordOS ni Contraseñas, y a una app restringida también se le niega el dispositivo de framebuffer.

[[shot:5]]

## Límites honestos

- **Las apps de Linux no están en caja de arena de forma predeterminada.** Se les aplican las reglas de red, la atribución y las restricciones que fijes; las restricciones no cubren el portapapeles, el Bluetooth a través de BlueZ en el bus del sistema, el bus del sistema en general ni los programas iniciados fuera del lanzador de apps.
- **El uso de cámara y micrófono por apps de Linux todavía no se muestra** en los indicadores. La lista de dispositivos del Monitor está planeada.
- **El broker todavía no cubre todo.** Permisos como Bluetooth o abrir otras apps se pueden declarar, pero no hay una llamada del broker para ellos, así que una app aislada directamente no puede hacer esas cosas.
- **Si falta una función del kernel, la caja de arena de las apps se debilita sin hacer ruido.** Si faltan Landlock o un controlador de cgroup, las apps igual arrancan, con una advertencia en el registro. Los ayudantes, en cambio, se niegan a correr sin seccomp.

## Leé el código

- [server/src/sec/sandbox.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sandbox.c): Landlock, seccomp, capacidades, reglas de red
- [server/src/sec/brokersrv.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokersrv.c): el broker, del lado del servidor
- [server/src/sec/perms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/perms.c): permisos por app
- [server/src/sec/brokercam.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokercam.c): la cámara para apps aisladas
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c): las restricciones del firewall en el broker
- [server/src/sec/cgroup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/cgroup.c) y [server/src/remote/cgsup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/cgsup.c): scopes de las apps en manos de root
- [server/src/app/appclient.c](https://github.com/lordbasex/LordOS/blob/main/server/src/app/appclient.c): el proceso de la app aislada
- [sdk/privacy.js](https://github.com/lordbasex/LordOS/blob/main/sdk/privacy.js) y [sdk/powerbox.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/powerbox.jsx): los avisos y el diálogo de archivos
- [server/tests/sandbox_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/sandbox_test.c) y [tests/lpk-evil](https://github.com/lordbasex/LordOS/tree/main/tests/lpk-evil): las pruebas
- [docs/PRIVACY.md](https://github.com/lordbasex/LordOS/blob/main/docs/PRIVACY.md)
