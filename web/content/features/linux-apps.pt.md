## Em poucas palavras

O LordOS tem um desktop próprio, mas por baixo continua sendo Debian. As dezenas de milhares de programas do repositório do Debian estão disponíveis, e os apps gráficos feitos com GTK ou Qt, além de navegadores como Chromium e Firefox, abrem como janelas do LordOS, com a mesma moldura, o mesmo Dock e o mesmo modo claro ou escuro dos apps nativos.

[[shot:0]]

O segredo é uma língua em comum. Os apps Linux falam **Wayland**, um protocolo para dizer "esta é a minha janela, estes são os meus pixels, me mande o mouse e o teclado". O servidor gráfico do LordOS também fala essa língua. Então um app que não sabe nada do LordOS ainda consegue viver no desktop dele, como um viajante que fala o idioma local e não precisa de guia.

[[diagram:linux]]

## Como instalar

- **A Loja** mostra os apps do Debian por categoria, com busca, e mostra um plano (o que será instalado ou removido) antes de fazer qualquer coisa. Ela também instala os apps `.lpk` do próprio LordOS e arquivos `.deb` locais.
- **O motor de pacotes próprio do LordOS** lê suas fontes do apt, verifica os índices assinados com código OpenPGP próprio, resolve as dependências e confere cada arquivo baixado contra o SHA-256 do índice assinado. Só então o `dpkg`, a ferramenta do próprio Debian, desempacota e configura o pacote. O banco de dados do dpkg continua sendo escrito só pelo dpkg, então `apt` e `dpkg` seguem funcionando normalmente no Terminal.
- **O Terminal** funciona como em qualquer sistema Debian. Cada aba já vem com as variáveis do Wayland definidas, então iniciar um programa GTK ou Qt por ele simplesmente funciona.

[[shot:1]]

## Como abrir

Os apps com um arquivo `.desktop` aparecem no lançador de apps, no menu iniciar do tema Lord Classic e na Busca, com seus ícones. Clicar num deles passa por uma porta propositalmente estreita no `lord-server`:

1. A interface só pode passar o **id** de um arquivo `.desktop` já instalado em `/usr/share/applications` ou `~/.local/share/applications`, nunca um comando.
2. O `lord-server` relê esse arquivo, confere que é um aplicativo de verdade, visível e que não roda em terminal, e monta o comando a partir da linha `Exec` seguindo a especificação, sem shell e sem argumentos de quem chamou. O "Abrir com" do Arquivos pode acrescentar exatamente um arquivo, só se o app declarar os tipos de arquivo que trata e o arquivo estiver na sua pasta pessoal ou num disco que você montou.
3. O app é iniciado no seu próprio cgroup, `app-lordos-<id>.scope`, criado pelo root, para que o Monitor, o firewall por app e suas restrições saibam a qual app pertence cada processo. Se você restringiu os seus arquivos para o app, é aqui também que ele recebe a própria visão: namespaces próprios, uma pasta pessoal vazia só com as pastas dele, e Landlock.

O JavaScript do LordOS não consegue escrever arquivos `.desktop`, então a interface não tem como inventar um comando: ela só pode escolher um app que já está instalado.

## Como eles ficam e se comportam

