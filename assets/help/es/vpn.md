---
title: VPN
section: system
order: 61
keywords: vpn, wireguard, openvpn, ipsec, ikev2, pptp, openconnect, anyconnect, fortinet
---
# VPN

En [Ajustes → Red](settings:network), la fila **VPN**:

- **Importar** una configuración: **WireGuard** (`.conf`), **OpenVPN** (`.ovpn`, con sus
  certificados al lado), **IPsec/IKEv2**, **PPTP**, y opcionalmente **OpenConnect** (AnyConnect)
  y **Fortinet**, que se activan con un botón (se instalan solos).
- **Conectar** y **desconectar** con el interruptor. Se ve el estado, la dirección y el tráfico.
- **Túnel completo o parcial:** según la configuración, va todo el tráfico por la VPN o solo el de
  ciertas redes. La ruta normal del equipo nunca se toca: al desconectar todo vuelve a como estaba.

Importar o borrar una VPN necesita ser administrador; conectar y desconectar, no. Las claves y
contraseñas quedan guardadas solo para root. **PPTP es inseguro**: está para redes viejas y se
marca como tal.

## Fortinet con inicio de sesión único

Si tu organización te hace iniciar sesión en su propia página (inicio de sesión único, muchas veces
con una app de autenticación), elegí **Autenticación → Inicio de sesión único** al agregar la VPN
Fortinet: sin usuario ni contraseña. Al conectar, LordOS abre la página de inicio de sesión de la
organización en tu navegador y muestra "Esperando que inicies sesión en la página de la
organización…". Iniciá sesión ahí; la VPN se conecta cuando terminás. Tenés 5 minutos; **Cancelar**
lo detiene y **Abrir página de inicio de sesión** la vuelve a abrir.

## "No se pudo verificar el certificado de este servidor"

LordOS muestra la huella del certificado, para quién y quién lo emitió. Muchas veces el servidor
manda una cadena de certificados incompleta, que otros sistemas completan solos. Compará la huella
con la que te da tu organización y elegí **Confiar** (un administrador): LordOS la recuerda para esa
VPN y conecta. Si después el servidor muestra otro certificado, LordOS avisa que cambió y vuelve a
preguntar.

Para cambiar una VPN guardada, apagala y elegí **⋯ → Editar…** en su fila. El botón del tacho la borra.

## Fortinet: más servidores, certificado de cliente

- **Más servidores:** agregá los otros servidores de la organización con **+ Agregar servidor**
  (`servidor` o `servidor:puerto`). LordOS los prueba en orden y usa el primero que responde; la
  fila de la VPN dice cuál mientras está conectada.
- **Certificado de cliente:** si tu organización te dio un certificado, elegí **Certificado de
  cliente → Elegir…** y seleccioná el certificado y su clave privada (archivos PEM; también sirve un
  solo archivo con los dos). Escribí la contraseña de la clave si tiene. Los archivos PKCS#12
  (`.p12`, `.pfx`) todavía no se admiten: exportalos antes como PEM.
- **Descripción:** una nota que se ve debajo del nombre de la VPN.
