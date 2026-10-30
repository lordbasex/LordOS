## Em poucas palavras

Um app instalável do LordOS (um pacote `.lpk`) não recebe as chaves da sua casa. Ele roda num quarto só dele, com a porta trancada por fora. Quando precisa de algo, pede ao sistema por uma única portinhola, o **broker**. O sistema confere o que você decidiu e, se você ainda não decidiu, pergunta com um aviso desenhado pelo sistema, não pelo app.

[[shot:0]]

Três ideias fazem a maior parte do trabalho:

- **Isolamento.** Cada app `.lpk` é um processo próprio, trancado com recursos do kernel Linux antes que qualquer código dele rode.
- **Mediação.** Tudo o que toca nas suas coisas (arquivos, câmera, microfone, área de transferência, senhas) passa pelo broker no `lord-server`, que confere a permissão a cada pedido.
- **Sua decisão, guardada onde o app não alcança.** As decisões ficam em `~/.config/lordos/permisos.json`, que os apps isolados não conseguem ver nem escrever.

[[diagram:permission]]

## Quais apps são isolados, exatamente?

Esta é a primeira pergunta que um leitor atento deve fazer, então aqui vai a resposta precisa:

| Tipo de app | Processo próprio | Sandbox (Landlock + seccomp) | cgroup por app | Broker e permissões | Firewall por app |
|---|---|---|---|---|---|
| Apps `.lpk` instalados | sim | sim | sim, com limites de memória e CPU | sim | sim |
| Senhas, Mail, Calendário, Contatos e Lembretes (apps incluídos com processo próprio; os quatro últimos desde a 0.8.2) | sim | sim | sim | sim | sim |
| Outros apps incluídos (Arquivos, Ajustes, Terminal…) | não, rodam dentro do `lord-server` | não | não (contam como "LordOS") | o uso de câmera, microfone e tela é conferido em C | sim, como "LordOS" |
| Apps Linux abertos pelo lançador de apps (apt, a Loja) | sim, um processo comum | **não** por padrão; com "sem acesso aos seus arquivos", namespaces próprios e Landlock | sim, `app-lordos-<id>.scope`, sem limites | **não** | **sim**, mais as restrições que você definir (câmera, microfone, USB, Bluetooth, arquivos, notificações) |
| Programas iniciados pelo Terminal | sim | não | dividem o lugar do Terminal na árvore | não | como "Terminal", com suas próprias regras (desde a 0.8.5; antes, como "LordOS") |

Ou seja: **os apps Linux não ficam em sandbox por padrão.** Eles rodam como o seu usuário, exatamente como em qualquer sistema Debian, e podem ler sua pasta pessoal. O que o LordOS acrescenta para eles é a atribuição (o Monitor sabe qual app fez cada conexão), o firewall por app aplicado pelo kernel e as restrições que você escolher para um app: sem câmera, sem microfone, sem USB, sem sockets Bluetooth, sem notificações e "sem acesso aos seus arquivos", que a partir da próxima abertura inicia o app em namespaces próprios, com uma pasta pessoal vazia que só tem as pastas dele. O Flatpak não é usado nem integrado pelo LordOS, e não foi testado.

Os apps incluídos fazem parte do código do próprio sistema, revisado junto com ele; a maioria não recebe o isolamento por processo que os pacotes de terceiros recebem. As exceções são Senhas, porque guarda seus segredos, e, desde a 0.8.2, Mail, Calendário, Contatos e Lembretes, porque leem o que chega da internet: cada um roda como seu próprio processo isolado e chega ao e-mail e ao seu chaveiro só por pedidos ao broker permitidos para aquele app e nenhum outro.

## Como o sandbox funciona

O sandbox é código próprio do LordOS (`server/src/sec/sandbox.c`), não bubblewrap, Flatpak nem firejail. Ele usa recursos do kernel diretamente e é aplicado **depois** que o app se conecta ao compositor e ao broker, e **antes** de qualquer linha do código do app ser carregada.

[[diagram:sandbox]]

