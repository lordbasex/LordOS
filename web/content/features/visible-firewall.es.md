## En pocas palabras

La mayoría de los firewalls piensan en puertos y direcciones. El firewall de LordOS piensa en **apps**: vos decidís cuáles de tus apps pueden usar la red, y cuánto. Y como está integrado al Monitor, podés ver qué está haciendo cada app antes de decidir.

[[shot:0]]

Imaginate un edificio con un guardia en la puerta de cada departamento en lugar de uno solo en la calle. Cuando una app intenta abrir una conexión, el guardia de su puerta revisa su lista en una fracción de microsegundo y la deja pasar o le contesta "no permitido". El guardia está dentro del kernel Linux, así que una app insistente no puede simplemente rodearlo.

[[diagram:path]]

## Qué ves y qué decidís

- **Monitor → Red** lista las apps con conexiones, cuánto mandó y recibió cada una, y un mapa del mundo con un arco hacia cada país con el que hablan. Cada app tiene un interruptor de **Regla**: si lo apagás, a la app se le niega la red al instante, con contadores de conexiones permitidas y denegadas que salen directo del kernel. Si abrís una app, cada nombre al que llegó (o la dirección, cuando el resolvedor del sistema no dio nombre) tiene su propio interruptor para permitir o bloquear, que dice qué lo decide en ese momento. El gráfico se aleja del minuto en vivo a 1 hora, 24 horas, 7 días y 30 días, con un pequeño historial guardado en tu carpeta personal: bytes por minuto y por hora, nunca una dirección, un nombre ni un país.
- **Ajustes → Privacidad → Red de las apps** tiene un modo y dos listas:
  - **Silencioso** (el predeterminado): lo que no tiene regla se permite y se registra, para que lo revises en el Monitor. **Apagado**: solo valen tus denegaciones explícitas y las restricciones. **Denegar en silencio** (desde 0.8.5): solo se conecta lo que una regla permite, y todo lo demás se deniega y se registra, mientras los servicios propios de esta computadora siguen funcionando.
  - **Apps confiables**: se permiten sin registrar. Una denegación explícita que pongas igual gana.
  - **Apps restringidas**: marcás lo que una app no puede hacer. Las restricciones de red son **sin internet** (solo LAN), **sin LAN**, **sin red** y **sin conexiones entrantes**. Las demás quitan dispositivos y datos (ver [Más allá de la red](#mas-alla-de-la-red)).

[[shot:1]]

[[shot:2]]

## Cómo funciona

**1. Cada app tiene un nombre que el kernel puede ver.** Cuando abrís una app de Linux desde el lanzador de apps, `lord-server` le pide a root que la ubique en su propio cgroup, `app-lordos-<id>.scope`. Las apps `.lpk` instaladas reciben `app-lordos-lpk-<id>.scope`. Estos scopes los crea y los tiene root, así que una app no se puede mudar a otro.

**2. Un servicio root chico aplica las reglas.** `lord-netperm` carga sus programas eBPF en el cgroup del servicio de LordOS, que contiene todas las sesiones:

| Gancho del kernel | Qué revisa |
|---|---|
| `connect4`, `connect6` | TCP y UDP conectado |
| `sendmsg4`, `sendmsg6` | UDP enviado con una dirección |
| `bind4`, `bind6` | entrantes: "sin conexiones entrantes" y reglas de entrada |
| `sock_create` | "sin red": la app ni siquiera puede crear un socket de internet |
| `cgroup/dev` | los dispositivos de cámara, USB, captura ALSA y framebuffer de una app de Linux, en `open()` |
| LSM `socket_create` | los sockets Bluetooth de una app de Linux |

Cada programa averigua a qué app pertenece el proceso que llama (por el cgroup ancestro al nivel del scope, así que los procesos hijos cuentan como la misma app), decide y devuelve permitir o denegar. Una app denegada recibe `EPERM`, "Operation not permitted".

[[shot:4]]

**3. Tus reglas se convierten en mapas del kernel.** Cambiás las reglas en la sesión; `lord-server` las guarda en `~/.config/lordos/firewall.json` y le manda a `lord-netperm` un único mensaje sin datos: `RELOAD`. Después el servicio lee el archivo por su cuenta, con validación estricta, y compila las reglas en mapas hash y de coincidencia por prefijo más largo. Las reglas nunca viajan por el socket.

**4. Se decide al conectar y se aplica en cada paquete.** La decisión se toma cuando se abre una conexión o se reserva un puerto: unos **0,5 µs** por connect UDP en una VM de prueba, TCP dentro del ruido. Desde 0.8.3 la misma decisión se aplica también a cada paquete de una app cuyas reglas deniegan algo, así que una regla nueva corta las conexiones que ya están abiertas (ver la sección siguiente). Eso cuesta unos 150 ns por paquete, el 1,6 % de un núcleo a 104.000 paquetes por segundo, y unos 30 ns para una app sin reglas de denegación; el rendimiento de TCP no cambia más allá del ruido (`docs/FIREWALL.md`).

## Bloquear una app corta lo que ya está abierto

Desde 0.8.3, apagar la regla de una app, agregar una regla que deniega un destino o una restricción como "sin internet" no solo frena las conexiones nuevas: un video que ya se estaba reproduciendo también se corta. El kernel descarta los paquetes de las conexiones que la regla nueva deniega, y `lord-netperm` cierra esos sockets, así que la app ve que la conexión terminó en lugar de quedarse colgada. Solo se corta lo que la regla nueva deniega: los otros destinos, la red local con "sin internet" y los clientes de un servidor que corre la app siguen funcionando. Volver a permitir funciona al instante, y el Monitor muestra los bytes descartados como "Tráfico bloqueado".

En una Raspberry Pi 5, el cierre necesita el kernel de LordOS compilado con la destrucción de sockets (`6.18.57-lordos.3`); con uno anterior los paquetes se descartan igual, pero la app recién ve que la conexión terminó cuando vence su propio tiempo de espera.

## ¿Funciona por dominio?

El motor sí, con cuidado sobre de dónde salen los nombres. Una conexión solo lleva una dirección IP, así que para aplicar una regla como "permitir solo `example.com`" el firewall tiene que saber a qué nombres corresponde una dirección. Eso lo aprende **solo del resolvedor del sistema**: se suscribe al monitor de systemd-resolved (o, sin él, lee las respuestas del puerto 53 de los servidores de `/etc/resolv.conf`). Nunca confía en respuestas DNS que una app recibió en sus propios sockets, que una app maliciosa podría falsificar.

[[diagram:dns]]

En la VM de prueba, "denegar todo menos example.com" dejó pasar `https://example.com` y denegó `https://www.debian.org`, 20 de 20 veces incluso con la caché del resolvedor vacía. Una app que usa su propio DNS sobre HTTPS obtiene direcciones que el firewall nunca aprendió, así que bajo una regla así quedan denegadas.

**Dónde se escriben.** Ajustes → Privacidad → **Reglas de red por app** muestra el valor predeterminado de cada app ("Permitir todo" o "Bloquear todo salvo las reglas") y sus reglas en el orden en que ganan, porque gana la más específica, con cuándo coincidió cada una por última vez. La hoja para agregar una regla acepta un dominio exacto, `*.dominio`, una dirección o una red, con un puerto y un protocolo opcionales, y revisa lo que escribís mientras lo escribís. En **Monitor → Red**, "Permitir este dominio" o "Bloquear este dominio" sobre un destino de una app abre la misma hoja, completada con el nombre que dio el resolvedor del sistema para esa dirección.

**La trampa, dicha con honestidad: el DNS cifrado.** Un navegador que resuelve nombres por su cuenta con DNS sobre HTTPS nunca le pregunta al resolvedor del sistema, así que sus nombres no se aprenden. "Permitir solo example.com" se sigue cumpliendo (las direcciones que obtiene nunca se aprendieron, así que se bloquean), pero "Bloquear example.com" no lo frena. Las reglas por dirección y por red, el valor predeterminado de la app y sus restricciones siguen valiendo, y Ajustes lo dice debajo de las reglas.

## Más allá de la red

La misma lista de apps restringidas también puede quitar la cámara, el micrófono, la pantalla, el USB, tus archivos, el portapapeles, el Bluetooth y las notificaciones. Una restricción les gana a los permisos que hayas dado antes: la app recibe un rechazo sin aviso, y restringir una app en marcha le quita la cámara y el micrófono al instante. Cada una se aplica donde se entrega el recurso:

[[diagram:restrictions]]

- **Las apps aisladas de LordOS** (apps `.lpk`, Contraseñas y, desde 0.8.2, Mail, Calendario, Contactos y Recordatorios): las ocho, en el broker que ya media todo lo que reciben.
- **Las apps incluidas** (la fila "LordOS"): cámara, micrófono, pantalla y notificaciones. Esa fila es el escritorio mismo, así que archivos, USB, portapapeles y Bluetooth se restringen por app.
- **Las apps de Linux abiertas desde el lanzador de apps:** los dispositivos de cámara, USB, captura ALSA y framebuffer se rechazan en el kernel (`cgroup/dev`); los sockets Bluetooth, con un programa BPF LSM; el micrófono y las cámaras de PipeWire, con la guardia de PipeWire de LordOS, en vivo, identificando a cada cliente por las credenciales de su socket; las notificaciones, con el servidor de notificaciones de LordOS, que le pregunta al bus quién llama y descarta la notificación si no lo puede saber; y "sin acceso a tus archivos" abre la app, desde su próximo inicio, en sus propios espacios de nombres con una carpeta personal vacía y Landlock. La captura de pantalla nunca estuvo a su alcance: no hay protocolo de captura, y las apps X11 (con Xwayland desde 0.8.3) solo se ven entre ellas, nunca a las apps Wayland ni la pantalla de LordOS.

## Nuevo en 0.8.5

Esto llegó con LordOS 0.8.5-beta, publicada el 27 de octubre de 2026.

- **Monitor → Red, segunda generación.** La lista de apps ahora es un árbol: cada app se abre en los sitios y direcciones con los que habla, y cada dirección en sus puertos, con lo enviado y lo recibido, un pequeño medidor en vivo y un interruptor para permitir o bloquear en cada destino. Un solo botón agrupa la vista por app, dominio, dirección, puerto o país y la filtra por dirección, protocolo y regla; la búsqueda convierte `app:`, `domain:`, `ip:`, `port:`, `proto:` y `country:` en etiquetas que podés combinar.
- **Se muestran los sockets de ping, raw y de paquetes** debajo de la app que los abrió. Los sockets IP raw y de paquetes pueden ver o falsificar tráfico, y la fila lo dice, con el protocolo o el ethertype y la interfaz.
- **Las partes propias de LordOS tienen sus propias filas y reglas.** Mail y la sincronización del calendario, la Tienda y las actualizaciones, las impresoras y los escáneres, la Terminal (con los nombres de los programas que corrés en ella) y las actualizaciones de las listas de bloqueo corren cada una en un proceso propio, en su propio scope, así que podés verlas y bloquearlas por separado y lo hace cumplir el kernel. Bloquear la Tienda pregunta antes, porque se frenan las actualizaciones de seguridad. La resolución de nombres, la sincronización de la hora, el servicio de red y tus túneles VPN también se muestran, y nunca se bloquean. En desarrollo para la 0.8.7 (la última versión publicada es la 0.8.6), la búsqueda en la red de Archivos tiene su propia fila. El tráfico de un recurso de red montado no la tiene: el cliente del kernel abre sus propios sockets, fuera de cualquier app.
- **Listas de bloqueo.** Ajustes → Privacidad → Listas de bloqueo ofrece listas públicas de sitios de publicidad, rastreadores, malware y ataques, cada una con su fuente y su licencia. Activás una y LordOS la descarga por HTTPS una vez por día, con un descargador aislado que solo recibe un socket ya conectado, revisa cada línea dos veces y bloquea esos destinos para todas las apps. Una regla que escribiste para un sitio siempre le gana a una lista, y una dirección solo se bloquea cuando todos los nombres que dio el resolvedor del sistema para ella están en la lista, así un servidor compartido no se corta por accidente.
- **Denegar en silencio**, el modo de arriba, también desde un ítem nuevo de la barra de menús.
- **Actividad de red en la barra de menús** (opcional, apagada de forma predeterminada): las velocidades de subida y bajada, y un panel con los últimos cinco minutos, las apps que mueven datos ahora, lo denegado hace poco, el modo del firewall y las listas de bloqueo.

## Las preguntas del lector escéptico

**¿Se aplica también a las apps de Linux?** Sí. Es uno de sus puntos principales: Chromium, Firefox o cualquier app abierta desde el lanzador de apps tiene su propio scope y sus propias reglas. Las apps incluidas de LordOS y los programas iniciados desde la Terminal cuentan juntos como "LordOS"; desde 0.8.5 la Terminal y las partes de red propias de LordOS tienen filas propias.

**¿Puede una app escaparse de su scope?** Se probó. Una app de Linux no puede anotarse en otro cgroup, porque cada archivo `cgroup.procs` del árbol de la sesión es de root. Todavía puede pedirle al gestor systemd del usuario que arranque un proceso nuevo fuera del árbol (`systemd-run --user`). LordOS también cierra eso: `lord-netperm` engancha además sus programas al `user@<uid>.service` de cada usuario, y cualquier proceso ahí queda sujeto a la unión más estricta de las restricciones de las apps restringidas que ese usuario tenga corriendo. Probado con `systemd-run --user --scope`, un servicio transitorio y `setsid`: todos denegados.

[[shot:3]]

**¿Puedo permitir una conexión una sola vez, o que me pregunte?** El motor de reglas conoce tres duraciones (para siempre, hasta que la app se cierre, una sola vez) y un modo "preguntar", pero el aviso de preguntar todavía no está hecho: hoy "preguntar" se comporta como silencioso. Está diseñado como un aviso de reintento: la primera conexión se deniega, te aparece *Permitir una vez / Siempre / Denegar*, y el reintento de la app pasa.

**¿Qué pasa si el servicio se detiene?** Se cierran sus enganches y todo queda permitido. El escritorio no depende de él. En la ISO final el servicio estaba corriendo con sus siete programas de red enganchados.

**¿El servicio en sí es peligroso?** Se lo mantiene chico a propósito: no ejecuta programas, no recibe argumentos, no tiene sockets de internet propios (su unidad solo permite sockets Unix, de paquetes y netlink; con netlink cierra las conexiones que una regla deniega) y acepta un solo tipo de mensaje. Usa su propio cargador sobre la llamada al sistema `bpf()`, con los programas compilados dentro del binario; sin libbpf ni bcc.

## Límites honestos

- **ICMP** (ping) solo se bloquea con "sin red". `listen` sin `bind` se decide como una conexión saliente. (Las conexiones entrantes a un puerto que ya está abierto se deciden por paquete desde 0.8.3.)
- **La unión de `user@` es gruesa:** mientras corre una app restringida, cada proceso del gestor systemd de ese usuario queda sujeto a la unión de restricciones.
- **Un scope recién creado** se registra en microsegundos, pero una conexión hecha antes pasaría con las reglas predeterminadas.
- **El Monitor muestra los nombres que dio el resolvedor del sistema** para cada destino, o la dirección cuando no dio ninguno. Nunca hace DNS inverso, así que ninguna dirección sale de la máquina solo porque la miraste.
- **Las reglas por dominio no ven a las apps que usan su propio DNS cifrado** (ver arriba).
- **Lo que las restricciones no cubren para las apps de Linux:** el Bluetooth a través de BlueZ en el bus del sistema (solo se rechazan los sockets Bluetooth directos), el bus del sistema en general, el portapapeles y los programas iniciados fuera del lanzador de apps. Un cliente nuevo de PipeWire de una app restringida tiene unos milisegundos antes de que la guardia lo restrinja, y mientras corre una app con la cámara o el Bluetooth restringidos, tus propios servicios de audio tampoco pueden abrir una cámara ni un socket Bluetooth nuevos.
- **Las apps de Linux sin caja de arena pueden editar el archivo de reglas**, porque corren como vos, salvo que estén restringidas con "sin acceso a tus archivos". Las apps `.lpk` instaladas no llegan a él.

## Leé el código

- [server/src/netperm/np.bpf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/np.bpf.c): los programas eBPF de cgroup y LSM
- [server/src/netperm/netperm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/netperm.c): el servicio root
- [server/src/netperm/npcompile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/npcompile.c): las reglas convertidas en mapas del kernel
- [server/src/netperm/npdns.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/npdns.c): los nombres desde el resolvedor del sistema
- [server/src/fw/rules.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fw/rules.c): reglas, precedencia y el lector estricto del archivo
- [server/src/ui/js_fw.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_fw.c): `lord.fw`, la API de la sesión
- [apps/monitor/Firewall.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/monitor/Firewall.jsx) y [apps/monitor/FwRules.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/monitor/FwRules.jsx): las secciones de Ajustes
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c) y [server/src/sec/pwguard.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/pwguard.c): las restricciones en el broker y la guardia de PipeWire
- [server/tests/netperm_integration.sh](https://github.com/lordbasex/LordOS/blob/main/server/tests/netperm_integration.sh): la prueba con el kernel, 18 de 18
- [docs/FIREWALL.md](https://github.com/lordbasex/LordOS/blob/main/docs/FIREWALL.md) y [docs/MONITOR.md](https://github.com/lordbasex/LordOS/blob/main/docs/MONITOR.md)
