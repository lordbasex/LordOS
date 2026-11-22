## Em poucas palavras

O **Máquinas** (Machines) roda outros sistemas em janelas do seu desktop LordOS, em velocidade nativa com KVM onde o hardware permite. Ele saiu na **0.8.8.5** (alfa): cartões de download para Debian, Ubuntu, Fedora e Kali que conferem as assinaturas dos fabricantes e podem instalar sem perguntas, snapshots, clones, migração de outros apps de virtualização e, desde a 0.8.8.7, pastas compartilhadas.

**Em desenvolvimento depois da 0.8.9.4.** Tudo o que vem a seguir está no repositório e ainda não num lançamento; o último lançamento é a **0.8.9.4**. Traz máquinas com Windows 11, as ferramentas do LordOS para Windows, uma área de transferência que só se move quando você cola, transferências de arquivos rápidas nos dois sentidos, o modo apps, som e microfone, redes de verdade, 3D para máquinas Linux e a imagem na velocidade total do convidado.

[[shot:0]]

## Windows 11, dos servidores da própria Microsoft

Um cartão novo do assistente Nova máquina monta uma máquina que o Windows 11 aceita como está, em PCs de 64 bits:

- **Dois caminhos.** *Seu Windows 11*: o Máquinas abre a página de download da Microsoft, encontra a ISO em Downloads quando ela está completa e a confere com o SHA-256 que a Microsoft publica para cada um dos seus 38 idiomas. *Testar por 90 dias*: a Enterprise Evaluation da Microsoft, em inglês, espanhol ou português, baixada do servidor da Microsoft e guardada só se o hash publicado bater.
- **O LordOS não guarda nada da Microsoft.** O LordOS não hospeda, não inclui nem ativa o Windows, e a licença é assunto seu.
- **O que o Windows 11 pede:** UEFI com inicialização segura, um TPM 2.0 próprio para cada máquina, pelo menos 4 GB, 2 processadores e 64 GB, e disco e rede virtio rápidos cujos drivers o instalador carrega sozinho, sem o passo “Carregar driver”.
- **Instalar sem perguntas.** A sua conta local e uma senha opcional, o nome do computador, a edição com a sua chave (ou a chave que a Microsoft publica para instalar sem ativar), sem pedir uma conta online, as opções de privacidade desligadas, e o idioma, o teclado e o fuso horário deste computador. No fim, o Máquinas apaga o disco pequeno que tinha a senha e tira os dois discos. Ou use o instalador do próprio Windows, guiado, com os drivers já lá.
- **O TPM acompanha a máquina**: os snapshots o guardam, voltar a um o repõe e os clones o levam junto.

## O LordOS numa máquina

O primeiro cartão do assistente de Nova máquina é o próprio LordOS, em desenvolvimento:

- **A ISO live** da versão que o cartão nomeia, para PCs de 64 bits e arm64, conferida com a chave de publicação do LordOS embutida no LordOS antes de confiar em um único byte.
- **Instalação sem perguntas**: a sua conta e senha (guardada só como hash), o nome do computador, o fuso horário, o teclado e o seu idioma. Só o disco novo da própria máquina pode ser apagado.
- **4 processadores, 4 GB, 32 GB e 3D** por padrão, e o agente do convidado rodando desde a primeira inicialização, então o modo apps funciona logo (com a próxima ISO).

## As ferramentas do LordOS para Windows

Um programa pequeno que o LordOS constrói, instalado sozinho depois de uma instalação desassistida (ou pelo botão do agente), rodando como você, nunca como serviço:

- **A resolução segue a janela**, exata, de um quarto a meio segundo depois de cada redimensionamento.
- **Copiar e colar** texto, texto formatado (limpo de scripts neste computador) e imagens.
- **O modo apps**, **os arquivos soltos** e o **Abrir com** (abaixo).
- **O ponteiro desenhado pelo LordOS**, então movê-lo não espera mais pela máquina. Uma chave por máquina o devolve ao convidado.
- **Um clone ganha o próprio nome de computador**, usado a partir da próxima reinicialização.
- **Elas se mantêm atualizadas sozinhas.** Quando este LordOS traz ferramentas mais novas, Máquinas as instala enquanto a máquina roda e nada está sendo copiado; uma chave nos ajustes de Máquinas e em Configurar de cada máquina desliga isso, e então uma notificação oferece **Atualizar**.

