LordOS es software libre y las contribuciones son bienvenidas: código, documentación, traducciones, pruebas en hardware real y buenos reportes de errores. Esta página explica cómo participar y las pocas reglas que sigue toda contribución.

## Cómo participar

El repositorio es público en [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). Podés:

- probar LordOS y [reportar problemas](/legal/issues/) en [GitHub Issues](https://github.com/lordbasex/LordOS/issues);
- probarla en tu hardware y contarnos qué funciona y qué no;
- **seguir** (watch) el repositorio o darle una **estrella** para enterarte de las versiones.

## Fork y pull request

1. Hacé un **fork** del repositorio en GitHub y creá una rama para tu cambio.
2. Hacé tu cambio, con pruebas cuando tenga sentido, y corré las suites de pruebas (ver más abajo).
3. Hacé el commit con la firma de conformidad (`git commit -s`, explicado más abajo).
4. Abrí un **pull request contra `main`**, explicando qué cambia y por qué, y enlazando el issue que resuelve si hay uno.
5. Un mantenedor lo revisa. Puede que te pidamos cambios; cuando esté listo, se integra.

Para cambios grandes (una app nueva, un servicio del sistema nuevo, una dependencia nueva), abrí primero un issue para que acordemos el diseño antes de que le dediques tiempo.

## Licencia de las contribuciones

LordOS usa licencias **inbound = outbound** (la misma licencia de entrada y de salida): al contribuir, licenciás tu contribución bajo la misma **licencia MIT** que el resto del proyecto ([LICENSE](https://github.com/lordbasex/LordOS/blob/main/LICENSE)). El copyright de tu trabajo sigue siendo tuyo; no hay cesión de derechos ni un acuerdo de contribución aparte para firmar.

## Certificado de Origen del Desarrollador (sign-off)

Cada commit tiene que llevar la firma de conformidad, una línea al final del mensaje del commit:

```
Signed-off-by: Tu Nombre <vos@example.com>
```

`git commit -s` la agrega por vos, con el nombre y el correo de tu configuración de Git. Al agregarla, certificás el [Developer Certificate of Origin 1.1](https://developercertificate.org/) (Certificado de Origen del Desarrollador), que dice, en resumen, que:

- escribiste la contribución, o tenés derecho a enviarla bajo la licencia de código abierto del proyecto;
- si se basa en un trabajo anterior, ese trabajo está bajo una licencia de código abierto compatible que te permite enviarlo;
- entendés que la contribución y tu firma son públicas y quedan guardadas para siempre en el historial del proyecto.

Usá tu nombre real (o el nombre con el que se te conoce públicamente). Los pull requests con commits sin firma de conformidad no se pueden integrar.

## Reglas del proyecto

- **Solo en inglés.** Todo el código, los identificadores, los comentarios, los mensajes de commit y la documentación en Markdown se escriben en inglés. La excepción son los textos que ve el usuario: viven en los catálogos de idioma (`assets/locales/<idioma>/`) y pasan por la traducción, con el inglés como idioma predeterminado.
- **Las pruebas tienen que pasar.** Corré `make -C server test` para las suites autónomas (y `make -C server test-full` cuando toques los procesos auxiliares o el sandbox), y `node tools/i18n-check.mjs` cuando toques textos que ve el usuario. Sumá una prueba por cada error que arregles y cada función que agregues, cuando se pueda.
- **C prolijo.** C11 legible y modular, que compile sin advertencias con `-Wall -Wextra`. Respetá la política de dependencias de [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md).
- **No afirmes compatibilidad sin probar.** Si decís que algo funciona con un hardware o una app, es porque lo probaste.
- **Reglas de marca.** Seguí el [kit de marca](https://github.com/lordbasex/LordOS/blob/main/docs/LORDOS-BRAND.md) para el logo, los colores y la escritura "LordOS". Usá nombres genéricos para las funciones y dejá afuera de los textos públicos las marcas y los nombres de productos de otras empresas; `node tools/brand-check.mjs` lo verifica.
- **Nada de secretos.** Nunca subas claves privadas, contraseñas ni datos personales.

## Traducciones

LordOS viene en inglés, español y portugués. Otros idiomas se suman como **paquetes de idioma** armados a partir de los catálogos de traducción; mirá [docs/LANGUAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/LANGUAGES.md). Las correcciones a las traducciones que ya existen también son bienvenidas.

## Código de conducta

Todas las personas que participan en LordOS siguen nuestro [Código de conducta](/legal/code-of-conduct/). Los problemas de seguridad nunca se reportan en público: mirá la [Política de seguridad](/legal/security/).
