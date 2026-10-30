¿Encontraste algo que no funciona, o tenés una idea para mejorar LordOS? Esta página explica dónde contarnos y qué incluir, para que podamos actuar rápido.

## Dónde reportar

- **Por ahora** el repositorio de LordOS en GitHub es privado, así que los reportes de errores y los pedidos de funciones van por [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es). Empezá el asunto con "Bug" o "Feature request".
- **Cuando el repositorio sea público**, usá [GitHub Issues](https://github.com/lordbasex/LordOS/issues). Elegí la plantilla de reporte de error o de pedido de función; los issues en blanco están desactivados para que cada reporte traiga la información que necesitamos.

Antes de escribir, revisá las [preguntas frecuentes](/faq/) y, cuando sean públicos, los issues que ya existen: quizás alguien ya lo reportó.

## Los problemas de seguridad van por otro lado

**Nunca reportes una vulnerabilidad en un issue público.** Seguí la [Política de seguridad](/legal/security/) y usá [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es) con "Security" en el asunto. Los issues públicos sobre problemas de seguridad se van a ocultar y se van a tratar en privado.

## Qué incluir en un reporte de error

- **La versión de LordOS**, como aparece en la app Acerca de LordOS (por ejemplo "0.8.6 · Beta").
- **Cómo la instalaste:** la ISO (live o instalada), la imagen para Raspberry Pi 5 o el `.deb` en Debian 13.
- **Tu hardware:** el modelo de la computadora o de la placa, la placa de video, la memoria y cualquier dispositivo que tenga que ver (impresora, touchpad, adaptador Wi-Fi, monitor).
- **Pasos para reproducirlo:** qué hiciste, un paso por línea, desde un arranque limpio si podés.
- **Qué esperabas** y **qué pasó en cambio**, con el texto exacto de cualquier mensaje de error.
- **Registros:** abrí la app Registros, buscá las entradas del momento del problema y copialas en el reporte. Las capturas de pantalla o una grabación corta también ayudan.

Un problema por reporte, por favor. Si encontrás varios, mandá varios reportes.

## Qué no incluir

Los registros y las capturas pueden tener información privada. Antes de mandarlos, sacá o tapá:

- contraseñas, frases de contraseña, códigos de recuperación, códigos 2FA y claves privadas;
- datos personales: nombres, direcciones, teléfonos, correos, nombres de cuentas;
- el contenido de correos, documentos o mensajes privados;
- direcciones IP, nombres de redes Wi-Fi y datos de VPN que no quieras compartir.

Nunca te vamos a pedir una contraseña.

## Pedidos de funciones

Contanos qué querés hacer y por qué, no solo la solución que tenés en mente: el problema nos ayuda a encontrar el diseño correcto. Decinos si lo necesitás para el uso diario y si otros escritorios o apps lo resuelven de una forma que te guste (describí el comportamiento; no hace falta nombrar productos). No podemos prometer que vamos a hacer cada pedido, pero los leemos todos y ayudan a definir la hoja de ruta.

## Qué pasa después

Somos un equipo chico, así que esto son objetivos, no garantías:

- confirmamos los reportes que llegan por el formulario de contacto en unos **5 días hábiles**;
- clasificamos cada reporte: intentamos reproducirlo, pedimos más información si hace falta y le ponemos etiquetas y una prioridad;
- los arreglos salen en la próxima versión que pueda incluirlos, y las notas de versión en [GitHub Releases](https://github.com/lordbasex/LordOS/releases) los mencionan.

## Etiquetas

Cuando los issues sean públicos, estas son las etiquetas que usamos:

| Etiqueta | Significado |
|---|---|
| `bug` | algo no funciona como está documentado |
| `enhancement` | una función nueva o una mejora |
| `documentation` | documentación, páginas de ayuda o textos del sitio |
| `hardware` | depende de una computadora, placa o dispositivo en particular |
| `needs-info` | necesitamos más datos de quien lo reportó para seguir |
| `good first issue` | un buen punto de partida para quienes recién empiezan a contribuir |
| `duplicate` | ya se reportó en otro lado (enlazamos el original) |
| `wontfix` | decidimos no cambiarlo, y explicamos por qué |

¿Querés arreglarlo vos? Mirá [Cómo contribuir](/legal/contributing/).
