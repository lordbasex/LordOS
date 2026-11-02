## En pocas palabras

Contraseñas es el gestor de contraseñas de LordOS, y la misma maquinaria es el **llavero del sistema**: el lugar donde las apps guardan secretos como contraseñas de Wi-Fi, credenciales de VPN y contraseñas de cuentas de correo. Todo se guarda en un archivo de bóveda cifrado, y todo lo que toca tus secretos en claro corre en un único proceso chico y encerrado, `lord-vault`.

[[shot:0]]

Pensalo como el sector de cajas de seguridad de un banco. La ventana de Contraseñas es el mostrador donde hablás; `lord-vault` es la bóveda detrás de la pared. El empleado (`lord-server`) pasa documentos por una ranura, pero nunca tiene la llave. Solo sale lo que pedís ver, de a un ítem por vez.

[[diagram:vault]]

## Qué hace

- **Ítems para todo:** sitios web, tarjetas, Wi-Fi (con un código QR para conectarse desde un teléfono), servidores y claves SSH, licencias con sus archivos, documentos de identidad con fotos, billeteras cripto con una advertencia extra, notas seguras. Dieciséis categorías incluidas, más las tuyas, en árbol, con etiquetas.
- **Códigos 2FA** (TOTP y HOTP) con cuenta regresiva en vivo. Podés agregar uno desde una imagen QR; el secreto nunca sale de `lord-vault` y la ventana solo recibe el código actual.
- **Un generador** de contraseñas y de frases de contraseña a partir de una lista de 7776 palabras.
- **Un informe de seguridad** con contraseñas débiles, repetidas, viejas y filtradas. "Filtradas" se revisa **solo sin conexión**: contra una lista incluida de las 10.000 contraseñas más comunes y, si le das uno, un archivo local de hashes de contraseñas filtradas. Nunca se consulta nada en línea; `lord-vault` no tiene red en absoluto.
- **Importación** desde archivos KDBX (versiones 3.1 y 4), desde exportaciones CSV de navegadores y desde exportaciones CSV y JSON de otros gestores, con lectores propios de LordOS. **Exportación** a una copia cifrada, o a CSV después de una advertencia en rojo.

[[shot:3]]

## Cómo la desbloqueás

La bóveda tiene una única clave aleatoria de 32 bytes. Nunca se deriva de tu contraseña. En cambio, cada forma de desbloqueo envuelve esa misma clave:

- **una contraseña maestra**, estirada con Argon2id, con su costo medido en tu máquina para que tarde unos 0,7 s;
- **una llave de seguridad FIDO2** con la extensión `hmac-secret`, a través del cliente CTAP2 propio de LordOS;
- **un archivo llave**, por ejemplo en un pendrive;
- **una contraseña más un archivo llave**, como dos factores;
- **una clave de recuperación imprimible** de 40 caracteres.

Varios métodos pueden convivir. Por defecto, la bóveda se cierra sola después de 5 minutos sin uso, cuando bloqueás la pantalla y cuando cambiás de usuario.

[[shot:1]]

## Cómo protege tus secretos

**El archivo.** Un único archivo `.lvault`, cifrado con XChaCha20-Poly1305. El encabezado se autentica junto con el cuerpo, así que cambiar cualquier byte, intercambiar una ranura de desbloqueo o truncar el archivo se detecta. La batería de pruebas invierte cada byte de una bóveda y verifica que se detecte cada uno: 1165 de 1165. El guardado es atómico, con un `.bak` de la versión anterior.

**El proceso.** `lord-server` arranca `lord-vault` desde una ruta fija y sin argumentos, y se encierra antes de leer nada: sin red, sin sockets nuevos, sin ejecutar programas, sin archivos salvo la carpeta de la bóveda que recibe ya abierta. Otros procesos de tu usuario no pueden depurarlo ni volcar su memoria, y los secretos viven en memoria bloqueada (`mlock`) que queda fuera de los volcados de memoria y se borra al liberarse.

