---
title: Copias de seguridad
app: backup
section: apps
order: 44
keywords: copia de seguridad, backup, restaurar, time machine, recuperar archivos
---
# Copias de seguridad

Copias automáticas de tu carpeta personal.

## Empezar

1. En **Ajustes** de la app elegí el **destino**: un disco externo (mejor en ext4) o una carpeta.
   Las copias se guardan en `Axia DE-Copias`.
2. Elegí la **frecuencia**: cada hora, cada día, cada semana o a mano.
3. **Copiar ahora** hace la primera.

## Cómo funciona

- Cada copia es una foto completa de tu carpeta personal en ese momento, pero **solo ocupa lo que
  cambió**: los archivos iguales a la copia anterior no se vuelven a guardar.
- **Se conservan** todas las copias de las últimas 24 horas, una por día del último mes y una por
  semana de antes. Si el disco se llena, se borran las más viejas.
- **No frena el escritorio:** corre con la prioridad más baja de procesador y de disco, y se pausa
  sola cuando el equipo usa la batería (se puede apagar).
- Si se corta en el medio (se desconecta el disco, se apaga el equipo), la próxima sigue desde
  donde quedó.
- **Exclusiones:** carpetas que no querés copiar. La caché y la Papelera nunca se copian.
- Corre con tu usuario: no hace falta contraseña de administrador.

## Restaurar

En **Copias** elegí la fecha y navegá como en Archivos. Podés **restaurar** un archivo, una
carpeta o **todo**. Si el archivo ya existe, se guardan los dos (con «(restaurado)» en el nombre),
salvo que elijas reemplazar.

Al terminar, un [aviso](help:notifications) te cuenta cómo fue. El cifrado de las copias todavía no
está disponible: si lo necesitás, usá un disco cifrado.
