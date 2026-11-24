<!-- BORRADOR: no publicado. Espera la aprobación del mantenedor (gpus.meta.json). -->

En qué tarjetas gráficas funciona LordOS, quién lo comprobó y cómo. El soporte de NVIDIA es **experimental y lo
verifica la comunidad**: no tenemos todas las tarjetas, así que tus informes de compatibilidad son la forma en que se
verifica un modelo.

## Estado

| Estado | Qué significa |
|---|---|
| ✓ **Verificada** | El equipo de LordOS la probó en su propio hardware con esta línea de versiones. |
| ✓ **Verificada por la comunidad** | Al menos dos informes de compatibilidad independientes coinciden, y el equipo los revisó. |
| ◐ **Experimental** | Un informe o ninguno: puede funcionar, nadie lo confirmó todavía. Toda tarjeta NVIDIA empieza acá. |
| ✗ **No funciona** | Reportada rota, con un issue abierto. |

## Tarjetas probadas

| Tarjeta | Driver del kernel | Qué se probó | Estado | Comprobado por |
|---|---|---|---|---|
| AMD Radeon RX 580 (Polaris, Ellesmere) | `amdgpu`, Mesa `radeonsi` | Escritorio compuesto en la GPU, dos monitores DisplayPort, Vulkan (RADV) | ✓ Verificada | Equipo de LordOS |
| Intel UHD Graphics 630 (Comet Lake) | `i915` con GVT-g | Compartida con Máquinas como GPU virtual (GVT-g), junto a la RX 580 que dibuja el escritorio | ✓ Verificada | Equipo de LordOS |
| NVIDIA GeForce RTX 3060 (GA106) | `nvidia` (el driver propietario) | El escritorio funciona, compuesto en la CPU: LordOS todavía no compone sobre el driver propietario de NVIDIA | ◐ Experimental | Un informe de la comunidad ([#171](https://github.com/lordbasex/LordOS/issues/171)) |
| NVIDIA GeForce RTX 5070 Ti 16 GB (GB203) | `nvidia` 620.73.04 (los módulos de kernel abiertos de NVIDIA) | El escritorio funciona, fluido, sin cortes ni fallas; el informe no dice si se compuso en la GPU o en la CPU. Las bibliotecas de CUDA estaban instaladas y llama.cpp corrió sobre ella | ◐ Experimental | Un informe de la comunidad ([#235](https://github.com/lordbasex/LordOS/issues/235)) |

Más gráficos Intel y AMD corren LordOS en los
[informes de hardware](https://github.com/lordbasex/LordOS/blob/main/docs/HARDWARE-REPORT.md) de la comunidad; esta
página lista las tarjetas comprobadas específicamente en gráficos.

## NVIDIA: qué funciona hoy

- **Cualquier tarjeta NVIDIA** muestra el escritorio: con el driver `nouveau` de Mesa, o con el driver de NVIDIA y la
  pantalla compuesta en la CPU. Ajustes → Tarjetas gráficas la marca *Experimental — todavía no verificada en el modelo
  de tu tarjeta*.
- **IA y cómputo (CUDA)** y **el escritorio con el driver de NVIDIA** están planeados, todavía no disponibles: instalar
  el driver de NVIDIA desde Ajustes y componer el escritorio sobre él llegan cuando se aprueben sus acciones del sistema
  y el diseño de sincronización explícita.

## Enviá un informe

1. En LordOS, abrí **Ajustes → Tarjetas gráficas** y elegí **Enviar un informe de compatibilidad**.
2. Leé el informe completo. Tiene solo datos técnicos: las tarjetas, sus drivers, cuál dibuja el escritorio, cómo se
   compone la pantalla, las versiones de Mesa y de NVIDIA, los drivers de Vulkan y de video instalados y Secure Boot.
   Nunca nombres, rutas de tu carpeta personal, números de serie ni direcciones.
3. Elegí **Abrir el formulario en GitHub**, contá qué funcionó y qué no, y envialo. Sin navegador, ejecutá
   `lordos-issue --gpu` en Terminal y pegá lo que imprime en un
   [informe gráfico nuevo](https://github.com/lordbasex/LordOS/issues/new?template=gpu-report.yml).

## Cómo se revisan los informes

- El equipo lee cada informe y comprueba que sus datos cierran (los ids PCI, el driver y su versión, qué tarjeta dibuja
  el escritorio), y pregunta en el issue cuando falta algo.
- Un modelo pasa a **Verificada por la comunidad** cuando dos informes independientes del mismo modelo y familia de
  driver coinciden; un informe de que algo está roto abre un bug y marca el modelo **No funciona** hasta que se arregle.
- A esta página llegan solo la tarjeta, su driver y el resultado, con un enlace al issue público. No se copia nada más.
