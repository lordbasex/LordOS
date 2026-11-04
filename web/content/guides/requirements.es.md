Qué necesita cada imagen de LordOS. Si un número de esta página no coincide con otra, la que mantenemos al día es esta.

## De un vistazo

| | PC (ISO) | Máquina virtual | Raspberry Pi 5 / Raspberry Pi 500 | Paquete para Debian 13 |
|---|---|---|---|---|
| Archivo | `lordos-live-<versión>-amd64.iso` | `lordos-<versión>-amd64.ova`, o una ISO | `lordos-<versión>-rpi5-arm64.img.xz` | `install.sh` |
| Procesador | Intel o AMD de 64 bits (amd64) | al menos 4 CPU | Raspberry Pi 5 o Raspberry Pi 500 | amd64 o arm64 |
| Memoria | 4 GB mínimo, 8 GB recomendado | 4 GB mínimo, lo ideal 16 GB | con 4 GB anda, se recomiendan 8 o 16 GB | igual que en una PC |
| Almacenamiento | disco de 8 GB mínimo, 32 GB recomendado | disco de 32 GB (el de la OVA crece con el uso) | tarjeta SD de 16 GB (8 GB mínimo) o NVMe | 2 GB libres en `/var` |
| Para instalar | un pendrive de 2 GB o más | el archivo OVA | una tarjeta SD o un disco NVMe | un sistema con Debian 13 |

## PC

- **Procesador:** cualquier Intel o AMD de 64 bits. No hay versión de 32 bits.
- **Memoria:** 4 GB mínimo; se recomiendan 8 GB, y 16 GB con muchas apps abiertas.
- **Almacenamiento:** para instalar hace falta un disco de al menos 8 GB (el límite del instalador es 8 GiB); se recomiendan 32 GB o más. Instalar al lado de otro sistema necesita un disco GPT con partición EFI, la computadora arrancada en modo UEFI y al menos 8 GB libres. Esa opción todavía no se corrió nunca hasta el final, así que hacé una copia de seguridad antes.
- **Firmware:** UEFI o BIOS. Desactivá el arranque seguro (Secure Boot).
- **Gráficos:** cualquiera. Con una placa de video compatible (por Mesa: AMD, Intel y otras) el escritorio se compone en la GPU; sin ella, el camino por CPU siempre funciona.
- **Para arrancarlo:** un pendrive de 2 GB o más; la ISO pesa unos 1,1 GB. El sistema en vivo también se puede copiar a la memoria al arrancar (hay una opción en el menú de arranque); para eso, dale a la computadora 4 GB o más.

## ISO arm64

`lordos-live-<versión>-arm64.iso` es para máquinas y máquinas virtuales arm64 que arrancan con UEFI. Por ahora solo se corrió en QEMU emulado, no en hardware real. Los números de memoria y almacenamiento son los de la PC.

## Máquina virtual

- **Procesadores y memoria:** al menos 4 CPU y 4 GB de memoria. Con 2 CPU no rinde bien. Con muchas apps abiertas, lo ideal son 16 GB.
- **Hipervisor:** la OVA se abre en VirtualBox 7 y en VMware; la ISO funciona en esos y en otros, como Proxmox. Para la ISO se recomienda UEFI, con el tipo de sistema Debian de 64 bits.
- **La computadora que la corre:** una PC de 64 bits con la virtualización por hardware activada en el firmware, y la memoria de la máquina libre además de la que ya usa.
- **Almacenamiento:** el disco de la OVA es de 32 GB y crece a medida que se usa.

Paso a paso: [Máquina virtual (OVA)](/es/virtual-machine/).

## Raspberry Pi 5 y Raspberry Pi 500

- **Placa:** Raspberry Pi 5 de 4, 8 o 16 GB. Con 4 GB anda; se recomiendan 8 o 16 GB. La **Raspberry Pi 500** (una Pi 5 dentro de un teclado) usa la misma imagen y también funciona: la probó Steve Sande, ¡gracias! Los demás modelos de Raspberry Pi no están soportados.
- **Almacenamiento:** una tarjeta SD de 16 GB o más (8 GB mínimo), o un disco NVMe en la M.2 HAT+, que es más rápido.
- **Alimentación y refrigeración:** se recomiendan la fuente oficial de 27 W (5 V, 5 A) y refrigeración activa.

[LordOS Imager](/es/imager/) graba la imagen y prepara el primer arranque.

## Sobre un Debian 13 que ya tenés

`curl -fsSL https://lordos.org/install.sh | sh` agrega LordOS a Debian 13 (trixie), amd64 o arm64, arrancado con systemd. Necesita root o `sudo` y 2 GB libres en `/var`. Otras distribuciones basadas en Debian pueden funcionar, con una advertencia.

## No soportado

- Procesadores de 32 bits.
- Arrancar con Secure Boot activado.
- Modelos de Raspberry Pi que no sean la Pi 5 ni la Pi 500.
