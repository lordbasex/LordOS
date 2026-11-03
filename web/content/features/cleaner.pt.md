**Novo na 0.8.7.**

## A versão curta

O app Limpeza mostra o que ocupa o seu disco e ajuda a remover o que pode sair com segurança. A regra é simples: **analisar, revisar, planejar, confirmar, executar, verificar**. Uma análise só lê. Nada muda até você ver o que vai acontecer e dizer sim.

[[shot:0]]

Pense em esvaziar uma casa com cuidado. Alguém percorre tudo e faz uma lista, sem tocar em nada. Você revisa a lista. Só então algo sai da casa, e cada item é conferido de novo na porta.

[[diagram:flow]]

## O que ele faz

- **Analisar** executa Limpeza, Armazenamento e Duplicados um após o outro e depois mostra um painel do que foi realmente encontrado: um cartão para cada coisa encontrada, com os tamanhos reais e os arquivos reais por trás.
- **Limpeza** lista caches, downloads de ferramentas de desenvolvimento, caches de navegadores, temporários antigos, instaladores antigos e a Lixeira, cada um com seu motivo, seu risco e o que vai acontecer com ele. Durante uma análise você vê as três regras que mais encontraram até o momento, com seus tamanhos.
- **Projetos de código** na sua pasta pessoal: compilados de Node, Rust, Maven, JavaScript, Python, Gradle e Dart. O nome de uma pasta não prova nada, então cada uma é provada pelos arquivos ao lado ou dentro dela, e as dependências Node só são oferecidas quando o projeto inteiro está há 60 dias sem alterações. Você os escolhe um a um e eles vão para a Lixeira.
- **Vídeos grandes** de 100 MiB ou mais na Área de Trabalho e nas suas gravações de tela também são escolhidos um a um.
- **Arquivos grandes** filtra por tamanho, idade e tipo.
- **Armazenamento** mostra as maiores pastas como bolhas, um mapa, uma lista de pastas e um resumo por tipo. Cada pasta é desenhada com o que ela é, para que vídeos, músicas e código se distingam de relance. No mapa, faixas em cada bloco mostram o que é refeito sozinho e o que convém rever antes, e uma linha dá o espaço livre, as duas quantidades e o que não foi verificado.
- **Duplicados** encontra cópias exatas nas pastas que você escolher e, se você pedir, imagens parecidas. Nada é selecionado por você.
- **Atividade** registra o que foi executado: quantidades e tamanhos, nunca um caminho.

Uma coisa fica no disco com nomes dentro: o mapa de armazenamento é guardado entre sessões, em `~/.cache/lordos/cleaner-map.txt`, legível só pelo seu usuário. Ele tem nomes e tamanhos de pastas, e fica neste computador. Vem ativado: desligue em Ajustes ou remova com Apagar em Atividade. Um mapa guardado não tem nada sobre o que se possa agir; para limpar, analise de novo.

[[shot:1]]

## O que ele não faz

As regras de limpeza nunca alcançam cookies, sessões, histórico ou senhas, nem `~/.cache` inteiro. O app não diz proteger seu computador nem procura malware. Não tem "liberar memória" nem teste de velocidade: nenhum dos dois poderia ser feito com honestidade. Os tamanhos mostrados antes de agir são estimativas, e ele avisa, porque links físicos, arquivos abertos, compressão e snapshots mudam o que é realmente liberado.

## Como ele se mantém seguro

A análise roda no `lord-clean`, um auxiliar isolado que só pode ler. Os planos são montados no `lord-server`, não no app, e o app só guarda identificadores de candidatos, nunca a autoridade para tocar um caminho. Tudo é conferido de novo logo antes de agir, inclusive os arquivos que provaram uma pasta de projeto.

Arquivos de cache regeneráveis sob pastas fixas podem ser apagados de verdade, como o seu próprio usuário, e só por este caminho: o `lord.fs` continua sem uma exclusão geral. Todo o resto, inclusive as pastas de projetos, vai para a Lixeira, de onde pode ser restaurado. Três ações administrativas fechadas limpam os arquivos do sistema, cada uma confirmada; num sistema de teste descartável elas fizeram o que suas regras dizem.

## O que seu movimento custa

O app vem em três visuais, segue o modo claro e escuro e funciona em quatro idiomas: inglês, espanhol, português e francês. Seu layout se inverte para idiomas da direita para a esquerda, verificado por enquanto com um idioma de teste com texto em inglês. Seu movimento só roda com a janela em primeiro plano e para por completo com Reduzir movimento. As partes que se movem ficam em quadros separados para que a tela seja repintada o mínimo possível; os números estão em `docs/CLEANER-TESTS.md`. Foram medidos numa máquina virtual e num Raspberry Pi 5 com seu monitor, onde a sessão usou 4,5–7,2 % de um núcleo com a tela inicial parada, 12,2–18,0 % analisando e 0,1 % com o app fechado.

[[shot:2]]

Cada página de resultados tem Recomeçar e Analisar de novo, as listas têm Selecionar tudo e Duplicados tem Selecionar cópias, que deixa uma cópia de cada grupo.

Um limite: numa sessão live uma análise não encontra nada, por causa de como o sistema live organiza seus arquivos. Um sistema instalado não é afetado.
