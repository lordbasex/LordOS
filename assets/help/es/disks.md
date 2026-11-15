---
title: Discos
app: disks
section: apps
order: 43
keywords: discos, pendrive, usb, formatear, particiones, smart, salud, montar, expulsar, iso
---
# Discos

Para ver y preparar los discos y pendrives, como GNOME Discos.

## Ver

- Los discos internos, los externos y las imágenes, con sus particiones, cuánto ocupan y dónde
  están montadas.
- **Salud (SMART):** «Comprobar salud» lee lo que informa el disco: estado, temperatura, horas
  encendido, sectores reasignados o pendientes y, en los NVMe, la vida usada. Los discos virtuales
  y muchas tarjetas no informan nada.

## Hacer

- **Montar**, **desmontar** y **expulsar** un pendrive antes de sacarlo.
- **Formatear** un pendrive o una partición en **FAT32** (anda en todos lados, archivos de hasta
  4 GB), **exFAT** (pendrives grandes, sin ese límite), **ext4** (Linux) o **NTFS** (para discos compartidos
  con otros sistemas operativos, si está instalado su programa).
- **Renombrar** el volumen (desmontado).
- **Borrar:** rápido (la tabla y las firmas) o escribiendo ceros en todo.
- **Crear un USB de arranque** con una imagen `.iso` o `.img`: se copia tal cual y después se
  **verifica** leyendo el disco.

## Seguridad

Antes de borrar nada, un diálogo muestra exactamente qué se pierde (disco, particiones, dónde están
montadas y cuántos datos tienen) y hay que escribir el nombre del disco. **El disco del sistema no
se puede formatear ni borrar** desde acá. Los discos internos solo los cambia un
administrador; los pendrives, cualquier usuario.

Un administrador puede montar una **partición libre del disco del sistema**, como la de otro
sistema operativo instalado en el mismo disco. Nunca se ofrecen las particiones en uso (montadas,
cifradas, swap) ni las que el sistema se reserva (EFI, arranque, reservadas o de recuperación). Si el
otro sistema operativo no se apagó del todo (inicio rápido o hibernación), su partición se monta en
solo lectura: apagalo por completo para poder escribir.