- **A moldura.** Os apps que pedem ao compositor para desenhar sua moldura (apps Qt, foot, Chromium com a barra de título do sistema) recebem a moldura do tema. Os apps GTK 3, como o Firefox, a recebem por um segundo protocolo de decoração que o LordOS também fala. Os apps GTK 4 que desenham a própria barra a mantêm, e o LordOS apara a sombra deles.
- **Navegadores com uma só barra.** Desde a 0.8.4, o Chromium e o Firefox pré-instalados desenham uma só barra com suas abas e os botões da janela dentro dela, do lado que o tema do LordOS usa (à esquerda no Lord Flow, à direita no Lord Classic), e acompanham ao vivo uma troca de tema ou de modo claro/escuro. Um botão em Ajustes → Aparência, "Navegadores: abas na barra de título", devolve os dois à moldura do LordOS. O compositor respeita a moldura que cada app pede, mesmo quando ela muda com a janela aberta.
- **Claro e escuro, ao vivo.** O próprio LordOS serve o portal de desktop "Settings" no barramento da sessão, então os apps GTK 4 e libadwaita seguem o modo, a cor de destaque e a ordem dos botões. Os apps GTK 3 seguem pelo keyfile do GSettings, e o LordOS traz um tema GTK combinando. Os dois mudam ao vivo quando você troca de modo. Da 0.8.8 à 0.8.8.7, a última versão lançada, os apps GTK 4 ficavam no modo escuro com destaque azul qualquer que fosse a sua escolha; isso está corrigido na 0.8.8.8 (em desenvolvimento).
- **Abrir e salvar com o diálogo do LordOS** (desde a 0.8.8): os apps Linux sem um diálogo de arquivos próprio pedem um pelo portal padrão FileChooser, e o LordOS responde com o próprio diálogo, que parece e funciona como o Arquivos, dentro da pasta pessoal.
- **Direitos de administrador pelo aviso do LordOS** (desde a 0.8.8): um app que os pede com `pkexec`, como um lançador que se atualiza sozinho, recebe o aviso de administrador do LordOS, que mostra o app e o comando, em vez de parar com o erro 127. Programas que pedem ao polkit pelo D-Bus ainda não encontram agente.
- **Os cliques chegam à superfície certa** (desde a 0.8.8): o compositor agora respeita a região de entrada que um app define, então as abas, a barra e as páginas do Firefox voltam a responder ao mouse.
- **Uma só área de transferência.** Um texto copiado num app Linux cola no LordOS e vice-versa; as imagens continuam disponíveis depois que o app de origem fecha e aparecem no histórico da área de transferência (❖V).
- **Selecionar para copiar, clique do meio para colar.** Desde a 0.8.3 a seleção primária funciona entre apps Wayland, e entre eles e os apps X11. Desde a 0.8.5 o app que copiou também vê a própria cópia na área de transferência, como diz o protocolo Wayland. Na 0.8.8.8 (em desenvolvimento) os campos de texto do próprio LordOS e o Terminal também participam.
- **Os vídeos mantêm a tela ligada** (desde a 0.8.5): um reprodutor de vídeo Linux pode pedir que a tela não escureça nem bloqueie enquanto toca (`zwp_idle_inhibit_manager_v1`), e os temporizadores de inatividade esperam enquanto a janela dele está à vista.
- **Nítido em qualquer escala.** Cada tela é um `wl_output` separado, e a escala fracionária deixa GTK 4, Qt 6 e Chromium renderizarem com nitidez de 125 a 175 %.
- **Gestos do touchpad.** O driver de touchpad próprio do LordOS repassa os gestos de pinça, deslizar e segurar aos apps Linux, então o zoom com pinça funciona nos navegadores.
- **Jogos e apps de GPU.** Com uma GPU, os apps podem entregar buffers de GPU direto ao LordOS (`linux-dmabuf`), usar tela cheia de verdade, prender o ponteiro e receber o movimento bruto do mouse. Na 0.8.7, a captura vale só com o ponteiro sobre a janela e o teclado no seu app, e ❖Esc sempre a libera.
- **As notificações** dos apps Linux chegam pelo serviço de notificações padrão, que o `lord-server` fornece, e podem ser configuradas por app em Ajustes.
- **Senhas:** o Secret Service padrão é servido pelo chaveiro do LordOS, então os navegadores podem guardar seus segredos ali sem outro daemon de chaveiro.
- **Forçar saída** (desde a 0.8.5): no Lord Flow, Forçar saída… no menu da coroa lista os apps abertos e encerra o escolhido sem perguntar a ele. Para um app Linux encerra só o processo desse app; um app X11 recebe um pedido para fechar.
- **Apps padrão sem escolher** (desde a 0.8.6): se você não escolheu um app para um tipo de arquivo, o arquivo abre com o padrão do sistema ou com o primeiro app instalado que declara o seu tipo, mostrado como "Automático" em Ajustes → Apps padrão. Os apps que você escolheu continuam como estão.

