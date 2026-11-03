LordOS viene como una máquina virtual lista para amd64: `lordos-<versión>-amd64.ova`. Ya está instalada, así que no hay instalador: la abrís en tu hipervisor, la arrancás y creás tu cuenta.

## Qué necesitás

- Una PC de 64 bits (amd64) con la virtualización por hardware activada en el firmware. La OVA no funciona en computadoras con procesador ARM: para esas, usá la ISO arm64.
- **VirtualBox 7** (probado en 7.2) o **VMware** Fusion o Workstation.
- Memoria y procesadores para la máquina: **al menos 4 CPU y 4 GB de memoria**. Con 2 CPU no rinde bien. Con muchas apps abiertas, **lo ideal son 16 GB**. La computadora que la corre necesita eso libre, además de lo que ya usa.
- Disco: la descarga pesa unos 1,3 GB. El disco de la máquina es de 32 GB y crece a medida que se usa; al principio ocupa unos pocos GB.

Todos los números de cada imagen están en [Requisitos del sistema](/es/requirements/).

## 1. Descargala y comprobala

Descargá `lordos-<versión>-amd64.ova`, `SHA256SUMS` y `SHA256SUMS.sig` de la [última versión](https://github.com/lordbasex/LordOS/releases). En la carpeta con los archivos:

```sh
shasum -a 256 -c SHA256SUMS --ignore-missing
```

"OK" al lado del nombre quiere decir que es el archivo que publicamos. Cómo comprobar también la firma está en [Firma de código](/es/legal/code-signing/).

## 2. Importala

**VirtualBox:** Archivo → Importar servicio virtualizado, elegí el `.ova` y tocá Importar. La máquina llega con 4 CPU, 4 GB de memoria y la pantalla ya configurada.

**VMware Fusion:** Archivo → Importar, elegí el `.ova` y dónde guardar la máquina. **VMware Workstation:** Archivo → Abrir, elegí el `.ova`. Si VMware dice que el archivo no cumple la especificación OVF, tocá Reintentar.

## 3. Creá tu cuenta

Arrancá la máquina. Se abre sola **Configurar LordOS**:

1. **Idioma y región** y **Teclado**.
2. **Usuario**: tu nombre, el nombre de usuario y la contraseña.
3. **Resumen**, y después **Crear cuenta**.
4. **Reiniciar**. Aparece la pantalla de inicio de sesión: entrá con la contraseña que elegiste.

Hasta que la cuenta exista, la máquina corre una sesión de configuración sin contraseña, a la que solo se llega desde su propia pantalla. Terminá este paso antes de dejarla prendida.

## Ajustar la máquina

Con la máquina apagada, en su configuración:

- **Procesadores y memoria:** subilos si la computadora tiene lugar; con muchas apps, lo ideal son 16 GB de memoria.
- **Pantalla:** en VirtualBox viene VBoxSVGA; VMSVGA también funciona.
- **Red:** NAT, la opción por defecto, alcanza para navegar y actualizar. Para que otras computadoras de tu red lleguen a ella (por ejemplo, con escritorio remoto), usá un adaptador puente.

## Si algo sale mal

- **VirtualBox dice `VERR_ALREADY_EXISTS` al importar:** quedó en `VirtualBox VMs` la carpeta de una máquina anterior con el mismo nombre. Quitar una máquina con "Solo quitar" deja sus archivos. Cambiá el nombre en la pantalla de importación o borrá esa carpeta vieja.
- **Pantalla negra o ventana chica:** revisá el controlador de pantalla (arriba) y dale a la máquina al menos 4 GB de memoria.
- **Va lenta:** fijate que tenga 4 CPU o más y que la virtualización por hardware esté activada en el firmware de la computadora.

Para reportar un error, corré `lordos-issue` en la Terminal de la máquina y adjuntá el archivo que escribe al [issue](https://github.com/lordbasex/LordOS/issues).
