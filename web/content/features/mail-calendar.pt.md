## Em poucas palavras

O LordOS traz quatro apps nativos para suas informações pessoais: **E-mail**, **Calendário**, **Contatos** e **Lembretes**. São apps comuns do LordOS, desenhados pelo mesmo motor que o resto do desktop. O incomum é onde acontece o trabalho arriscado: todo protocolo de rede e todo leitor de dados vindos de fora roda num único processo ajudante trancado, o **`lord-maild`**, que nem tem permissão para abrir uma conexão de rede sozinho.

[[shot:0]]

O e-mail é o caminho mais comum para atacar um computador. Uma mensagem é um documento de um desconhecido: HTML, imagens, anexos, convites. Por isso o LordOS a trata como um pacote aberto numa sala lacrada. Se algo nele for malicioso e tomar conta da sala, ainda assim não consegue chamar ninguém lá fora, não consegue executar nada e não alcança seus arquivos.

[[diagram:mail]]

## O que você ganha

- **E-mail:** três painéis, caixa de entrada unificada, conversas, sinalizadores e seis categorias coloridas que outros clientes também enxergam, assinaturas, rascunhos salvos no servidor, "enviar mais tarde", anexos abertos com seus apps padrão e notificações de e-mails novos. Escreva em texto formatado, markdown ou texto simples.
- **Calendário:** visões de dia, semana e mês, vários calendários coloridos, locais ou sincronizados por CalDAV, eventos que se repetem e alarmes.
- **Convites** que chegam por e-mail: Aceitar, Talvez ou Recusar. O evento entra no Calendário e a resposta vai para o organizador.
- **Lembretes:** listas, prazos, prioridades, sinalizadores e as visões Hoje / Agendados / Todos / Sinalizados / Concluídos, sincronizados por CalDAV.
- **Contatos:** locais ou sincronizados por CardDAV, com grupos e fotos. O E-mail completa os endereços a partir deles.
- **Os alarmes tocam com os apps fechados,** porque o ajudante continua rodando durante a sessão.
- **Qualquer conta IMAP e SMTP** funciona. Para alguns provedores muito usados, as configurações do servidor são preenchidas a partir de uma tabela embutida, sem consultar nada online.

[[shot:5]]

[[shot:6]]

## Como funciona

1. **O ajudante já começa trancado.** O `lord-server` inicia o `lord-maild` a partir de um caminho fixo, sem argumentos e com um ambiente mínimo. Antes de ler qualquer coisa da rede, ele carrega as autoridades certificadoras do sistema e se tranca com o mesmo sandbox de ajudante dos outros decodificadores do LordOS: escrita só na sua pasta de cache, sem `socket`, `connect`, `bind`, `execve` nem `fork`, só threads, sem capacidades, com cgroup próprio.
2. **Ele pede cada conexão.** Para alcançar um servidor, ele manda ao `lord-server` um pedido: host e porta. O `lord-server` confere esse destino no seu arquivo de contas e só então conecta e devolve o socket já conectado. Os destinos permitidos são exatamente os servidores IMAP, SMTP e de calendário/contatos das suas contas, mais outros hosts do mesmo domínio na porta 443 quando um servidor de calendário redireciona durante a descoberta.
3. **O TLS é conferido direito.** Toda conexão é verificada contra o repositório de certificados do sistema e o nome do servidor. Um certificado que não passa na verificação nunca é aceito em silêncio: o E-mail mostra o problema e a impressão digital, e só se você apertar *Confiar neste certificado* aquele certificado exato fica lembrado para aquela conta.
4. **O HTML é traduzido, não executado.** O LordOS não tem motor web, e o E-mail não precisa de um. O sanitizador próprio do LordOS transforma o HTML de uma mensagem numa pequena árvore de blocos, tabelas, trechos de texto, imagens e links, e o E-mail a desenha com views nativas. Scripts, estilos, frames, formulários, SVG, mídia, tratadores de eventos e truques de "preheader" escondido são descartados.

## O que você vê e decide

