## Em poucas palavras

O **LordOS Web Runtime** é um ambiente de execução nativo para aplicações web, escrito em C e feito sobre o WPE WebKit, que instala e executa apps web e PWAs como apps do LordOS, sem um navegador externo. Seu chat, seu e-mail ou seu quadro branco deixam de ser mais uma aba: ganham janela própria com a moldura do LordOS, ícone próprio no Dock com o contador de não lidas, notificações com o nome e o som do app, barra de menu e, se você quiser, continuam rodando em segundo plano atrás de um ícone ao lado do relógio.

Não é um cliente nativo de nenhum serviço. **O LordOS não muda os sites nem os PWAs deles; executa-os como os sites os servem.** A página na janela é a interface web do próprio serviço, sem mudanças, a mesma que um navegador mostra. O LordOS acrescenta a janela, a integração com o desktop e a segurança em volta. Onde falta ao WPE WebKit algo de que um desktop precisa (a área de transferência, arrastar e soltar, os menus Editar e do clique direito, claro e escuro), o `lord-web` acrescenta pequenos scripts próprios e documentados (os que cuidam da sua área de transferência e do que você solta falam com o LordOS só a partir de um mundo isolado que a página não vê), sem tocar no código nem no conteúdo do site.

[[shot:6]]

É novo no **LordOS 0.8.8.8**, lançado em 12 de novembro de 2026, a versão mais recente. Fica num pacote opcional, `lordos-web`: sem ele, o resto do LordOS funciona igual, e a Loja o instala para você com o seu primeiro app web.

[[figure:web-runtime]]

## O que o torna diferente

| | Um site numa janela do desktop | Um PWA instalado por um navegador | LordOS Web Runtime |
|---|---|---|---|
| O que executa a página | Uma visualização web simples numa janela | O motor e o perfil desse navegador | Um processo `lord-web` próprio por app e perfil, sobre o WPE WebKit do Debian |
| Contas | Normalmente uma | Os perfis desse navegador | Quantos perfis você quiser, cada um com seus cookies e dados |
| Sandbox do motor | Depende do invólucro | O desse navegador | O do WebKit (bubblewrap + seccomp), sempre ativo |
| Permissões | Depende do invólucro | Guardadas pelo navegador | Perguntadas uma vez por app e guardadas em Ajustes → Apps web |
| Firewall e limites | — | — | A linha própria do app no firewall, limites de memória e CPU |
| Desktop | Uma janela | Alguma integração | Ícone e contador no Dock, notificações com o som do app, barra de menu, menu Editar, menu do clique direito do LordOS, bandeja, claro e escuro |

A coluna do meio descreve como esses apps funcionam em geral, não um produto específico; o LordOS não muda como um navegador executa os próprios PWAs.

## Como obter apps web: a seção Apps web da Loja

A Loja tem uma seção **Apps web**, um banner e uma prateleira em Descobrir, e os apps web aparecem na busca e na categoria deles. Cada cartão diz "App web", para você distingui-los dos apps do LordOS e dos apps Linux.

[[shot:0]]

- **Um catálogo de 37 apps vem com o LordOS**: chat, e-mail e calendário, escritório, notas e tarefas, música e vídeo, gráficos. Cada um passou na verificação automática do LordOS em 11 de novembro de 2026 (carregou e deu seu nome). Para ser honesto sobre onde estamos: **por enquanto só um deles está marcado como testado no LordOS**, um app de chat que o mantenedor usa com duas contas. Os demais dizem "Ainda não verificado" até alguém testá-los, e a Loja avisa antes de você instalar um.
- **Qualquer site, pelo endereço.** Digite um endereço em *Adicionar a partir de um endereço* e aperte Verificar. O LordOS abre a página à parte, numa sessão descartável sem nenhum dos seus dados, lê o manifesto de app web e o ícone do site e mostra o endereço real. Você pode mudar o nome. **Nada é instalado até você apertar Instalar.** Só endereços seguros (`https`) são aceitos, e a verificação não se deixa levar de um site público para a sua própria rede.
- **O primeiro app web instala o motor.** Na primeira vez, a Loja diz que o motor web do LordOS é necessário uma única vez, mostra o download e o espaço em disco do pacote `lordos-web`, pede a senha de administrador, instala-o e depois instala o seu app, sem outro clique. A partir do segundo app não há etapa do motor.

[[shot:1]]

*Os nomes, logos e ícones de produtos e serviços do catálogo são marcas dos seus respectivos titulares e são usados só para identificar os serviços. O LordOS não é afiliado a eles nem tem seu endosso ou patrocínio; "testado no LordOS" só quer dizer que o projeto LordOS verificou que o site funciona no LordOS Web Runtime.*

[[shot:2]]

Cada app web tem sua página na Loja, como qualquer outro app: sempre atualizado (é a web), o dia em que o LordOS o testou, o espaço em disco, a memória (o valor típico medido, ou o real enquanto roda), quem o faz, o site verificado, o motor, as permissões que pode pedir, os perfis, o modo em segundo plano e as limitações conhecidas.

