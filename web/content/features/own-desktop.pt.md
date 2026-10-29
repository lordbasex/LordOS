## Em poucas palavras

O LordOS é uma distribuição GNU/Linux baseada no Debian 13. O que o torna diferente é o desktop: em vez de trazer o GNOME ou o KDE, o LordOS tem um ambiente de desktop próprio, construído a partir dos seus componentes fundamentais sobre o Debian e o Wayland.

Pense numa casa. O Debian e o kernel Linux são a fundação, o encanamento e a fiação: são reaproveitados, bem testados e mantidos por uma grande comunidade. O LordOS constrói os cômodos onde você de fato vive: a tela que você vê, as janelas que você move, os apps que você abre e os guardas nas portas.

[[shot:0]]

[[diagram:layers]]

## O que é nosso e o que é reaproveitado

Aqui a precisão importa, porque é fácil exagerar no "próprio".

**Escrito para o LordOS (neste repositório):**

- **O servidor gráfico e compositor Wayland** (`lord-server`, em C). Ele define os modos de tela usando diretamente a interface DRM/KMS do kernel, sem libdrm; lê teclados, mouses, touchpads e telas sensíveis ao toque pelo evdev, sem libinput; compõe todas as janelas; e fala o protocolo Wayland com os apps Linux.
- **O motor de interface.** O desktop e os apps nativos são escritos em JavaScript com React. Eles rodam no QuickJS (um motor JavaScript pequeno, incluído como código-fonte) dentro do `lord-server`, com uma árvore de nós nativa, um motor de layout flexbox, um rasterizador 2D e um motor de texto escritos em C. Não há nenhum motor de navegador web por trás da interface.
- **Os apps nativos:** Arquivos, Ajustes, Terminal, Loja, E-mail, Calendário, Contatos, Lembretes, Senhas, Monitor, Fotos, Player, Visualizador de PDF, Discos, Backups e os demais, todos apps React no mesmo motor.
- **Os serviços de segurança:** o lançador de sandbox dos apps, o broker de permissões, o armazenamento de permissões por app, o serviço de firewall (`lord-netperm`), a assinatura de pacotes e o cofre de senhas (`lord-vault`).
- **Serviços do sistema** como o gerenciador de sessões e a tela de login (`lord-sessiond`), o serviço de rede (`lord-netd`) e o serviço de ventoinhas (`lord-fand`).

**Reaproveitado do Debian e do mundo do software livre:**

- o kernel Linux e os pacotes do Debian (o `dpkg` continua desempacotando e configurando cada pacote);
- o Mesa, para a composição opcional na GPU;
- o PipeWire, para o som;
- o próprio protocolo Wayland, via `libwayland-server` e `libwayland-client`;
- a `libxkbcommon`, para os layouts de teclado;
- bibliotecas usadas só dentro de processos ajudantes em sandbox: FFmpeg (mídia), Poppler e libarchive (documentos), OpenSSL (TLS para e-mail e área de trabalho remota), Monocypher (a criptografia do cofre).

A regra que o projeto segue está escrita em `docs/ARCHITECTURE.md`: X11, wlroots, libdrm, libinput, WebKit, GTK e Qt não são usados dentro do próprio LordOS. Apps GTK e Qt continuam rodando, como clientes Wayland.

## Como um quadro chega à tela

Veja o que acontece, passo a passo, quando você passa o mouse sobre o Dock:

1. O kernel entrega um evento de entrada pelo evdev. O `lord-server` o lê e descobre qual nó da interface está sob o ponteiro.
2. Ele chama o tratador de eventos em JavaScript. O React atualiza seu estado e informa ao motor quais nós mudaram: um estilo, um filho, um texto.
3. O motor recalcula o layout e determina a região alterada: só a parte da tela que realmente mudou.
4. Ele repinta só essa região, copia para o buffer traseiro, desenha o cursor e troca a página no próximo retraço vertical.

