## Resumo

O LordOS é uma distribuição GNU/Linux livre baseada no Debian 13 que traz um ambiente de desktop próprio, construído a partir dos seus componentes fundamentais sobre Debian e Wayland. Em vez do GNOME ou do KDE, ele tem seu próprio servidor gráfico e compositor Wayland escritos em C (`lord-server`), seu próprio motor de interface (React rodando sobre o motor JavaScript QuickJS, com layout, pintura e texto em código C nativo), seus próprios apps nativos e seus próprios serviços de segurança: um lançador de apps em sandbox, um broker de permissões, um armazenamento de permissões por app, um firewall por app aplicado pelo kernel, assinatura de pacotes e um cofre de senhas criptografado. Ele reaproveita o que é maduro e amplamente revisado: o kernel Linux, o Debian e seus pacotes, Mesa, PipeWire, o protocolo Wayland, o Xwayland para apps X11 e algumas bibliotecas (FFmpeg, Poppler, libarchive, OpenSSL, libsrtp2, usrsctp, Monocypher) confinadas em pequenos processos ajudantes em sandbox ou no gerenciador de sessões.

Este documento descreve o projeto e o estado atual da **0.8.5 Beta**, lançada em 27 de outubro de 2026, para um leitor técnico que quer conferir o projeto em vez de acreditar nele. **A 0.8.5 Beta é o último lançamento**; a 0.8.0 (18 de outubro) foi o primeiro lançamento público, seguida pelas 0.8.1, 0.8.2, 0.8.3, 0.8.4, a 0.8.4.1 (só pacote) e a 0.8.5, cada uma listada no `CHANGELOG.md`. Cada afirmação aponta para um arquivo no repositório público. Os números são os medidos nos registros de teste do projeto e indicam onde foram medidos. O que está projetado mas não construído está marcado como tal. Desde a 0.8.5, muitas das afirmações também podem ser conferidas no próprio computador do leitor com as Verificações das Informações do sistema e o Banco de testes (§11), e o `docs/PAPER-VALIDATION.md` lista cada afirmação deste documento com a forma de conferi-la.

## 1. Visão

### 1.1 O problema

Um desktop Linux moderno é montado a partir de muitos projetos independentes: um servidor gráfico ou compositor, um toolkit, um gerenciador de sessão, um daemon de configurações, um chaveiro, um gerenciador de rede, uma interface de firewall e os apps. Cada um é bom no que faz, mas é nas emendas entre eles que as coisas ficam lentas, inconsistentes ou inseguras. Três emendas em especial motivaram o LordOS:

- **Peso.** Um desktop que desenha sua interface com um motor web, ou que carrega vários toolkits ao mesmo tempo, precisa de centenas de megabytes antes de o usuário abrir qualquer coisa. O LordOS quer ser confortável em máquinas modestas e num Raspberry Pi 5.
- **Confiança nos apps.** Num desktop Linux tradicional, todo programa que o usuário roda pode ler a pasta pessoal inteira, gravar o microfone, abrir qualquer conexão de rede e ler a área de transferência. O sandbox existe, mas costuma ser um complemento que o próprio desktop não entende.
- **Visibilidade.** O usuário raramente vê qual app está falando com qual servidor, e mais raramente ainda consegue dizer "este app não pode usar a internet" de um jeito que o app não consiga contornar.

### 1.2 Princípios

O projeto segue uma lista curta de regras, escritas em `AGENTS.md` e `docs/ARCHITECTURE.md`:

1. **Ser dono das partes que o usuário toca; reaproveitar a fundação.** O servidor gráfico, o compositor, o motor de interface, os apps e os serviços de segurança são código do LordOS. O kernel, os pacotes do Debian, Mesa, PipeWire e o protocolo Wayland são reaproveitados.
2. **Uma política de dependências rígida.** X11, wlroots, libdrm, libinput, WebKit, GTK e Qt não são usados dentro do LordOS. Bibliotecas que leem dados não confiáveis só são linkadas por ajudantes em sandbox, nunca pelo compositor.
3. **A interface não executa comandos.** O JavaScript do LordOS não tem API para executar programas arbitrários. As exceções são poucas, estreitas, e cada uma está aprovada e documentada no `AGENTS.md`: o shell de login do Terminal; abrir um app `.desktop` instalado (opcionalmente com um arquivo que o usuário abriu, ou com uma das ações nomeadas do próprio app); iniciar um app `.lpk` instalado no seu sandbox; o Xwayland, iniciado pelo próprio `lord-server` com uma linha de comando fixa só quando um app X11 se conecta; os conectores de rede dos componentes e o baixador de listas de bloqueio, que reexecutam os binários do próprio LordOS com argumentos fixos; e o processo filho de download da Loja, que se coloca no seu próprio escopo. Nenhuma recebe texto da interface.
4. **Root só por portas fechadas.** Tudo o que precisa de root passa por uma tabela fixa de ações validadas ou por um pequeno serviço dedicado com protocolo fechado.
5. **O caminho pela CPU sempre tem que funcionar.** A composição na GPU é uma otimização, nunca um requisito.
6. **O compositor nunca espera.** Ajudantes, apps e serviços falam com o `lord-server` por sockets não bloqueantes; um app travado trava só a si mesmo.
7. **Verificar num sistema real.** Cada marco é conferido numa máquina virtual com capturas de tela reais antes de ser dado como pronto, e compatibilidade que não foi testada não é anunciada.

### Arquivos-fonte

- [AGENTS.md](https://github.com/lordbasex/LordOS/blob/main/AGENTS.md)
- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md)

## 2. Arquitetura

### 2.1 Camadas

[[diagram:layers]]

De baixo para cima:

- **O kernel Linux** fornece DRM/KMS para as telas, evdev para a entrada e os recursos de segurança sobre os quais o LordOS se apoia: Landlock, seccomp-bpf, cgroups v2 e eBPF. Em PCs é o kernel do Debian; no Raspberry Pi 5 é a compilação do LordOS do kernel do Raspberry Pi com Landlock ativado (§7.4).
- **O Debian 13** fornece o espaço de usuário: `dpkg` e o repositório de pacotes, systemd, Mesa, PipeWire, `libwayland`, `libxkbcommon`, CUPS, BlueZ e o resto.
- **O `lord-server`** é o servidor gráfico e compositor Wayland. Ele controla telas e entrada diretamente, faz a composição e serve o protocolo Wayland para os apps Linux.
- **O motor de interface** mora dentro do `lord-server`: o QuickJS roda a interface em React, e código C nativo faz layout, pintura e texto.
- **Os apps** são de três tipos: os apps incluídos e o tema, que rodam no contexto JavaScript do `lord-server`; os apps `.lpk` instalados, cada um no seu próprio processo em sandbox com sua própria instância do motor; e os apps Linux, que se conectam ao compositor como clientes Wayland comuns.

### 2.2 Processos e privilégios

[[diagram:processes]]

Um LordOS em execução tem poucos processos, com papéis bem separados:

| Processo | Roda como | Papel |
|---|---|---|
| `lord-sessiond` | root | tela de login, sessões, empréstimo da tela e da entrada para a sessão visível, área de trabalho remota |
| `lord-netd` | root | a rede do sistema: DHCP, Wi-Fi, VPN; só as ações `net-*` |
| `lord-netperm` | root (capacidades limitadas) | regras de rede por app, e as restrições de dispositivos e Bluetooth dos apps Linux, aplicadas com programas eBPF de cgroup e de LSM |
| `lord-fand` | root (sem capacidades) | controle das ventoinhas pela temperatura |
| `lord-server` | o usuário | um por sessão: compositor, motor de interface, broker |
| `lord-server --app <id>` | o usuário, em sandbox | um por app `.lpk` em execução, e os apps do sistema que rodam isolados: Senhas, E-mail, Calendário, Contatos, Lembretes e o Teste do isolamento das Informações do sistema |
| `lord-doc`, `lord-media`, `lord-maild`, `lord-vault`, `lord-fetch` | o usuário, em sandbox | ajudantes que processam dados não confiáveis ou segredos (o `lord-fetch` baixa as listas de bloqueio do firewall) |
| `lord-tls` | `nobody`, em sandbox, no escopo de download da Loja | HTTPS para os downloads do motor de pacotes (0.8.5): confere o certificado e o nome do servidor, e depois só repassa bytes |
| `lord-server --net-connector mail\|print\|lists` | o usuário, no seu próprio escopo | abre sockets de rede para um componente do LordOS, para que o firewall possa vê-lo e bloqueá-lo separadamente |
| `Xwayland` | o usuário, escopo e namespace de rede próprios | só enquanto um app X11 está conectado, iniciado pelo `lord-server` com uma linha de comando fixa |
| tela de login | `lordos-greeter` | desenha a interface de login; nunca verifica senhas por conta própria |

Cada processo root tem uma interface estreita e documentada. A §6.3 descreve cada uma.

### 2.3 O que é nosso e o que é reaproveitado

| Nosso (neste repositório) | Reaproveitado |
|---|---|
| Servidor gráfico e compositor Wayland (C) | Kernel Linux |
| Motor de interface: nós nativos, layout flexbox, rasterizador, shaping de texto, bidi, métodos de entrada | Debian 13 e seus pacotes; `dpkg` |
| Renderizador React (configuração de host para o `react-reconciler`) | React 19.3.1, `react-reconciler` 0.34 (empacotados no tema) |
| Apps nativos e temas | QuickJS 2025-04-26 (incorporado como código-fonte) |
| Sandbox, broker, permissões, assinatura, serviço de firewall, cofre | Mesa (opcional), PipeWire, `libwayland`, `libxkbcommon` |
| Gerenciador de sessão, serviço de rede, serviço de ventoinhas | systemd, systemd-resolved, BlueZ, CUPS, wpa_supplicant, OpenVPN, o daemon do strongSwan |
| Motor de pacotes Debian (OpenPGP, xz, HTTP e HTTPS, resolvedor) | FFmpeg, Poppler, libarchive, OpenSSL, Monocypher (só nos ajudantes) |
| Driver de touchpad, emulador de terminal, interpretador WebAssembly | stb (fontes e imagens), fontes sob a OFL |
| Área de trabalho remota por WebRTC: ICE-lite, estabelecimento do DTLS-SRTP, empacotamento RTP, controle de banda (a codificação, no `lord-media`) | libsrtp2 e usrsctp (na ponte do `lord-sessiond`), libx264, libvpx, Opus e VA-API (no `lord-media`) |
| O coletor das Informações do sistema, `lordos-sysreport`, `lordos-fetch`, o Banco de testes | Xwayland (o pacote do Debian, sob demanda), smartmontools, `pci.ids` e `usb.ids` |

O LordOS conversa com vários serviços do Debian pelos protocolos deles, com clientes próprios: D-Bus (BlueZ, perfis de energia, data e hora), IPP (CUPS), o protocolo do PulseAudio (PipeWire) e o Secret Service. libdbus, sd-bus, libcups, libpulse, libbpf e libapt não são usados.

### 2.4 Serviços do sistema

- **Rede (`lord-netd`).** A rede pertence ao sistema, não a uma sessão. O `lord-sessiond` inicia o `lord-netd` no boot, com caminho fixo e sem argumentos, para que a máquina tenha endereço antes de alguém fazer login. Ele roda só as ações fechadas `net-*`: seu próprio cliente DHCP e código netlink, Wi-Fi pelo `wpa_supplicant` do Debian, WireGuard pelo código netlink próprio do LordOS, OpenVPN, IPsec/IKEv2 pelo `charon` do strongSwan controlado via VICI com cliente próprio do LordOS e, opcionalmente, túneis OpenConnect, PPTP e compatíveis com Fortinet. Ele só fala com os ajudantes root das sessões, por um socket que aceita apenas uid 0.
- **Ventoinhas (`lord-fand`).** Um serviço root que lê a temperatura da CPU a cada 2 s e escreve o ciclo das ventoinhas a partir de um perfil, desde o início do boot. Ele só controla ventoinhas que a própria varredura encontrou: o conector de ventoinha do Raspberry Pi 5 e controladores de placa-mãe de uma lista permitida. Ventoinhas de notebook, de GPU e desconhecidas nunca são escritas. Ao parar, ou depois de uma falha, ele restaura as configurações originais do kernel. Não roda programas, não tem rede, nem capacidades, nem nós de dispositivo.
- **Configurações de hardware.** Telas (modos vindos do KMS com nomes do EDID, reversão de 15 segundos com "manter estas configurações?", persistência por tela, rotação, Luz Noturna), Bluetooth pelo BlueZ, bateria e perfis de energia, ociosidade e tampa, data e hora, notificações dos apps Linux e apps padrão. As mudanças chegam como uevents do kernel e sinais D-Bus; nada fica consultando em loop. Num Raspberry Pi 5, Ajustes → Firmware da placa mostra e atualiza o bootloader que fica na EEPROM da placa por meio de duas ações administrativas fechadas (0.8.4.1).
- **Atualizações.** Os sistemas instalados se atualizam no lugar a partir do repositório APT assinado do LordOS em lordos.org, por Loja → Atualizações ou com o `apt`; mudanças que são mais do que arquivos novos rodam como migrações numeradas e idempotentes (0.8.1). O motor de pacotes da Loja segue as prioridades do apt (`/etc/apt/preferences.d`, 0.8.5), então na imagem do Raspberry Pi "Atualizar tudo" escolhe as mesmas versões que o `apt` e o kernel fixado continua bloqueado, e aceita fontes escritas como `Architectures: amd64,arm64`, como o apt. A atualização da 0.8.4.1 ou da 0.8.4 para a 0.8.5 não roda nenhuma migração; foi testada com o pacote do lançamento gerado como 0.8.5~rc2 numa máquina VMware com a 0.8.4.1 e num Raspberry Pi 5 com a 0.8.4, as duas com o `apt`, seguido de um reinício.

### Arquivos-fonte

