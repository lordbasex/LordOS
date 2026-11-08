## En pocas palabras

**Nuevo en la 0.8.8.6, publicada el 8 de noviembre de 2026.**

LordOS lee texto en voz alta en tu propio equipo, sin internet. Seleccioná algo en el Editor de texto, la Terminal, Notas o una app de Linux como Firefox, apretá **❖Esc** y lo escuchás. Apretá ❖Esc otra vez y se detiene. Nada de lo que leés se manda a ningún lado: la voz se hace en tu procesador, o en tu placa de video cuando tiene una que ayuda.

Pensalo como un lector sentado a tu lado que solo lee lo que le señalás. No toma notas, no hace llamadas y no sale de la habitación.

[[diagram:flow]]

## Dónde lo encontrás

- **Habla en toda app con texto.** *Edición → Habla* en el Editor de texto y la Terminal (en Notas, en Ver), y lo mismo en el menú del clic derecho del Editor de texto, la Terminal y Notas: *Iniciar locución*, *Detener locución* y *Guardar como audio…*. Iniciar locución lee la selección, o todo el texto cuando no hay nada seleccionado.
- **También las apps de Linux.** Una app de Linux recibe un menú *Edición* con Habla en la barra de menús (en Lord Classic, en la tira de menús de su ventana), que lee lo que seleccionaste en ella.
- **Un reproductor pequeño.** Mientras lee, una píldora muestra la onda de la voz en color, con leer de nuevo, pausa y seguir, detener y cerrar. Lo podés arrastrar a cualquier lugar, incluso a otra pantalla, y la próxima lectura se abre ahí.
- **Guardar como audio…** pregunta dónde guardar con el diálogo de siempre y escribe un archivo WAV.
- **Voz en tiempo real** (❖⇧Esc): un cuadro donde escribís, apretás Enter y se dice con la voz que elegiste. Las frases recientes quedan abajo, solo en memoria, para decirlas otra vez. Ayuda a quien no puede o prefiere no hablar.
- **El comando `say`** en una terminal: `say "Hola"`, `echo texto | say`, `say -f notas.txt -o notas.wav`, `say -v <voz>`, `say --voices`.

El idioma (español, inglés o portugués) se deduce del propio texto; cuando no se puede, se usa el de la interfaz.

## Las voces que elegís

**LordOS no trae ninguna voz.** En **Ajustes → Inteligencia artificial → Voz** descargás solo lo que querés, por idioma:

- un modelo **Estándar** (128 MB), rápido en cualquier equipo, y uno **HD** (672 MB; inglés 1,3 GB), más natural;
- **23 voces**, 11 femeninas y 12 masculinas, cada una hablando todos los idiomas. **▶** reproduce una muestra grabada antes de que descargues nada.

Vienen como **paquetes firmados de LordOS** desde el propio repositorio de paquetes de LordOS, nunca desde un sitio de terceros. Solo se ofrecen voces cuyas grabaciones permiten la redistribución y el uso comercial; las voces con licencia no comercial o de origen no documentado quedaron afuera.

## Tu propia voz

**Voz personal** arma una voz con la tuya en alrededor de un minuto, y habla todos los idiomas:

1. LordOS explica qué es y te pide el consentimiento antes de grabar nada.
2. Primero escucha el ambiente tres segundos. Si hay demasiado ruido, o el micrófono no manda nada, lo dice.
3. Leés seis frases cortas, una por vez (cada una se detiene sola cuando terminás), o **hablás libremente** de 15 a 30 segundos sobre lo que quieras, para quien leer en voz alta le cuesta.

La grabación queda en tu cuenta, en este equipo. **Usar una grabación…** hace lo mismo a partir de un archivo de audio (WAV, MP3, M4A, OGG, Opus o FLAC).

## Cómo te protege

- **El motor está encerrado.** La voz la hace `lord-tts`, un ayudante pequeño que corre en el sandbox de ayudantes de LordOS: sin red, sin sockets, sin arrancar programas, y solo puede escribir en su propia carpeta de caché. Es el único programa que enlaza las bibliotecas de voz. Cuando usa la placa de video, solo puede abrir el nodo de render de la placa, nunca la pantalla que maneja `lord-server`.
- **Tu texto nunca va en una línea de comandos.** `lord-server` arranca `lord-say` desde una ruta fija, con argumentos fijos, y le pasa el texto como un archivo en memoria sellado. Lo único que puede elegir la interfaz es un nombre de voz, verificado en C.
- **Guardar no se puede desviar.** `lord-server` verifica la carpeta que elegiste, crea el archivo ahí y lo renombra cuando el audio termina, así `say` nunca ve una ruta.
- **La selección de una app de Linux se queda con ella.** El escritorio la lee solo mientras esa app tiene el teclado, y nunca llega a JavaScript.
- **Las apps aisladas no lo pueden usar.** Las apps `.lpk` instaladas no tienen API de voz.

## Velocidad

Medido en una PC, un Intel i9-10900 con una Radeon RX 580: unos 10 segundos de voz en 1,3 s en la placa de video y 3 s en el procesador, con unos 220 MB de memoria. Lee una oración por vez, así la primera suena mientras se hace la siguiente. Una oración que sale con un siseo debajo de la voz se detecta midiéndola y se hace de nuevo.

## Límites honestos

- **Tres idiomas:** español, inglés y portugués.
- **Primero descargás un modelo.** Hasta entonces, Habla avisa que no hay ninguna voz instalada.
- **No se probó en la Raspberry Pi** (los caminos de código de su procesador y el driver gráfico de la Pi 5).
- **Lee lo que seleccionás; no es un lector de pantalla.** Un lector de pantalla, el zoom y el alto contraste siguen en la hoja de ruta.

## Leé el código

- [docs/VOICE.md](https://github.com/lordbasex/LordOS/blob/main/docs/VOICE.md): el diseño, los modelos, las voces y las pruebas
- [server/src/doc/h_tts.c](https://github.com/lordbasex/LordOS/blob/main/server/src/doc/h_tts.c): `lord-tts`, el motor aislado
- [server/src/tts/speak.c](https://github.com/lordbasex/LordOS/blob/main/server/src/tts/speak.c): cómo `lord-server` arranca `lord-say`
- [server/src/ui/js_voice.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_voice.c): la API `lord.voice`
- [apps/settings/PersonalVoice.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/settings/PersonalVoice.jsx) y [sdk/livespeech.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/livespeech.jsx): Voz personal y Voz en tiempo real

La voz viene de Pocket TTS de Kyutai (pesos del modelo bajo CC-BY-4.0), corrido por audio.cpp (Apache-2.0) y su ggml (MIT).
