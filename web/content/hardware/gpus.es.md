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
| NVIDIA GeForce RTX 3060 (GA106) | `nvidia` (el driver propietario) | El escritorio funciona, compuesto en la CPU: en LordOS 0.8.8.9 el arranque de la GPU no obtuvo un contexto GL de este driver | ◐ Experimental | Un informe de la comunidad ([#171](https://github.com/lordbasex/LordOS/issues/171)) |
| NVIDIA GeForce RTX 5070 Ti 16 GB (GB203) | `nvidia` 620.73.04 (los módulos de kernel abiertos de NVIDIA) | El escritorio funciona, fluido, sin cortes ni fallas; el informe no dice si se compuso en la GPU o en la CPU. Las bibliotecas de CUDA estaban instaladas y llama.cpp corrió sobre ella | ◐ Experimental | Un informe de la comunidad ([#235](https://github.com/lordbasex/LordOS/issues/235)) |
| Intel HD Graphics 520 (Skylake GT2) | `i915`, Mesa `iris` | El escritorio funciona, fluido, sin fallas, compuesto en la GPU | ◐ Experimental | Un informe de la comunidad ([#278](https://github.com/lordbasex/LordOS/issues/278)) |
| Intel UHD Graphics (Tiger Lake-H GT1) | `i915`, Mesa `iris` | Dibuja el escritorio, compuesto en la GPU, en una laptop con una segunda GPU NVIDIA; el informe dice que funciona con problemas que no nombra | ◐ Experimental | Un informe de la comunidad ([#274](https://github.com/lordbasex/LordOS/issues/274)) |
| NVIDIA GeForce GTX 1070 (GP104) | `nvidia` 580.196.11 (el driver propietario) | El escritorio funciona, fluido, sin fallas, compuesto en la GPU con el OpenGL ES de NVIDIA; reproducción de video | ◐ Experimental | Un informe de la comunidad ([#276](https://github.com/lordbasex/LordOS/issues/276)) |
| NVIDIA GeForce GTX 1650 Mobile / Max-Q (TU117M) | `nvidia` 550.163.01 (el driver propietario, de los paquetes de Debian) | La segunda GPU de una laptop híbrida, junto a la tarjeta Intel que dibuja el escritorio. Monitor → GPU no mostró sus números: todavía no encuentra NVML donde lo ponen los paquetes de Debian | ◐ Experimental | Un informe de la comunidad ([#274](https://github.com/lordbasex/LordOS/issues/274), [#273](https://github.com/lordbasex/LordOS/issues/273)) |

Más gráficos Intel y AMD corren LordOS en los
[informes de hardware](https://github.com/lordbasex/LordOS/blob/main/docs/HARDWARE-REPORT.md) de la comunidad; esta
página lista las tarjetas comprobadas específicamente en gráficos.

## NVIDIA: qué funciona hoy

- **El driver libre es el predeterminado.** Cualquier tarjeta NVIDIA muestra el escritorio con el driver `nouveau` de
  Mesa. Ajustes → Tarjetas gráficas la marca *Experimental — todavía no verificada en el modelo de tu tarjeta*.
- **El driver propio de NVIDIA es opcional**: Tienda → Para tu equipo lo ofrece para una tarjeta NVIDIA mientras se usa
  el driver libre.
- **El escritorio con el driver de NVIDIA es experimental.** Un informe de la comunidad lo muestra compuesto en la GPU,
  fluido (una GTX 1070, Pascal, driver 580.196.11, [#276](https://github.com/lordbasex/LordOS/issues/276)); uno
  anterior, en LordOS 0.8.8.9, se componía en la CPU porque el arranque de la GPU no obtuvo un contexto GL (una RTX 3060,
  [#171](https://github.com/lordbasex/LordOS/issues/171)); y en el hilo de #276 otra persona vio que las ventanas de
  algunas apps no se mostraban después de instalar el driver (una GTX 960 y una RTX 3070 Ti con 550.163.01). En la
  tarjeta que dibuja tu sesión, Ajustes → Tarjetas gráficas muestra cómo se compone de verdad el escritorio.
- **IA y cómputo (CUDA)** está planeado; LordOS todavía no lo ofrece.

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