## A área de transferência, só quando você cola

Uma máquina é tratada como não confiável: um programa dentro dela poderia ler a sua área de transferência ou trocar o que você copiou.

- **Sob demanda.** Uma máquina só fica sabendo *que* há algo para colar (texto, texto formatado ou uma imagem), nunca o conteúdo, até que um programa nela cole de fato, e só enquanto você a está usando: a janela dela na frente, ou uma das janelas do modo apps, ou dentro de 3 segundos.
- **Proteção em segundo plano.** Uma máquina que tenta ler ou mudar a sua área de transferência enquanto você está em outra coisa é bloqueada, a tentativa é registrada e você recebe um só aviso por rodada.
- **Perguntar antes de cada colagem.** Escolha em Configurar → Compartilhamento e uma barrinha mostra o que vai passar, com *Colar* e *Não colar*, ou *Trazer para o LordOS* e *Ignorar* para uma cópia que sai. Ela cola só o que mostrou, e não pode ser atravessada por uma barra levantada debaixo do seu ponteiro.
- **Os segredos ficam.** Uma senha copiada de um gerenciador de senhas dentro de uma máquina não chega ao seu histórico da área de transferência.

## Arquivos para dentro e para fora

- **Arraste arquivos para a janela de uma máquina** a partir do Arquivos: eles chegam à pasta Downloads da pessoa conectada no convidado, com uma linha de progresso, e recebem o nome só quando o tamanho e o SHA-256 batem.
- **Uma pasta de transferência para cada máquina**, `~/Machines/Transfer/<nome>`, rápida nos dois sentidos. Uma máquina Linux a tem em `/mnt/LordOS` e como pasta `LordOS` em cada home; uma máquina com Windows a recebe como unidade, preparada sozinha depois de uma instalação desassistida.
- **Abrir com uma máquina.** No Arquivos, clique com o botão direito num documento ou num programa → Abrir com → a máquina: ela inicia ou é retomada se precisar, o arquivo é copiado para dentro e aberto lá com o app de sempre, e um programa roda na própria janela do LordOS. Você pode escolher abrir sempre um tipo de arquivo com uma máquina.
- **Soltar num app.** Arquivos soltos numa janela do modo apps vão para essa janela: para a pasta que ela mostra, para um app que aceita arquivos ou para a área de transferência dela.

| Medido no PC de teste (arquivo de 2 GB) | Pelo agente do convidado | Pela pasta de transferência |
|---|---|---|
| Para Downloads de um convidado Ubuntu | cerca de 75 s | **17,6 s**, com o hash conferido |
| Para Downloads de um convidado Windows 11 | 78–80 s | **25,6 s**, com o hash conferido |
| Para fora da máquina, salvo na pasta ou unidade LordOS dela | não era possível | **cerca de 2,5 s** |

## Modo apps

Cada janela da máquina vira a própria janela do LordOS, com o título do convidado, o nome da máquina e o ícone do app no Dock, na barra de tarefas e no alternador, enquanto a área de trabalho e a barra de tarefas do convidado ficam ocultas. Os apps da Loja de um convidado Windows têm cada um a própria entrada e o próprio ícone. Mover, redimensionar, minimizar ou fechar uma janela faz o mesmo no convidado. A janela da máquina vira um controle pequeno com os programas do convidado, e o LordOS só pode iniciar um programa que o próprio convidado listou.

## Som, microfone e redes

- **O som** toca nos alto-falantes deste computador, com o mudo e o volume do próprio convidado. **O microfone** abre só enquanto a opção Microfone da máquina está ligada, a permissão de microfone do LordOS deixa o Máquinas, o convidado está gravando e a máquina está rodando; uma luz mostra isso.
- **Cada máquina com a própria saída e entrada**, escolhidas no Configurar ou pelo botão do alto-falante enquanto ela roda. Um dispositivo desconectado passa para o padrão do sistema e volta quando você o conecta de novo.
- **Quatro modos de rede.** *Compartilhada*, agora com até 32 **portas encaminhadas**, abertas só para este computador por padrão. *Em ponte*: a máquina entra na sua rede cabeada como mais um dispositivo e recebe o endereço do DHCP do seu roteador. *Somente host*: alcança este computador e mais nada, sem senha de administrador. *Isolada*. O Configurar diz o que cada um significa para o firewall: o tráfego de uma máquina em ponte não passa pelo firewall do LordOS.

