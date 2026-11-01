LordOS lo hace un equipo chico, y tratamos de trabajar como lo hacen los proyectos de código abierto serios. Esta página explica, con honestidad, con qué normas internacionales están alineados nuestros procesos, qué implementa cada una y qué falta todavía.

**LordOS e iPERFEX no tienen ninguna certificación ISO.** "Alineado con" significa que seguimos las prácticas principales de una norma y publicamos la evidencia de abajo; no significa que un auditor nos haya evaluado o certificado, ni que cumplamos todos los requisitos.

## Divulgación y gestión de vulnerabilidades: ISO/IEC 29147 e ISO/IEC 30111

**Qué hacemos:** una [Política de seguridad](/legal/security/) pública con un canal de reporte, alcance, puerto seguro y tiempos de respuesta objetivo (acuse de recibo en 3 días hábiles, evaluación en 10, arreglos de problemas graves en general dentro de 30, divulgación coordinada a los 90 días); un [security.txt](/.well-known/security.txt) legible por máquinas; créditos y avisos para las vulnerabilidades corregidas; versiones firmadas para que se pueda confiar en los arreglos.

**Qué falta:** un proceso interno documentado de gestión (clasificación, puntaje de gravedad, pasos de arreglo y publicación); una página pública de avisos de seguridad; identificadores CVE, a través de una CVE Numbering Authority o de GitHub; el reporte privado de vulnerabilidades de GitHub, que todavía no está activado.

## Cumplimiento de licencias de código abierto: ISO/IEC 5230 (OpenChain)

**Qué hacemos:** un inventario de cada componente de terceros incluido y su licencia en [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md); avisos de licencia en la página de [Licencias](/legal/licenses/), en el repositorio y en el archivo de copyright de cada paquete de Debian; una lista de materiales de software (SBOM) en SPDX 2.3, generada por `tools/release/make-sbom.sh` y adjunta a cada versión a partir de la 0.8.0 beta; el proceso está documentado en [docs/COMPLIANCE.md](https://github.com/lordbasex/LordOS/blob/main/docs/COMPLIANCE.md). Las contribuciones entran bajo la misma licencia MIT con una firma de conformidad (mirá [Contribuir](/legal/contributing/)).

**Qué falta:** una política de código abierto escrita; una persona designada como responsable del cumplimiento; capacitación para quienes contribuyen; una autocertificación OpenChain.

## Garantía de seguridad del código abierto: ISO/IEC 18974 (OpenChain)

**Qué hacemos:** la base Debian recibe las actualizaciones de seguridad de Debian, que se instalan desde la Tienda o con apt; el SBOM enumera los componentes y versiones de cada versión; los archivos de cada versión y los paquetes `.lpk` están firmados; la política de seguridad define cómo recibimos y gestionamos los reportes.

**Qué falta:** un seguimiento sistemático de vulnerabilidades conocidas (CVE) contra el SBOM de cada versión; un proceso documentado para eso; una autocertificación de conformidad OpenChain.

## Prácticas de seguridad de la información (al estilo ISO/IEC 27001)

Aplicamos algunas prácticas que describe ISO/IEC 27001, solo donde son reales:

- **Mínimo privilegio:** las apps corren en su propio sandbox (Landlock, seccomp, cgroups, sin capacidades), y los procesos auxiliares que manejan datos no confiables (correo, documentos, multimedia, la bóveda de contraseñas) se quedan sin privilegios ni acceso a la red, como documenta [docs/PRIVACY.md](https://github.com/lordbasex/LordOS/blob/main/docs/PRIVACY.md). Las credenciales de publicación del sitio solo pueden escribir en un bucket de almacenamiento y refrescar una distribución de la CDN.
- **Versiones firmadas:** cada archivo de una versión figura en un `SHA256SUMS` firmado (mirá [Firma de código](/legal/code-signing/)).
- **Custodia de claves:** la clave de firma de versiones se guarda solo en la máquina del mantenedor, nunca en el repositorio ni en una máquina de compilación, con una copia de respaldo fuera de línea.

**Qué falta:** un sistema de gestión de seguridad de la información (SGSI), una evaluación de riesgos formal, controles documentados y auditorías internas o externas. **No** tenemos la certificación ISO/IEC 27001.

## Accesibilidad del sitio: WCAG 2.2 AA

**Qué hacemos:** el sitio apunta al nivel AA de WCAG 2.2: acceso con teclado con un contorno de foco visible, un enlace para saltar al contenido, movimiento reducido, temas claro y oscuro, regiones y textos alternativos, como describe la [Declaración de accesibilidad](/legal/accessibility/).

**Qué falta:** una auditoría completa contra WCAG 2.2 AA, con pruebas con lectores de pantalla; del lado de la distribución, un lector de pantalla, la lupa y un tema de alto contraste.

## security.txt: RFC 9116

**Qué hacemos:** `/.well-known/security.txt` con los campos Contact, Expires, Preferred-Languages, Canonical y Policy.

**Qué falta:** una firma OpenPGP del archivo.

## Consultas

Si ves una brecha que no listamos, o querés ayudar a cerrar una, escribinos a través de [nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es).
