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
| NVIDIA GeForce RTX 3060 (GA106) | `nvidia` (o driver proprietário) | A área de trabalho funciona, composta na CPU: o LordOS ainda não compõe sobre o driver proprietário da NVIDIA | ◐ Experimental | Um relatório da comunidade ([#171](https://github.com/lordbasex/LordOS/issues/171)) |
| NVIDIA GeForce RTX 5070 Ti 16 GB (GB203) | `nvidia` 620.73.04 (os módulos de kernel abertos da NVIDIA) | A área de trabalho funciona, fluida, sem cortes nem falhas; o relatório não diz se foi composta na GPU ou na CPU. As bibliotecas do CUDA estavam instaladas e o llama.cpp rodou nela | ◐ Experimental | Um relatório da comunidade ([#235](https://github.com/lordbasex/LordOS/issues/235)) |

Mais gráficos Intel e AMD rodam o LordOS nos
[relatórios de hardware](https://github.com/lordbasex/LordOS/blob/main/docs/HARDWARE-REPORT.md) da comunidade; esta
página lista as placas verificadas especificamente em gráficos.

## NVIDIA: o que funciona hoje

- **Qualquer placa NVIDIA** mostra a área de trabalho: com o driver `nouveau` do Mesa, ou com o driver da NVIDIA e a
  tela composta na CPU. Ajustes → Placas de vídeo a marca como *Experimental — ainda não verificada no modelo da sua
  placa*.
- **IA e computação (CUDA)** e **a área de trabalho no driver da NVIDIA** estão planejadas, ainda não disponíveis:
  instalar o driver da NVIDIA pelos Ajustes e compor a área de trabalho sobre ele vêm depois que suas ações de sistema
  e o design de sincronização explícita forem aprovados.

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