- **Landlock** limita os arquivos que o processo pode tocar: acesso só de leitura ao próprio pacote, ao runtime compartilhado de apps, às fontes e às bibliotecas; leitura e escrita apenas em `~/.local/share/lordos/appdata/<id>`. Nada mais da sua pasta pessoal, e nada de `/dev`.
- **seccomp-bpf** é uma lista de chamadas de sistema permitidas, aplicada a todas as threads. Chamadas como `ptrace`, `mount`, `bpf`, `execve`, `keyctl` e carregamento de módulos simplesmente não estão nela. O app também não consegue abrir novos sockets Unix, então não alcança sozinho o barramento da sessão, o serviço de impressão nem o socket Wayland.
- **Sem privilégios.** `no_new_privs` é ativado e todas as capacidades são removidas.
- **Um cgroup próprio**, `app-lordos-lpk-<id>.scope`, com limite suave de memória de 192 MB, limite rígido de 512 MB e no máximo um núcleo de CPU. O escopo é criado pelo root (`lord-sessiond`) e pertence a ele, então o app não consegue se mudar para outro lugar.
- **Rede só com permissão.** Sem a permissão `network`, o seccomp recusa criar sockets de internet e o Landlock recusa connect e bind TCP. Com ela (declarada no manifesto **e** permitida por você), eles são liberados, e o firewall por app continua valendo.

**São usados namespaces?** Não para os apps `.lpk`. O `lord-server` roda sem root, e criar um namespace de rede exige privilégios que ele não tem. O projeto usa Landlock, seccomp e cgroups no lugar disso, e deixa o app no namespace de rede da máquina, sob as regras acima. O único lugar em que o LordOS usa namespaces é um app Linux restrito com "sem acesso aos seus arquivos": ele inicia em namespaces próprios de usuário, montagem e PID, sem privilégios, montados por código próprio do LordOS no lançador, então não alcança os sockets da sessão.

O projeto mantém um app de teste propositalmente malicioso (`tests/lpk-evil`). Ele não declara nenhuma permissão e tenta de tudo ao abrir: ler `~/.ssh/id_rsa` e `settings.json`, escrever fora da própria pasta, ler a área de transferência escondido, abrir `/etc/shadow`, abrir um socket, executar `/bin/sh`, `ptrace`, abrir a placa de vídeo. Todas as tentativas são bloqueadas. A suíte de testes do sandbox passa em 14 de 14 verificações para apps, 14 de 14 com rede e 17 de 17 para os ajudantes.

[[shot:1]]

## O que você vê e decide

- **O aviso.** Na primeira vez que um app pede algo, o sistema mostra: *"App" quer usar o microfone — Permitir / Não permitir*. A resposta fica salva para aquele app.
- **Ajustes → Privacidade** lista cada permissão com os apps que a pediram e um controle Permitir / Perguntar / Negar.
- **Indicadores.** Um ponto verde na barra de menus enquanto a câmera está em uso, e um laranja para o microfone. Ao tocar nele, aparece qual app está usando.

Permissões que um app pode declarar: arquivos, área de transferência, notificações, rede, câmera, microfone, tela, escuta de portas, USB, Bluetooth e apps. Um app não consegue uma permissão que não declarou.

[[shot:4]]

## As perguntas do leitor cético

**Dá para contornar o aviso?** Não pelo app isolado. O aviso é desenhado pela sessão, fora da janela do app. A resposta chega diretamente ao `lord-server`, e o broker lê a decisão salva, em C, a cada pedido. O arquivo de decisões fica fora do que o Landlock deixa o app ver, e o `lord.fs` se recusa a escrevê-lo. Um app até poderia desenhar um diálogo falso dentro da própria janela, mas clicar nele não muda nada. A brecha, sendo honesto: um app Linux, que por padrão não tem sandbox e roda como você, poderia editar o `permisos.json` como qualquer outro arquivo da sua pasta pessoal, a menos que esteja restrito com "sem acesso aos seus arquivos".

**Existe "Permitir uma vez" ou "Permitir sempre"?** Hoje o aviso oferece Permitir e Não permitir, e a decisão fica lembrada até você mudá-la em Ajustes. Ainda não há "permitir uma vez" para as permissões dos apps.

**O app recebe só o arquivo que eu escolho?** Sim. Para abrir ou salvar, um app isolado chama o diálogo de arquivos do sistema. O `lord-server` o desenha (o "powerbox"), abre o arquivo que você escolheu com as suas permissões e passa ao app **só aquele descritor de arquivo**. O app nunca vê a pasta nem o resto do disco. Nenhuma permissão extra é necessária, porque escolher o arquivo já é o consentimento.

