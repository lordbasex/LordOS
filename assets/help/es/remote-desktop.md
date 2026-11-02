---
title: Escritorio remoto
section: system
order: 63
keywords: remoto, acceso remoto, navegador, compartir pantalla, lan
---
# Escritorio remoto

Desde otra computadora de tu red podés usar Axia DE con un navegador web, sin instalar nada.

1. Prendelo en [Ajustes → Compartir](settings:sharing) → «Permitir el acceso desde la red».
   Viene **apagado**.
2. En la otra computadora abrí la dirección que muestra Ajustes (`https://<IP>:5673`).
3. El navegador avisa que el certificado es propio: compará la **huella** con la que muestra Ajustes.
4. Entrá con tu usuario y tu contraseña.

- Con **tu mismo usuario** ves y manejás la misma sesión que está en el monitor.
- Con **otro usuario**, esa persona tiene su propio escritorio, al mismo tiempo.
- En Ajustes elegís quién puede entrar (todos o solo el grupo `lordos-remote`) y ves quién está
  conectado, con un botón para desconectarlo.

Funciona antes de que nadie entre en el equipo. Los intentos fallidos esperan cada vez más. Las
ventanas protegidas se ven en negro.