[[shot:2]]

[[shot:3]]

## Em desenvolvimento para a 0.8.8.8

Ainda não lançada; a última versão lançada é a 0.8.8.7.

- **Arrastar e soltar.** Arraste arquivos do Arquivos para um app Linux (ele recebe uma cópia; segure Shift para movê-los), de um app Linux para uma pasta do Arquivos, ou entre apps Linux. Só os apps que não estão num sandbox podem entregar arquivos ao LordOS. Apps X11 (pelo Xwayland) também participam, já construído mas ainda não verificado na nossa máquina de testes.
- **Arquivos copiados.** Copie no Arquivos e cole num gerenciador de arquivos Linux, ou o contrário. Arquivos copiados em outro app só são colados depois que o Arquivos pergunta, com Cancelar como padrão.
- **Seu método de entrada dentro dos apps Linux.** Os métodos de entrada de coreano, japonês e chinês do LordOS agora digitam nos apps GTK e Qt, com a lista de candidatos no cursor, e nunca em campos de senha.
- **Ícones de bandeja.** Apps Linux que põem um ícone na bandeja o mostram ao lado dos itens de status do LordOS, com o menu dele. Um ícone que só existe em SVG também é desenhado, na cor da barra (verificado com o applet de volume).
- **O que está tocando e as teclas de mídia.** Navegadores e players de vídeo e de música aparecem no reprodutor da Central de Controle com título, capa e controles, e as teclas de reproduzir/pausar, próxima e anterior chegam até eles.
- **Diálogos e links.** O diálogo de um app Linux abre sobre a janela dele e se move com ela, e um link aberto pelo LordOS traz para a frente o navegador que já está aberto. Uma janela não pode pegar o foco sozinha.
- **Compartilhar uma tela ou uma janela.** Uma videochamada num navegador pode compartilhar uma tela ou uma só janela: o seletor próprio do LordOS mostra as telas e as janelas com uma imagem de cada uma e o nome do app que pede, nada é compartilhado até você apertar Compartilhar, e o Parar do indicador de compartilhamento encerra. Verificado na nossa máquina de testes com um navegador. Senhas e as outras janelas protegidas saem pretas.
- **Apps que tomam os atalhos.** Uma máquina virtual, uma área de trabalho remota ou um jogo podem tomar ❖, ⌥Tab e o resto enquanto estão na frente, depois que você permite uma vez; ❖Esc sempre os devolve. O aviso nomeia o app pelo que o LordOS verificou que ele é, não pelo que ele diz ser.
- **Atalhos globais.** O apertar para falar de um app de chamadas ou os atalhos de um gravador funcionam enquanto outro app está na frente. Você escolhe as teclas na folha do LordOS, e as muda ou esquece em Ajustes → Teclado.
- **Apps X11.** Participam de arrastar e soltar, e ficam nítidos em telas HiDPI porque se escalam sozinhos (Ajustes → Telas traz de volta o modo antigo, borrado).
- **Apps de terminal.** Programas como o htop abrem numa aba do Terminal a partir do lançador de apps.
- **Apps web.** A seção Apps web da Loja instala apps web, cada um numa janela e num processo próprios com suas próprias contas, pelo pacote opcional `lordos-web`. Ainda não testado na nossa máquina de testes.

Atalhos globais, arrastar com apps X11, apps X11 nítidos, apps de terminal e apps web estão construídos e testados, mas ainda não foram verificados na nossa máquina de testes.

## Quais protocolos Wayland existem

`wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (um por tela), `xdg_wm_base` com popups, `wl_seat` (ponteiro, teclado, toque), `wl_data_device_manager` (área de transferência), `zxdg_decoration_manager_v1`, o protocolo de decoração no servidor do KDE, `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1`, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1`, `zwp_primary_selection_device_manager_v1` (desde a 0.8.3) e `zwp_idle_inhibit_manager_v1` (desde a 0.8.5) e, na 0.8.7, `zwp_text_input_manager_v3`, para o teclado na tela (na 0.8.8.8, em desenvolvimento, também para os métodos de entrada do LordOS). Na 0.8.8.8 (em desenvolvimento) o compositor também oferece `xdg_activation_v1`, `zxdg_exporter_v2` e `zxdg_importer_v2`, e `xdg_wm_dialog_v1`, e o `wl_data_device` ganha arrastar e soltar; mais adiante na 0.8.8.8 também `zwp_keyboard_shortcuts_inhibit_manager_v1` e `wp_security_context_manager_v1`.

**Confira de fora** (desde a 0.8.5): o **Banco de testes** (`lordos-testbench`) é uma janela com um padrão de teste que confere o servidor gráfico como qualquer app o vê: o tempo dos quadros e quando eles chegam de fato à tela, cada tecla do teclado, o mouse e a roda, e os protocolos Wayland que o sistema oferece.

Ausente de propósito: qualquer protocolo que permita a um app capturar a tela ou as janelas de outros apps. Na 0.8.8.8 (em desenvolvimento) um app só pode pedir pelo portal ScreenCast, e recebe só a tela ou a janela que você escolhe, enquanto o indicador de compartilhamento mostra isso.

[[shot:4]]

## As perguntas do leitor cético

**Os apps Linux ficam em sandbox?** Não por padrão. Isso é importante: eles rodam como o seu usuário, como em qualquer sistema Debian, e podem ler sua pasta pessoal. O que o LordOS acrescenta é um cgroup por app, a atribuição no Monitor, o firewall por app aplicado pelo kernel (permitir, negar, só LAN, sem conexões de entrada, regras por domínio ou endereço) e restrições que você pode definir por app: sem câmera, sem USB e sem sockets Bluetooth (no kernel), sem microfone (o guarda do PipeWire do LordOS mais os nós de captura da placa de som), sem notificações e "sem acesso aos seus arquivos", aplicada a partir da próxima abertura. Elas não cobrem a área de transferência, o Bluetooth através do BlueZ no barramento do sistema nem programas iniciados fora do lançador de apps. Só os apps `.lpk` do próprio LordOS recebem o sandbox completo.

**Um app Linux consegue ver outras janelas ou gravar a tela?** Não pelo LordOS. O compositor só entrega a um app as superfícies dele mesmo e não oferece nenhum protocolo de captura de tela, apps X11 só veem outros apps X11 (nunca os apps Wayland nem o LordOS), até a 0.8.8.7 não há portal de captura de tela (na 0.8.8.8, em desenvolvimento, um app pode pedir, e recebe só o que você escolhe no seletor próprio do LordOS, nunca se você o restringiu de capturar a tela), e um app restrito também tem o dispositivo de framebuffer negado, então janelas protegidas como a de Senhas também ficam privadas para eles.

**E os apps que só falam X11?** Desde o LordOS 0.8.3 Beta eles abrem pelo Xwayland, que só inicia quando um deles se conecta, com a moldura de janela do tema, o ícone no Dock e a área de transferência compartilhada. Eles têm a mesma identidade por app, o mesmo firewall e a mesma atribuição no Monitor que os apps Wayland; apps com restrições de dispositivos ou de arquivos não recebem tela X. Foram testados o XTerm, um app Java Swing e um jogo SDL.

**E o Flatpak?** Hoje o LordOS não o usa nem o integra. Não está planejado para nenhuma versão: para manter o sistema estável ele não está sendo construído, e em `docs/FLATPAK.md` fica uma revisão do projeto para o futuro, com o que os apps Flatpak precisariam para seguir o modelo do LordOS (identidade própria no firewall e no Monitor, permissões em Ajustes → Privacidade e os avisos do próprio LordOS).

