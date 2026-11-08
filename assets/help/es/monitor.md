---
title: Monitor
app: monitor
section: apps
order: 26
keywords: monitor, procesos, cpu, memoria, red, conexiones, discos, mapa, gpu, vram
---
# Monitor

Muestra qué pasa en el equipo: procesador, memoria, red, discos, procesos y las conexiones de red
de cada app.

- **CPU, memoria y discos:** gráficos en vivo, por núcleo y por proceso.
- **Procesos:** ordenados por lo que más usan, con búsqueda.
- **Red:** una fila por app con su tráfico y sus conexiones, y un **mapa** con arcos hasta el país
  de cada servidor. El país sale de una base local: no se consulta a nadie.
- **GPU:** cada tarjeta gráfica dibujada en vivo, su uso, temperatura, ventilador, memoria, relojes y consumo, las
  apps que la usan (y qué API gráficas usan), las API instaladas, y si la voz de LordOS está funcionando en ella. Un
  administrador puede poner a mano el ventilador de una tarjeta AMD; vuelve a automático si la tarjeta se calienta
  demasiado y al cerrar la sesión.
- **Privacidad:** qué apps usaron la cámara, el micrófono o la pantalla.

Cerrado no gasta nada: los datos se toman solo mientras la ventana está abierta.