**La ventana.** La propia ventana de Contraseñas corre como su propio proceso aislado (`lord-server --app passwords`), no en el JavaScript del escritorio. Los valores revelados nunca entran en la memoria del escritorio. Las listas muestran títulos y nombres de usuario, nunca secretos; un valor viaja solo cuando tocás revelar, copiar o editar.

**La pantalla.** La ventana de Contraseñas está protegida: sale en negro en las capturas, las grabaciones de pantalla y el escritorio remoto.

**El portapapeles.** Un secreto copiado queda fuera del historial del portapapeles, se marca como sensible para las apps de Linux que respetan esa convención, y se borra a los 30 segundos (configurable) si todavía sigue ahí.

[[shot:2]]

## El llavero del sistema

- **Se abre al ingresar.** `lord-sessiond` verifica tu contraseña de ingreso, se la pasa una sola vez a `lord-vault` para abrir el llavero de la sesión y borra su copia.
- **Las apps de LordOS** guardan sus secretos con el `keyring` del SDK. Cada app ve solo sus propios ítems. Para las apps `.lpk` instaladas, `lord-server` fija el dueño a partir de la identidad verificada de la app, así que una app no puede hacerse pasar por otra. Si una app pide un ítem de otra app, el sistema primero te pregunta a vos.
- **Las apps de Linux** usan el Secret Service estándar de freedesktop por D-Bus, servido por código propio de LordOS. Chromium se probó de punta a punta: guardó un login, se reinició, buscó su clave en el llavero de LordOS y completó el login. Desde 0.8.5 una sesión que arranca con el equipo espera a su bus de sesión, así que con el ingreso automático el llavero de LordOS tiene el Secret Service desde el principio, y si una app trae gnome-keyring, solo corre su parte PKCS#11; en la VM de prueba Chrome pidió entonces la clave por el aviso del llavero de LordOS.

[[shot:4]]

## Las preguntas del lector escéptico

**¿Quién puede leer el llavero de la sesión?** Dentro de LordOS, cada app solo sus propios ítems, salvo que compartas uno. Por el Secret Service, la regla de acceso es la de freedesktop: cualquier app de Linux de tu sesión que llegue al bus de la sesión puede leer la colección desbloqueada, como con otros llaveros de escritorio. Las apps `.lpk` aisladas no llegan al bus.

**¿La app Contraseñas puede leer la llave de seguridad directamente?** No. Usar una llave de seguridad pasa por el aviso del permiso `usb`; después `lord-server` abre el dispositivo y se lo pasa directo a `lord-vault`. Ni la ventana de la app ni su JavaScript lo tienen nunca.

## Límites honestos

- **No se probaron llaves de seguridad reales.** El camino FIDO2 se probó solo contra un autenticador por software. Las llaves que solo verifican con un lector de huellas incorporado, y el protocolo de PIN 2 más nuevo, no están soportados.
- **El Secret Service es mínimo:** una sola colección, sin reglas de acceso por app, sin avisos al borrar.
- **El área de trabajo grande de Argon2 no se puede bloquear en memoria** con el límite predeterminado de Debian; queda fuera de los volcados y se borra después de cada uso.
- **Algunas variantes de KDBX no están soportadas** (Twofish y Argon2 versión 0x10).

## Leé el código

- [server/src/vault/vault.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vault.c): el proceso `lord-vault` y su caja de arena
- [server/src/vault/vfile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vfile.c): el formato `.lvault` y las ranuras de desbloqueo
- [server/src/vault/smem.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/smem.c): la zona de memoria bloqueada y borrada
- [server/src/vault/keyring.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/keyring.c) y [server/src/vault/secretsvc.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/secretsvc.c): el llavero y el Secret Service
- [server/src/vault/ctap2.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/ctap2.c): el cliente FIDO2
- [server/src/sec/brokerkr.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokerkr.c): el llavero para apps aisladas
- [apps/passwords](https://github.com/lordbasex/LordOS/tree/main/apps/passwords): la app
- [docs/PASSWORDS.md](https://github.com/lordbasex/LordOS/blob/main/docs/PASSWORDS.md)
