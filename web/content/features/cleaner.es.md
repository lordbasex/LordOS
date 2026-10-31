**En desarrollo para 0.8.7. La última versión publicada, 0.8.6-beta, no incluye Limpieza.** Esta página describe el trabajo en la rama de desarrollo; todavía puede cambiar antes de publicarse.

## La versión corta

Limpieza muestra qué ocupa tu disco y te ayuda a quitar lo que se puede quitar con seguridad. Su regla es simple: **analizar, revisar, planificar, confirmar, ejecutar, verificar**. Un análisis solo lee. Nada cambia hasta que viste lo que va a pasar y dijiste que sí.

[[shot:0]]

Pensalo como vaciar una casa con cuidado. Alguien la recorre y hace una lista, sin tocar nada. Vos revisás la lista. Recién entonces algo sale de la casa, y cada cosa se comprueba otra vez en la puerta.

[[diagram:flow]]

## Qué hace

- **Analizar** corre Limpieza, Almacenamiento y Duplicados uno tras otro y después muestra un tablero con lo que encontró de verdad: una tarjeta por cada cosa encontrada, con sus tamaños reales y los archivos reales detrás.
- **Limpieza** lista cachés, descargas de herramientas de desarrollo, cachés de navegadores, temporales viejos y la Papelera, cada uno con su motivo, su riesgo y lo que le va a pasar.
- **Archivos grandes** filtra por tamaño, antigüedad y tipo.
- **Almacenamiento** muestra las carpetas más grandes como burbujas, un mapa, una lista de carpetas y un resumen por tipo. Cada carpeta se dibuja con lo que es, así videos, música y código se distinguen de un vistazo.
- **Duplicados** encuentra copias exactas en las carpetas que elijas. No se selecciona nada por vos.
- **Actividad** registra lo que se ejecutó: cantidades y tamaños, nunca una ruta.

[[shot:1]]

## Lo que no va a hacer

Las reglas de limpieza nunca alcanzan cookies, sesiones, historial ni contraseñas, ni `~/.cache` entero. La app no dice proteger tu computadora ni busca malware. No tiene botón para "liberar memoria" ni prueba de velocidad: ninguna de las dos cosas podría hacerla con honestidad. Los tamaños que muestra antes de actuar son estimaciones, y lo dice, porque los enlaces duros, los archivos abiertos, la compresión y las instantáneas cambian lo que se libera de verdad.

## Cómo se mantiene seguro

El análisis corre en `lord-clean`, un helper aislado que solo puede leer. Los planes se arman en `lord-server`, no en la app, y la app solo tiene identificadores de candidatos, nunca la autoridad para tocar una ruta. Todo se comprueba otra vez justo antes de actuar.

Los archivos de caché regenerables bajo carpetas fijas se pueden borrar de verdad, como tu propio usuario, y solo por este camino: `lord.fs` sigue sin tener un borrado general. Todo lo demás va a la Papelera, desde donde se puede restaurar. Tres acciones de administración cerradas limpian los archivos del sistema, cada una confirmada; son la única parte que todavía no se ejecutó en una máquina de prueba.

## Liviano en una computadora modesta

La app viene con tres aspectos, sigue el modo claro y oscuro, y funciona en inglés, español y portugués, también de derecha a izquierda. Su movimiento corre solo con la ventana al frente y se detiene por completo con Reducir movimiento. Las partes que se mueven van en cuadros separados para que la pantalla se repinte lo menos posible; los números están en `docs/CLEANER-TESTS.md`. Se midieron en una máquina virtual, no en una Raspberry Pi.

[[shot:2]]
