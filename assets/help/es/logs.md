---
title: Registros
app: logs
section: apps
order: 45
keywords: registros, logs, journal, errores, depurar, consola, diagnóstico
---
# Registros

Los registros del sistema y de LordOS. Sirve para ver qué pasó cuando
algo falla.

## Fuentes

- **Mis registros:** tu journal de systemd (tus apps y servicios), leído con tus permisos.
- **Sistema** y **Kernel:** el journal del sistema. Lo ven los administradores (grupo sudo) por
  el ayudante de administración, o quien esté en el grupo de lectura del journal.
- **LordOS:** los registros de tu sesión (`compositor`, `shell`, `xwayland`…; "sesión anterior" es la de
  antes de este inicio de sesión), y debajo el registro propio de cada **app** y de cada **app web**. Están
  en `/var/log/lordos/users/<vos>`, que solo vos podés leer, y los viejos se comprimen y se borran después
  de una semana. **Registros anteriores** son los que dejaron versiones anteriores en `~/.cache/lordos`.

## Filtrar

- **Nivel:** todo, información, avisos o errores.
- **Tiempo:** últimos 15 minutos, la última hora, 24 horas o todo.
- **Proceso o unidad**, y **buscar** en el texto de los mensajes.

Un clic en una entrada muestra el mensaje completo con sus campos (hora, proceso, PID, unidad).

## En vivo y exportar

- **En vivo** sigue los mensajes nuevos a medida que llegan, sin recargar.
- **Exportar** guarda lo que se ve (con los filtros) en un archivo de texto en tu carpeta, para
  mandarlo si pedís ayuda.
