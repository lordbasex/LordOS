## A versão curta

**O Lord Workspaces é novo na 0.8.9, publicada em 18 de novembro de 2026.**

O LordOS não tem um desktop: tem três, e você escolhe. O **Lord Flow** tem uma barra de menus em cima e um Dock embaixo. O **Lord Classic** põe tudo numa barra de tarefas com menu iniciar. O **Lord Workspaces** organiza seu trabalho por atividades, com um layout para cada tela e widgets na área de trabalho.

Os três rodam sobre o mesmo servidor gráfico, o mesmo motor de interface e os mesmos apps. Arquivos, Ajustes, Mail, o Terminal e seus apps Linux são iguais em todos. O que muda é o caminho até eles, e você pode mudar quando quiser, ao vivo, sem sair da sessão.

## Lord Flow

**Desde a 0.8.0, a primeira versão.** Uma barra de menus em cima e um Dock embaixo, com vidro suave e cantos arredondados. É o desktop que muita gente já sabe usar, para ninguém ter que aprender outro jeito de trabalhar só para experimentar um sistema novo.

- **O Dock** se amplia sob o ponteiro, mostra quais apps estão abertos e dá a cada app as suas próprias ações. Também pode ficar à esquerda ou à direita.
- **A barra de menus** tem os menus do app, os itens de status e o relógio; a **Central de controle** reúne rede, som, brilho, Não perturbe e o resto.
- **O lançador de apps** mostra todos os apps, e a Busca encontra apps, ajustes e arquivos.
- **Encaixe janelas** arrastando-as para as bordas: meia tela, um quarto ou a tela inteira (no Lord Flow desde a 0.8.9).
- **Sempre no topo** no menu da janela deixa uma janela por cima das outras enquanto você digita em outra (em desenvolvimento, em todos os desktops do LordOS; os apps X11 também podem pedir isso).
- **Desktops** com uma Visão geral, desde a 0.8.7, e um seletor de apps no ❖Tab.

**Para quem é:** quem quer começar a trabalhar sem aprender nada novo.

[[shot:0]]

## Lord Classic

**Desde a 0.8.3.** Tudo numa barra embaixo: o botão iniciar, a busca, os apps fixados e abertos, uma bandeja com configurações rápidas e o relógio.

- **O menu iniciar** tem um campo de busca que busca como a busca do sistema (apps, ajustes, arquivos, notas, a calculadora, Ajuda), uma grade de apps fixados que você mesmo organiza, os apps usados recentemente, todos os apps de A a Z ou por categoria (Jogos, Gráficos, Internet, Escritório…, cada uma com sua contagem e aberta ali mesmo, também pelo teclado; em desenvolvimento) e o botão de desligar.
- **Configurações rápidas** junto ao relógio: rede, Bluetooth, Não perturbe, Luz noturna, modo escuro, volume.
- **Uma barra de tarefas em cada tela**, ligada por padrão; o menu iniciar e os painéis abrem na tela onde você clicou.
- **A barra de tarefas** pode ficar centralizada ou alinhada ao início, em qualquer lado da tela.
- **Menus dentro da janela**: sem barra de menus em cima, os menus de um app ficam numa faixa sob o título.

**Para quem é:** quem vem de um desktop com barra de tarefas e quer se sentir em casa.

[[shot:1]]

## Lord Workspaces

**Novo na 0.8.9.** Seu trabalho não é uma pilha de janelas: é um conjunto de coisas que você está fazendo. O Lord Workspaces dá a cada uma o seu lugar.