Se houver uma GPU com aceleração 3D, a composição passa pelo Mesa (GBM, EGL e GLES2). Se não houver, ou se o único renderizador for por software, o caminho da CPU faz o trabalho. O caminho da CPU precisa continuar funcionando sempre: é uma regra rígida no código (`LORD_GPU=0` o força), e a opção "modo seguro" do grub usa esse caminho.

[[diagram:frame]]

## Onde os apps Linux se encaixam

Há três tipos de janela num desktop LordOS, e elas se conectam em camadas diferentes:

- **Os apps incluídos e o tema** (barra de menus, Dock, Arquivos, Ajustes…) rodam dentro do próprio contexto JavaScript do `lord-server`, no motor de interface. As exceções são Senhas e, desde a 0.8.2, Mail, Calendário, Contatos e Lembretes, que rodam como processos isolados, como os apps `.lpk`.
- **Os apps `.lpk` instalados** rodam cada um como um processo separado (`lord-server --app <id>`, o mesmo binário em "modo app"). Esse processo tem sua própria cópia do motor de interface, se tranca num sandbox e desenha num buffer de memória compartilhada, que entrega ao compositor como um cliente Wayland comum.
- **Os apps Linux** (GTK, Qt, Chromium, Firefox…) se conectam ao socket Wayland do compositor. Eles se desenham sozinhos; o tema coloca sua moldura de janela em volta deles.

Ou seja, o compositor é o ponto de encontro. O motor de interface é usado pela interface e pelos apps do próprio LordOS, não pelos apps Linux.

[[shot:3]]

## Leve por projeto

O LordOS mira máquinas modestas. Algumas medições reais dos registros de teste do projeto:

- A ISO do teste final, ociosa um minuto após o login numa VM com 4 GB e 4 núcleos: **465 MB de RAM em uso** para o sistema inteiro na sessão live e **487 MB** no sistema instalado, com a CPU **99 % ociosa**. O boot levou 16,3 s (`docs/ISO.md`, "Final ISO test").
- Num Raspberry Pi 5 (16 GB), na tela de login e sem monitor: `lord-server` com **36 MB de RSS** e 0,6 % de CPU (`docs/RASPBERRY.md`).
- Quando a máquina não tem placa com 3D, o `lord-server` nem carrega o Mesa e fica em torno de 32 MB de RSS (`docs/GPU.md`).
- Desde a 0.8.5 a tela de login não fica mais na memória atrás da sua sessão: ela fecha 10 segundos depois que uma sessão assume a tela e volta a iniciar em poucos milissegundos quando você bloqueia ou troca de usuário. Numa VM do VMware o sistema inteiro passou de **603 para 548 MB** em uso, e bloquear a tela passou de 903 ms para **141 ms**.
- Em placas de vídeo virtuais que concluem cada troca de quadro na hora (`vmwgfx`, do VMware), a 0.8.5 ajusta os quadros à frequência da tela em vez de desenhar 500–580 por segundo: **59,8 quadros por segundo** a 60 Hz, percentil 99 de 18,4 ms. O tempo de cada boot aparece em Sobre o LordOS → Tempo de inicialização e fica guardado para comparar versões (`docs/RESULTS.md`).
- **Em desenvolvimento para a 0.8.6 (a última versão é a 0.8.5 Beta):** num Raspberry Pi 5 com um monitor de 1920×1080, o teste de desktop do LordBench move uma janela com um quadrado que gira dentro a **59,8–60 quadros por segundo** (53–58 antes do trabalho no quadro de uma janela arrastada, que deixa cada pixel igual), e a cerca de 31 (30,5–31,1) enquanto todos os núcleos estão ocupados com uma carga de baixa prioridade (24–25 antes; `docs/BENCHMARK.md`). Desenhar a janela arrastada no chip gráfico e pintar o quadro seguinte enquanto o anterior espera a tela também foram medidos ali e estão desligados por padrão: o primeiro é exato mas não deu ganho mensurável, o segundo deu 33–36 quadros por segundo com carga mas o ponteiro chegou à tela 17–29 ms mais tarde.