- [server/src/main.c](https://github.com/lordbasex/LordOS/blob/main/server/src/main.c)
- [server/src/admin.c](https://github.com/lordbasex/LordOS/blob/main/server/src/admin.c)
- [server/src/net/netd.c](https://github.com/lordbasex/LordOS/blob/main/server/src/net/netd.c)
- [server/src/fan/fand.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fan/fand.c)
- [server/src/hw](https://github.com/lordbasex/LordOS/tree/main/server/src/hw)

## 3. O compositor

### 3.1 Telas sem libdrm

O `lord-server` configura os modos de tela com os ioctls DRM do kernel diretamente (`kms.c`): enumera conectores, CRTCs e planos, aloca buffers e troca a página no retraço vertical. Ele escolhe a placa que tem conectores, e não simplesmente `card0`, o que importa em placas onde o controlador de vídeo e a GPU são dispositivos separados (no Raspberry Pi 5, `vc4` e `v3d`).

### 3.2 Duas formas de montar um quadro

- **Caminho pela CPU.** A interface e as janelas dos apps são copiadas para um canvas, e um buffer "dumb" é apresentado com uma troca de página. Não precisa de nada além do kernel. É o padrão quando não há placa com 3D, e `LORD_GPU=0` o força (o modo de segurança do menu de boot o usa). Desde a 0.8.5, quadros pesados são pintados num pool de até 8 threads (`par.c`), só enquanto a pintura leva mais de 70 % de um quadro na taxa alvo (na 0.8.6, em desenvolvimento, também enquanto o quadro inteiro leva mais de 60 %): o desfoque do fundo em fases e as primitivas locais a cada linha divididas em faixas de linhas, com os mesmos pixels de uma só thread (§7.2).
- **Caminho pela GPU.** GBM, EGL e GLES2 do Mesa (`server/src/gpu/gpu.c`). A interface vira uma textura, e só a região alterada é enviada a cada quadro. As janelas dos apps não são copiadas para o canvas da interface: a interface deixa um buraco transparente onde cada janela fica, e as texturas das janelas são desenhadas por baixo, de modo que um app que só muda o próprio conteúdo não provoca repintura da interface. Os apps podem entregar buffers de GPU (`linux-dmabuf`) sem cópias, e um app em tela cheia com um buffer adequado é exibido diretamente (scanout direto). O cursor usa o plano de cursor do hardware.

O caminho é escolhido na inicialização: uma placa sem 3D (simpledrm, bochs, virtio-gpu sem virgl e outras) usa a CPU sem nem carregar o Mesa; um renderizador OpenGL por software também volta para a CPU, porque compor com ele custa mais que o caminho de CPU próprio do LordOS. Se um quadro na GPU falha, a GPU é desligada e o caminho pela CPU assume.

**Estado:** o caminho pela GPU foi exercitado com o renderizador por software do Mesa em máquinas virtuais. Os testes em placas AMD e Nvidia reais estão pendentes.

### 3.3 Janelas e entrada

- **Gerenciamento de janelas em C.** Mover e redimensionar rodam em C, com um evento por quadro e sem JavaScript, e a janela sendo movida é desenhada a partir da sua própria camada. As quatro bordas e os cantos redimensionam, com cursores próprios do LordOS.
- **Entrada sem libinput.** Teclados, mouses e mesas digitalizadoras são lidos do evdev. Os layouts de teclado vêm do `libxkbcommon` (todos os layouts XKB, variantes e compose).
- **Touchpad e tela sensível ao toque.** O LordOS tem seu próprio driver multitoque (`server/src/tp`): aceleração do ponteiro, toque para clicar e clique com dois dedos, rejeição da palma, rolagem cinética e natural, pinça e gestos de três dedos. Os gestos chegam tanto à interface do LordOS quanto aos apps Linux (`zwp_pointer_gestures_v1`). Ele é testado com gestos gravados no `make test`.

### 3.4 Compatibilidade Wayland

O compositor (`server/src/wl/wl.c`) usa apenas `libwayland-server` e código gerado a partir do XML dos protocolos. Ele oferece: `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_output` (um por tela), `xdg_wm_base` com popups, `wl_seat` (ponteiro, teclado, toque), `wl_data_device_manager` (uma área de transferência compartilhada com o LordOS), `zwp_primary_selection_device_manager_v1` (colar com o clique do meio, 0.8.3), `zxdg_decoration_manager_v1`, o protocolo de decoração do lado do servidor do KDE (para que apps GTK 3 como o Firefox recebam a moldura do tema), `zxdg_output_manager_v1`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_linux_dmabuf_v1` v4, `wp_presentation`, `zwp_relative_pointer_manager_v1`, `zwp_pointer_constraints_v1`, `zwp_pointer_gestures_v1`, `wp_cursor_shape_manager_v1` e `zwp_idle_inhibit_manager_v1` (um player de vídeo mantém a tela ligada, 0.8.5). O compositor respeita a moldura que um app pede, também quando ela muda com a janela aberta (0.8.4).

**Conferido de fora.** O Banco de testes (`lordos-testbench`, 0.8.5) é um cliente Wayland comum que faz bind de cada um desses protocolos na versão anunciada e o exercita: formatos de shm, saídas e seus modos, xdg-output, escala fracionária, uma subsuperfície com viewport, um popup, os estados maximizado e tela cheia, o feedback de dmabuf, idas e voltas da área de transferência e da seleção primária, idle-inhibit, formato do cursor, ponteiro relativo e gestos. A primeira execução descobriu que o compositor não enviava uma seleção nova de volta ao cliente que a definiu, como o protocolo exige, e que faltava o idle-inhibit; os dois foram corrigidos no mesmo dia, e a execução agora dá 16 aprovados, 0 falhas na VM 116 (`docs/PAPER-VALIDATION.md`).

As janelas Linux entram no gerenciador de janelas do SDK como qualquer outra, com título, foco, maximizar, minimizar e ícone no Dock. Apps que pedem decoração do lado do servidor recebem a moldura do tema; apps que desenham a própria (GTK 4) a mantêm, e a sombra delas é recortada. Apps GTK 4 e libadwaita seguem o modo claro/escuro, a cor de destaque e a ordem dos botões por meio de uma implementação de `org.freedesktop.portal.Settings` que o próprio `lord-server` serve no barramento da sessão; o GTK 3 segue pelo keyfile do GSettings.

**Apps X11 (0.8.3).** Quando um app que só fala X11 se conecta ao socket X da sessão, o `lord-server` inicia o `/usr/bin/Xwayland` do Debian com uma linha de comando fixa, em modo rootless, no seu próprio escopo e no seu próprio namespace de rede, com `no_new_privs`; o compositor é o gerenciador de janelas X (`server/src/wl/xwm.c`), então as janelas X11 recebem a mesma moldura, a mesma entrada no Dock, a mesma identidade por app, as mesmas regras de firewall e as mesmas restrições das janelas Wayland, e a área de transferência funciona nos dois sentidos. Apps `.lpk` isolados e apps Linux com restrições de dispositivos não recebem `DISPLAY`. O pacote `xwayland` do Debian precisa estar instalado.

Ainda pendentes: arrastar e soltar, os métodos de entrada do LordOS dentro dos apps Linux (`text-input` v3), ativação (`xdg-activation`), sincronização explícita, buffers de um só pixel, controle de tearing, tipo de conteúdo, mesas digitalizadoras e ícones de app em SVG. Nenhum protocolo de captura de tela é oferecido aos apps.

### 3.5 Várias telas e HiDPI

Todas as telas conectadas são controladas ao mesmo tempo, estendidas ou espelhadas, num único espaço lógico, com o arranjo lembrado pela identidade EDID e suporte a conectar e desconectar a quente. Cada tela é pintada na sua própria escala (100, 125, 150, 175 ou 200 %), então texto e vetores são rasterizados na resolução real dela. Os apps Linux recebem um `wl_output` por tela e escala fracionária. Testado com duas saídas virtuais (1280×800 e 2560×1600) nos dois caminhos; monitores reais, painéis de notebook e hubs DisplayPort não foram testados.

Desde a 0.8.5, Ajustes → Telas numera cada tela (a principal é a 1) e **Identificar** mostra esse número em tamanho grande em cada monitor; arrastar uma tela no arranjo a encaixa borda com borda e a alinha com as outras, soltá-la nunca deixa duas telas sobrepostas ou separadas, e o servidor mantém uma posição que se encaixa exatamente. Uma janela pode ser enviada a outra tela pelo menu Janela, e um app pode ser atribuído a uma tela pelo seu item do Dock, guardado pelo nome do conector (HDMI-A-1, DP-2…), para distinguir dois monitores do mesmo modelo (conferido na VM 118). A tela de login e de bloqueio aparece só na tela principal, com o papel de parede em todas as telas, e acompanha as telas conectadas ou desconectadas (VM 118, duas telas).

### 3.6 Sessões e login

[[diagram:sessions]]

O `lord-sessiond` é o gerenciador de sessões próprio do LordOS:

- **A tela de login** é um `lord-server` rodando como o usuário sem privilégios `lordos-greeter`, com um tema especial. Ela envia pedidos de login ao `lord-sessiond`; **as senhas são sempre verificadas pelo processo root** contra o `/etc/shadow`, nunca pelo JavaScript da tela de login. Tentativas que falham são respondidas num tempo fixo de 1,25 s, então o tempo não revela nada. Desde a 0.8.5 ela segue os eventos de tela do kernel, então um monitor conectado depois do boot a mostra, e ela não fica mais rodando por trás da sessão do usuário: fecha 10 segundos depois que uma sessão assume a tela (cerca de 53 MB próprios, 78 MB contando as páginas compartilhadas, medidos numa VM do VMware; o sistema inteiro passou de 603 para 548 MB em uso) e volta a iniciar em poucos milissegundos num bloqueio, numa troca de usuário ou num logout. Num bloqueio a tela escurece primeiro e só então a tela de login é iniciada, então o desktop desbloqueado nunca fica visível nesse meio-tempo.
- **Primeiro o barramento de sessão do usuário (0.8.5).** O `lord-sessiond` inicia o `user@<uid>` e espera o socket do barramento de sessão do usuário (até 5 s) antes de iniciar a sessão. Com o login automático ela começava alguns milissegundos cedo demais, então o Secret Service do `lord-vault` e o servidor de notificações nunca entravam no barramento (§6.10).
- **Uma sessão por usuário**, todas podendo existir ao mesmo tempo (troca rápida de usuário). Cada uma é um `lord-server` que começa como root e abandona os privilégios logo em seguida.
- **Empréstimo da tela.** O `lord-sessiond` abre a placa de vídeo e os dispositivos de entrada como root e os empresta à sessão que está no monitor, por um socket Unix. Numa troca, ele os revoga: `EVIOCREVOKE` faz os descritores de entrada da sessão anterior pararem de funcionar, e `DROP_MASTER` retira o controle da tela, antes de qualquer coisa ser entregue à próxima sessão. Desde a 0.8.5 o acesso continua sendo cortado na hora, mas os dispositivos são fechados numa thread de vida curta, porque o último fechamento de um mouse USB espera suas transferências pararem: do bloqueio até a tela de login passou de 903 ms para 141 ms numa VM do VMware Fusion, e uma passagem lenta é registrada com o tempo de cada etapa. A sessão visível continua compondo na GPU sem nunca ser root. Uma sessão fora do monitor desenha em memória, e só se alguém a estiver vendo remotamente.
- **Bloquear a tela realmente tira a sessão do monitor**; não é uma camada desenhada por cima. Se a sessão do usuário travar, a tela de login continua funcionando.
- **Boot.** Um tema de GRUB do LordOS desenha o menu de boot, e um tema de Plymouth do LordOS desenha a tela de abertura (a coroa com uma barra de progresso). A passagem não tem nenhum quadro preto nem texto de console: o Plymouth para de desenhar e solta o controle da tela enquanto seu último quadro continua visível, o `lord-sessiond` assume o controle da tela e mostra o primeiro quadro real da tela de login, e só então o Plymouth termina, mantendo a tela de abertura até ela ser substituída. Desde a 0.8.5 a tela de abertura mostra o tempo desde que começou, Sobre o LordOS → Tempo de inicialização mostra quanto levou este boot desde o início do kernel até a tela de login (ou o desktop, com login automático), e cada boot é registrado com a versão e o build do LordOS (`journalctl -b -u lordos | grep "first frame"`) e guardado em `/var/lib/lordos/boot-times.jsonl` (os últimos 200 boots).

**Área de trabalho remota.** O mesmo serviço oferece o desktop a um navegador, em `https://<endereço>:5673` (5673 soletra "LORD" num teclado de telefone). Ela vem **desligada por padrão**, é ativada em Ajustes → Compartilhar e, por padrão, só aceita a rede local (0.8.3; pode-se escolher uma lista de redes permitidas ou qualquer endereço, e as interfaces VPN do próprio LordOS contam como locais). Usa TLS 1.2 ou superior com um certificado P-256 autoassinado cuja impressão digital os Ajustes mostram para comparação, uma política de segurança de conteúdo rígida, verificação de mesma origem e um cookie de sessão `Secure`, `HttpOnly` e `SameSite=Strict`; a página de login nunca lista as contas de usuário. **Três logins com falha bloqueiam o endereço de origem** por 15 minutos, depois 1 hora, depois 24 horas (IPv6 por /64), com persistência feita só pelo `lord-sessiond`, e com listas manuais de bloqueio e de permissão; as falhas são respondidas num tempo fixo de 1,25 s. Tudo o que vem da rede passa por parsers rígidos com testes unitários e uma rodada de fuzzing sob ASan e UBSan (0.8.3).

Desde a 0.8.4 a tela viaja como **vídeo WebRTC**: H.264 (VP8 quando o navegador não tem H.264) com o som da sessão em Opus, codificado uma única vez por sessão pelo `lord-media` (VA-API na placa de vídeo quando possível, x264 caso contrário) para todos os espectadores, com as áreas paradas perfeitas no pixel graças a blocos sem perda. Teclado, mouse e área de transferência vão por um canal de dados WebRTC. Além do UDP, o vídeo se conecta por TCP na própria porta 5673, então basta um firewall ou VPN que deixe passar só essa porta; a página se reconecta sozinha depois de uma queda, e diz por que quando realmente não consegue se conectar. Servidores STUN e um relay TURN (UDP, TCP ou TLS) podem ser adicionados para outras redes. O processo que atende uma conexão abandona o root assim que sabe de quem é a sessão e fica só com quatro descritores. Desde a 0.8.5 o vídeo segue a taxa de atualização da tela do navegador (60, 120, 144 Hz…) em vez de 30 fixos, a banda sobe em segundos em vez de minutos, e uma barra superior mostra os quadros por segundo, o codificador e se a tela é composta na GPU ou na CPU (§7.2). Reiniciar e Desligar só são oferecidos na sessão que está no monitor. O acesso pela internet não é previsto; recomenda-se uma VPN.

**Login remoto (SSH).** Ajustes → Compartilhar também liga o servidor OpenSSH do Debian (0.8.1) por meio de ações administrativas fechadas. Desde a 0.8.5 uma **opção avançada, desligada por padrão, permite que o root entre por SSH**, só com chave pública ou com chave ou senha, para testes e administração. Só administradores podem mudá-la, e cada mudança leva a senha do próprio administrador, conferida como root contra o `/etc/shadow`. Os modos escrevem só texto fixo em `/etc/ssh/sshd_config.d/05-lordos-root.conf` (lido antes do `10-lordos.conf` que vem com o sistema, que mantém `PermitRootLogin no`), verificado com `sshd -t`. O único texto do usuário que é escrito é uma linha de chave pública validada em C (um tipo conhecido, um blob que corresponde a ele, um comentário imprimível, sem opções) em `/root/.ssh/authorized_keys`; as chaves são listadas com suas impressões digitais SHA-256, e a senha do root vai ao `chpasswd` pela entrada padrão. Um aviso fica na página enquanto a opção está ligada. Testes: `build/ssh-root-test` (17 verificações, as mesmas impressões digitais que `ssh-keygen -l`) e uma execução de ponta a ponta como root na VM 116 (senha errada recusada, uma chave adicionada e aceita a partir de outro computador, uma duplicada recusada, a chave removida, o modo desligado, `ssh root@` recusado).

### Arquivos-fonte

- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c), [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c), [server/src/outputs.c](https://github.com/lordbasex/LordOS/blob/main/server/src/outputs.c)
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c), [server/src/tp](https://github.com/lordbasex/LordOS/tree/main/server/src/tp), [server/src/ui/grab.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/grab.c)
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c), [server/src/ui/portal.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/portal.c)
- [server/src/remote/sessiond.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/sessiond.c), [server/src/remote/monitor.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/monitor.c), [server/src/remote/sess.c](https://github.com/lordbasex/LordOS/blob/main/server/src/remote/sess.c)
- [tools/boot/plymouth](https://github.com/lordbasex/LordOS/tree/main/tools/boot/plymouth), [tools/iso/grub](https://github.com/lordbasex/LordOS/tree/main/tools/iso/grub), [docs/SESSIONS.md](https://github.com/lordbasex/LordOS/blob/main/docs/SESSIONS.md)

## 4. O motor de interface

### 4.1 Não é um navegador

A interface do LordOS é escrita em JavaScript com React, mas não há motor web por baixo. WebKit ou Chromium custariam de 100 a 300 MB de RAM por processo e não seriam código do LordOS. Em vez disso, o tema e os apps usam um renderizador React (`runtime/index.js`) que cria e atualiza **nós nativos** em C: `View`, `Text`, `Image`, além de superfícies para janelas Wayland e mídia. Os estilos são um subconjunto do CSS (flexbox, cores, bordas arredondadas e por lado, sombras, desfoque, transformações, transições) aplicado por código C que o projeto controla. O modelo é parecido com o de um framework móvel que renderiza React em views nativas, só que aqui o lado "nativo" é do próprio LordOS.

### 4.2 As peças

- **JavaScript:** QuickJS 2025-04-26, um pequeno motor ES2023 em C, incorporado como código-fonte e embutido no `lord-server`.
- **React:** React 19.3.1 com `react-reconciler` 0.34, empacotados com esbuild em cada tema. O tema padrão com todos os apps pesa cerca de 1,8 MB (540 KB comprimido) na 0.8.5.
- **Layout:** implementação própria de flexbox do LordOS (`layout.c`), com quebra de linha, bordas lógicas de início/fim e espelhamento da direita para a esquerda.
- **Pintura:** rasterizador 2D próprio do LordOS, com retângulos arredondados com antialiasing, sombras em cache, desfoque em resolução reduzida e rastreamento de regiões alteradas: só a região que mudou é repintada. Em ARM, laços NEON opcionais aceleram cópias e máscaras, verificados como idênticos bit a bit ao código C.
- **Texto:** os glifos são rasterizados com `stb_truetype`. O shaping é código OpenType próprio do LordOS (substituições GSUB e posicionamento GPOS, emoji colorido), com bidi Unicode completo, junção do árabe, shaping índico e o Universal Shaping Engine (409 de 409 frases de teste idênticas ao HarfBuzz em `tests/hb_compare.py`), quebra de linha CJK, separação de palavras em tailandês e fonte de reserva por escrita. HarfBuzz, FreeType, fontconfig, fribidi e ICU não são linkados pelo `lord-server`.
- **Métodos de entrada:** coreano (hangul), japonês (kana e SKK) e chinês (pinyin), em `server/src/ime.c`, sem ibus nem fcitx.

[[diagram:frame]]

### 4.3 Como o JavaScript chega ao sistema

O lado JavaScript chama serviços nativos pelas APIs `lord.*` implementadas em C (`server/src/ui/js_*.c`): `lord.fs` (arquivos), `lord.settings`, `lord.net`, `lord.pkg`, `lord.media`, `lord.mail`, `lord.vault`, `lord.fw`, `lord.monitor`, `lord.fan` e outras. Toda chamada valida seus argumentos em C. As regras são rígidas:

- O `lord-server` roda sem root, e **nenhuma API executa comandos arbitrários**.
- O `lord.fs` só escreve na pasta pessoal e nas montagens do próprio usuário, não tem "apagar" genérico (só mover para a Lixeira) e recusa as pastas que o sistema lê para iniciar ou instalar algo, ou que guardam decisões de segurança: `~/.local/share/applications`, `~/.config/autostart`, a pasta de apps instalados, `settings.json`, o arquivo de permissões, as regras do firewall, as listas de bloqueio, o arquivo de contas de e-mail e a pasta do cofre. Desde a 0.8.5 suas chamadas de leitura alcançam todo o sistema, só para leitura, como o usuário (Arquivos → Este computador; `/proc`, `/sys`, `/dev` e `/run` do kernel ficam de fora, e as permissões do próprio sistema decidem o resto); a escrita nunca foi ampliada, e os apps isolados mantêm só a própria pasta de dados.
- Tudo o que precisa de root passa pelo `admin.c`: uma tabela fechada de ações com argumentos validados do lado root, caminhos de programa fixos e configuração gerada pelo LordOS, nunca texto do usuário passado a um shell. As senhas vão pela entrada padrão, nunca pela linha de comando.
- Trabalho longo nunca bloqueia um quadro: ajudantes e serviços são consultados pelo laço principal sem espera.

### 4.4 Idiomas

A interface vem em inglês (o padrão), espanhol e português, e novos idiomas entram como pacotes de idioma, sem mudar código. O texto passa pelo `t('namespace.key')` do SDK, com plurais e formatos regionais. A interface inteira se espelha para idiomas da direita para a esquerda, e a região (`LANG`/`LC_*`) é repassada aos apps Linux. `node tools/i18n-check.mjs` confere que toda chave existe em todos os idiomas e que nenhuma marca de terceiros aparece na interface.

### Arquivos-fonte

- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js), [sdk/index.js](https://github.com/lordbasex/LordOS/blob/main/sdk/index.js)
- [server/src/ui/js.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js.c), [server/src/ui/node.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/node.c), [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c), [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c)
- [server/src/ui/shape.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/shape.c), [server/src/ui/otf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/otf.c), [server/src/ui/bidi.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/bidi.c), [server/src/ime.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ime.c)
- [server/src/ui/js_sys.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_sys.c), [server/src/fsops.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fsops.c)

## 5. Aplicativos

### 5.1 Apps nativos

Trinta e um apps nativos vêm com o LordOS, todos apps React sobre o mesmo motor e o mesmo SDK: Arquivos, Ajustes, Terminal, Loja, Editor de Texto, Notas, Calculadora, Monitor, Visualizador de PDF, Fontes, Emoji e Símbolos, Scanner, Fotos, Player, Capturas de Tela, Câmera, Gravador, Senhas, Discos, Backups, Logs, Relógio, Ajuda, E-mail, Calendário, Contatos, Lembretes, LordOS Imager (0.8.4), Informações do sistema (0.8.5), Sobre e o instalador. Desde a 0.8.5 o manifesto de cada app traz sua própria versão (todos começam na 0.8.5; o build recusa um app sem uma versão válida), mostrada em Loja → Instalados e na janela Sobre do app, para que os apps possam depois ser lançados separadamente do LordOS. Alguns destaques:

- **Arquivos:** desde a 0.8.5 a barra lateral tem **Este computador**, para navegar por todo o sistema só para leitura como o usuário (as permissões do próprio sistema decidem; `/proc`, `/sys`, `/dev` e `/run` aparecem com um cadeado e não são abertos), e cada disco com um sistema de arquivos: um que não está montado aparece esmaecido e um clique o monta e abre (um disco interno pede um administrador, como em Discos), e um removível pode ser ejetado. As pastas em que o usuário não pode escrever não mostram ações que as mudariam (§4.3).

- **Terminal:** emulador compatível com xterm escrito pelo LordOS em C, com abas, painéis divididos, perfis, imagens embutidas e Sixel, e sessões que sobrevivem a um reinício do `lord-server`.
- **Monitor:** CPU, memória, discos (com temperatura e saúde SMART, 0.8.2), processos (encerrar um processo, Obter informações, 0.8.1), refrigeração e conexões de rede por app num mapa-múndi, com o país consultado numa base local. A segunda geração de Monitor → Rede (0.8.5) é uma árvore de cada app até os sites, endereços e portas com que ele fala, com os bytes enviados e recebidos, um botão permitir/bloquear em cada destino, uma busca com chips `app:`, `domain:`, `ip:`, `port:`, `proto:` e `country:`, e um histórico de tráfego de uma hora a 30 dias. Ele lista sockets TCP, UDP, ICMP (ping), IP brutos e de pacotes sob o app que os abriu (um `ping` rodado no Terminal conta sob o Terminal), e avisa quando um programa pode ver ou forjar tráfego abaixo do TCP e do UDP. Um único botão de menu agrupa a vista por app, domínio, endereço, porta ou país e a filtra por direção, protocolo e estado da regra, lembrado por usuário. No mapa cada app tem sua própria cor, os arcos até um país continuam visíveis, pontos em movimento mostram a direção de qualquer tráfego (laranja saindo, verde entrando), e o mapa segue a busca e os filtros. A thread de amostragem só existe enquanto o app está aberto.
- **Informações do sistema** (0.8.5, Sobre o LordOS → Relatório do sistema…): cada peça de hardware e como o LordOS a enxerga; a placa de vídeo mostrada camada por camada (driver, saída de vídeo, nó de render, contexto de GPU, composição, scanout direto) com o motivo exato quando a tela é composta na CPU; um painel de Compatibilidade; os módulos de memória a partir das tabelas do firmware, por meio de uma ação administrativa só de leitura que nunca lê números de série; uma aba Relatório com o texto exato antes de ser copiado ou salvo (nada é enviado a lugar nenhum); e as Verificações, com **Testar** para o Teste do isolamento (§6.4, §11.4). O `lordos-sysreport report` imprime o mesmo relatório num terminal (em Markdown ou `--json`, sem números de série, endereços nem nomes), e o `lordos-fetch` mostra a coroa do LordOS ao lado de um resumo do computador e de como os apps são isolados (`--share` deixa de fora os nomes de usuário e de host).
- **Banco de testes** (`lordos-testbench`, 0.8.5): uma janela de padrão de teste, um cliente Wayland comum, que confere o servidor gráfico de fora: tempo e apresentação dos quadros, cada tecla, o mouse e a roda, os protocolos Wayland oferecidos (§3.4), uma cena 3D que diz se quem a renderiza é uma GPU real ou o processador, vídeo gerado a 24, 30 ou 60 quadros por segundo e a captura do ponteiro. As teclas que o LordOS reserva para si aparecem contornadas; seu último relatório aparece em Informações do sistema → Verificações.
- **LordOS Imager** (0.8.4): grava uma imagem do LordOS com a assinatura conferida num cartão SD ou pendrive USB, nunca num disco interno, a lê de volta para comparar e prepara o primeiro boot do Raspberry Pi 5 (nome, idioma, teclado, uma conta só com o hash da senha, Wi-Fi só com a chave derivada, SSH, área de trabalho remota, tema e refrigeração). O mesmo app existe para macOS, Linux e Windows.
- **Discos:** leitor SMART próprio do LordOS, o relatório SMART completo com smartmontools, formatação e pendrives inicializáveis verificados com SHA-256.
- **Backups:** snapshots com hard links e criptografia opcional, rodando com baixa prioridade.
- **O Dock** (0.8.1) é organizado pelo usuário, com apps fixados e as ações próprias dos apps; o tema padrão ganhou encaixe de janelas e uma visão de tarefas, e o tema **Lord Classic** virou uma segunda aparência completa (0.8.3, §8).

### 5.2 Apps `.lpk` instaláveis

Os apps nativos de terceiros são empacotados como `.lpk`: um tar comprimido com gzip contendo um manifesto, o `app.js` empacotado, um ícone obrigatório e traduções e recursos opcionais. Um app não traz React nem o SDK; usa os do sistema, o que o mantém pequeno (o app de exemplo Fractal pesa 5 KB). Eles são instalados sem root em `~/.local/share/lordos/apps/<id>/`. O instalador recusa links, dispositivos, caminhos absolutos, `..`, caracteres estranhos, mais de 2.000 arquivos ou 64 MB, e substitui a versão anterior de forma atômica. Cada um roda isolado (§6.4).

### 5.3 A Loja e os pacotes Debian

A Loja (redesenhada na 0.8.5) tem uma barra lateral em três grupos (Descobrir, Para este computador, Instalados e Atualizações; Inteligência Artificial e as dez categorias do Debian com suas contagens; apps do LordOS) e mostra os apps como cartões numa grade que preenche a janela: ícone, nome, categoria, duas linhas de resumo, o tamanho do download e um botão Obter, Abrir ou Ver. Descobrir abre com um banner em destaque, depois a prateleira de IA e uma prateleira por categoria; a página de um app tem uma linha de dados (versão, tamanho, categoria, distribuição). Ela mostra os apps do Debian por categoria, com busca, além de apps `.lpk` e arquivos `.deb` locais, sugestões para o hardware deste computador (firmware faltando, drivers de impressoras e scanners, ferramentas para máquinas convidadas, 0.8.2; com página própria, Para este computador, desde a 0.8.5) e as atualizações do próprio LordOS (0.8.1), e sempre mostra um plano antes de instalar ou remover. Os pacotes Debian são tratados pelo **motor de pacotes próprio do LordOS** (`server/src/pkg`): ele lê as fontes do apt, verifica as assinaturas do `InRelease` com código OpenPGP próprio (RSA e Ed25519), recusa índices expirados, reaproveitados (replay), de outra suíte ou com dados depois da assinatura, confere cada índice e cada `.deb` com os valores SHA-256 do índice assinado e resolve as dependências (planos idênticos aos do apt em 12 casos de teste). Só o último passo é do Debian: o `dpkg` desempacota e configura, e continua sendo o único que escreve no seu banco de dados, então o `apt` segue funcionando. Desde a 0.8.5, as fontes `https://` são baixadas por HTTPS através do `lord-tls`, um helper isolado que verifica a cadeia de certificados e o nome do servidor e só repassa bytes (§6); antes, eram baixadas por `http://`. A integridade continua dependendo das assinaturas e dos hashes nos dois casos; o HTTPS acrescenta privacidade (os nomes dos pacotes baixados deixam de ficar visíveis na rede) e recusa um servidor que não é quem diz ser, e um download seguro nunca é redirecionado para HTTP sem criptografia. O `lord-tls` é executado pelo processo filho de download (que roda como `nobody` no seu próprio escopo de download) com um caminho fixo, sem argumentos, com o ambiente vazio e o socket já conectado; ele aplica o sandbox dos ajudantes antes de ler qualquer coisa, usa TLS 1.2 ou superior com SNI, nunca interpreta HTTP e nunca decide nada sobre os pacotes. A atualização e as instalações da Loja o usaram na VM 116 e numa máquina VMware. Desde a 0.8.5 o motor também envia um `+` no nome de um pacote como `%2B`, como o apt, então pacotes como `kms++-utils` são baixados da CDN de lordos.org.

**Inteligência Artificial (0.8.5).** Uma categoria da Loja montada a partir de um catálogo curado no repositório (`apt/catalog.json`, com as chaves dos fabricantes em `apt/keys/` e as receitas de build do LordOS em `apt/recipes/`): nove apps para amd64 e arm64, cada um com seu tipo, fabricante, licença, resumo e uma fonte fixada; um app aparece esmaecido numa arquitetura que ele não suporta. Claude Code, Claude Desktop, Visual Studio Code e Cursor são instalados a partir do repositório APT assinado do próprio fabricante ("direto"; o `apt/build.sh check` confere a impressão digital fixada da chave, o `InRelease` assinado, o hash do índice e o pacote de cada arquitetura): a Loja primeiro diz de quem é o repositório que vai adicionar e a impressão digital da sua chave, e depois uma ação de administrador (`pkg-source-add`) o grava com a chave do fabricante, que precisa ter exatamente a impressão digital fixada no catálogo, e baixa as listas; da interface só vem o id do app, nunca uma URL, chave, nome de arquivo ou campo. O arquivo `.sources` (com `Signed-By` essa chave) e o chaveiro são arquivos de formato fixo chamados `lordos-<id>`, ou com os próprios nomes do fabricante quando o pacote dele sempre escreve a própria fonte (o `cursor.sources` do Cursor, porque uma segunda entrada para o mesmo repositório com outro `Signed-By` faz o apt parar de ler qualquer fonte); o `pkg-source-remove` remove o repositório junto com o app, a menos que outro app instalado do catálogo o use. Os pacotes dos fabricantes nunca são modificados; o que os scripts de manutenção deles fazem ao ser instalados (VS Code, Claude Desktop e Cursor adicionam a própria fonte do APT) está documentado em `docs/UPDATES.md`. llama.cpp 0.7.0, whisper.cpp 1.9.5 e codex-cli 0.164.0 são compilados pelo LordOS a partir de fontes oficiais verificadas, em contêineres `debian:trixie` sem rede (o codex-cli é o binário estático da OpenAI, com sua assinatura sigstore conferida com o cosign), e goose 1.56.0 e opencode 1.19.4 são os pacotes dos fabricantes copiados depois de conferir um SHA-256 fixado; os cinco são publicados no repositório assinado do LordOS, gerado e assinado na máquina do mantenedor. Um app que o LordOS compila e que as listas locais ainda não têm oferece baixá-las de novo. O llama.cpp é compilado com todas as variantes de CPU (a melhor é escolhida na execução) e um backend Vulkan carregado só quando existe um dispositivo Vulkan, e baixa os modelos do Hugging Face na primeira execução (`-hf`); nenhum modelo vem incluído. Testado numa máquina VMware: llama.cpp instalado pela Loja e `llama-server -hf ggml-org/Qwen3-0.6B-GGUF:Q8_0` respondeu pela sua API HTTP na CPU a cerca de 33 tokens/s (sem dispositivo Vulkan); numa GPU ainda não foi testado em hardware real (só foram conferidos o caminho pela CPU e o carregamento do backend com Vulkan por software). Na VM 116, as fontes do Cursor e do VS Code foram adicionadas, o Cursor foi instalado (o próprio script dele reescreveu o mesmo arquivo), o `apt update` e a atualização da própria Loja continuaram limpos, e ele foi removido.

**Comerciais (0.8.5).** Apps gratuitos sem código-fonte público, dos repositórios APT assinados dos seus fabricantes, com a chave que o LordOS fixa: Chrome (amd64 e arm64), Edge e Spotify (amd64). A página diz, antes de qualquer instalação, que o LordOS não pode revisar nem corrigir o que eles fazem. Os pacotes deles escrevem a própria fonte do APT, então o catálogo indica os arquivos que o Chrome e o Edge escrevem (`google-chrome.sources`, `microsoft-edge.sources` e seus chaveiros) e o LordOS escreve exatamente esses; para o Spotify, que adiciona `spotify.list` a menos que esse arquivo exista, um campo `optout` do catálogo faz o `pkg-source-add` criá-lo vazio (só quando ele não existe; o `pkg-source-remove` só o apaga enquanto continua vazio). Testado numa máquina VMware com a ação administrativa e o motor de pacotes: as três fontes adicionadas, os três apps instalados (Chrome 156.0.8078.47, Edge 156.0.4314.50, Spotify 1.2.95), `apt update` sem nenhum erro nem aviso e a atualização do motor limpa depois, e a janela do Chrome abrindo na sessão. O pacote do Spotify também copia sua chave para `/etc/apt/trusted.gpg.d/`, na qual o apt confia para qualquer fonte sem `Signed-By` (as fontes do próprio Debian têm um). O Steam precisa do multiarch i386, que o motor de pacotes ainda não faz.

**O banner em destaque (0.8.5).** O banner de Descobrir pode mudar sem um novo lançamento: `apt/featured/featured.json` (um objeto JSON de esquema 1 de no máximo 64 KB) e `banner.png` (um PNG de no máximo 2 MB) são conferidos pelo `apt-repo.sh` e publicados assinados com os índices (`main/featured/`). O motor de pacotes só os aceita de uma fonte cujo Release assinado diz `Origin: LordOS`, confere seus hashes, interpreta o JSON e a assinatura do PNG, e os guarda em `/var/lib/lordos/pkg/`; a interface os lê com `lord.pkg.featured()`, só para leitura. Enquanto a imagem não chega, a Loja desenha um esqueleto do banner (que pulsa, a menos que Reduzir movimento esteja ligado), e um repositório sem banner mostra um app do catálogo no lugar. Testado na VM 116 contra lordos.org/apt-staging.

### 5.4 Apps Linux

Apps Linux com um arquivo `.desktop` aparecem no lançador de apps. A abertura passa por `lord.apps.launch(id)`: a interface só pode indicar um arquivo `.desktop` instalado; o `lord-server` o relê, confere, monta o `argv` a partir da linha `Exec` conforme a especificação e o executa com `execve`, sem shell nem argumentos de quem chamou, dentro do seu próprio escopo de cgroup pertencente ao root. Chromium e Firefox vêm pré-instalados na ISO e na imagem do Raspberry Pi; desde a 0.8.4 eles desenham uma única barra com suas abas e os botões de janela do LordOS (um botão em Ajustes → Aparência traz de volta a moldura do LordOS), e uma extensão opcional mantém o vídeo fluido sem decodificação por hardware (0.8.2). Apps que só falam X11 rodam pelo Xwayland (§3.4). **Os apps Linux não ficam em sandbox por padrão**: eles rodam como o usuário, como em qualquer Debian. Ganham atribuição, o firewall por app e as restrições por app que o dono definir (§6.6), incluindo uma visão opcional "sem acesso aos seus arquivos" aplicada pelo lançador. Desde a 0.8.5 um arquivo `.desktop` cujo nome tem um espaço ou outro caractere incomum (o do LordOS Imager é `LordOS Imager.desktop`) recebe um id com esses caracteres trocados por `-` e é iniciado a partir do seu arquivo real, e **Forçar saída…** (o menu da coroa do Lord Flow) encerra só o próprio processo de um app Linux ou isolado, por meio de `lord.wayland.kill`, que só aceita um cliente Wayland do mesmo usuário (a um app X11 se pede, em vez disso, que feche).

### 5.5 WebAssembly

O LordOS inclui seu próprio interpretador WebAssembly em C (`server/src/wasm`), exposto como o global padrão `WebAssembly`, para que um app nativo possa trazer módulos compilados de C, C++, Rust ou Zig. Ele cobre o WebAssembly 2.0 sem SIMD (o MVP mais extensão de sinal, conversões saturadas, multi-valor, memória em bloco e tipos de referência), passa em 26.290 de 26.290 asserções da suíte de testes oficial na VM e transforma todo erro num trap em vez de uma falha. No exemplo do projeto, ele rodou de 3 a 5 vezes mais rápido que o mesmo código em JavaScript.

### 5.6 Instalar o LordOS

- **ISO live** para PCs de 64 bits, com instalador gráfico (UEFI e BIOS).
- **Imagem para Raspberry Pi 5**, para cartão SD ou NVMe (§7.4), gravada e preparada com o LordOS Imager, que também grava um arquivo de primeira execução que a placa aplica uma única vez no primeiro boot (validado de forma rígida; nada é aplicado se alguma parte estiver errada).
- **Num Debian 13 já instalado**: `curl -fsSL https://lordos.org/install.sh | sh`. O script pergunta antes de mudar qualquer coisa, baixa o `SHA256SUMS` e verifica sua assinatura Ed25519 com a chave de release embutida no script, recusa continuar sem uma assinatura válida, confere o SHA-256 do `.deb`, instala com o apt e configura o sistema como a ISO. Os arquivos do release são publicados no GitHub Releases.

### Arquivos-fonte

- [apps](https://github.com/lordbasex/LordOS/tree/main/apps), [server/src/lpk.c](https://github.com/lordbasex/LordOS/blob/main/server/src/lpk.c), [tools/lpk.mjs](https://github.com/lordbasex/LordOS/blob/main/tools/lpk.mjs)
- [server/src/pkg](https://github.com/lordbasex/LordOS/tree/main/server/src/pkg), [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c)
- [server/src/wasm/wasm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wasm/wasm.c), [server/src/ui/js_wasm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_wasm.c)
- [tools/install/install.sh](https://github.com/lordbasex/LordOS/blob/main/tools/install/install.sh)

## 6. Segurança

### 6.1 Modelo de ameaças

O LordOS é projetado contra estas ameaças:

- **Um app instalado malicioso ou comprometido** que quer os arquivos, as chaves, o microfone, a câmera, a área de transferência ou a rede do usuário.
- **Documentos e mensagens maliciosos**: PDFs, arquivos compactados, fontes, imagens, vídeos e e-mails feitos para explorar um leitor.
- **Um app Linux que quer mandar dados para fora**, ou escapar das regras definidas para ele.
- **Um atacante na rede local** tentando usar a área de trabalho remota ou adulterar downloads.
- **Pacotes e releases adulterados.**

Fora do modelo, por enquanto: um kernel ou uma conta root comprometidos; ataques físicos; canais laterais; e apps Linux que o dono não restringiu, que rodam como o usuário, como em qualquer Debian, e podem ler os arquivos dele. Os apps Linux são confinados apenas pelas restrições que o dono define para eles (§6.6), e só quando são abertos pelo lançador de apps.

### 6.2 Privilégio mínimo

- O `lord-server` roda como o usuário. Numa sessão gerenciada, ele nunca segura os dispositivos de vídeo ou de entrada por conta própria: o `lord-sessiond` os empresta e os revoga.
- Bibliotecas que leem dados não confiáveis só são linkadas por ajudantes: FFmpeg, libva e os codificadores de vídeo pelo `lord-media`; Poppler e libarchive pelo `lord-doc`; OpenSSL pelo `lord-maild`, pelo `lord-fetch`, pelo `lord-tls` (HTTPS para os downloads do motor de pacotes: confere o certificado e o nome do servidor e só repassa bytes; sobre os pacotes quem decide continua sendo o índice assinado) e pelo `lord-sessiond`; libsrtp2 e usrsctp pelo `lord-sessiond`, que as usa só na ponte que roda como o usuário depois do login; Monocypher pelo `lord-vault`.
- Os ajudantes recebem arquivos e dispositivos como descritores abertos pelo `lord-server`; nunca abrem caminhos.
- O compositor nunca espera por um ajudante ou por um app, então uma falha ou um travamento fica contido.

### 6.3 Os processos root e suas interfaces fechadas

| Processo | Aceita | Nunca faz |
|---|---|---|
| ajudante `admin.c` | uma tabela fixa de ações, com argumentos validados como root | ações genéricas, texto do usuário num shell |
| `lord-sessiond` | pedidos de login da tela de login; JSON curto de sessão (≤ 4 KB) | deixar a tela de login verificar senhas; ler pixels como root |
| seu processo filho de posicionamento em cgroups | `enter <scope> <limits>` só para o próprio processo que pede, vindo da folha `server` de uma sessão e rodando um binário da pasta do `lord-server` | posicionar processos arbitrários; devolver a árvore ao usuário |
| `lord-netd` | ações `net-*` vindas apenas de pares root | ações que não são de rede; rodar qualquer coisa vinda de uma sessão |
| `lord-netperm` | `RELOAD` sem dados | rodar programas; receber argumentos; abrir sockets de internet |
| `lord-fand` | `STATUS` para qualquer um; `PROFILE`, `CURVE`, `FORCE` para administradores | indicar caminhos ou dispositivos; rodar programas; usar a rede |
| o processo filho de download da Loja | mover o próprio pid para o seu escopo de download do root, vindo da folha `server` da sua sessão, e depois baixar para `nobody` | posicionar qualquer outro processo |

A tabela do ajudante administrativo ganhou, na 0.8.5, só ações fechadas do mesmo tipo: `pkg-source-add` e `pkg-source-remove <id>` (§5.3: da interface só vem um id do catálogo; a entrada, a impressão digital da sua chave e cada campo são conferidos como root contra o catálogo pertencente ao root que vem no pacote lordos), `ssh-root`, `ssh-root-key`, `ssh-root-password` e o `ssh-root-status`, só de leitura (§3.6: só administradores, cada mudança com a senha do próprio administrador conferida como root), e o `hw-memory-modules`, só de leitura (Informações do sistema: os dispositivos de memória do SMBIOS interpretados de forma rígida em C, nunca números de série, etiquetas de patrimônio nem números de peça).

### 6.4 O que é isolado, e o sandbox de apps

Nem todo programa no LordOS fica em sandbox. Isto é exatamente o que vale para cada tipo:

| Tipo | Processo | Sandbox | Escopo de cgroup | Broker e permissões | Firewall e restrições (§6.6) |
|---|---|---|---|---|---|
| Apps `.lpk`, e Senhas, E-mail, Calendário, Contatos e Lembretes (0.8.2) | próprio (`lord-server --app <id>`) | Landlock, seccomp, sem capacidades | próprio, do root, com limites | cada chamada é conferida em C; os quatro apps de PIM têm só os seus próprios pedidos de e-mail, de uma lista permitida | regras de rede e as oito restrições que não são de rede, no broker |
| Os demais apps incluídos | dentro do `lord-server` | não (fazem parte do desktop) | o do `lord-server` | sem broker; câmera, microfone e tela são conferidos em C | como "LordOS": regras de rede; câmera, microfone, tela e notificações |
| Apps Linux pelo lançador de apps | próprio | nenhum por padrão; com "sem acesso aos seus arquivos", namespaces próprios de usuário, montagem e PID mais Landlock, a partir da próxima abertura | próprio, do root | não | regras de rede; câmera, microfone, USB, sockets Bluetooth, arquivos e notificações; não a área de transferência |
| Programas do Terminal | filhos do shell do Terminal | não | o escopo de componente do próprio Terminal (0.8.5) | não | como "Terminal": regras de rede, com os nomes dos programas no Monitor |
| Apps X11 | sob o Xwayland | como a sua entrada no lançador | próprio, como os apps Wayland | não | o mesmo que os apps Linux |

Um programa iniciado de outro jeito, fora do lançador de apps, não está em nenhum escopo de app e não recebe regras por app. Flatpak e bubblewrap não são usados.

Cada app `.lpk`, e o app Senhas, roda como `lord-server --app <id>`: o mesmo binário em "modo app", iniciado pelo `lord-server` com um id de pacote validado como único argumento. O processo se conecta ao compositor como cliente Wayland e abre o socket do broker, depois aplica o sandbox e só então carrega o código do app (`server/src/sec/sandbox.c`):

- **Landlock** (ABI até 6): acesso somente leitura ao pacote, ao runtime compartilhado de apps, às fontes e às bibliotecas; leitura e escrita só em `~/.local/share/lordos/appdata/<id>`; escopo restrito para sockets Unix abstratos e sinais.
- **seccomp-bpf**: uma lista de chamadas de sistema permitidas, em todas as threads (TSYNC), sem `ptrace`, `mount`, `kexec`, `bpf`, `perf_event_open`, `keyctl`, carregamento de módulos ou `execve`. Novos sockets Unix e `socketpair` são recusados, porque o Landlock até a ABI 6 não filtra `connect` para sockets Unix com nome; isso mantém o app longe do barramento da sessão, do serviço de impressão e do socket Wayland.
- **Sem privilégios**: `no_new_privs`, um conjunto de capacidades vazio e o bounding set descartado.
- **cgroup v2**: `app-lordos-lpk-<id>.scope`, criado pelo root, com `memory.high` de 192 MB, `memory.max` de 512 MB e um núcleo de CPU.
- **Rede**: sem a permissão `network` (declarada **e** permitida), o seccomp recusa sockets de internet e o Landlock recusa connect e bind TCP. Namespaces de rede não são usados, porque criar um exige privilégios que o `lord-server` não tem.

Se faltar um recurso do kernel, o app inicia mesmo assim e o log diz qual parte está faltando. O app de teste propositalmente malicioso (`tests/lpk-evil`) falha em tudo o que tenta; o `build/sandbox-test` passa em 14 de 14 verificações para apps e 14 de 14 com rede. Medido numa VM: cerca de 13,7 MB de RSS por app isolado, inicialização mediana de 106 ms e 0,89 ms para aplicar o sandbox.

**No próprio computador do usuário (0.8.5).** Informações do sistema → Verificações → **Testar** inicia o pacote oculto do sistema `sysprobe` do jeito que qualquer app isolado inicia; em modo app, só para esse pacote, o `server/src/app/probe.c` assume o mesmo sandbox e tenta o que precisa falhar: a pasta pessoal, `/etc/passwd`, o `cgroup.procs` do cgroup raiz, um socket de internet e um Unix, `ptrace`, um namespace de usuário, `mount` e `exec` (a própria pasta de dados precisa funcionar, como controle). Um processo filho criado com fork antes do sandbox assume o perfil dos ajudantes de decodificação e tenta um socket, `exec`, `fork` e arquivos; e o teste pede ao seu broker o que precisa ser recusado (um caminho fora da sua pasta, a área de transferência sem permissão, a chave de segurança, o e-mail). O `lord-server` acrescenta o que só ele pode ler: `NoNewPrivs`, `Seccomp` e `CapEff` do processo e o `memory.max` do seu cgroup. Na VM 116 todas as tentativas foram recusadas (9 + 4 + 4), com seccomp no modo 2, sem novos privilégios e sem capacidades. O teste nunca recebe uma permissão nem uma liberação de rede.

### 6.5 O broker, as permissões e o diálogo de arquivos

[[diagram:permission]]

O app isolado só fala com o `lord-server` por um socket `SOCK_SEQPACKET`. Cada pedido é conferido em C: acesso a arquivos só dentro da pasta de dados do próprio app; leitura da área de transferência só com a permissão `clipboard`; pedidos de permissão respondidos pela decisão salva ou, se não houver, por um **aviso desenhado pelo sistema** (*Permitir / Não permitir*); câmera e microfone só através do `lord-server`.

- **As decisões** ficam salvas por usuário em `~/.config/lordos/permisos.json`, fora do alcance do app. A consulta é O(1).
- **O diálogo de arquivos é o powerbox próprio do LordOS** (`sdk/powerbox.jsx`), não o `xdg-desktop-portal`: o `lord-server` o desenha, abre o arquivo escolhido e passa só aquele descritor. A única interface de portal de desktop que o LordOS implementa é `org.freedesktop.portal.Settings`, para o tema do GTK.
- **Microfone:** o `lord-server` é dono do fluxo do PipeWire e dá ao app um anel compartilhado somente leitura.
- **Câmera:** o `lord-server` abre o dispositivo e o entrega só ao `lord-media`; o app recebe uma visão somente leitura dos quadros decodificados e precisa confirmar cada um, e o `lord-server` valida cada confirmação.
- **Revogação:** negar a câmera ou o microfone nos Ajustes os interrompe na hora (o `lord-media` do app é encerrado; o fluxo do PipeWire é fechado). A permissão `network` é decidida na inicialização.
- **Chaveiro:** os pedidos ao chaveiro de um app isolado levam um dono que o `lord-server` define a partir do id verificado do app.

**Restrições valem mais que permissões.** Uma restrição do firewall (§6.6) vence qualquer decisão salva: o pedido é recusado sem aviso, mesmo que o app já tenha tido permissão, e restringir um app em execução tira dele a câmera e o microfone na hora, como revogar.

**Limites.** Não há "permitir uma vez". Algumas permissões declaráveis (Bluetooth, abrir apps, notificações) ainda não têm chamada no broker, então os apps isolados simplesmente não podem usar esses recursos; escutar numa porta depende da permissão de rede. Um app Linux rodando como o usuário poderia editar o arquivo de permissões, a menos que esteja restrito com "sem acesso aos seus arquivos".

### 6.6 O firewall por app, suas restrições e o Monitor

[[diagram:firewall]]

O `lord-netperm` aplica regras por app no kernel. Os apps Linux abertos pelo lançador de apps rodam em `app-lordos-<id>.scope`, os apps `.lpk` em `app-lordos-lpk-<id>.scope`, e o resto da sessão conta como `lordos`. Todos os escopos pertencem ao root. O serviço carrega sete programas de rede (`connect4/6`, `sendmsg4/6`, `bind4/6`, `sock_create`), um programa de dispositivos (`cgroup/dev`) e um programa LSM por cgroup sobre `socket_create` no cgroup do `lordos.service`, descobre o app de cada processo pelo seu ancestral de cgroup e devolve permitir ou negar (`EPERM`). Os programas são compilados dentro do binário e carregados com o carregador próprio do LordOS pela chamada de sistema `bpf()`, que também lê o BTF do kernel para achar o gancho LSM; libbpf e bcc não são usados. Se o kernel recusar o programa de dispositivos ou o LSM, os programas de rede continuam funcionando e o serviço avisa.

- **As regras** aceitam domínios exatos, curingas, redes IPv4/IPv6, `lan`, `internet`, portas, protocolos e direção. As restrições (sem internet, sem LAN, sem rede, sem conexões de entrada) vêm primeiro; depois vence a regra mais específica, com negar ganhando de permitir em especificidade igual; depois os apps confiáveis; depois o modo (silencioso, o padrão, ou desligado). O `npcompile.c` pré-calcula isso em mapas de hash e de maior prefixo correspondente. Um teste roda o código de decisão do kernel contra o decisor de referência em 480.000 conexões aleatórias: 0 diferenças.
- **Os nomes de domínio** vêm só do resolvedor do sistema: a interface de monitoramento do systemd-resolved ou, sem ele, respostas da porta 53 dos servidores configurados, lidas por um socket de pacotes que só recebe. Respostas que um app recebeu nos próprios sockets nunca são levadas em conta. Por isso, um app que resolve nomes por conta própria com DNS criptografado (DNS sobre HTTPS dentro de um navegador, por exemplo) não é alcançado pelas regras de domínio: uma regra "permitir só example.com" continua bloqueando-o, mas uma regra "bloquear example.com" não o impede. As regras por endereço e por rede, o padrão do app e suas restrições continuam valendo, e os Ajustes dizem isso abaixo das regras.
- **As regras na interface.** Ajustes → Privacidade → Regras de rede por app mostra o padrão de cada app ("Permitir tudo" ou "Bloquear tudo exceto as regras") e suas regras na ordem em que vencem, com quando cada uma coincidiu pela última vez; a folha que adiciona uma regra confere o que é digitado enquanto se digita. Em Monitor → Rede, "Permitir / Bloquear este domínio" num destino de um app abre a mesma folha, preenchida com o nome que o resolvedor do sistema deu para aquele endereço.
- **Fugas.** A árvore de cgroups pertence ao root, então um app não consegue se mover. Um processo que pede ao gerenciador systemd do usuário uma nova unidade (`systemd-run --user`) cai sob `user@<uid>.service`; o `lord-netperm` anexa os mesmos programas ali e submete esses processos à união mais estrita das restrições dos apps restritos daquele usuário em execução. Testado: todas as tentativas de fuga negadas, outros usuários não afetados.
- **Custo**: cerca de 0,5 µs por connect UDP, TCP dentro do ruído, nenhuma mudança na vazão, já que nada roda por pacote.
- **O Monitor** mostra as conexões de cada app (a partir de `sock_diag`, mais os sockets ICMP, brutos e de pacotes a partir de `/proc/net`) e os destinos que o firewall informou há pouco, os bytes, os destinos por país a partir de uma base local e um botão de Regra por app. Nenhum endereço sai da máquina para consultas: sem DNS reverso, sem geolocalização online.
- **Conexões abertas (0.8.3).** As regras valem para as conexões já abertas, não só para as novas: os programas também descartam os pacotes que uma regra agora nega, e o `lord-netperm` destrói esses sockets via `sock_diag` (`SOCK_DESTROY`), então um app bloqueado vê suas conexões terminarem numa fração de segundo (0,04–0,37 s para TCP e 0,08 s para QUIC num Pi 5 com o kernel do LordOS).
- **Os componentes do próprio LordOS (0.8.5).** A sincronização de e-mail e calendário, a Loja e as atualizações, as impressoras e scanners, o Terminal e as atualizações das listas de bloqueio rodam cada um num escopo próprio, então cada um pode ser visto e bloqueado separadamente; seus sockets são abertos por um pequeno conector que o `lord-server` reexecuta com argumentos fixos, que entra no escopo do componente e devolve só um socket conectado. Um conector (`lord-server --net-connector mail|print|lists`) tem uma lista de argumentos e um ambiente fixos, `no_new_privs`, não pode ser despejado (dump), só responde pedidos de tamanho fixo (família, tipo, endereço, porta), sem nomes, caminhos nem execução, e falha fechado: se não consegue entrar no seu escopo, ele sai e o pedido falha. O processo filho de download da Loja entra no seu próprio escopo no mesmo espírito antes de baixar para `nobody` (§6.3). A resolução de nomes, a sincronização de horário, o serviço de rede e os túneis VPN são mostrados e nunca bloqueados; bloquear a Loja pede confirmação antes, porque as atualizações de segurança param.
- **Listas de bloqueio e "negar em silêncio" (0.8.5).** Listas públicas de sites de publicidade, rastreadores, malware e ataques (cada uma com sua fonte e sua licença) são baixadas por HTTPS uma vez por dia pelo `lord-fetch`, um ajudante sem sockets próprios e com o certificado conferido contra as autoridades do sistema, e cada linha é validada de novo; uma regra que o usuário escreve para um site sempre vence uma lista. Um novo modo do firewall, "negar em silêncio", deixa conectar só o que uma regra permite e registra o resto, enquanto os serviços do próprio computador seguem funcionando. Um item opcional da barra de menus mostra as taxas de tráfego e o que foi negado há pouco.

**Restrições além da rede.** Além das quatro restrições de rede (sem internet, sem LAN, sem rede, sem conexões de entrada), o dono pode restringir de um app a câmera, o microfone, a tela, o USB, os arquivos, a área de transferência, o Bluetooth e as notificações. Cada uma é aplicada onde o recurso é entregue, conferida contra o escopo do app e nunca contra um nome que o app dá a si mesmo, e vence qualquer permissão concedida em `permisos.json`.

[[diagram:restrictions]]

| Restrição | Apps `.lpk` e Senhas | Apps incluídos ("LordOS") | Apps Linux pelo lançador de apps |
|---|---|---|---|
| câmera | broker (`brokercam.c`) | `js_media.c` | `cgroup/dev` nos nós V4L2, e o guarda do PipeWire |
| microfone | broker | `js_media.c` | o guarda do PipeWire, e `cgroup/dev` nos nós de captura ALSA |
| tela | broker (sem API de captura no modo app) | `js_media.c` | por projeto: sem protocolo de captura, sem servidor X, sem portal de captura de tela; `/dev/fb*` negado |
| USB | broker (a chave de segurança de Senhas) | não oferecida | `cgroup/dev`: nós USB diretos, hidraw e serial USB |
| arquivos | broker (seletor de arquivos, gravações da câmera) | não oferecida | o lançador: namespaces próprios, uma home em tmpfs e Landlock, a partir da próxima abertura |
| área de transferência | broker | não oferecida | não disponível |
| Bluetooth | broker (sem API de Bluetooth no modo app) | não oferecida | programa BPF LSM sobre `socket_create`: sem sockets `AF_BLUETOOTH` |
| notificações | a permissão `notifications` | `sdk/notify.js` | `notifyd`, pela identidade verificada de quem chama |

Para os apps isolados, o broker (`sec/restrict.c`) é o único ponto de aplicação: um pedido restrito é recusado sem aviso, e restringir um app em execução tira dele a câmera e o microfone na hora (`tests/broker_restrict_test.c` cobre cada restrição, com e sem ela). A linha "LordOS" é o próprio desktop (Arquivos, copiar e colar, Discos, os ajustes de Bluetooth), então ela não oferece arquivos, USB, área de transferência nem Bluetooth; em vez disso, restringem-se apps individuais. Para os apps Linux:

- **Dispositivos.** No cgroup v2, o controlador de dispositivos é um programa BPF. O do `lord-netperm` recusa `open()` e `mknod()` das classes de dispositivo que o app tem restritas (V4L2, USB direto, hidraw, serial USB, nós de captura ALSA, `/dev/fb*`) e permite todo o resto. Vale a partir do próximo `open`; um descritor já aberto continua utilizável.
- **Sockets Bluetooth.** O gancho de sockets inet nunca vê `AF_BLUETOOTH`, então um programa LSM por cgroup sobre `socket_create` os recusa.
- **Arquivos.** Logo antes do `execve`, o filho do lançador entra em namespaces próprios de usuário, montagem e PID, sem privilégios. A pasta pessoal é um tmpfs vazio com só as pastas próprias do app (derivadas do seu id de desktop, nunca de campos que um arquivo `.desktop` controla) e o arquivo com que ele foi aberto; a pasta de runtime tem só os sockets do Wayland e do som, então o barramento de sessão e o systemd do usuário ficam fora de alcance; depois o Landlock limita o resto. Vale a partir da próxima abertura. Apps GTK, Firefox e Chromium rodam sem suas integrações de D-Bus.
- **Notificações.** O `notifyd` pede ao barramento o PID de quem chama (só conta uma resposta enviada pelo próprio barramento), acha o escopo do app e descarta a notificação de um app restrito; um erro ou uma resposta atrasada também a descartam (falha fechada).
- **Microfone e câmera pelo PipeWire.** O guarda do PipeWire (`sec/pwguard.c`), o cliente de PipeWire próprio do `lord-server`, identifica cada cliente pelo PID que o PipeWire tirou das credenciais do socket e, para um app restrito, esconde os nós de captura e destrói seus streams de captura em menos de um segundo, incluindo uma gravação já em andamento. Enquanto um app restrito roda, um cliente cuja identidade não pode ser estabelecida é tratado como restrito.

**Limites.** O modo "perguntar" está projetado, mas hoje se comporta como o silencioso. O ICMP só é bloqueado por "sem rede"; conexões a uma porta já aberta não são conferidas. As restrições para apps Linux não cobrem:

- o BlueZ via D-Bus: um app com o Bluetooth restrito ainda pode pedir ao `bluetoothd`, pelo barramento do sistema, que procure, pareie e conecte;
- o barramento do sistema, que continua ao alcance mesmo com "sem acesso aos seus arquivos";
- a área de transferência;
- apps iniciados fora do lançador de apps, que não estão em nenhum escopo de app;
- os efeitos colaterais da união do `user@`: enquanto um app restrito roda, os serviços do próprio usuário ficam sujeitos à mesma união (o áudio de chamadas de um headset, que o PipeWire abre a cada chamada, para durante esse tempo);
- uma janela de poucos milissegundos em que um cliente novo de PipeWire de um app restrito tem todas as permissões, ou em que um stream de captura pode receber áudio antes de ser destruído.

### 6.7 Ajudantes que processam dados não confiáveis

[[diagram:helpers]]

O `lord-doc` (PDF, arquivos compactados, fontes, imagens do scanner), o `lord-media` (vídeo, áudio, fotos, câmera), o `lord-maild` (e-mail, calendários, contatos), o `lord-vault` (segredos) e, desde a 0.8.5, o `lord-fetch` (as listas de bloqueio do firewall) e o `lord-tls` (HTTPS para o motor de pacotes) compartilham um único sandbox reforçado, `sandbox_apply_helper()`:

- sem `socket`, `socketpair`, `connect`, `bind`, `listen`, `accept`; sem `execve`; `clone` só para threads; seccomp em todas as threads, e se o seccomp não puder ser aplicado o ajudante encerra;
- arquivos só por descritores passados pelo `lord-server`; ioctls só por classe (DRM, V4L2, DMA-BUF), quando necessário;
- resultados devolvidos em memória compartilhada selada, validados pelo `lord-server`.

O `lord-media` nem sequer recebe o socket do PipeWire; o áudio passa por um anel compartilhado cujo fluxo pertence ao `lord-server`. O `lord-maild` não consegue criar sockets: ele pede ao `lord-server`, que só se conecta aos servidores do arquivo de contas (e, depois que o usuário pede para carregar imagens, só a endereços públicos nas portas 80/443 por dez minutos) e entrega o socket já conectado. O `lord-fetch` e o `lord-tls` também não têm sockets próprios: cada um recebe um socket já conectado (pelo conector `lists`, ou pelo processo filho de download da Loja), confere o certificado do servidor contra as autoridades do sistema e o nome do servidor com TLS 1.2 ou superior, e nunca é relaxado; o `lord-fetch` faz um único HTTP GET de no máximo 16 MB, interpreta a lista de forma rígida e devolve os redirecionamentos ao `lord-server` (no máximo três, só https), que valida cada linha de novo. O `build/sandbox-test helper` passa em 17 de 17 verificações.

### 6.8 Assinatura de pacotes e níveis de confiança

Os pacotes `.lpk` trazem um manifesto com o SHA-256 de cada arquivo e uma assinatura Ed25519 sobre ele. SHA-256 e Ed25519 são código próprio do LordOS (o Ed25519 segue a aritmética do TweetNaCl), testados com os vetores do NIST e da RFC 8032 (55 de 55). A verificação acontece em C na instalação e no carregamento. A Loja mostra o nível de confiança: **LordOS** (chave oficial, compilada dentro do binário para que nenhum arquivo possa acrescentar outra), **Autor verificado** (listado no catálogo assinado), **Assinado por <nome>** ou **Não assinado**, com um aviso. As atualizações precisam ser assinadas com a mesma chave. Um catálogo assinado traz uma lista de revogação. Se você editar um app instalado, o LordOS o marca como "Modificado por você", pode restaurar o original ou pode compartilhar sua versão assinada com sua própria chave, dando crédito ao original.

**Estado:** os pacotes `.lpk` oficiais ainda são assinados com uma chave de desenvolvimento, até a chave definitiva substituí-la. Um repositório de apps do LordOS assinado para apps `.lpk` ainda não foi construído; o próprio LordOS se atualiza a partir de um repositório APT assinado (§6.9).

### 6.9 Assinatura dos releases

Cada arquivo de release (o `.deb`, a ISO e a imagem do Raspberry Pi) aparece com seu SHA-256 no `SHA256SUMS`, e o `SHA256SUMS` é assinado com a chave de release do LordOS (Ed25519). A chave pública é `tools/release/lordos-release.pub.pem`, também embutida no instalador para Debian. Sua impressão digital (SHA-256 da chave pública em DER) é:

`c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

A chave privada fica só na máquina do mantenedor, nunca no repositório nem numa máquina de build. Cada release também traz uma lista de materiais de software (SBOM) SPDX 2.3 por arquitetura, listada no `SHA256SUMS` e portanto coberta pela assinatura, e suas notas saem do `CHANGELOG.md`, mantido no formato Keep a Changelog. A §11 mostra como conferir um download.

- **O repositório APT (0.8.1).** `lordos.org/apt` é um repositório APT assinado, gerado e assinado só na máquina do mantenedor; os sistemas instalados se atualizam a partir dele. No Raspberry Pi 5 ele também espelha o atualizador do bootloader e os poucos pacotes do Raspberry Pi de que precisa, copiados só depois que a assinatura deles e o SHA-256 de cada arquivo batem com uma lista fixada (0.8.4.1).
- **O código do próprio LordOS é verificado antes de rodar (0.8.2).** Todo bundle JavaScript que o LordOS roda como código próprio (os temas, o host de apps, os apps do sistema) só é carregado por `codesig_load`, que o confere contra `code-manifest.json` e sua assinatura Ed25519 com a chave de releases. Um bundle modificado não é carregado e o usuário vê o porquê; as Verificações das Informações do sistema o informam (§11.4). Builds de desenvolvimento sem um manifesto assinado avisam isso.
- **Os apps do sistema não podem ser removidos** pela Loja (0.8.2).

### 6.10 O cofre

O `lord-vault` guarda as senhas e o chaveiro da sessão em arquivos `.lvault`: uma chave de cofre aleatória de 32 bytes, envolvida uma vez por método de desbloqueio (senha com Argon2id, com custo calibrado por máquina; FIDO2 `hmac-secret` com o cliente CTAP2 próprio do LordOS; arquivo-chave; senha mais arquivo-chave; chave de recuperação); conteúdo selado com XChaCha20-Poly1305, tendo o cabeçalho inteiro como dados associados; anexos selados separadamente; gravações atômicas com backup. Todo byte invertido e todo truncamento são detectados (1.165 de 1.165 no teste). Os segredos ficam numa área de memória travada com `mlock`, excluída dos core dumps e apagada ao ser liberada, num processo que não pode ser despejado (dump). A janela de Senhas roda no seu próprio processo em sandbox, então os valores revelados nunca entram no JavaScript do desktop. O chaveiro abre no login com a senha que o `lord-sessiond` verificou e atende os apps Linux pela implementação própria do Secret Service do LordOS. Desde a 0.8.5 a sessão só começa quando o barramento de sessão do usuário existe (§3.6), então `org.freedesktop.secrets` e `org.freedesktop.Notifications` pertencem à sessão do LordOS desde o primeiro segundo; e quando um app puxa o gnome-keyring, um drop-in que vem no pacote lordos (`gnome-keyring-daemon.service.d/lordos.conf`) mantém só o seu componente PKCS#11, então o chaveiro do LordOS continua com o Secret Service e não aparece uma segunda janela "escolha uma senha para o novo chaveiro". Testado na máquina VMware depois de um reinício com login automático: os dois nomes pertencem à sessão do LordOS, e o Chrome pede pelo diálogo do próprio chaveiro do LordOS. Quando vários apps pedem ao mesmo tempo, uma única resposta no diálogo do chaveiro agora vale para todos eles. Chaves de segurança de hardware reais ainda não foram testadas.

### 6.11 Janelas protegidas

Uma janela marcada como protegida (como a de Senhas) é pintada de preto em todo caminho que copia pixels para fora: capturas de tela, gravações, miniaturas, a área de trabalho remota e os menus e a tela cheia do Wayland. O monitor real a mostra normalmente. Os apps Linux nem têm protocolo de captura de tela, para começo de conversa. Desde a 0.8.2, as teclas digitadas nos diálogos do próprio LordOS (o chaveiro, as senhas de administrador) nunca chegam ao app Linux que está à frente.

### 6.12 Divulgação e normas

Vulnerabilidades são relatadas como descreve o `SECURITY.md`, com um contato legível por máquina em `https://lordos.org/.well-known/security.txt` (RFC 9116). Os processos do LordOS estão **alinhados com, não certificados em**, ISO/IEC 29147 e ISO/IEC 30111 (divulgação e tratamento de vulnerabilidades), ISO/IEC 5230 (conformidade de licenças do OpenChain: o inventário em `THIRD_PARTY.md`, os avisos de licença e o SBOM) e ISO/IEC 18974 (garantia de segurança do OpenChain: o SBOM mais as atualizações de segurança do Debian para os pacotes de que o LordOS depende). O lordos.org publica as páginas legais e de confiança (política de segurança, assinatura de código, licenças, privacidade, ciclo de suporte, normas) e diz o que ainda falta: sem autocertificação formal, sem cruzamento automático do SBOM com bases de vulnerabilidades, sem SBOM das imagens completas e sem auditoria de terceiros.

### Arquivos-fonte

- [server/src/sec/sandbox.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sandbox.c), [server/src/sec/brokersrv.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokersrv.c), [server/src/sec/perms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/perms.c), [server/src/sec/cgroup.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/cgroup.c)
- [server/src/netperm/np.bpf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/np.bpf.c), [server/src/netperm/netperm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/netperm.c), [server/src/fw/rules.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fw/rules.c)
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c), [server/src/sec/pwguard.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/pwguard.c), [server/src/hw/notifyd.c](https://github.com/lordbasex/LordOS/blob/main/server/src/hw/notifyd.c), [server/src/apps.c](https://github.com/lordbasex/LordOS/blob/main/server/src/apps.c), [server/tests/broker_restrict_test.c](https://github.com/lordbasex/LordOS/blob/main/server/tests/broker_restrict_test.c)
- [server/src/sec/sign.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/sign.c), [server/src/pkg/crypto.c](https://github.com/lordbasex/LordOS/blob/main/server/src/pkg/crypto.c), [tools/release/sign-release.sh](https://github.com/lordbasex/LordOS/blob/main/tools/release/sign-release.sh), [tools/release/make-sbom.sh](https://github.com/lordbasex/LordOS/blob/main/tools/release/make-sbom.sh)
- [server/src/vault/vault.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vault.c), [server/src/vault/vfile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vfile.c)
- [server/src/media/capture.c](https://github.com/lordbasex/LordOS/blob/main/server/src/media/capture.c)
- [SECURITY.md](https://github.com/lordbasex/LordOS/blob/main/SECURITY.md), [docs/COMPLIANCE.md](https://github.com/lordbasex/LordOS/blob/main/docs/COMPLIANCE.md), [CHANGELOG.md](https://github.com/lordbasex/LordOS/blob/main/CHANGELOG.md)

## 7. Desempenho

Todos os números abaixo foram medidos pelo projeto e estão documentados nos arquivos citados. A maioria vem de máquinas virtuais; as medições em hardware real estão indicadas como tal.

### 7.1 Memória e repouso

**Duas grandezas diferentes.** As primeiras linhas medem **o sistema inteiro**: a coluna "used" do `free`, que é tudo o que ocupam o kernel, os serviços do Debian (systemd, journald, udev, a rede, PipeWire, CUPS…), os serviços root do LordOS e a sessão do usuário, sem o cache de páginas que o kernel pode devolver. As outras linhas são a **memória residente (RSS) de um processo**, como `ps` ou `/proc/<pid>/status` a informam. Um RSS não é o consumo do sistema inteiro, e RSS não podem ser somados: as páginas das bibliotecas compartilhadas são contadas em cada processo que as mapeia. Para comparar o LordOS com outra área de trabalho, compare o "used" do `free` na mesma máquina, depois do mesmo tempo em repouso e com os mesmos apps abertos; a divisão justa das páginas compartilhadas por processo é o PSS (`/proc/<pid>/smaps_rollup`). Como repetir as duas medições, e os resultados de cada versão, estão em §11.5.

| Medição | Valor | Onde |
|---|---|---|
| Sistema inteiro, ISO live, em repouso depois de um minuto (4 GB, 4 núcleos) | 465 MB em uso, CPU 99 % ociosa | VM 117, `docs/ISO.md` |
| Sistema inteiro, instalado a partir da ISO, em repouso | 487 MB em uso, CPU 99 % ociosa | VM 120, `docs/ISO.md` |
| Boot, ISO live com UEFI | 16,3 s (4,9 s de kernel + 11,4 s de espaço de usuário) | VM 117, `docs/ISO.md` |
| `lord-server` sem placa com 3D (Mesa não carregado) | cerca de 32 MB de RSS | `docs/GPU.md` |
| Uma sessão de usuário (`lord-server`), testes de área de trabalho remota | cerca de 50 MB; tela de login 41 MB | VM 118, `docs/REMOTE.md` |
| Um app `.lpk` isolado | 13,7 MB de RSS | VM 122, `docs/PRIVACY.md` |
| Sistema inteiro, 0.8.5~rc1 atualizado a partir da 0.8.4.1, um minuto depois do login (VM do VMware, 4,7 GB, GPU virtual) | 606–622 MB em uso; o PSS de todos os processos soma 316 MB, dos quais 110 MB são o `lord-server` da sessão e 78 MB o da tela de login (naquele build ela ficava para trocar de usuário e bloquear) | `results/0.8.5~rc1-beta/vmware-fusion` |
| A tela de login fechada depois do login (correção da 0.8.5, o mesmo tipo de VM do VMware) | sistema inteiro de 603 → 548 MB em uso | `CHANGELOG.md`, 0.8.5 |
| Sistema inteiro, 0.8.5-beta atualizado a partir da 0.8.4.1, cerca de cinco minutos depois de um reinício com login automático (a mesma VM do VMware, composição na GPU com SVGA3D; Chrome, VS Code, Spotify e llama.cpp instalados, sem rodar) | 592 MB em uso | `results/0.8.5-beta/vmware-fusion` |
| Boot, a mesma máquina com a 0.8.5-beta | primeiro quadro no monitor 7,4 s depois do início do kernel (`systemd-analyze`: 3,3 s de kernel + 4,3 s de espaço de usuário) | `results/0.8.5-beta/vmware-fusion` |

### 7.2 Desenho

- **Vblank por software (0.8.5).** Uma placa virtual que completa as trocas de imagem na hora (`vmwgfx` do VMware) fazia o compositor e seus apps desenharem 500–580 quadros por segundo numa tela de 60 Hz; o LordOS agora detecta isso e marca o ritmo da atualização com uma grade fixa: 59,8 quadros por segundo, p99 de 18,4 ms, nenhum quadro fora do tempo (`results/0.8.5~rc1-beta/vmware-fusion`). Na 0.8.5-beta lançada, o Banco de testes com sua vista 3D na mesma máquina mediu 59,83 quadros por segundo ao longo de 1.796 quadros, p99 de 17,9 ms, um quadro fora do tempo, 1.795 apresentados (`results/0.8.5-beta/vmware-fusion`). A mesma grade também marca o ritmo das sessões sem monitor, então esperar em milissegundos inteiros não transforma mais 60 quadros por segundo em 57.
- **Arrastar pelo caminho da GPU (correção da 0.8.5).** Uma janela arrastada é pintada a partir de uma camada em cache; no caminho da GPU cada pixel totalmente transparente dessa camada era tomado pelo buraco de uma janela Linux, e agora só os pixels dentro de uma superfície de app. Na 0.8.5-beta ainda podiam aparecer pequenos quadrados pretos nos cantos arredondados de uma janela arrastada, nos caminhos da CPU e da GPU: ao restaurar os cantos, a camada tomava o pixel guardado como opaco. Na 0.8.6 (em desenvolvimento) ela mistura os dois pixels com o alfa, então a camada fica transparente fora da curva; um teste compara cada canto com uma repintura completa nos dois caminhos (§10.2).
- **Um quadro de arrasto mais barato (0.8.6, em desenvolvimento).** O quadro de uma janela arrastada agora pinta só a parte da sua camada que mudou e só a parte alterada do que está embaixo dela, com os mesmos pixels: 2.417 hashes de quadros registrados por caminho têm de bater, e a única mudança aprovada deixa os cantos arredondados sob a janela um nível mais perto de uma repintura completa. No caminho da GPU a própria janela pode ser composta pela GPU com a aritmética exata da CPU, depois de um autoteste de cada combinação de pixels ("GPU drag"), e o quadro seguinte pode ser pintado enquanto a troca de página anterior está pendente; as duas coisas foram medidas no Raspberry Pi 5 e estão desligadas por padrão. Os resultados no Raspberry Pi 5 estão em §7.4.
- Arrastar uma janela numa tela de 1280×800 pelo caminho da CPU: cerca de 6,7–6,9 ms de trabalho por quadro, por volta de 120 quadros por segundo (VM 118, `docs/DISPLAYS.md`).
- A mesma janela numa tela de 2560×1600 a 150 %: cerca de 5,1 ms por quadro.
- Vídeo: pintar direto do quadro decodificado quando só um vídeo opaco mudou levou o `lord-server` de 42 % para 13 % de um núcleo com um vídeo 1080p em janela, e de 92 % para 15 % em tela cheia (`docs/ROADMAP.md`, mídia).
- O Monitor fechado não custa nada: a thread de amostragem não existe até ele ser aberto (`docs/MONITOR.md`).
- **O caminho pela CPU na 0.8.5** (VM 116, sem GPU, tela composta na CPU, vista pela área de trabalho remota num navegador com uma tela de 60 Hz; `docs/REMOTE.md`, "Measurements (0.8.5, CPU reference)"): o desfoque do fundo de uma janela (raio 20) passou de 27,7 ms para 6,0 ms com uma thread e 2,7 ms com oito, com pixels idênticos; uma cena de teste de 1280×800, de cerca de 18 para 55–58 quadros por segundo; uma sessão de 1920×1080 tocando um clipe de 60 fps, de 45 (um núcleo a 100 %) para 55–58 quadros por segundo com o pool de threads. Um vídeo 1080p a 24 fps no navegador tocou em tempo real sem nenhum quadro descartado pelo player ao longo de 17 s. O codificador H.264 por software leva 12–13 ms por quadro com 1280–1568 px de largura, então chega no máximo a uns 75–80 quadros por segundo. AV1 a 60 fps continua sendo o caso difícil sem GPU: o navegador o decodifica por software na mesma máquina que compõe a tela.

### 7.3 GPU

Nas VMs só havia um renderizador por software disponível. Com ele, o caminho pela CPU sai mais barato que a composição "na GPU", e é por isso que o LordOS usa o caminho pela CPU quando o único renderizador é por software. O benefício real do caminho pela GPU (composição e apresentação quase de graça, tela cheia sem cópias) é esperado em placas reais, mas **hardware AMD e Nvidia real ainda não foi medido**. Desde a 0.8.5 duas ferramentas informam isso em qualquer máquina: as Informações do sistema mostram cada camada da GPU e o motivo exato quando a tela é composta na CPU, e o Banco de testes desenha uma cena 3D com EGL e diz se o renderizador é uma GPU de hardware, uma virtual ou por software (na VM 116: `llvmpipe`, por software), com seu tempo de desenho e se seus quadros chegam à tela sem cópia.

### 7.4 O Raspberry Pi 5

O Raspberry Pi 5 é a única placa suportada. Resultados numa placa real (16 GB, cartão SD, sem monitor salvo indicação), de `docs/RASPBERRY.md`:

| Medição | Valor |
|---|---|
| Compilar todos os binários do LordOS na placa | 1 min 02 s, 0 avisos |
| `make -C server test` | 23 aprovados, 0 falhas, 0 pulados |
| Cópia e máscara com NEON vs. C puro | 9,90 → 4,33 ms e 11,09 → 5,49 ms por tela (2,0–2,3×), idênticos bit a bit |
| Em repouso na tela de login, sem monitor | `lord-server` 36 MB de RSS, 0,6 % de CPU; sistema com 450 MB de 16 GB; 38–43 °C, sem limitação térmica |
| Gerar a imagem SD na placa | 8 min 18 s; 503 MB comprimida |
| Compilar o kernel do LordOS na placa | cerca de 50 min por variante |

**O kernel do LordOS.** O kernel do próprio Raspberry Pi não inclui o Landlock. Sem ele, o teste hostil do ajudante de documentos falha em 5 das suas 25 verificações de sistema de arquivos e o teste do sandbox não consegue rodar. O LordOS compila o kernel do Raspberry Pi a partir do código-fonte e da configuração padrão dele, mudando só três opções (Landlock ligado, a lista de LSMs, o sufixo de versão), e o fixa para que um kernel mais novo do repositório não o substitua. A imagem também ativa o cgroup de memória, que o Raspberry Pi desliga no boot.

**Ventoinhas.** No Pi 5 num gabinete de teste, o `lord-fand` manteve a ventoinha desligada a 50–53 °C durante uma carga de 4 threads (o gabinete de alumínio funciona como dissipador), a levou a 55 % (4.500 RPM) e à velocidade máxima (7.400 RPM) com curvas de teste e restaurou as configurações do próprio kernel depois de `SIGTERM` e `kill -9` (`docs/FANS.md`).

**Monitor e imagem.** Desde a 0.8.4.1 a imagem do Raspberry Pi 5 mostra o desktop num monitor HDMI no primeiro boot (a linha do driver de vídeo no `config.txt` agora vem depois das linhas de autodetecção do firmware, e o LordOS espera até 30 s pelo driver), acerta o relógio pela rede e traz Chromium e Firefox ESR. Desde a 0.8.4 o kernel do LordOS para o Pi (`6.18.57-lordos.3-rpi-2712`) também destrói as conexões de um app bloqueado. **Medido com monitor (LordBench, 0.8.6 em desenvolvimento):** a imagem 0.8.5 inicializada com um monitor de 1920×1080 a 60 Hz em qualquer uma das duas portas HDMI chega à tela de login 5,99 s depois que o kernel inicia (systemd: 2,1 s de kernel e 4,2 s do resto). Com composição na GPU (V3D), uma janela movida pelo compositor se mantém em 59,4 quadros por segundo, com um quadro montado em 10,9 ms (mediana) e 13,0 ms (percentil 99) e 1,0 % dos quadros atrasados; com uma carga em todos os núcleos na menor prioridade cai para 24 quadros por segundo com 96,5 % atrasados, um problema que o LordBench encontrou: o cgroup do compositor pesava o mesmo que o da carga, então a baixa prioridade da carga não contava. Dar ao compositor dez vezes o peso de CPU de um app elevou o mesmo teste, sem o monitor, de 43–45 para 56–57 quadros por segundo (31–33 % → 7–8 % atrasados) e levou um movimento do ponteiro à tela em 22 ms em vez de 32 ms (mediana); com o monitor só foi medido junto com a prioridade de tempo real a seguir. Além disso, a thread principal do compositor e suas threads de pintura agora rodam com a menor prioridade de tempo real, que o lord-sessiond permite e o lord-server abandona antes de iniciar qualquer coisa, então nenhum app a herda; na VM 116 sob carga isso levou os quadros atrasados de 2,7–2,9 % para 0,28 % e o 1 % mais lento dos movimentos do ponteiro de 31–32 para 25 ms, como sem carga, com a carga recebendo o mesmo tempo de CPU. No Pi 5 com seu monitor as duas mudanças levaram o teste com carga de 24 para 29–30 quadros por segundo e o compositor não espera mais pela CPU, mas com a carga um quadro leva 15–16 ms para ser montado em vez de 7, então não alcança a atualização de 60 Hz; essa causa ainda está sendo procurada. A cena do LordBench tinha um quadrado que devia girar e ficava parado, porque recebia uma chave de estilo que o motor não tem; agora ele gira, e isso deixa o teste mais pesado: com ele, esse código dava 53–58 quadros por segundo sem carga e 24–25 com a carga. Em desenvolvimento para a 0.8.6, o quadro de uma janela arrastada pinta só o que mudou (a parte alterada da camada da janela e do que está embaixo dela), com os mesmos pixels, conferido quadro a quadro contra hashes registrados; com isso o mesmo teste dá 59,8–60 quadros por segundo sem carga (percentil 99 de 11,3–11,6 ms) e 30 com a carga, onde um quadro é montado em 12,9 ms (mediana) em vez de cerca de 20. Com a carga quase todos os quadros continuam atrasados. Uma medição final da versão em desenvolvimento (2026-10-29) mediu as duas mudanças que faltavam. Compor na GPU a janela arrastada passa no seu autoteste na GPU do Pi e deu exatamente os pixels da CPU nos 138 quadros comparados, mas sem ganho mensurável (30,8–31,6 quadros por segundo com a carga, contra 30,5–31,1 sem ela). Pintar o quadro seguinte enquanto o anterior espera a troca de página deu 33–36 quadros por segundo com a carga, mas um movimento do ponteiro chegou à tela 17 ms mais tarde sem carga (42 em vez de 25,5 ms, mediana) e 29 ms mais tarde com a carga (78 em vez de 49 ms). As duas ficam no código e estão desligadas por padrão. Com os padrões finais o Pi 5 mostra 59,9 quadros por segundo sem carga e ainda passa de 60 para cerca de 31 (30,5–31,1) com uma carga em todos os núcleos. O que resta é o tempo da GPU por quadro, que passa de 1,5 ms sem carga para 14–16 ms com a carga, enquanto os relógios e a temperatura (36,7–47,2 °C) não mostram nenhuma redução de frequência nem por temperatura; a causa provável é a disputa pela memória entre a GPU e os núcleos ocupados, ainda não comprovada. O alarme de subtensão da placa está presente e marcou 0 com a fonte oficial. Em 3 minutos de carga em todos os núcleos com o ventilador a placa manteve 5,9 GFLOP/s com 55,6 °C no máximo, e decodifica vídeo 1080p por software a 258 (H.264), 246 (HEVC), 204 (VP9) e 159 (AV1) quadros por segundo.

### 7.5 PCs modernos

Ainda não há medições em PCs reais no repositório. A ISO é testada em máquinas virtuais (UEFI e BIOS); está planejado testá-la numa máquina AMD real a partir do pendrive live. O LordBench (0.8.6) foi projetado para medir de forma repetível memória e CPU em repouso, tempo de boot, tempos de quadro, vídeo e abertura de apps, e os relatórios de hardware da comunidade (0.8.7), para coletar essas medições de máquinas reais (`docs/BENCHMARK.md`). Em desenvolvimento para a 0.8.6 (não publicada; a última versão é a 0.8.5-beta), sua primeira parte já está construída: um auxiliar isolado, `lord-bench`, que mede a CPU, a memória, a GPU fora da tela, o armazenamento e uma carga contínua, para perto do limite de temperatura ou quando o processador reduz a velocidade, e registra as condições de cada teste, que Informações do Sistema → Desempenho mostra como «×N» de máquinas de referência; também mede a decodificação de vídeo por software e os codificadores da área de trabalho remota, os tempos de quadro do desktop e o tempo de um movimento do ponteiro até a tela, sozinho e sob uma carga de baixa prioridade, e o tempo de boot em várias reinicializações; também roda a partir de um terminal (`lordos-sysreport bench`). Também em desenvolvimento, mede o trabalho da própria interface do LordOS com o código real do motor (o layout e o desenho de uma cena de desktop fixa em full HD, e a composição tipográfica de parágrafos em latim, árabe, devanágari, tailandês e chinês com fontes embutidas no auxiliar, que não abre arquivos; na máquina virtual de referência 37 quadros por segundo em um núcleo e 40 com as threads de desenho, 619 passagens de composição por segundo em um núcleo e 3.595 em oito; no Raspberry Pi 5, em dois testes rápidos, 39 e 45 quadros por segundo e 630 e 2.520 passagens de composição por segundo em um e quatro núcleos), comparado com uma referência só quando medido com a mesma versão do LordOS, e o `lord-bench` verifica o próprio isolamento antes de qualquer trabalho: seis coisas que não deve conseguir fazer (abrir um socket local ou uma conexão de rede, executar um programa, iniciar um processo, criar um arquivo, ler um arquivo do sistema) foram todas recusadas nessa máquina, e um teste em que alguma passe é marcado como instável. Num Raspberry Pi ele vigia o firmware da placa durante todo o teste: o alarme de subtensão, o único indicador que o kernel do LordOS para o Pi expõe, os indicadores completos de limitação do firmware onde um kernel mais antigo os tem, e os 80 °C, onde o firmware começa a reduzir a velocidade do processador, que param um teste de pico como limitado; a seção Raspberry Pi de Informações do sistema lê o mesmo alarme e os mesmos indicadores e deixa de fora o que o kernel não mostra. Isso foi testado com arquivos de sistema simulados e no Pi 5, onde o alarme está presente e marca 0 com a fonte oficial; uma subtensão real não foi provocada. As referências são uma máquina virtual e um Raspberry Pi 5 (§7.4); a decodificação de vídeo por hardware e uma referência de PC real ainda não estão construídas e passaram para a 0.8.7.

### Arquivos-fonte

- [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md), [docs/RASPBERRY.md](https://github.com/lordbasex/LordOS/blob/main/docs/RASPBERRY.md), [docs/FANS.md](https://github.com/lordbasex/LordOS/blob/main/docs/FANS.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md)
- [tools/rpi/build-kernel.sh](https://github.com/lordbasex/LordOS/blob/main/tools/rpi/build-kernel.sh), [tools/rpi/build-img.sh](https://github.com/lordbasex/LordOS/blob/main/tools/rpi/build-img.sh)
- [server/src/canvas.c](https://github.com/lordbasex/LordOS/blob/main/server/src/canvas.c)

## 8. Experiência visual

- **Um guia de design.** Os apps seguem um guia comum (`docs/DESIGN.md`): uma grade de espaçamento de 8 px, uma escala tipográfica enxuta, alturas de controle consistentes e uma única ação na cor de destaque por tela. Os tokens são exportados pelo SDK (`spacing`, `typeScale`, `controlSize`), e cada app passou por rodadas de acabamento documentadas, com antes e depois, nos modos claro e escuro, da direita para a esquerda e em janelas pequenas.
- **Temas.** Um tema é um desktop completo escrito em React: barra de menus, Dock, lançador de apps, Central de Controle e moldura das janelas, além dos seus tokens. O LordOS traz o **Lord Flow**, com barra de menus e Dock, e desde a 0.8.3 o **Lord Classic** como segunda aparência completa: barra de tarefas, menu iniciar com busca, bandeja, ajustes rápidos, painel de notificações, encaixe de janelas com layouts e um estilo próprio para cada app. Os dois têm encaixe de janelas e visão de tarefas. Os temas trocam ao vivo; as configurações persistem. Na 0.8.5 os dois temas ganharam esses nomes (antes "Padrão" e "Clássico"; a escolha do usuário é mantida), Ajustes → Aparência → Estilo da área de trabalho passou a ser o único lugar que lista todos os temas instalados, e se um tema é recusado, o último tema que carregou fica como reserva.
- **Menus e janelas (0.8.5).** Um menu Janela mais completo nos dois temas: Minimizar, Zoom, Preencher, Centralizar; Mover e redimensionar para metades, quartos e terços, ou arranjos de duas ou quatro janelas; Mover para outra tela; Trazer tudo para a frente; Voltar ao tamanho anterior; e as janelas abertas. No Lord Flow, clicar no nome do app na barra de menus abre o menu do app (Sobre o app com sua própria versão, seus Ajustes, Ocultar, Ocultar outros, Mostrar tudo, Sair), e o menu da coroa acrescenta a Loja com o número de atualizações pendentes, Itens recentes e Forçar saída… (§5.4). O Lord Classic, que não tem barra de menus, mostra os menus próprios de um app numa faixa sob a barra de título. Um item do Dock pode abrir seu app no login e, com várias telas, ser atribuído a uma delas (§3.5). Apps transparentes como o Terminal ganham uma borda e uma linha da barra de título opacas.
- **Claro e escuro, cores de destaque.** Os dois modos e a cor de destaque se aplicam ao vivo aos apps nativos, aos apps isolados e, pelo portal e pelo GSettings, aos apps GTK.
- **Direita para a esquerda.** A interface inteira se espelha para árabe, hebraico e escritas parecidas, incluindo os apps isolados, que seguem o idioma da sessão ao vivo.
- **Lumo.** O mascote do LordOS é o Lumo, uma pequena raposa celestial, usado em telas de boas-vindas, estados vazios e material de marca, com um conjunto fixo de poses definido no guia da marca.
- **Acessibilidade hoje:** uma opção de "reduzir movimento" que desliga as animações decorativas, atalhos de teclado em toda parte e telas escaláveis até 200 %. Um leitor de tela, zoom e alto contraste estão no roadmap e **ainda não foram construídos**.

### Arquivos-fonte

- [docs/DESIGN.md](https://github.com/lordbasex/LordOS/blob/main/docs/DESIGN.md), [docs/LORDOS-BRAND.md](https://github.com/lordbasex/LordOS/blob/main/docs/LORDOS-BRAND.md)
- [themes](https://github.com/lordbasex/LordOS/tree/main/themes), [sdk/controls.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/controls.jsx), [sdk/i18n.js](https://github.com/lordbasex/LordOS/blob/main/sdk/i18n.js)

## 9. Extensibilidade

- **O SDK** (`@lordos/sdk`) é a única coisa que os apps importam: janelas, configurações, tokens do tema, controles, arquivos, mídia, notificações, chaveiro, impressão e mais. Os apps não importam nada dos temas, então qualquer app funciona em qualquer tema.
- **Escrever um app** é uma pasta com `manifest.json` e `index.jsx`. `node tools/lpk.mjs <folder>` empacota sem React nem o SDK, acrescenta o ícone e as traduções e assina se houver uma chave; `keygen`, `sign`, `verify` e `catalog` cobrem o resto. Os apps podem incluir módulos WebAssembly e desenhar pixels diretamente com `<Image pixels>`.
- **Os temas** seguem um contrato fixo (`Desktop`, `MenuBar`, `Dock`, `Launcher`, `ControlCenter`, `WindowFrame`, tokens), documentado em `themes/README.md`, com o tema Lord Classic como modelo. Desde a 0.8.5 o `docs/THEMES-GUIDE.md` explica, para um desenvolvedor React novo no LordOS, as partes de um tema (theme.json, tokens, o shell, molduras das janelas, menus, camadas), direita para a esquerda, claro e escuro, várias telas e as regras de desempenho, com uma lista de conferência, e `examples/theme-minimal/` é um pequeno tema funcional para começar. Um tema que o LordOS não assinou ainda não carrega num build de lançamento: não há formato de pacote de tema nem decisão de confiança do usuário.
- **A interoperabilidade com Linux** se dá por padrões: Wayland, o portal de configurações, o Secret Service, o serviço de notificações, os apps padrão do `mimeapps.list` e os arquivos `.desktop`.
- **Automação:** hoje não há API de scripts ou automação além do SDK. Desde a 0.8.5 a Loja instala apps de IA (§5.3), mas eles são apps Linux comuns, com as mesmas regras de qualquer outro; integrações de assistentes de IA com o próprio desktop (por exemplo, pelo Model Context Protocol) **não existem no LordOS**; são apenas uma possível direção futura e teriam de respeitar a mesma regra de que o JavaScript não executa comandos.

### Arquivos-fonte

- [sdk](https://github.com/lordbasex/LordOS/tree/main/sdk), [apps/README.md](https://github.com/lordbasex/LordOS/blob/main/apps/README.md), [themes/README.md](https://github.com/lordbasex/LordOS/blob/main/themes/README.md)
- [docs/NATIVE-APPS.md](https://github.com/lordbasex/LordOS/blob/main/docs/NATIVE-APPS.md), [docs/WASM.md](https://github.com/lordbasex/LordOS/blob/main/docs/WASM.md), [examples/apps/fractal](https://github.com/lordbasex/LordOS/tree/main/examples/apps/fractal)

## 10. Roadmap e limitações

### 10.1 Onde as coisas estão

A 0.8.0 Beta foi o primeiro lançamento público (18 de outubro de 2026); a 0.7.0 foi um build interno e nunca foi publicada. Os marcos M1 a M9 estão completos: o servidor gráfico, o motor de interface, o SDK e os temas, o terminal, idiomas e escritas complexas, compatibilidade com apps Linux, pacotes e serviços do sistema, sessões, isolamento e o firewall, e os conjuntos de apps do M9 (documentos e impressão, mídia, apps de sistema, senhas, configurações de hardware, e-mail e calendário). Desde então: 0.8.1 (atualizações pela Loja, o Dock, SSH), 0.8.2 (código do sistema verificado, apps de PIM isolados, saúde dos discos, histórico de tráfego), 0.8.3 (apps X11, Lord Classic, reforço da área de trabalho remota, corte de conexões abertas), 0.8.4 (área de trabalho remota como vídeo WebRTC com som, LordOS Imager), 0.8.4.1 (bootloader do Raspberry Pi pelos Ajustes, imagem do Pi com o HDMI corrigido) e a 0.8.5 Beta, lançada em 27 de outubro de 2026 (Informações do sistema com Verificações e o Teste do isolamento, o Banco de testes, resultados versionados, a segunda geração de Monitor → Rede, listas de bloqueio, o modo "negar em silêncio", componentes em seus próprios escopos, Arquivos → Este computador, um caminho pela CPU mais rápido, quadros com ritmo marcado em placas virtuais, HTTPS no motor de pacotes, uma Loja redesenhada com as categorias Inteligência Artificial e Comerciais, cujos repositórios de fabricantes são instalados com a chave fixada pelo LordOS, e um banner em destaque assinado, root por SSH como opção avançada, um menu Janela mais completo, a tela de login fechada depois do login, a sessão iniciada quando seu barramento de sessão existe). O Changelog mais abaixo lista cada lançamento.

### 10.2 Limitações conhecidas

- Hardware real: GPUs AMD e Nvidia, monitores reais em PCs, notebooks (bateria, brilho da tela, tampa), controladoras Bluetooth, impressoras e scanners, chaves FIDO2 e provedores de e-mail reais não foram testados.
- O Raspberry Pi 5 é a única placa suportada, com o build do LordOS do seu kernel com Landlock; sua imagem 0.8.5 foi inicializada e usada com um monitor de 1920×1080 nas duas portas HDMI; outros monitores e resoluções não foram testados.
- Os apps Linux não ficam em sandbox por padrão. Apps X11 precisam do pacote `xwayland` do Debian.
- Firewall: sem modo "perguntar"; as regras por domínio não veem apps que resolvem nomes com o próprio DNS criptografado.
- As restrições para apps Linux não cobrem o BlueZ via D-Bus, o barramento do sistema, a área de transferência nem apps iniciados fora do lançador de apps; a união do `user@` também sujeita os serviços do próprio usuário enquanto um app restrito roda; um cliente novo de PipeWire tem uma janela de poucos milissegundos antes de o guarda restringi-lo (§6.6).
- A chave de assinatura de `.lpk` ainda é uma chave de desenvolvimento, e ainda não há um repositório assinado para apps `.lpk`.
- Permissões: sem "permitir uma vez"; o uso de câmera e microfone pelos apps Linux não é mostrado.
- O Secure Boot precisa estar desligado; a assinatura com um shim está pendente.
- O motor de pacotes ainda não faz multiarch i386, então o Steam não é oferecido.
- Apps de IA numa GPU: o backend Vulkan do llama.cpp não foi testado em hardware AMD, NVIDIA e Intel real.
- Arrastar uma janela na 0.8.5-beta: podem aparecer pequenos quadrados pretos nos seus cantos arredondados enquanto ela se move, no monitor e nos quadros da área de trabalho remota (corrigido para a 0.8.6, em desenvolvimento, §7.2).
- No Raspberry Pi 5 um app que ocupa todos os núcleos ainda reduz pela metade a taxa de quadros do desktop (cerca de 31 em vez de 60 quadros por segundo no teste do LordBench): o tempo da GPU por quadro cresce com a carga, e as duas mudanças medidas para isso na 0.8.6 (em desenvolvimento) deram muito pouco e estão desligadas por padrão (§7.4).
- Boot com BIOS legada: um retângulo preto (a área de terminal do GRUB enquanto ele carrega o kernel e o initrd) aparece por alguns segundos entre o menu de boot e a tela de abertura.
- Wayland: sem arrastar e soltar, sem métodos de entrada do LordOS dentro dos apps Linux (`text-input`), sem `xdg-activation`, sincronização explícita, controle de tearing ou mesas digitalizadoras (§3.4).
- Sem GPU, vídeo AV1 a 60 fps é pesado, já que o navegador o decodifica por software na mesma máquina.
- Acessibilidade (leitor de tela, zoom, alto contraste) não foi construída.

### 10.3 Próximos passos

A seguir, como planejado em `docs/ROADMAP.md` e `docs/BENCHMARK.md`: **0.8.6, LordBench**, medições repetíveis de CPU, memória, GPU, compositor, vídeo e armazenamento; **0.8.7, relatórios de hardware da comunidade**, publicados a partir das Informações do sistema com o consentimento do usuário; e uma conexão recusada pelo firewall por app, conferida no próprio computador do usuário. O Xwayland e o tema Lord Classic, planejados aqui para a 1.0 na 0.8.0, saíram na 0.8.3. Flatpak e Flathub não estão planejados para nenhuma versão: fica uma revisão de design em `docs/FLATPAK.md` para o futuro, mas, para manter a estabilidade do sistema, não é construído.

Depois da 1.0: um app de virtualização para desktop construído sobre KVM e QEMU; um contêiner Android com LineageOS e apps da Loja do LordOS; o modo "perguntar" do firewall; acessibilidade; e um vídeo de demonstração completo.

### Arquivos-fonte

- [docs/ROADMAP.md](https://github.com/lordbasex/LordOS/blob/main/docs/ROADMAP.md), [CHANGELOG.md](https://github.com/lordbasex/LordOS/blob/main/CHANGELOG.md)

## 11. Como verificar o projeto você mesmo

### 11.1 Compile

Numa máquina de desenvolvimento com Node.js, empacote os temas, o SDK e os apps:

```
npm install && npm run build
```

No Debian 13, compile os binários e rode os testes:

```
sudo apt-get install -y --no-install-recommends build-essential
make -C server -j4
make -C server test        # every self-contained suite
make -C server test-full   # adds the helpers, the sandbox and the HarfBuzz comparison
```

O `make test` falha se alguma suíte falhar; uma suíte cuja ferramenta ou cujos dados não estão instalados informa "skipped" em vez de passar em silêncio. Os testes do sandbox precisam de um kernel com Landlock. A ISO e a imagem do Raspberry Pi são geradas como descrito em `docs/ISO.md` e `docs/RASPBERRY.md`. Na VM 116, com a árvore da 0.8.5: 23 aprovados, 0 falhas, 10 pulados (as suítes de Node.js, que rodam na máquina de desenvolvimento).

### 11.2 O que os testes cobrem

- **Terminal e texto:** o emulador VT, o shaping OpenType comparado ao HarfBuzz, o bidi Unicode comparado ao arquivo de teste oficial, os métodos de entrada, a separação de palavras em tailandês comparada à libthai.
- **Segurança:** o sandbox (app, rede, ajudante), a assinatura (vetores do NIST e da RFC 8032), o estado dos pacotes, as regras de caminhos de extração contra arquivos compactados maliciosos, as proteções do `lord.fs`, as regras do firewall (com 20.000 mutações de JSON e 30.000 operações aleatórias contra uma referência linear), o leitor de DNS (200.000 pacotes aleatórios), o compilador dos mapas do kernel (480.000 conexões) e o teste de integração com o kernel (18 de 18 numa VM).
- **Leitores:** e-mail, MIME, sanitizador de HTML, iCalendar, vCard e XML (30.953 verificações com fuzzing por mutação), o formato do cofre, os importadores, QR, CBOR, serialização D-Bus, o leitor da base de países, journal, TZif e IPP.
- **Motor e hardware:** layout e pintura, NEON vs. C, o driver de touchpad com gestos gravados, as ventoinhas em árvores sysfs simuladas, o arranjo de telas, o som e a suíte de especificação do WebAssembly. Desde a 0.8.6 (em desenvolvimento; a última versão publicada é a 0.8.5 Beta), cada quadro de cenas aleatórias, e de uma cena do Monitor com um Terminal translúcido e desfocado minimizado para o Dock, é comparado com uma repintura completa por meio de uma cópia da contabilidade de dano e buffer duplo do compositor; essa comparação encontrou e corrigiu sete erros de repintura parcial, e o mesmo teste em um desktop em execução numa máquina virtual (a cena da 0.8.2, 22 ciclos de minimizar e restaurar, 54 comparações por execução, antes e depois, com um único retângulo danificado e com vários) não encontrou nenhuma área desatualizada. Uma janela com desfoque coberta por inteiro por outra opaca não é mais repintada a cada mudança perto dela: um Terminal translúcido atrás do mapa em movimento do Monitor fazia cada quadro repintar 46 % da tela, agora de 11 a 15 % com um único retângulo danificado e de 3 a 4 % com vários nessa máquina virtual, conferido da mesma forma, sem nenhuma área desatualizada.
- **Informações do sistema (0.8.5):** o coletor sobre cinco máquinas falsas (um desktop AMD, um notebook Intel, um desktop Nvidia, um Raspberry Pi 5, uma máquina virtual) cheias de números de série, UUIDs, endereços MAC e nomes que não podem chegar a um relatório (nenhum chega, 460 verificações), o leitor de memória SMBIOS contra tabelas forjadas e malformadas, e a avaliação das Verificações e do Teste do isolamento (56 verificações).

Na geração da ISO final, o `make -C server test` informou 23 aprovados, 0 falhas, 1 pulado (dados de bidi não instalados).

### 11.3 Confira um release

Baixe o `SHA256SUMS`, o `SHA256SUMS.sig`, os arquivos do release e a chave pública (`tools/release/lordos-release.pub.pem`). Confira que a impressão digital da chave é `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e` e então, com o OpenSSL 3:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

O SBOM de cada arquitetura (`lordos_<version>_<arch>.spdx.json`) também está no `SHA256SUMS`, então a mesma conferência o cobre.

### 11.4 Confira um sistema em execução

- `systemctl status lord-netperm lord-fand lordos` mostra os serviços; o teste da ISO confirmou os sete programas de rede anexados. Se o kernel recusar o programa de dispositivos ou o LSM, o `lord-netperm` informa `"devices":false` ou `"bluetooth":false`.
- `cat /proc/<pid>/status | grep -E 'Seccomp|NoNewPrivs'` num processo `lord-server --app` mostra o sandbox.
- `systemd-cgls` mostra a árvore `lordos-session-<uid>` pertencente ao root, com um escopo por app.
- Área de trabalho remota: `ss -ltn` não mostra nada na porta 5673 até você ativá-la em Ajustes → Compartilhar.
- **Informações do sistema → Verificações (0.8.5)** faz essas verificações por você e diz, para cada uma, se ela foi encontrada configurada ou realmente testada: o servidor gráfico sem root, o Landlock, o firewall por app com suas peças do kernel, cada bundle do código do próprio LordOS verificado nesta sessão, a porta da área de trabalho remota fechada quando ela está desligada (lido de `/proc/net`), a árvore de cgroups da sessão pertencente ao root, a composição na GPU, e **Testar** para o Teste do isolamento (§6.4).
- **`lordos-testbench --auto`** imprime um relatório JSON com a lista de verificação do Wayland, os tempos de quadro e de apresentação, o renderizador 3D e a latência da entrada até a tela; `lordos-sysreport report --json` imprime o relatório de hardware sem números de série, endereços nem nomes.

### 11.5 Resultados versionados

Cada versão publica suas medições como arquivos em `results/<versão>/<máquina>/` (e anexados ao release do GitHub): o relatório do hardware, o relatório do banco de testes, a memória do sistema inteiro com o PSS de cada processo e o tempo de boot, cada pasta com suas condições e os comandos exatos que a produziram (`tools/results/collect.sh`, `docs/RESULTS.md`). A partir da 0.8.6 entram os resultados do LordBench. Um número deste documento que tenha esse arquivo pode ser comparado com ele e repetido.

## 12. Licença e créditos

O código do LordOS é distribuído sob a **licença MIT**. Os componentes de terceiros mantêm suas próprias licenças, listadas em `THIRD_PARTY.md`: QuickJS e stb (incorporados como código-fonte), Monocypher (CC0/BSD-2), a lista de palavras da EFF (CC BY 3.0 US), a lista de senhas comuns (MIT), os dados de países do DB-IP Lite (CC BY 4.0), Natural Earth (domínio público) e as fontes Inter, JetBrains Mono e Plus Jakarta Sans (OFL). Cada release traz um SBOM SPDX do que o pacote inclui e daquilo de que depende (`docs/COMPLIANCE.md`).

O LordOS é criado pelo grupo **iPERFEX**: **Pablo Miranda** e **Federico Pereira**.

## 13. Glossário

- **Broker:** o único socket pelo qual um app isolado pede ao `lord-server` qualquer coisa que toque nas coisas do usuário; cada pedido é conferido em C.
- **cgroup (v2):** um mecanismo do kernel que agrupa processos para contabilizar e limitar recursos. O LordOS dá a cada app seu próprio grupo, um "escopo", e o usa para identificar o app no firewall.
- **Compositor:** o programa que combina as janelas de todos os apps na imagem final da tela.
- **DRM/KMS:** as interfaces do kernel para dispositivos gráficos e para configurar modos de tela.
- **eBPF:** pequenos programas verificados que rodam dentro do kernel; o LordOS os anexa aos cgroups para decidir sobre conexões de rede, abertura de dispositivos e sockets Bluetooth.
- **evdev:** a interface do kernel para dispositivos de entrada.
- **LSM (BPF LSM):** os ganchos de segurança do kernel; com o LSM `bpf`, um pequeno programa anexado por cgroup pode recusar uma operação, como criar um socket Bluetooth.
- **Namespace:** um recurso do kernel que dá a um processo sua própria visão de algo (usuários, montagens, ids de processo); o LordOS usa namespaces sem privilégios para os apps Linux restritos dos arquivos do usuário.
- **Ajudante:** um pequeno processo do LordOS que lida com dados não confiáveis ou segredos num sandbox mais rígido (`lord-doc`, `lord-media`, `lord-maild`, `lord-vault`, `lord-fetch`, `lord-tls`).
- **Landlock:** um recurso do kernel que permite a um processo sem privilégios restringir quais arquivos e portas de rede ele, e seus filhos, podem usar.
- **`.lpk`:** o formato de pacote do LordOS para apps nativos instaláveis.
- **`.lvault`:** o formato de arquivo do cofre criptografado do LordOS.
- **Guarda do PipeWire:** o cliente de PipeWire próprio do `lord-server` que esconde os dispositivos de captura dos apps Linux restritos e interrompe seus streams de captura.
- **Powerbox:** um diálogo de arquivos desenhado pelo sistema que entrega ao app só o arquivo escolhido pelo usuário.
- **QuickJS:** um pequeno motor JavaScript escrito em C.
- **SBOM:** uma lista de materiais de software, a listagem legível por máquina dos componentes que um release contém, no formato SPDX.
- **Escopo:** um cgroup para um app, como `app-lordos-lpk-<id>.scope`.
- **seccomp-bpf:** um filtro do kernel que restringe quais chamadas de sistema um processo pode fazer.
- **Secret Service:** o padrão D-Bus do freedesktop que os apps Linux usam para guardar segredos.
- **Wayland:** o protocolo entre os apps gráficos e o compositor.

## 14. Para ler mais

| Capítulo | Documentos em `docs/` |
|---|---|
| 1. Visão | ARCHITECTURE.md, LORDOS-BRAND.md |
| 2. Arquitetura | ARCHITECTURE.md, NETWORK.md, FANS.md, SETTINGS.md, SYSTEM.md |
| 3. Compositor | GPU.md, DISPLAYS.md, WINDOWS.md, TRACKPAD.md, WAYLAND.md, SESSIONS.md, REMOTE.md |
| 4. Motor de interface | ARCHITECTURE.md, LANGUAGES.md, TERMINAL.md |
| 5. Aplicativos | NATIVE-APPS.md, PACKAGES.md, UPDATES.md, WASM.md, FILES.md, MEDIA.md, DOCUMENTS.md, MAIL.md, ISO.md, INSTALL-DEBIAN.md |
| 6. Segurança | PRIVACY.md, FIREWALL.md, MONITOR.md, SIGNING.md, PASSWORDS.md, MEDIA.md, REMOTE.md, COMPLIANCE.md, e `SECURITY.md` |
| 7. Desempenho | ISO.md, RASPBERRY.md, FANS.md, DISPLAYS.md, GPU.md, REMOTE.md, BENCHMARK.md |
| 8. Experiência visual | DESIGN.md, LORDOS-BRAND.md, LANGUAGES.md, DOCK.md |
| 9. Extensibilidade | NATIVE-APPS.md, WASM.md, THEMES-GUIDE.md, e `apps/README.md`, `themes/README.md` |
| 10. Roadmap | ROADMAP.md, RELEASING.md, UPDATES.md e `CHANGELOG.md` |
| 11. Verificação | PAPER-VALIDATION.md, RESULTS.md, BENCHMARK.md, IMAGER.md |

## Changelog

Cada lançamento, com suas notas completas no GitHub (elas vêm do `CHANGELOG.md`, no formato Keep a Changelog). Este documento é atualizado a cada mudança.

| Versão | Data | O que trouxe |
|---|---|---|
| **0.8.5 Beta** | 2026-10-27 | Informações do sistema (hardware, a GPU por camadas, Compatibilidade, Relatório, Verificações e o Teste do isolamento), `lordos-sysreport`, `lordos-fetch`, o Banco de testes, resultados versionados, a segunda geração de Monitor → Rede (uma árvore por app, destino e porta; sockets ICMP, brutos e de pacotes; uma cor por app), os componentes do LordOS em seus próprios escopos, listas de bloqueio baixadas pelo `lord-fetch`, o modo "negar em silêncio", Arquivos → Este computador e cada disco, a Loja redesenhada com as categorias Inteligência Artificial e Comerciais e um banner em destaque assinado, downloads por HTTPS através do `lord-tls`, root por SSH como opção avançada, um caminho pela CPU em até 8 threads, quadros com ritmo marcado em placas virtuais, vídeo da área de trabalho remota na taxa de atualização de quem assiste, um menu Janela mais completo e Forçar saída, numeração e encaixe das telas, a tela de login fechada depois do login, bloqueio mais rápido, a correção do barramento de sessão e do chaveiro, idle-inhibit e a área de transferência devolvida ao seu dono. [Notas do lançamento](https://github.com/lordbasex/LordOS/releases/tag/v0.8.5-beta) |
| **0.8.4.1 Beta** | 2026-10-25 | Raspberry Pi 5: Ajustes → Firmware da placa (o bootloader), seu atualizador no repositório assinado do LordOS, uma imagem nova com o HDMI corrigido no primeiro boot, horário pela rede, navegadores e os pacotes de sistema da ISO. [Notas da versão](https://github.com/lordbasex/LordOS/releases/tag/v0.8.4.1-beta) |
| **0.8.4 Beta** | 2026-10-19 | Área de trabalho remota como vídeo WebRTC com som, por UDP ou TCP na sua própria porta, reconectando sozinha, com STUN e TURN; o LordOS Imager (no LordOS e para macOS, Linux e Windows) com o arquivo de primeira execução do Pi; navegadores com uma única barra superior; conexões de apps bloqueados fechadas no Pi 5. [Notas da versão](https://github.com/lordbasex/LordOS/releases/tag/v0.8.4-beta) |
| **0.8.3 Beta** | 2026-10-19 | Apps X11 pelo Xwayland; a seleção primária; o Lord Classic como segunda aparência completa; encaixe de janelas e a visão de tarefas; listas de permissão e de bloqueio da área de trabalho remota, um bloqueio após 3 logins com falha e uma revisão de segurança; regras do firewall aplicadas às conexões abertas. [Notas da versão](https://github.com/lordbasex/LordOS/releases/tag/v0.8.3-beta) |
| **0.8.2 Beta** | 2026-10-19 | O LordOS verifica o próprio código antes de rodá-lo; E-mail, Calendário, Contatos e Lembretes isolados; temperatura e saúde SMART dos discos; sugestões para este computador; histórico de tráfego; botões por destino; vídeo fluido no navegador sem decodificação por hardware. [Notas da versão](https://github.com/lordbasex/LordOS/releases/tag/v0.8.2-beta) |
| **0.8.1 Beta** | 2026-10-19 | Atualizações pela Loja e um repositório APT assinado, migrações entre versões, um Dock que você organiza, login remoto (SSH), Reiniciar e Desligar no menu da coroa, encerrar processos pelo Monitor, Bluetooth na imagem do Pi 5. [Notas da versão](https://github.com/lordbasex/LordOS/releases/tag/v0.8.1-beta) |
| **0.8.0 Beta** | 2026-10-18 | O primeiro lançamento público: os marcos M1 a M9. [Notas da versão](https://github.com/lordbasex/LordOS/releases/tag/v0.8.0-beta) |
