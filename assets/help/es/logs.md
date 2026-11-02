---
title: Registros
app: logs
section: apps
order: 45
keywords: registros, logs, journal, errores, depurar, consola, diagnóstico
---
# Registros

Los registros del sistema y de Axia DE. Sirve para ver qué pasó cuando
algo falla.

## Fuentes

- **Mis registros:** tu journal de systemd (tus apps y servicios), leído con tus permisos.
- **Sistema** y **Kernel:** el journal del sistema. Lo ven los administradores (grupo sudo) por
  el ayudante de administración, o quien esté en el grupo de lectura del journal.
- **Axia DE:** los registros de tu sesión (`~/.cache/axia`, como `session.log`).

## Filtrar

- **Nivel:** todo, información, avisos o errores.
- **Tiempo:** últimos 15 minutos, la última hora, 24 horas o todo.
- **Proceso o unidad**, y **buscar** en el texto de los mensajes.

Un clic en una entrada muestra el mensaje completo con sus campos (hora, proceso, PID, unidad).

## En vivo y exportar

- **En vivo** sigue los mensajes nuevos a medida que llegan, sin recargar.
- **Exportar** guarda lo que se ve (con los filtros) en un archivo de texto en tu carpeta, para
  mandarlo si pedís ayuda.