## 3D para máquinas Linux

Com **Gráficos 3D** em Configurar → Tela, uma máquina Linux desenha com a sua placa de vídeo pelo virgl (OpenGL), em PCs de 64 bits. Os quadros da máquina chegam ao LordOS sem que ele espere nunca o trabalho gráfico do convidado. Só essa máquina pode usar o dispositivo de render da placa, nunca a tela em si.

**As máquinas com Windows continuam em 2D**: não existe um driver 3D livre e estável para elas. O 3D para máquinas com Windows está em desenvolvimento, e ainda não há nada oferecido. As máquinas também são 2D no Raspberry Pi e num desktop que desenha sem placa de vídeo, e uma máquina 3D rodando não pode ser suspensa nem ter snapshots com a memória.

## Uma imagem mais fluida

O LordOS agora lê os quadros de cada máquina no próprio loop e repinta só a parte que mudou. Medido com um convidado Windows 11 que desenha 61 quadros por segundo:

| | Antes | Depois |
|---|---|---|
| Quadros por segundo mostrados | 29,5 | **60,0** |
| Pintura por quadro | 1,7 ms | **0,1 ms** |
| O servidor gráfico do LordOS, de um núcleo | 54 % | **16–18 %** |
| Mover o ponteiro | uma ida e volta pelo convidado, 35–39 ms | **desenhado pelo LordOS no quadro seguinte** |

O resto do desktop recebeu o mesmo cuidado: os Ajustes, a Loja, o Arquivos e o Monitor não congelam mais o ponteiro enquanto trabalham (mudar um ajuste com três apps abertos passou de 161 para 20 ms, uma pasta de 2.220 arquivos abre em 229 ms em vez de quase 2 s), e as chaves que iniciam algo lento mudam na hora com um indicador de espera.

## Também novo

- **Ordem de inicialização**: disco do sistema, CD/DVD e rede na ordem que você escolher, e um menu de inicialização no início se você quiser.
- **Resolução inicial**: *Segue a janela* ou um tamanho fixo, que todo sistema adota ao iniciar.
- **Um visual novo**: folhas sobre a janela, ajustes agrupados e 38 glifos próprios do LordOS que seguem a sua cor de destaque, e uma visão Armazenamento de para onde vai o seu espaço.
- **A Loja, organizada pela origem dos apps**: Debian, Flathub, Apps web e apps do LordOS, e os apps que o LordOS escolheu.
- **Teclas** (em desenvolvimento): Ctrl, Alt, Shift e Win mantidas para a próxima tecla, e combinações que o seu computador guardaria para si, como Ctrl+Alt+Del, enviadas direto à máquina.
- **O modo apps, pela barra de menus** (em desenvolvimento): um item junto ao relógio para cada máquina, cujo menu funciona como o menu Iniciar da máquina, e um painel com suas janelas abertas e seus apps.
- **Capturar o mouse** (em desenvolvimento) para jogos e programas 3D; ❖Esc ou Ctrl+Alt o soltam.
- **Uma placa de vídeo real para uma máquina** (em desenvolvimento): uma parte de uma iGPU Intel (de Broadwell a Comet Lake) com GVT-g, com um driver de inicialização próprio do LordOS para ela, escrito a partir de fontes públicas.
- **`machines` no Terminal** (em desenvolvimento): listar, iniciar, parar, tirar snapshots, clonar e mudar máquinas a partir de um terminal.

## O que foi testado, e o que não

Conferido num PC de 64 bits com KVM, principalmente com uma ferramenta de desenvolvimento que faz o papel do servidor gráfico do LordOS: a instalação desassistida do Windows 11 e as ferramentas dele (resolução, área de transferência nos dois sentidos, a lista de janelas, os ícones e as ações do modo apps), os números da pasta de transferência acima, os três modos de rede novos numa LAN real, o som e o microfone pelo PipeWire, e o 3D com um convidado Ubuntu numa Radeon RX 580.

**Ainda não:** o modo apps desenhando num monitor real, todos os painéis do Configurar na tela, Windows no Raspberry Pi, o 3D para máquinas com Windows, dispositivos USB passados para uma máquina e arrastar arquivos para fora de uma máquina.

O projeto completo, as regras de segurança e cada medição estão em `docs/VIRTUALIZATION.md` e `docs/MACHINES-GPU.md`.