Esses números vêm de máquinas virtuais e de uma placa, não de uma variedade de PCs reais. São um ponto de partida, não uma promessa para toda máquina.

[[shot:2]]

## Várias telas, texto nítido

O compositor controla várias telas ao mesmo tempo, estendidas ou espelhadas, e desenha cada uma na sua própria escala: 100, 125, 150, 175 ou 200 %. Texto, vetores e imagens são rasterizados na resolução real de cada tela, então um painel 4K a 150 % ao lado de um 1080p a 100 % fica nítido nos dois. Os apps Linux recebem um `wl_output` por tela e o protocolo de escala fracionária, então GTK 4, Qt 6 e Chromium também renderizam com nitidez. Desde a 0.8.5, Ajustes → Telas numera cada tela, Identificar mostra esse número grande no próprio monitor, e uma tela arrastada se encaixa borda com borda sem sobrepor. O menu Janela pode mover uma janela para outra tela, e o Dock pode atribuir um app a uma delas.

[[shot:1]]

## Limites honestos

- **GPUs reais ainda não foram verificadas.** O caminho da GPU foi exercitado com o renderizador por software do Mesa em máquinas virtuais. O teste em placas AMD e Nvidia reais ainda está pendente (`docs/GPU.md`).
- **Várias telas foram testadas com duas saídas virtuais**, não com monitores reais, painéis de notebook ou hubs DisplayPort (`docs/DISPLAYS.md`).
- **O Raspberry Pi 5 só foi testado com um monitor HDMI.** Desde a 0.8.4.1 a imagem mostra a tela de login e o desktop num monitor HDMI desde o primeiro boot, e o LordBench o mediu assim; dois monitores e 4K ainda não foram testados na placa (`docs/RASPBERRY.md`).
- **No Raspberry Pi 5 um app ocupado ainda reduz pela metade os quadros por segundo do desktop:** cerca de 31 em vez de 60 no teste de desktop do LordBench enquanto todos os núcleos estão ocupados. Com essa carga o chip gráfico leva 14–16 ms por quadro em vez de 1,5, sem reduzir a frequência nem por temperatura (o mais provável é que os núcleos e o chip disputem a memória, ainda sem comprovar); enviar e compor menos pixels por quadro é o próximo passo (`docs/GPU.md`).
- **Os apps X11 rodam pelo Xwayland desde a 0.8.3,** nítidos só em 100 % de escala e podendo ver uns aos outros, como em qualquer desktop X11. Arrastar e soltar entre apps Linux e os métodos de entrada do LordOS dentro de apps Linux estão pendentes (`docs/WAYLAND.md`).
- **A acessibilidade ainda está quase toda pela frente:** hoje existe um ajuste para "reduzir movimento"; leitor de tela, zoom e alto contraste estão no roadmap.

## Leia o código

- [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md): as camadas e a política de dependências
- [server/src/kms.c](https://github.com/lordbasex/LordOS/blob/main/server/src/kms.c): modos de tela com ioctls DRM, sem libdrm
- [server/src/input.c](https://github.com/lordbasex/LordOS/blob/main/server/src/input.c): entrada via evdev
- [server/src/gpu/gpu.c](https://github.com/lordbasex/LordOS/blob/main/server/src/gpu/gpu.c): composição opcional na GPU com Mesa
- [server/src/ui/layout.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/layout.c) e [server/src/ui/paint.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/paint.c): layout e pintura
- [runtime/index.js](https://github.com/lordbasex/LordOS/blob/main/runtime/index.js): o renderizador React para o motor nativo
- [server/src/wl/wl.c](https://github.com/lordbasex/LordOS/blob/main/server/src/wl/wl.c): o compositor Wayland
- [docs/GPU.md](https://github.com/lordbasex/LordOS/blob/main/docs/GPU.md), [docs/DISPLAYS.md](https://github.com/lordbasex/LordOS/blob/main/docs/DISPLAYS.md), [docs/ISO.md](https://github.com/lordbasex/LordOS/blob/main/docs/ISO.md)