**E apps como Chrome ou Spotify?** (desde a 0.8.5) Loja → Comerciais instala apps gratuitos cujo código-fonte não é público a partir dos repositórios assinados dos fabricantes, com a chave fixada pelo LordOS, e avisa antes de instalar qualquer coisa.

**Os pacotes vêm por HTTPS?** Desde o LordOS 0.8.5, sim para as fontes `https://`: um pequeno helper isolado, o `lord-tls`, verifica o certificado e o nome do servidor e criptografa o download, então ninguém na rede consegue ver quais pacotes você baixa, e um download seguro nunca é mandado de volta para HTTP sem criptografia. Até a 0.8.4.1 eles vinham por `http://`. Nos dois casos a segurança vem das assinaturas e dos hashes: todo índice precisa ter uma assinatura válida de uma chave confiável, não pode estar expirado nem ser mais antigo que o já guardado, e cada arquivo é conferido contra seu SHA-256 antes de ser usado, então ninguém na rede consegue alterá-los.

**O que é a categoria Inteligência Artificial?** (desde a 0.8.5) Uma página da Loja com uma lista curta de ferramentas de IA escolhidas pelo LordOS: agentes que programam (Claude Code, Codex, goose, opencode), editores (Visual Studio Code, Cursor), o app de desktop do Claude, o llama.cpp para rodar modelos de linguagem no seu computador e o whisper.cpp para transformar voz em texto. Os apps cujo fabricante tem um repositório assinado são instalados a partir dele: a Loja diz antes qual repositório vai adicionar e confere a chave do fabricante com a impressão digital fixada pelo LordOS. Os outros vêm do repositório assinado do LordOS, compilados pelo LordOS a partir de fontes verificadas ou copiados depois de conferir a assinatura do fabricante. O llama.cpp baixa um modelo na primeira vez que você o executa (`llama-server -hf <usuário>/<modelo>-GGUF`); roda na CPU, e numa GPU via Vulkan, que ainda não foi testado em hardware real.

## Limites honestos

- **Apps X11 se veem entre si,** como em qualquer desktop X11; ficam nítidos só em 100 % de escala (na 0.8.8.8, em desenvolvimento, eles se escalam sozinhos e ficam nítidos, salvo um app que não leia nenhuma configuração de DPI).
- **Fora da 0.8.8.7, a última versão lançada:** arrastar e soltar entre apps, os métodos de entrada do LordOS (coreano, japonês, chinês) dentro dos apps Linux e os pedidos de ativação de apps. Os três estão em desenvolvimento para a 0.8.8.8 (acima), onde arrastar com apps X11 já está construído mas ainda não verificado na nossa máquina de testes.
- **Ícones só em SVG:** até a 0.8.8.7 o LordOS não desenha SVG e usa os ícones PNG que o Debian publica para a maioria dos apps, ou a inicial; na 0.8.8.8 (em desenvolvimento) o `lord-media` desenha os ícones SVG no seu sandbox.
- **Apps de terminal** com arquivo `.desktop` não aparecem no lançador de apps até a 0.8.8.7; inicie-os pelo Terminal. Na 0.8.8.8 (em desenvolvimento) eles abrem numa aba do Terminal.
- **Os apps GTK 3 mantêm o próprio destaque azul;** a leitura do esquema de cores pelo portal no Qt 6 não foi testada.

## Leia o código

- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): o compositor Wayland e seus protocolos
- [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c): o lançador de `.desktop`
- [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c): o portal de configurações para GTK e libadwaita, e o portal FileChooser
- [sdk/wayland.js](https://github.com/lordbasex/LordOS/blob/main/sdk/wayland.js): as janelas Linux no gerenciador de janelas
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg): o motor de pacotes Debian
- [apps/store](https://github.com/lordbasex/LordOS/tree/main/apps/store): a Loja
- [docs/WAYLAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/WAYLAND.md), [docs/PACKAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/PACKAGES.md), [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md)
