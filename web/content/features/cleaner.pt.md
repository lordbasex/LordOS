**Em desenvolvimento para 0.8.7. A versão mais recente, 0.8.6-beta, não inclui o app Limpeza.** Esta página descreve o trabalho no ramo de desenvolvimento; ainda pode mudar antes do lançamento.

## A versão curta

O app Limpeza mostra o que ocupa o seu disco e ajuda a remover o que pode sair com segurança. A regra é simples: **analisar, revisar, planejar, confirmar, executar, verificar**. Uma análise só lê. Nada muda até você ver o que vai acontecer e dizer sim.

[[shot:0]]

Pense em esvaziar uma casa com cuidado. Alguém percorre tudo e faz uma lista, sem tocar em nada. Você revisa a lista. Só então algo sai da casa, e cada item é conferido de novo na porta.

[[diagram:flow]]

## O que ele faz

- **Analisar** executa Limpeza, Armazenamento e Duplicados um após o outro e depois mostra um painel do que foi realmente encontrado: um cartão para cada coisa encontrada, com os tamanhos reais e os arquivos reais por trás.
- **Limpeza** lista caches, downloads de ferramentas de desenvolvimento, caches de navegadores, temporários antigos e a Lixeira, cada um com seu motivo, seu risco e o que vai acontecer com ele.
- **Arquivos grandes** filtra por tamanho, idade e tipo.
- **Armazenamento** mostra as maiores pastas como bolhas, um mapa, uma lista de pastas e um resumo por tipo. Cada pasta é desenhada com o que ela é, para que vídeos, músicas e código se distingam de relance.
- **Duplicados** encontra cópias exatas nas pastas que você escolher. Nada é selecionado por você.
- **Atividade** registra o que foi executado: quantidades e tamanhos, nunca um caminho.

[[shot:1]]

## O que ele não faz

As regras de limpeza nunca alcançam cookies, sessões, histórico ou senhas, nem `~/.cache` inteiro. O app não diz proteger seu computador nem procura malware. Não tem "liberar memória" nem teste de velocidade: nenhum dos dois poderia ser feito com honestidade. Os tamanhos mostrados antes de agir são estimativas, e ele avisa, porque links físicos, arquivos abertos, compressão e snapshots mudam o que é realmente liberado.

## Como ele se mantém seguro

A análise roda no `lord-clean`, um auxiliar isolado que só pode ler. Os planos são montados no `lord-server`, não no app, e o app só guarda identificadores de candidatos, nunca a autoridade para tocar um caminho. Tudo é conferido de novo logo antes de agir.

Arquivos de cache regeneráveis sob pastas fixas podem ser apagados de verdade, como o seu próprio usuário, e só por este caminho: o `lord.fs` continua sem uma exclusão geral. Todo o resto vai para a Lixeira, de onde pode ser restaurado. Três ações administrativas fechadas limpam os arquivos do sistema, cada uma confirmada; são a única parte que ainda não foi executada numa máquina de teste.

## O que seu movimento custa

O app vem em três visuais, segue o modo claro e escuro e funciona em inglês, espanhol e português. Seu layout se inverte para idiomas da direita para a esquerda, verificado por enquanto com um idioma de teste com texto em inglês. Seu movimento só roda com a janela em primeiro plano e para por completo com Reduzir movimento. As partes que se movem ficam em quadros separados para que a tela seja repintada o mínimo possível; os números estão em `docs/CLEANER-TESTS.md`. Foram medidos numa máquina virtual, não num Raspberry Pi.

[[shot:2]]
