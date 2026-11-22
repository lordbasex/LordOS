<!-- BORRADOR para la aprobación del mantenedor (2026-11-22). No publicado en ningún lado. Publicar solo después del
     OK, cuando salga la versión con "Enviar un informe de compatibilidad" y la página "Tarjetas gráficas probadas"
     esté publicada. -->

# Buscamos testers: LordOS en tarjetas gráficas NVIDIA

**¿Tenés una tarjeta gráfica NVIDIA? Ayudanos a que LordOS funcione en ella.**

LordOS compone su escritorio en la tarjeta gráfica a través de Mesa, y eso está probado en gráficos Intel y AMD. NVIDIA
es distinto: no tenemos todas las tarjetas NVIDIA, y hasta ahora un solo informe de la comunidad (una RTX 3060 con el
driver de NVIDIA) es todo lo que sabemos. Por eso el soporte de NVIDIA en LordOS pasa a ser oficialmente
**experimental y verificado por la comunidad**, y crece con tus informes.

## Qué funciona hoy

- El escritorio se ve en cualquier tarjeta NVIDIA, con el driver `nouveau` de Mesa o con el driver propio de NVIDIA
  (en ese caso la pantalla se compone en el procesador, que es más lento pero funciona).
- **Ajustes → Tarjetas gráficas** marca una tarjeta NVIDIA como *Experimental — todavía no verificada en el modelo de tu
  tarjeta* y muestra lo que viene: **IA y cómputo (CUDA)** y **el escritorio con el driver de NVIDIA**. Los dos están
  planeados, todavía no disponibles, y LordOS no cambia nada en tu tarjeta hasta que lo estén.

## Qué te pedimos

1. Arrancá LordOS en tu computadora: alcanza con el USB live, no hace falta instalar nada.
2. Abrí **Ajustes → Tarjetas gráficas → Enviar un informe de compatibilidad → Crear informe**.
3. Leé el informe: tiene solo datos técnicos (las tarjetas, sus drivers, cuál dibuja el escritorio, las versiones de
   Mesa y de NVIDIA, los drivers de Vulkan y de video, Secure Boot). Nunca nombres, rutas de tu carpeta personal,
   números de serie ni direcciones.
4. **Abrí el formulario en GitHub**, contanos qué funcionó y qué no (el escritorio, video, juegos, suspensión,
   monitores externos) y envialo. ¿Sin navegador? Ejecutá `lordos-issue --gpu` en Terminal y pegá lo que imprime.

Nos sirven especialmente: tarjetas de las series GTX 10 (Pascal), RTX 20 (Turing), RTX 30 (Ampere), RTX 40 (Ada) y
RTX 50 (Blackwell), notebooks con NVIDIA más gráficos Intel o AMD, y computadoras con Secure Boot activado.

## Qué pasa después

El equipo de LordOS lee cada informe. Los modelos con informes que coinciden pasan a *verificada por la comunidad* en
[Tarjetas gráficas probadas](https://lordos.org/es/hardware/gpus/) (el enlace funciona cuando esa página se publique).
Los informes también ordenan los próximos pasos: instalar el driver de NVIDIA y CUDA desde Ajustes (con un registro
guiado de Secure Boot) y, después, componer el escritorio sobre el driver de NVIDIA.

¡Gracias por probar!
