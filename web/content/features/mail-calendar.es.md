## En pocas palabras

LordOS trae cuatro apps nativas para tu información personal: **Mail**, **Calendario**, **Contactos** y **Recordatorios**. Son apps comunes de LordOS, dibujadas por el mismo motor que el resto del escritorio. Lo inusual es dónde se hace el trabajo riesgoso: cada protocolo de red y cada lector de datos que vienen de afuera corre en un único proceso ayudante encerrado, **`lord-maild`**, que ni siquiera tiene permitido abrir una conexión de red por su cuenta.

[[shot:0]]

El correo es la forma más común de atacar una computadora. Un mensaje es un documento de un desconocido: HTML, imágenes, adjuntos, invitaciones. Por eso LordOS lo trata como un paquete que se abre en una habitación sellada. Si algo adentro es malicioso y toma la habitación, igual no puede llamar afuera, no puede ejecutar nada y no puede llegar a tus archivos.

[[diagram:mail]]

## Qué tenés

- **Mail:** tres paneles, bandeja de entrada unificada, conversaciones, marcas y seis categorías de color que también ven otros clientes, firmas, borradores guardados en el servidor, "enviar más tarde", adjuntos que se abren con tus apps predeterminadas y notificaciones de correo nuevo. Escribís en texto enriquecido, markdown o texto plano.
- **Calendario:** vistas de día, semana y mes, varios calendarios con colores, locales o sincronizados por CalDAV, eventos que se repiten y alarmas.
- **Invitaciones** que llegan por correo: Aceptar, Quizás o Rechazar. El evento va al Calendario y la respuesta le llega al organizador.
- **Recordatorios:** listas, fechas de vencimiento, prioridades, banderas y las vistas Hoy / Programados / Todos / Con bandera / Completados, sincronizados por CalDAV.
- **Contactos:** locales o sincronizados por CardDAV, con grupos y fotos. Mail autocompleta a partir de ellos.
- **Las alarmas suenan con las apps cerradas,** porque el ayudante sigue corriendo durante toda la sesión.
- **Cualquier cuenta IMAP y SMTP** funciona. Para algunos proveedores muy usados, la configuración de servidores se completa desde una tabla incluida, sin consultar nada en línea.

[[shot:5]]

[[shot:6]]

## Cómo funciona

1. **El ayudante arranca encerrado.** `lord-server` lanza `lord-maild` desde una ruta fija, sin argumentos y con un entorno mínimo. Antes de leer nada de la red, carga las autoridades certificantes del sistema y se encierra con la misma caja de arena para ayudantes que los otros decodificadores de LordOS: escritura solo en su carpeta de caché, sin `socket`, `connect`, `bind`, `execve` ni `fork`, solo hilos, sin capacidades, con su propio cgroup.
2. **Pide cada conexión.** Para llegar a un servidor le manda a `lord-server` un pedido: host y puerto. `lord-server` compara ese destino con tu archivo de cuentas, y recién entonces se conecta y le devuelve el socket conectado. Los destinos permitidos son exactamente los servidores IMAP, SMTP y de calendario/contactos de tus cuentas, más otros hosts del mismo dominio en el puerto 443 cuando un servidor de calendario redirige durante el descubrimiento.
3. **TLS se verifica como corresponde.** Cada conexión se verifica contra el almacén de certificados del sistema y el nombre del servidor. Un certificado que no verifica nunca se acepta en silencio: Mail muestra el problema y su huella, y solo si tocás *Confiar en este certificado* se recuerda exactamente ese certificado para esa cuenta.
4. **El HTML se traduce, no se ejecuta.** LordOS no tiene motor web, y Mail no lo necesita. El sanitizador propio de LordOS convierte el HTML de un mensaje en un árbol chico de bloques, tablas, tramos de texto, imágenes y enlaces, y Mail lo dibuja con vistas nativas. Se descartan scripts, estilos, frames, formularios, SVG, multimedia, manejadores de eventos y los trucos de "preencabezado" oculto.

## Qué ves y qué decidís