[[shot:2]]

**Como a câmera e o microfone são mediados?** O app nunca abre o dispositivo.
- *Microfone:* o `lord-server` abre o fluxo de captura do PipeWire e escreve o áudio num anel compartilhado; o app recebe uma cópia **somente leitura** dele.
- *Câmera:* o `lord-server` abre `/dev/videoN` e o entrega só ao ajudante de mídia em sandbox, o `lord-media`, que decodifica os quadros num anel compartilhado e selado. O app recebe um mapeamento somente leitura e precisa confirmar cada quadro; o `lord-server` valida cada confirmação (geração, posição, ritmo). Fotos e vídeos vão para um arquivo que você escolhe no diálogo de salvar do sistema.

[[diagram:camera]]

**Dá para revogar permissões com o app aberto?** Sim, para a câmera e o microfone: colocar qualquer um deles em Perguntar ou Negar em Ajustes corta na hora. No caso da câmera, o `lord-server` encerra o processo `lord-media` do app (testado: some em até 0,5 s); no do microfone, fecha o fluxo do PipeWire. Verificações como a da área de transferência valem a partir do próximo pedido. A permissão `network` é decidida quando o app inicia, então uma mudança vale no próximo lançamento; para tirar da rede um app que já está rodando na hora, use o botão do firewall no Monitor.

[[shot:3]]

**Posso garantir que um app nunca receba a câmera, peça o que pedir?** Sim: restrinja-o em Ajustes → Privacidade → Rede dos apps. Uma restrição do firewall vence as permissões: o app é recusado sem aviso mesmo que você o tenha permitido antes, e restringir um app em execução tira dele a câmera e o microfone na hora. Para os apps isolados, o broker aplica as oito restrições (câmera, microfone, tela, USB, arquivos, área de transferência, Bluetooth, notificações).

**E os apps que precisam de mais privilégios?** Não há como um app `.lpk` obter root ou capacidades extras. Tudo no LordOS que precisa de root passa por uma lista fechada de ações validadas (`server/src/admin.c`), e o JavaScript não consegue executar comandos arbitrários. Os apps Linux se comportam como em qualquer sistema Debian: se um programa precisa de `sudo`, você mesmo o executa no Terminal.

## O LordOS verifica o próprio código antes de executá-lo

Desde a 0.8.2, cada pacote de código que o LordOS executa como próprio (os temas com o shell e cada app nativo, a tela de login, o hospedeiro dos apps isolados e os apps do sistema que rodam como processo próprio) está listado com seu SHA-256 num manifesto assinado com a chave de versões do LordOS (Ed25519). O `lord-server` confere a assinatura com a chave pública compilada nele, e o hash de cada pacote a partir da mesma leitura com que o carrega, então não há um intervalo para trocar o arquivo. Um pacote que não confere não é executado: o desktop passa para outro tema e avisa, ou mostra uma tela de recuperação que explica como repará-lo, nunca uma tela em branco. Sobre o LordOS mostra "Verificado", ou "Compilação de desenvolvimento" para uma árvore de desenvolvimento sem assinatura. Isso protege contra alterações feitas por processos que não são root e contra modificação offline; o root ainda pode substituir o próprio `lord-server`, o que uma fase posterior (fs-verity, imagens A/B) deve cobrir.

## Confira no seu próprio computador (desde a 0.8.5)

Desde a 0.8.5, **Informações do sistema** (Sobre o LordOS → Relatório do sistema…) tem uma aba Verificações que testa estas afirmações no computador à sua frente: o servidor gráfico sem root, o Landlock, o firewall por app, o código verificado, a área de trabalho remota fechada quando está desligada, os escopos dos apps nas mãos do sistema. Cada verificação diz se o mecanismo só foi encontrado *configurado* ou se foi de fato *testado*. **Testar** inicia o Teste do isolamento, um app de teste oculto que começa exatamente como qualquer app isolado e tenta, dentro do mesmo sandbox, o que não deve conseguir fazer: os seus arquivos e os do sistema, sockets, `ptrace`, um namespace de usuário, `mount`, `exec`, sair do seu escopo. Ele também tenta o que o sandbox dos decodificadores deve recusar, e pede ao broker coisas que ele deve negar. O teste nunca recebe uma permissão nem acesso à rede, e nada é enviado a lugar nenhum.

