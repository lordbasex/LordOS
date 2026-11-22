---
title: Tarjetas gráficas
app: settings
section: system
order: 68
keywords: tarjeta gráfica, gpu, nvidia, amd, intel, driver, cuda, compatibilidad, informe, probar, nouveau, mesa, vulkan
---
# Tarjetas gráficas

[Ajustes → Tarjetas gráficas](settings:gpus) muestra cada tarjeta gráfica de la computadora, el driver que la maneja y cuál dibuja
el escritorio. Una tarjeta que no dibuja el escritorio se puede compartir con Máquinas, si lo admite.

## Tarjetas NVIDIA: experimental

El soporte de NVIDIA es **experimental y lo verifica la comunidad**. El equipo de LordOS probó gráficos Intel y AMD en
sus propias computadoras, pero no todos los modelos de NVIDIA, así que una tarjeta NVIDIA dice *"Experimental — todavía
no verificada en el modelo de tu tarjeta"*. Sus usos futuros aparecen como **planeados**:

- **IA y cómputo (CUDA):** instalar el driver de NVIDIA y sus bibliotecas CUDA desde Ajustes.
- **Escritorio:** dibujar el escritorio con el driver de NVIDIA.

Ninguno se puede activar todavía, y LordOS no cambia nada en la tarjeta hasta que estén listos: los dos necesitan
acciones del sistema que todavía se están revisando, y el escritorio necesita además sincronización explícita en el
servidor de pantalla.

## Cómo ayudar a probarla

1. Abrí **Ajustes → Tarjetas gráficas**, elegí **Enviar un informe de compatibilidad** y después **Crear informe**.
2. Leé el informe completo: muestra las tarjetas, sus drivers, cuál dibuja el escritorio, si la pantalla se compone en
   la tarjeta gráfica, las versiones de Mesa y de NVIDIA, los drivers de Vulkan y de video instalados y Secure Boot.
   Nunca incluye tu nombre, el nombre de la computadora, rutas de tu carpeta personal, números de serie ni direcciones
   de red.
3. Elegí **Abrir el formulario en GitHub**. Tu navegador abre el formulario de informes de LordOS ya completo; el
   informe entero también queda en el portapapeles. Contá qué funciona y qué no (el escritorio, video, juegos,
   suspensión) y envialo. No se envía nada hasta que lo hagas, y los informes en GitHub son públicos.

Sin navegador, abrí **Terminal** y ejecutá `lordos-issue --gpu`: imprime el mismo informe, listo para pegar.

El equipo de LordOS lee cada informe. Un modelo con buenos informes pasa de *experimental* a *verificado* en la lista
de tarjetas gráficas probadas de lordos.org.
