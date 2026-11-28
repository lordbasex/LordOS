---
title: Placas de vídeo
app: settings
section: system
order: 68
keywords: placa de vídeo, gpu, nvidia, amd, intel, driver, cuda, compatibilidade, relatório, testar, nouveau, mesa, vulkan
---
# Placas de vídeo

[Ajustes → Placas de vídeo](settings:gpus) mostra cada placa de vídeo do computador, o driver que a controla e qual desenha a
área de trabalho. Uma placa que não desenha a área de trabalho pode ser compartilhada com Máquinas, se ela permitir.

## Placas NVIDIA: experimental

O suporte a NVIDIA é **experimental e verificado pela comunidade**. A equipe do LordOS testou gráficos Intel e AMD nos
próprios computadores, mas não todos os modelos da NVIDIA, então uma placa NVIDIA diz *"Experimental — ainda não
verificada no modelo da sua placa"*. O driver livre (nouveau, do Mesa) é o padrão. O driver da própria NVIDIA é
opcional: Loja → Para este computador o oferece para uma placa NVIDIA enquanto o driver livre está em uso.
Seus usos aparecem assim:

- **IA e computação (CUDA):** **planejado**; o LordOS ainda não oferece.
- **Área de trabalho:** **experimental**. Na placa que desenha a sua sessão, os Ajustes mostram como a área de trabalho
  é composta de fato (na placa ou pelo processador). Com o driver da NVIDIA, até agora um relato da comunidade mostra a
  área de trabalho composta na placa (uma GTX 1070, Pascal, driver 580); um anterior (uma RTX 3060 no LordOS 0.8.8.9)
  era composto pelo processador, e outra pessoa viu janelas de alguns apps não aparecerem depois de instalar o driver.
  Mais relatos nos dizem quais placas e drivers funcionam.

## Como ajudar a testar

1. Abra **Ajustes → Placas de vídeo**, escolha **Enviar um relatório de compatibilidade** e depois **Criar
   relatório**.
2. Leia o relatório inteiro: ele mostra as placas, seus drivers, qual desenha a área de trabalho, se a tela é composta
   na placa de vídeo, as versões do Mesa e da NVIDIA, os drivers de Vulkan e de vídeo instalados e o Secure Boot. Ele
   nunca inclui seu nome, o nome do computador, caminhos da sua pasta pessoal, números de série nem endereços de rede.
3. Escolha **Abrir o formulário no GitHub**. Seu navegador abre o formulário de relatórios do LordOS já preenchido; o
   relatório inteiro também fica na área de transferência. Conte o que funciona e o que não funciona (a área de
   trabalho, vídeo, jogos, suspensão) e envie. Nada é enviado até você fazer isso, e os relatórios no GitHub são
   públicos.

Sem navegador, abra o **Terminal** e execute `lordos-issue --gpu`: ele imprime o mesmo relatório, pronto para colar.

A equipe do LordOS lê cada relatório. Um modelo com bons relatórios passa de *experimental* a *verificado* na lista de
placas de vídeo testadas em lordos.org.
