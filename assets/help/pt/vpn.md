---
title: VPN
section: system
order: 61
keywords: vpn, wireguard, openvpn, ipsec, ikev2, pptp, openconnect, anyconnect, fortinet
---
# VPN

Em [Ajustes → Rede](settings:network), a linha **VPN**:

- **Importar** uma configuração: **WireGuard** (`.conf`), **OpenVPN** (`.ovpn`, com os
  certificados ao lado), **IPsec/IKEv2**, **PPTP**, e opcionalmente **OpenConnect** (AnyConnect)
  e **Fortinet**, ativados com um botão (instalam-se sozinhos).
- **Conectar** e **desconectar** com o interruptor. Aparecem o estado, o endereço e o tráfego.
- **Túnel completo ou parcial:** conforme a configuração, todo o tráfego vai pela VPN ou só o de
  certas redes. A rota normal do computador nunca é alterada: ao desconectar, tudo volta a ser como
  era.

Importar ou apagar uma VPN exige ser administrador; conectar e desconectar, não. As chaves e senhas
ficam guardadas só para o root. **PPTP é inseguro**: existe para redes antigas e está marcado assim.

## Fortinet com login único

Se a sua organização faz você entrar pela própria página (login único, muitas vezes com um app
autenticador), escolha **Autenticação → Login único** ao adicionar a VPN Fortinet: sem usuário nem
senha. Ao conectar, o LordOS abre a página de login da organização no seu navegador e mostra
"Aguardando você fazer login na página da organização…". Faça login lá; a VPN conecta quando você
terminar. Você tem 5 minutos; **Cancelar** interrompe e **Abrir página de login** abre de novo.

## "Não foi possível verificar o certificado deste servidor"

O LordOS mostra a impressão digital do certificado, para quem e por quem foi emitido. Muitas vezes o
servidor envia uma cadeia de certificados incompleta, que outros sistemas completam sozinhos.
Compare a impressão digital com a que sua organização informa e escolha **Confiar** (um
administrador): o LordOS a guarda para essa VPN e conecta. Se depois o servidor mostrar outro
certificado, o LordOS avisa que mudou e pergunta de novo.

Para mudar uma VPN salva, desligue-a e escolha **⋯ → Editar…** na linha dela. O botão da lixeira a apaga.
