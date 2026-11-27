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
no verificada en el modelo de tu tarjeta"*. El driver libre (nouveau, de Mesa) es el predeterminado. El driver propio
de NVIDIA es opcional: Tienda → Para tu equipo lo ofrece para una tarjeta NVIDIA mientras se usa el driver
libre. Sus usos aparecen así:

- **IA y cómputo (CUDA):** **planeado**; LordOS todavía no lo ofrece.
- **Escritorio:** **experimental**. En la tarjeta que dibuja tu sesión, Ajustes muestra cómo se compone de verdad el
  escritorio (en la tarjeta o con el procesador). Con el driver de NVIDIA, hasta ahora un informe de la comunidad
  muestra el escritorio compuesto en la tarjeta (una GTX 1070, Pascal, driver 580); uno anterior (una RTX 3060 con
  LordOS 0.8.8.9) lo componía el procesador, y otra persona vio que las ventanas de algunas apps no se mostraban después
  de instalar el driver. Más informes nos dicen qué tarjetas y drivers funcionan.

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