- **As imagens remotas ficam bloqueadas** até você apertar *Carregar imagens* para aquela mensagem ou *Sempre deste remetente*. Mesmo assim, o `lord-server` libera só aqueles hosts de imagem, nas portas 80 e 443, por dez minutos, e recusa qualquer endereço que não seja público (rede local, loopback, metadados de nuvem e similares), conferido no endereço com que ele de fato se conecta. Pixels de rastreamento são contados e nunca exibidos.
- **Links enganosos são marcados** em vermelho com um aviso quando o texto visível parece um endereço mas aponta para outro lugar, ou usa truques como um endereço IP, `user@` ou caracteres parecidos. Clicar mostra o destino real. O E-mail nunca abre um navegador sozinho; você pode copiar o endereço.
- **Os próprios apps são isolados.** Desde a 0.8.2, E-mail, Calendário, Contatos e Lembretes rodam cada um como seu próprio processo isolado, como um app `.lpk` instalado, e chegam ao e-mail e ao chaveiro só por pedidos ao broker permitidos para esses quatro apps e nenhum outro. Desde a 0.8.5, as conexões com os seus servidores de e-mail também saem de um processo próprio, então têm sua própria linha e suas próprias regras em Monitor → Rede.
- **As senhas nunca tocam um arquivo.** O E-mail guarda as senhas das contas no chaveiro do sistema (veja Senhas) e as entrega ao `lord-maild` só em memória. O arquivo de contas guarda servidores, nomes de usuário, assinaturas e impressões digitais de certificados confiáveis, e o código se recusa a gravar uma senha nele.

[[shot:1]]

[[shot:2]]

## As perguntas do leitor cético

**E se uma mensagem explorar um bug no leitor?** É exatamente o que o projeto supõe que um dia vai acontecer. O leitor roda no `lord-maild`, que não consegue criar sockets, não consegue executar programas e só pode escrever no próprio cache. Os leitores são testados a fundo: 30.953 verificações no `mail-test`, incluindo fuzzing por mutação de MIME, conjuntos de caracteres, o sanitizador de HTML, iCalendar, vCard e XML, além de suítes separadas para a camada de rede e para o conector.

**Um ajudante sequestrado poderia falar com a minha rede local?** Só com os servidores do seu arquivo de contas. As liberações de imagens se limitam a endereços públicos, e a verificação é feita no endereço realmente conectado, então um truque de DNS não consegue redirecioná-lo para o seu roteador.

**O desktop trava enquanto o e-mail sincroniza?** Não. Os pedidos são assíncronos; dentro do ajudante, o trabalho de rede roda numa thread por conta. Medido numa VM de teste: uma pasta com 10.000 mensagens sincroniza em cerca de 2 s e abre a partir do cache local em cerca de 20 ms.

**O login por OAuth é suportado?** Está preparado, mas desativado. Os grandes provedores só emitem tokens de login para aplicativos registrados com eles, então é preciso um client ID registrado. Até lá, essas contas usam senhas de app.

[[shot:3]]

[[shot:4]]

## Limites honestos

- **Nenhuma conta de provedor real foi testada.** A VM de teste usou servidores IMAP, SMTP e CalDAV/CardDAV locais, com uma autoridade certificadora de teste. As configurações de provedores embutidas vêm dos valores publicados pelos próprios provedores.
- **Ainda não:** editar uma única ocorrência de um evento que se repete, criar calendários ou catálogos de endereços no servidor, conjuntos de caracteres CJK multibyte, e-mail criptografado ou assinado (S/MIME e OpenPGP) e IMAP QRESYNC.
- **O login por OAuth** fica desativado até um client ID ser registrado.

## Leia o código

- [server/src/mail/maild.c](https://github.com/lordbasex/LordOS/blob/main/server/src/mail/maild.c): o ajudante `lord-maild` e seu sandbox
- [server/src/mail/mailc.c](https://github.com/lordbasex/LordOS/blob/main/server/src/mail/mailc.c): o conector no `lord-server` que decide quais destinos são permitidos
- [server/src/mail/html.c](https://github.com/lordbasex/LordOS/blob/main/server/src/mail/html.c): o sanitizador de HTML
- [server/src/ui/js_mail.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_mail.c): `lord.mail`, a API da sessão
- [apps/mail](https://github.com/lordbasex/LordOS/tree/main/apps/mail), [apps/calendar](https://github.com/lordbasex/LordOS/tree/main/apps/calendar), [apps/contacts](https://github.com/lordbasex/LordOS/tree/main/apps/contacts), [apps/reminders](https://github.com/lordbasex/LordOS/tree/main/apps/reminders)
- [server/tests/mail_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/mail_test.c): os testes dos leitores e do sanitizador
- [docs/MAIL.md](https://github.com/lordbasex/LordOS/blob/main/docs/MAIL.md)