[[shot:3]]

[[shot:4]]

## Perfis: várias contas ao mesmo tempo

Um **perfil** é uma conta separada do mesmo app: a pessoal e a do trabalho, ou dois números do mesmo chat. Cada perfil tem janela própria, cookies próprios, dados salvos próprios e o próprio login; nada de um perfil pode ser lido por outro. Você pode abri-los ao mesmo tempo.

- Adicione, renomeie ou remova perfis em **Ajustes → Apps web**. Ao remover um, ele pergunta se os dados vão junto.
- Cada perfil pode ter uma **cor**. Com mais de um perfil, a moldura da janela mostra uma faixa fina dessa cor, o título e o alternador de janelas dizem "App — Perfil", e o ponto do perfil aparece sobre o ícone nas janelas, nas notificações e em Ajustes.
- O Dock abre o primeiro perfil; os outros estão no menu do clique direito do ícone ("Nova janela: Trabalho"). Você também pode dar a cada perfil **o próprio ícone no Dock**.

[[shot:7]]

## Privacidade e segurança

- **Um processo por app e perfil.** Cada perfil de cada app roda no próprio processo `lord-web`, e só o `lord-server` o inicia, de um caminho fixo, com um app e um perfil que conferiu antes. Um travamento afeta só aquela janela.
- **O sandbox do WebKit nunca é desligado.** Os processos da página e de rede rodam dentro do sandbox próprio do WebKit (bubblewrap e seccomp). O `lord-web` se recusa a iniciar se alguém tentar desligá-lo.
- **Linha própria no firewall e limites.** Cada app web tem identidade própria no firewall do LordOS e linha própria no Monitor, então você pode permiti-lo ou bloqueá-lo como qualquer app. Os perfis compartilham limites de memória e CPU na medida do seu computador (memória até três quartos do total, entre 1 e 16 GB, e metade dos núcleos, no mínimo dois).
- **Permissões perguntadas uma vez por app.** Notificações, câmera, microfone, compartilhamento de tela e localização são perguntados na primeira vez que o app precisa deles, e a sua resposta fica em **Ajustes → Apps web**, onde você pode mudá-la (Permitir, Perguntar, Negar). Só as páginas próprias do app os recebem: uma página de login de outro site, ou um quadro de outra origem, é negada e nunca perguntada.
- **Nenhuma API do LordOS chega à página.** Uma página não recebe `lord.*`, nem o broker, nem scripts com privilégios.
- **Copiar e colar.** O que uma página copia chega à sua área de transferência só a partir das páginas próprias do app, e só logo depois da sua própria tecla, clique ou toque na janela. O LordOS lê a sua área de transferência para a página só quando você cola.
- **Arquivos que você solta ou cola.** Uma página recebe só arquivos dos seus lugares (sua pasta pessoal, seus discos, `/tmp`), nunca de uma pasta oculta nem de um arquivo oculto (assim `~/.ssh` ou `~/.config` nunca são enviados), e nunca através de um link posto no caminho; no máximo 20 arquivos de até 64 MB cada. Um arrasto ou uma cópia vinda de um app contido (um app isolado do LordOS ou um Flatpak) dá a um app web só texto, nunca arquivos.

## Editar no dia a dia, como num navegador

Faltam ao WPE WebKit várias coisas que um navegador faz por você, então o `lord-web` as acrescenta:

- **Arrastar para selecionar texto**, clique duplo para uma palavra, triplo para um parágrafo, Shift+clique para estender.
- **Copiar e colar nos dois sentidos**, texto em qualquer idioma, **imagens** (uma imagem que você copiou cola num chat; uma página pode copiar uma imagem para a sua área de transferência) e **arquivos** (copie em Arquivos e cole no app web).
- **Arrastar arquivos** de Arquivos para a página, como num navegador: páginas de chat que mostram uma área para soltar quando chega um arquivo também funcionam.
- **O menu Editar** na barra de menu: Desfazer, Refazer, Recortar, Copiar, Colar e Selecionar tudo. Ajustes… no menu do app abre os ajustes daquele app, e Ajuda abre o site no seu navegador.
- **O menu do clique direito próprio do LordOS**: Recortar, Copiar, Colar, Selecionar tudo, Copiar endereço do link, Abrir link no navegador e Copiar imagem, conforme o que estiver sob o ponteiro. Uma página com menu próprio (um quadro branco, um editor) o mantém.
- Recarregue com Ctrl+R ou F5, volte e avance com Alt+Esquerda/Direita ou os botões laterais do mouse, e dê zoom com Ctrl+mais, menos e 0. Links para outros sites abrem no seu navegador; os downloads vão para a sua pasta Downloads com uma notificação.

[[shot:8]]

[[shot:9]]

## Claro e escuro

