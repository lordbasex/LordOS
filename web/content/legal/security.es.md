Nos tomamos en serio la seguridad de LordOS y agradecemos a cualquiera que nos ayude a encontrar y corregir problemas. Esta página explica cómo reportar una vulnerabilidad y qué podés esperar de nosotros.

## Cómo reportar una vulnerabilidad

Escribinos por **[nuestro formulario de contacto](https://www.iperfex.com/contacto/?lang=es)**, con "Security" en el asunto. Cuando el repositorio sea público, también vas a poder usar el reporte privado de vulnerabilidades de GitHub. Por favor, no abras un issue público, no lo publiques en redes sociales ni des detalles hasta que salga una corrección.

Incluí, en la medida de lo posible:

- la versión de LordOS (la ves en la app Acerca de LordOS, por ejemplo "0.8.6 · Beta") y si es la ISO, la imagen para Raspberry Pi o el paquete para Debian;
- el componente afectado (por ejemplo el compositor, el aislamiento de apps, el firewall por app, el escritorio remoto, la bóveda de contraseñas, la Tienda, el script de instalación para Debian o el sitio);
- los pasos para reproducirlo, una prueba de concepto si la tenés, y el impacto que ves;
- cómo te gustaría que te mencionemos, si querés que lo hagamos.

Podés escribir en inglés, español o portugués.

## Alcance

**Dentro del alcance:**

- el código propio de LordOS: `lord-server` (el servidor gráfico y compositor Wayland), `lord-sessiond` y la pantalla de inicio de sesión, el aislamiento de apps y el intermediario de permisos, `lord-netperm`, `lord-vault`, los procesos auxiliares de correo, documentos y multimedia, el motor de paquetes, las apps nativas y el SDK;
- los archivos de cada versión y sus firmas, y el script de instalación para Debian (`lordos.org/install.sh`);
- la configuración del sistema que trae LordOS (por ejemplo, los servicios que activa);
- el sitio lordos.org.

**Fuera del alcance** (reportalos a sus propios proyectos):

- vulnerabilidades de paquetes de Debian, del kernel Linux, de Mesa, de los navegadores o de otro software de terceros que también afecten a un Debian común ([equipo de seguridad de Debian](https://www.debian.org/security/)); avisanos también si LordOS lo empeora;
- apps `.lpk` de terceros (reportalas a su autor; avisanos si alguna escapa del aislamiento);
- ataques que requieran acceso físico a una computadora desbloqueada, ingeniería social, o denegación de servicio por saturación;
- reportes de escáneres automáticos sin un impacto demostrado, y encabezados de buenas prácticas faltantes sin consecuencias de seguridad.

## Puerto seguro

Si actuás de buena fe y seguís esta política, vamos a considerar autorizada tu investigación, no vamos a iniciar acciones legales en tu contra y lo vamos a dejar claro si otra persona lo hace. Actuar de buena fe significa que:

- probás solo en tus propias computadoras, máquinas virtuales y cuentas;
- no accedés, cambiás ni borrás datos de otras personas, y frenás apenas llegás a alguno;
- no degradás el sitio ni los sistemas de otros usuarios;
- nos das un tiempo razonable para corregir el problema antes de hacerlo público.

## Qué podés esperar

Somos un equipo chico, así que estos son nuestros objetivos, no garantías:

| Paso | Objetivo |
|---|---|
| Confirmamos que recibimos tu reporte | dentro de 3 días hábiles |
| Confirmamos el problema y te decimos qué tan grave nos parece | dentro de 10 días hábiles |
| Corregimos los problemas críticos y de gravedad alta | lo antes posible, normalmente dentro de 30 días |
| Divulgación pública coordinada | cuando sale la corrección, o a los 90 días, lo que ocurra primero, salvo que acordemos otra cosa |

Te vamos a mantener al tanto, te vamos a mencionar en las notas de la versión si querés, y vamos a publicar un aviso por cada vulnerabilidad corregida. Por ahora no ofrecemos recompensas pagas por errores.

## Verificar las versiones

Cada archivo de una versión (la ISO, la imagen para Raspberry Pi y el `.deb`) figura con su SHA-256 en `SHA256SUMS`, y `SHA256SUMS` está firmado con la clave de versiones de LordOS (Ed25519). El script de instalación para Debian se niega a instalar si la firma falta o no es válida.

- Clave pública: [lordos-release.pub.pem](/lordos-release.pub.pem)
- Huella (SHA-256 de la clave pública en DER):
  `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

Para verificar una descarga a mano:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

Si la huella que ves no coincide con la de arriba, no instales el archivo y avisanos. Cómo firmamos las versiones, las apps y los paquetes, y cómo protegemos las claves, está explicado en [Firma de código](/legal/code-signing/).

## security.txt

Esta política también está publicada en un formato legible por máquinas en [/.well-known/security.txt](/.well-known/security.txt) (RFC 9116).