[[shot:6]]

## Os ajudantes que leem dados não confiáveis

Arquivos vindos de fora são a porta de entrada clássica. O LordOS os abre em pequenos processos separados, trancados com ainda mais rigor que os apps: `lord-doc` (PDFs, arquivos compactados, fontes), `lord-media` (vídeo, música, fotos, câmera), `lord-maild` (e-mail, calendários, contatos) e `lord-vault` (senhas). Eles não têm rede, não conseguem criar sockets, não conseguem executar programas e só podem criar threads, não processos. Eles não abrem caminhos: o `lord-server` abre os arquivos e os passa como descritores. Se o seccomp não puder ser aplicado, o ajudante encerra em vez de rodar sem confinamento. Um PDF malicioso pode derrubar o `lord-doc`, mas não o desktop. Desde a 0.8.5 o `lord-fetch` (as listas de bloqueio do firewall) e o `lord-tls` (o HTTPS da Loja) são trancados da mesma forma e também não conseguem criar sockets: só recebem uma conexão que o `lord-server` ou a Loja abriu para eles. O `lord-bench` do LordBench (desde a 0.8.6) roda seus testes sob o mesmo confinamento.

## Janelas protegidas

Uma janela pode ser marcada como protegida. A de Senhas é. Os pixels dela saem pretos nas capturas de tela do LordOS, nas gravações de tela, nas miniaturas e na área de trabalho remota, enquanto o monitor de verdade a mostra normalmente. Os apps Linux não conseguem capturar a tela pelo LordOS: o compositor não oferece a eles nenhum protocolo de captura de tela, os apps X11 (pelo Xwayland desde a 0.8.3) só veem uns aos outros, nunca os apps Wayland, a tela do LordOS ou Senhas, e um app restrito também tem o dispositivo de framebuffer negado.

[[shot:5]]

## Limites honestos

- **Os apps Linux não têm sandbox por padrão.** Para eles valem as regras de rede, a atribuição e as restrições que você definir; as restrições não cobrem a área de transferência, o Bluetooth através do BlueZ no barramento do sistema, o barramento do sistema em geral nem programas iniciados fora do lançador de apps.
- **O uso de câmera e microfone por apps Linux ainda não aparece** nos indicadores. A lista de dispositivos do Monitor está planejada.
- **O broker ainda não cobre tudo.** Permissões como Bluetooth ou abrir outros apps podem ser declaradas, mas não há chamada do broker para elas, então um app isolado simplesmente não consegue fazer essas coisas.
- **A falta de um recurso do kernel enfraquece o sandbox dos apps sem alarde.** Se faltar o Landlock ou um controlador de cgroup, os apps iniciam mesmo assim, com um aviso no log. Já os ajudantes se recusam a rodar sem seccomp.

## Leia o código

- [server/src/sec/sandbox.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sandbox.c): Landlock, seccomp, capacidades, regras de rede
- [server/src/sec/brokersrv.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokersrv.c): o broker, lado do servidor
- [server/src/sec/perms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/perms.c): permissões por app
- [server/src/sec/brokercam.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokercam.c): a câmera para apps isolados
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c): as restrições do firewall no broker
- [server/src/sec/cgroup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/cgroup.c) e [server/src/remote/cgsup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/cgsup.c): escopos dos apps que pertencem ao root
- [server/src/app/appclient.c](https://github.com/lordbasex/LordOS/blob/main/server/src/app/appclient.c): o processo do app isolado
- [sdk/privacy.js](https://github.com/lordbasex/LordOS/blob/main/sdk/privacy.js) e [sdk/powerbox.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/powerbox.jsx): avisos e o diálogo de arquivos
- [server/tests/sandbox_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/sandbox_test.c) e [tests/lpk-evil](https://github.com/lordbasex/LordOS/tree/main/tests/lpk-evil): os testes
- [docs/PRIVACY.md](https://github.com/lordbasex/LordOS/blob/main/docs/PRIVACY.md)