- **Las imágenes remotas están bloqueadas** hasta que tocás *Cargar imágenes* para ese mensaje o *Siempre de este remitente*. Incluso así, `lord-server` permite solo esos hosts de imágenes, en los puertos 80 y 443, por diez minutos, y rechaza cualquier dirección que no sea pública (red local, loopback, metadatos de la nube y similares), verificada sobre la dirección a la que de verdad se conecta. Los píxeles de rastreo se cuentan y nunca se muestran.
- **Los enlaces engañosos se marcan** en rojo con una advertencia cuando el texto visible parece una dirección pero apunta a otro lado, o usa trucos como una dirección IP, `user@` o caracteres parecidos. Al tocarlo se muestra el destino real. Mail nunca abre un navegador por su cuenta; podés copiar la dirección.
- **Las apps mismas están aisladas.** Desde 0.8.2, Mail, Calendario, Contactos y Recordatorios corren cada una como su propio proceso aislado, igual que una app `.lpk` instalada, y llegan al correo y al llavero solo con pedidos al broker permitidos para estas cuatro apps y ninguna otra. Desde 0.8.5, las conexiones a tus servidores de correo también salen de un proceso propio, así que tienen su propia fila y sus propias reglas en Monitor → Red.
- **Las contraseñas nunca tocan un archivo.** Mail guarda las contraseñas de las cuentas en el llavero del sistema (ver Contraseñas) y se las pasa a `lord-maild` solo en memoria. El archivo de cuentas guarda servidores, nombres de usuario, firmas y huellas de certificados de confianza, y el código se niega a escribir una contraseña ahí.

[[shot:1]]

[[shot:2]]

## Las preguntas del lector escéptico

**¿Y si un mensaje aprovecha un error en el lector?** Es justamente lo que el diseño supone que en algún momento va a pasar. El lector corre en `lord-maild`, que no puede crear sockets, no puede ejecutar programas y solo puede escribir en su propia caché. Los lectores se prueban a fondo: 30.953 controles en `mail-test`, con fuzzing por mutación de MIME, juegos de caracteres, el sanitizador de HTML, iCalendar, vCard y XML, más baterías aparte para la capa de red y el conector.

**¿Un ayudante tomado podría hablar con mi red local?** Solo con los servidores de tu archivo de cuentas. Los permisos para imágenes se limitan a direcciones públicas, y el control corre sobre la dirección a la que realmente se conectó, así que un truco de DNS no lo puede redirigir a tu router.

**¿Se congela el escritorio mientras se sincroniza el correo?** No. Los pedidos son asíncronos; dentro del ayudante, el trabajo de red corre en un hilo por cuenta. Medido en una VM de prueba: una carpeta de 10.000 mensajes se sincroniza en unos 2 s y se abre desde la caché local en unos 20 ms.

**¿Se puede ingresar con OAuth?** Está preparado pero desactivado. Los proveedores grandes solo dan tokens de ingreso a aplicaciones registradas con ellos, así que hace falta un ID de cliente registrado. Hasta entonces, esas cuentas usan contraseñas de aplicación.

[[shot:3]]

[[shot:4]]

## Límites honestos

- **No se probaron cuentas de proveedores reales.** La VM de prueba usó servidores IMAP, SMTP y CalDAV/CardDAV locales con una autoridad certificante de prueba. La configuración incluida de los proveedores sale de los valores que publican ellos.
- **Todavía no:** editar una sola repetición de un evento que se repite, crear calendarios o libretas de direcciones en el servidor, juegos de caracteres CJK multibyte, correo cifrado o firmado (S/MIME y OpenPGP) e IMAP QRESYNC.
- **El ingreso con OAuth** está desactivado hasta que se registre un ID de cliente.

## Leé el código

- [server/src/mail/maild.c](https://github.com/lordbasex/LordOS/blob/main/server/src/mail/maild.c): el ayudante `lord-maild` y su caja de arena
- [server/src/mail/mailc.c](https://github.com/lordbasex/LordOS/blob/main/server/src/mail/mailc.c): el conector en `lord-server` que decide qué destinos se permiten
- [server/src/mail/html.c](https://github.com/lordbasex/LordOS/blob/main/server/src/mail/html.c): el sanitizador de HTML
- [server/src/ui/js_mail.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_mail.c): `lord.mail`, la API de la sesión
- [apps/mail](https://github.com/lordbasex/LordOS/tree/main/apps/mail), [apps/calendar](https://github.com/lordbasex/LordOS/tree/main/apps/calendar), [apps/contacts](https://github.com/lordbasex/LordOS/tree/main/apps/contacts), [apps/reminders](https://github.com/lordbasex/LordOS/tree/main/apps/reminders)
- [server/tests/mail_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/mail_test.c): las pruebas de los lectores y el sanitizador
- [docs/MAIL.md](https://github.com/lordbasex/LordOS/blob/main/docs/MAIL.md)