- **Atividades.** Até nove, cada uma com nome, ícone, cor, apps fixados e layout de janelas por tela. Pílulas em cima trocam entre elas, e deixar o ponteiro sobre uma mostra o que ela tem antes de você ir. Grupos dentro de uma atividade chegam a 36 lugares de trabalho.
- **A Context Spine**, um trilho fino à esquerda com a coroa, suas atividades, apps, arquivos e o Control Atlas. Em telas menores ela vira só ícones.
- **Seis layouts por tela**: Livre, Uma por vez, Duas metades, Principal e lateral, Três colunas e Grade. Cada monitor tem o seu, e as janelas minimizadas esperam no cabeçalho do monitor de onde vieram.
- **O Control Atlas**: claro ou escuro, alto contraste, o estilo de efeitos (Opaco, Translúcido ou Cristal), a cor de destaque, o fundo, o layout e suas atividades, num só lugar.
- **Widgets**: 21, em seis grupos, sobre uma grade de cada tela. Todos leem de um mesmo canal compartilhado, e só enquanto você os vê: um widget atrás de uma janela, numa tela desligada ou com a sessão bloqueada não lê nada.
- **Visual próprio**: um destaque rosa e os fundos de Cyberpunk City HD por padrão, uma apresentação de fundos opcional e um ponteiro preenchido com a sua cor de destaque.

**Para quem é:** quem trabalha com muitas janelas, ou com dois ou três monitores.

[[shot:2]]

## Como trocar

Vá em **Ajustes → Aparência → Estilo da área de trabalho** e escolha outro. A troca é imediata, sem sair da sessão: suas janelas abertas voltam, cada uma no seu desktop, e seus ajustes ficam.

Algumas coisas são de cada desktop. O Lord Workspaces começa com um destaque rosa e um fundo próprio, enquanto o Lord Flow e o Lord Classic mantêm o azul e o Lumo Celestial; quando você mesmo escolhe um destaque ou um fundo, ele fica seu em todos os desktops. Os widgets da área de trabalho funcionam nos três desde a 0.8.9.

## O primeiro início

Numa instalação nova, a primeira vez que uma conta nova entra, o LordOS pergunta qual desktop você quer:

1. **Escolha seu desktop.** Três cartões iguais, cada um com uma imagem real. Eles aparecem numa ordem aleatória a cada vez, nenhum vem escolhido e nenhum vem marcado como recomendado.
2. **Veja ao vivo.** O desktop escolhido é aplicado de verdade. Uma caixinha deixa você trocar entre os três, ao vivo, até um parecer certo.
3. **Mude quando quiser.** Uma imagem de Ajustes → Aparência mostra onde mudar depois.

Ele nunca aparece no sistema live, na tela de login nem para quem já usou o LordOS.

## Por que podemos ter vários

No LordOS, um desktop é um tema sobre um motor. O servidor gráfico, o compositor e o motor de interface são do LordOS, escritos para ele, e um tema é o desktop inteiro por cima: as barras, os painéis, os menus e cada moldura de janela, escritos em React e desenhados por esse motor. Por isso o Lord Workspaces não precisou de um fork do sistema, de um toolkit novo nem de uma cadeia de plugins, e por isso ele troca ao vivo.

Em outros desktops, como GNOME ou KDE, um desktop que funcione como o Lord Workspaces significaria empilhar extensões: uma para janelas lado a lado, outra para áreas de trabalho por monitor, uma terceira para uma barra de tarefas em cada tela, cada uma com seu próprio calendário de versões. Aqui é o próprio desktop.

É por isso também que virão mais. Se você tem uma ideia para o próximo, [conte para a gente no GitHub](https://github.com/lordbasex/LordOS/issues/new?template=feature_request.yml).

## Limites honestos

- **O Lord Workspaces** foi verificado com testes de cena em três telas e capturas sem tela. Em monitores reais e pela GPU ele só rodou no computador do mantenedor, com versões de teste, e ainda não foi verificado num idioma da direita para a esquerda.
- **Ainda não está no Lord Workspaces:** miniaturas ao vivo das atividades ocultas (a Visão geral mostra molduras com o ícone de cada app), vidro fosco de verdade sobre apps Linux pela GPU, e uma atividade diferente em cada monitor.
- **A apresentação de fundos** foi verificada só em telas de teste, não em monitores reais nem pela GPU.
- **O seletor do primeiro início** tem testes automáticos nos três desktops, mas ainda não foi visto num primeiro início real.
- **Um tema que você mesmo faça** por enquanto só roda numa versão de desenvolvimento do LordOS: uma versão publicada só carrega código que está no manifesto assinado do LordOS.
