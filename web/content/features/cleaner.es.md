**En desarrollo para 0.8.7. La última versión publicada, 0.8.6-beta, no incluye Limpieza.** Esta página describe el trabajo en la rama de desarrollo; todavía puede cambiar antes de publicarse.

## La versión corta

Limpieza muestra qué ocupa tu disco y te ayuda a quitar lo que se puede quitar con seguridad. Su regla es simple: **analizar, revisar, planificar, confirmar, ejecutar, verificar**. Un análisis solo lee. Nada cambia hasta que viste lo que va a pasar y dijiste que sí.

[[shot:0]]

Pensalo como vaciar una casa con cuidado. Alguien la recorre y hace una lista, sin tocar nada. Vos revisás la lista. Recién entonces algo sale de la casa, y cada cosa se comprueba otra vez en la puerta.

[[diagram:flow]]

## Qué hace

- **Analizar** corre Limpieza, Almacenamiento y Duplicados uno tras otro y después muestra un tablero con lo que encontró de verdad: una tarjeta por cada cosa encontrada, con sus tamaños reales y los archivos reales detrás.
- **Limpieza** lista cachés, descargas de herramientas de desarrollo, cachés de navegadores, temporales viejos, instaladores antiguos y la Papelera, cada uno con su motivo, su riesgo y lo que le va a pasar. Mientras corre un análisis ves las tres reglas que más encontraron hasta ese momento, con sus tamaños.
- **Proyectos de código** en tu carpeta personal: compilados de Node, Rust, Maven, JavaScript, Python, Gradle y Dart. El nombre de una carpeta no prueba nada, así que cada una se prueba con los archivos que tiene al lado o adentro, y las dependencias de Node se ofrecen solo cuando el proyecto entero lleva 60 días sin cambios. Los elegís uno por uno y van a la Papelera.
- **Videos grandes** de 100 MiB o más en el Escritorio y en tus grabaciones de pantalla también se eligen uno por uno.
- **Archivos grandes** filtra por tamaño, antigüedad y tipo.
- **Almacenamiento** muestra las carpetas más grandes como burbujas, un mapa, una lista de carpetas y un resumen por tipo. Cada carpeta se dibuja con lo que es, así videos, música y código se distinguen de un vistazo. En el mapa, unas bandas en cada mosaico muestran lo que se regenera solo y lo que conviene revisar antes, y una línea da el espacio libre, las dos cantidades y lo que quedó sin escanear.
- **Duplicados** encuentra copias exactas en las carpetas que elijas. No se selecciona nada por vos.
- **Actividad** registra lo que se ejecutó: cantidades y tamaños, nunca una ruta.

Una cosa sí queda en el disco con nombres adentro: el mapa de almacenamiento se guarda entre sesiones, en `~/.cache/lordos/cleaner-map.txt`, legible solo por tu usuario. Tiene nombres y tamaños de carpetas, y se queda en esta computadora. Viene activado: lo apagás en Ajustes o lo borrás con Borrar en Actividad. Un mapa guardado no tiene nada sobre lo que se pueda actuar; para limpiar, analizás otra vez.

[[shot:1]]

## Lo que no va a hacer

Las reglas de limpieza nunca alcanzan cookies, sesiones, historial ni contraseñas, ni `~/.cache` entero. La app no dice proteger tu computadora ni busca malware. No tiene botón para "liberar memoria" ni prueba de velocidad: ninguna de las dos cosas podría hacerla con honestidad. Los tamaños que muestra antes de actuar son estimaciones, y lo dice, porque los enlaces duros, los archivos abiertos, la compresión y las instantáneas cambian lo que se libera de verdad.

## Cómo se mantiene seguro

El análisis corre en `lord-clean`, un helper aislado que solo puede leer. Los planes se arman en `lord-server`, no en la app, y la app solo tiene identificadores de candidatos, nunca la autoridad para tocar una ruta. Todo se comprueba otra vez justo antes de actuar, incluidos los archivos que probaron una carpeta de proyecto.

Los archivos de caché regenerables bajo carpetas fijas se pueden borrar de verdad, como tu propio usuario, y solo por este camino: `lord.fs` sigue sin tener un borrado general. Todo lo demás, incluidas las carpetas de proyectos, va a la Papelera, desde donde se puede restaurar. Tres acciones de administración cerradas limpian los archivos del sistema, cada una confirmada; son la única parte que todavía no se ejecutó en una máquina de prueba.

## Lo que cuesta su movimiento

La app viene con tres aspectos, sigue el modo claro y oscuro, y funciona en inglés, español y portugués. Su diseño se invierte para los idiomas de derecha a izquierda, comprobado por ahora con un idioma de prueba con texto en inglés. Su movimiento corre solo con la ventana al frente y se detiene por completo con Reducir movimiento. Las partes que se mueven van en cuadros separados para que la pantalla se repinte lo menos posible; los números están en `docs/CLEANER-TESTS.md`. Se midieron en una máquina virtual y en una Raspberry Pi 5 con su monitor, donde la sesión tomó 4,5–7,2 % de un núcleo con la pantalla inicial quieta, 12,2–18,0 % analizando y 0,1 % con la app cerrada.

[[shot:2]]

Cada página de resultados tiene Empezar de nuevo y Analizar de nuevo, las listas tienen Seleccionar todo y Duplicados tiene Seleccionar copias, que deja una copia de cada grupo.