Os apps web seguem o modo claro ou escuro do desktop, ao vivo: mude o Modo escuro na Central de controle e um app web aberto muda na hora. Uma página que pergunta ao navegador `matchMedia('(prefers-color-scheme: dark)')`, como fazem muitos apps com um tema "Sistema", recebe a resposta do LordOS. No modo escuro também não há um clarão branco enquanto a página carrega.

**O limite honesto:** o WPE WebKit 2.48 não dá ao LordOS nenhuma forma de definir o esquema de cores da página, então uma página que escolhe as cores só com CSS (regras `@media (prefers-color-scheme: dark)`) fica clara até um WebKit mais novo permitir. Esses sites costumam ter um ajuste de tema próprio: escolha Escuro lá. A página do app na Loja diz isso quando se sabe.

[[shot:12]]

## Desempenho

- **Janelas ocultas pausam.** Uma janela minimizada, em outro desktop, fechada para o segundo plano ou totalmente coberta por outra janela avisa à página que está oculta: a página para de desenhar, as animações pausam e os temporizadores rodam no máximo uma vez por segundo, enquanto rede, áudio e notificações continuam. Medido na nossa máquina virtual de desenvolvimento com uma página de teste pesada: **de 115 % de uma CPU para 0,4 %** minimizada, e o compositor de 5 % para 0,3 %. Mostrada de novo, desenha na hora.
- **Pressão de memória.** O WebKit descarta os caches que pode reconstruir quando uma página cresce (cerca de um terço da sua memória, até 1 GB), e também imagens decodificadas e código compilado passada a metade (cerca de 1,5 GB), verificado a cada 30 segundos; um modelo de cache mais leve não mantém nenhum processo web ocioso.
- **Limites na medida do seu computador**, como acima, compartilhados pelos perfis do app.
- **O Monitor mostra a memória real** de cada app web, contada uma vez por memória proporcional (PSS) entre seus processos. Uma conta do app de chat testado usou cerca de 810 MB na primeira medição.

## Ajustes → Apps web

Cada app web instalado tem sua página: o site e de onde veio, os perfis (abrir, renomear, cor, remover, com os dados que cada um guarda em disco), as permissões, *Continuar rodando em segundo plano* (fechar a janela o mantém rodando, então as notificações continuam chegando), *Mostrar na barra de menus* (um ícone ao lado do relógio com Abrir e Sair), *Mostrar não lidas no Dock*, *Cada perfil com seu próprio ícone no Dock*, o som das notificações (o do LordOS, o do site ou nenhum), o ícone (o do próprio site ou o mosaico com letra do LordOS) e Desinstalar.

[[shot:10]]

[[shot:11]]

## Limitações conhecidas

- **Ainda não testados:** 36 dos 37 apps do catálogo ainda não estão marcados como testados no LordOS. Carregaram, mas alguns recursos podem não funcionar.
- **Sem notificações push com o app fechado.** O WPE não tem serviço de push no Linux, então as notificações chegam só enquanto o app roda: deixe-o em segundo plano para isso.
- **Sem vídeo nem música com DRM** (o Widevine não está disponível para o WPE no Linux), então os catálogos protegidos dos serviços de música e vídeo não tocam.
- **As chamadas** dependem do WebRTC do WPE WebKit e do serviço; podem ainda não funcionar e não foram testadas.
- **O modo escuro só por CSS** fica claro, como acima.
- **Desfazer** funciona só em páginas com histórico próprio (um editor rico); um campo de texto simples não consegue desfazer.
- **O contador de não lidas** vem do título da página ("(3) …"), a única fonte que o WPE dá; a foto de quem manda a notificação não aparece.
- Alguns serviços passam para outro endereço depois do login; a página funciona, mas os links dela podem então abrir no seu navegador.
- *Adicionar a partir de um endereço* confere para onde um site leva a cada passo, mas um site ainda poderia responder um endereço a essa verificação e outro ao WebKit (DNS rebinding). Ela roda numa sessão descartável vazia, sem nenhum dos seus dados, e só lê um nome e um ícone.

## Leia o código

- [server/src/web](https://github.com/lordbasex/LordOS/tree/main/server/src/web): o `lord-web`, o LordOS Web Runtime
- [server/src/ui/js_webapps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_webapps.c): o gerenciador no `lord-server`
- [assets/webapps/catalog.json](https://github.com/lordbasex/LordOS/blob/main/assets/webapps/catalog.json): o catálogo
- [apps/store/webapps.js](https://github.com/lordbasex/LordOS/blob/main/apps/store/webapps.js) e [apps/settings/WebApps.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/settings/WebApps.jsx): a Loja e Ajustes
- [docs/WEBAPPS.md](https://github.com/lordbasex/LordOS/blob/main/docs/WEBAPPS.md): todo o design, com cada verificação

*WhatsApp, Telegram, Discord e os demais nomes de produtos, logos e ícones exibidos são marcas dos seus respectivos titulares. São usados só para identificar os serviços; o LordOS não é afiliado a eles nem tem seu endosso ou patrocínio. Veja a [política de marca](/legal/trademark/).*
