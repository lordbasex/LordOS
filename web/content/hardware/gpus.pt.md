<!-- RASCUNHO: não publicado. Aguarda a aprovação do mantenedor (gpus.meta.json). -->

Em quais placas de vídeo o LordOS funciona, quem verificou e como. O suporte a NVIDIA é **experimental e verificado
pela comunidade**: não temos todas as placas, então os seus relatórios de compatibilidade são a forma de verificar um
modelo.

## Estado

| Estado | O que significa |
|---|---|
| ✓ **Verificada** | A equipe do LordOS testou no próprio hardware com esta linha de versões. |
| ✓ **Verificada pela comunidade** | Pelo menos dois relatórios de compatibilidade independentes concordam, e a equipe os revisou. |
| ◐ **Experimental** | Um relatório ou nenhum: pode funcionar, ninguém confirmou ainda. Toda placa NVIDIA começa aqui. |
| ✗ **Não funciona** | Relatada como quebrada, com uma issue aberta. |

## Placas testadas

| Placa | Driver do kernel | O que foi testado | Estado | Verificado por |
|---|---|---|---|---|
| AMD Radeon RX 580 (Polaris, Ellesmere) | `amdgpu`, Mesa `radeonsi` | Área de trabalho composta na GPU, dois monitores DisplayPort, Vulkan (RADV) | ✓ Verificada | Equipe do LordOS |
| Intel UHD Graphics 630 (Comet Lake) | `i915` com GVT-g | Compartilhada com Máquinas como GPU virtual (GVT-g), ao lado da RX 580 que desenha a área de trabalho | ✓ Verificada | Equipe do LordOS |
| NVIDIA GeForce RTX 3060 (GA106) | `nvidia` (o driver proprietário) | A área de trabalho funciona, composta na CPU: no LordOS 0.8.8.9 a partida da GPU não obteve um contexto GL deste driver | ◐ Experimental | Um relatório da comunidade ([#171](https://github.com/lordbasex/LordOS/issues/171)) |
| NVIDIA GeForce RTX 5070 Ti 16 GB (GB203) | `nvidia` 620.73.04 (os módulos de kernel abertos da NVIDIA) | A área de trabalho funciona, fluida, sem cortes nem falhas; o relatório não diz se foi composta na GPU ou na CPU. As bibliotecas do CUDA estavam instaladas e o llama.cpp rodou nela | ◐ Experimental | Um relatório da comunidade ([#235](https://github.com/lordbasex/LordOS/issues/235)) |
| Intel HD Graphics 520 (Skylake GT2) | `i915`, Mesa `iris` | A área de trabalho funciona, fluida, sem falhas, composta na GPU | ◐ Experimental | Um relatório da comunidade ([#278](https://github.com/lordbasex/LordOS/issues/278)) |
| Intel UHD Graphics (Tiger Lake-H GT1) | `i915`, Mesa `iris` | Desenha a área de trabalho, composta na GPU, num notebook com uma segunda GPU NVIDIA; o relatório diz que funciona com problemas que não nomeia | ◐ Experimental | Um relatório da comunidade ([#274](https://github.com/lordbasex/LordOS/issues/274)) |
| NVIDIA GeForce GTX 1070 (GP104) | `nvidia` 580.196.11 (o driver proprietário) | A área de trabalho funciona, fluida, sem falhas, composta na GPU com o OpenGL ES da NVIDIA; reprodução de vídeo | ◐ Experimental | Um relatório da comunidade ([#276](https://github.com/lordbasex/LordOS/issues/276)) |
| NVIDIA GeForce GTX 1650 Mobile / Max-Q (TU117M) | `nvidia` 550.163.01 (o driver proprietário, dos pacotes do Debian) | A segunda GPU de um notebook híbrido, ao lado da placa Intel que desenha a área de trabalho. O Monitor → GPU não mostrou os números dela: ainda não encontra a NVML onde os pacotes do Debian a colocam | ◐ Experimental | Um relatório da comunidade ([#274](https://github.com/lordbasex/LordOS/issues/274), [#273](https://github.com/lordbasex/LordOS/issues/273)) |

Mais gráficos Intel e AMD rodam o LordOS nos
[relatórios de hardware](https://github.com/lordbasex/LordOS/blob/main/docs/HARDWARE-REPORT.md) da comunidade; esta
página lista as placas verificadas especificamente em gráficos.

## NVIDIA: o que funciona hoje

- **O driver livre é o padrão.** Qualquer placa NVIDIA mostra a área de trabalho com o driver `nouveau` do Mesa.
  Ajustes → Placas de vídeo a marca como *Experimental — ainda não verificada no modelo da sua placa*.
- **O driver da própria NVIDIA é opcional**: Loja → Para este computador o oferece para uma placa NVIDIA enquanto o
  driver livre está em uso.
- **A área de trabalho no driver da NVIDIA é experimental.** Um relato da comunidade a mostra composta na GPU, fluida
  (uma GTX 1070, Pascal, driver 580.196.11, [#276](https://github.com/lordbasex/LordOS/issues/276)); um anterior, no
  LordOS 0.8.8.9, era composto na CPU porque a partida da GPU não obteve um contexto GL (uma RTX 3060,
  [#171](https://github.com/lordbasex/LordOS/issues/171)); e no tópico do #276 outra pessoa viu janelas de alguns apps
  não aparecerem depois de instalar o driver (uma GTX 960 e uma RTX 3070 Ti com 550.163.01). Na placa que desenha a sua
  sessão, Ajustes → Placas de vídeo mostra como a área de trabalho é composta de fato.
- **IA e computação (CUDA)** está planejado; o LordOS ainda não oferece.

## Envie um relatório

1. No LordOS, abra **Ajustes → Placas de vídeo** e escolha **Enviar um relatório de compatibilidade**.
2. Leia o relatório inteiro. Ele tem só dados técnicos: as placas, seus drivers, qual desenha a área de trabalho, como
   a tela é composta, as versões do Mesa e da NVIDIA, os drivers de Vulkan e de vídeo instalados e o Secure Boot.
   Nunca nomes, caminhos da sua pasta pessoal, números de série nem endereços.
3. Escolha **Abrir o formulário no GitHub**, conte o que funcionou e o que não, e envie. Sem navegador, execute
   `lordos-issue --gpu` no Terminal e cole a saída num
   [novo relatório gráfico](https://github.com/lordbasex/LordOS/issues/new?template=gpu-report.yml).

## Como os relatórios são revisados

- A equipe lê cada relatório e confere se os dados batem (os ids PCI, o driver e sua versão, qual placa desenha a área
  de trabalho), e pergunta na issue quando falta algo.
- Um modelo passa a **Verificada pela comunidade** quando dois relatórios independentes do mesmo modelo e família de
  driver concordam; um relatório de que algo está quebrado abre um bug e marca o modelo como **Não funciona** até ser
  corrigido.
- Chegam a esta página só a placa, seu driver e o resultado, com um link para a issue pública. Nada mais é copiado.
