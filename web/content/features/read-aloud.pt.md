## Em poucas palavras

**Em desenvolvimento para a 0.8.8.6; ainda não foi lançado. O último lançamento é a 0.8.8.5.**

O LordOS lê texto em voz alta no seu próprio computador, sem internet. Selecione algo no Editor de Texto, no Terminal, nas Notas ou num app Linux como o Firefox, pressione **❖Esc** e você ouve. Pressione ❖Esc de novo e ele para. Nada do que você lê é enviado a lugar nenhum: a fala é feita no seu processador, ou na sua placa de vídeo quando ela ajuda.

Pense num leitor sentado ao seu lado que só lê o que você aponta. Ele não anota nada, não faz ligações e não sai da sala.

[[diagram:flow]]

## Onde você encontra

- **Fala em todo app com texto.** *Editar → Fala* no Editor de Texto e no Terminal (nas Notas, em Ver), e o mesmo no menu do clique direito do Editor de Texto, do Terminal e das Notas: *Iniciar fala*, *Parar fala* e *Salvar como áudio…*. Iniciar fala lê a seleção, ou o texto todo quando nada está selecionado.
- **Apps Linux também.** Um app Linux ganha um menu *Editar* com Fala na barra de menus (no Lord Classic, na faixa de menus da janela), que lê o que você selecionou nele.
- **Um pequeno reprodutor.** Enquanto lê, uma pílula mostra a onda da voz em cores, com ler de novo, pausar e continuar, parar e fechar. Você pode arrastá-lo para qualquer lugar, até para outra tela, e a próxima leitura abre ali.
- **Salvar como áudio…** pergunta onde salvar com o diálogo de sempre e grava um arquivo WAV.
- **Fala ao vivo** (❖⇧Esc): uma caixa onde você digita, pressiona Enter e é dito com a voz escolhida. As frases recentes ficam embaixo, só na memória, para dizer de novo. Ajuda quem não pode ou prefere não falar.
- **O comando `say`** num terminal: `say "Olá"`, `echo texto | say`, `say -f notas.txt -o notas.wav`, `say -v <voz>`, `say --voices`.

O idioma (espanhol, inglês ou português) é deduzido do próprio texto; quando não dá, usa-se o da interface.

## As vozes que você escolhe

**O LordOS não traz nenhuma voz.** Em **Ajustes → Inteligência artificial → Voz** você baixa só o que quiser, por idioma:

- um modelo **Padrão** (128 MB), rápido em qualquer computador, e um **HD** (672 MB; inglês 1,3 GB), mais natural;
- **23 vozes**, 11 femininas e 12 masculinas, cada uma falando todos os idiomas. **▶** toca uma amostra gravada antes de você baixar qualquer coisa.

Elas vêm como **pacotes assinados do LordOS** do próprio repositório de pacotes do LordOS, nunca de um site de terceiros. Só são oferecidas vozes cujas gravações permitem a redistribuição e o uso comercial; vozes com licença não comercial ou de origem não documentada ficaram de fora.

## Sua própria voz

A **Voz pessoal** cria uma voz com a sua em cerca de um minuto, e ela fala todos os idiomas:

1. O LordOS explica o que é e pede seu consentimento antes de gravar qualquer coisa.
2. Primeiro ele escuta o ambiente por três segundos. Se há barulho demais, ou o microfone não manda nada, ele diz.
3. Você lê seis frases curtas, uma de cada vez (cada uma para sozinha quando você termina), ou **fala livremente** de 15 a 30 segundos sobre o que quiser, para quem tem dificuldade em ler em voz alta.

A gravação fica na sua conta, neste computador. **Usar uma gravação…** faz o mesmo a partir de um arquivo de áudio (WAV, MP3, M4A, OGG, Opus ou FLAC).

## Como ele protege você

- **O motor está trancado.** A fala é feita pelo `lord-tts`, um pequeno ajudante que roda no sandbox de ajudantes do LordOS: sem rede, sem sockets, sem iniciar programas, e só pode escrever na sua própria pasta de cache. Ele é o único programa que liga as bibliotecas de fala. Quando usa a placa de vídeo, só pode abrir o nó de render da placa, nunca a tela que o `lord-server` controla.
- **Seu texto nunca vai numa linha de comando.** O `lord-server` inicia o `lord-say` a partir de um caminho fixo, com argumentos fixos, e lhe passa o texto como um arquivo em memória selado. A única coisa que a interface pode escolher é um nome de voz, conferido em C.
- **Salvar não pode ser desviado.** O `lord-server` confere a pasta que você escolheu, cria o arquivo ali e o renomeia quando o áudio termina, então o `say` nunca vê um caminho.
- **A seleção de um app Linux fica com ele.** O desktop só a lê enquanto esse app tem o teclado, e ela nunca chega ao JavaScript.
- **Apps isolados não podem usar.** Apps `.lpk` instalados não têm API de voz.

## Velocidade

Medido num PC, um Intel i9-10900 com uma Radeon RX 580: cerca de 10 segundos de fala em 1,3 s na placa de vídeo e 3 s no processador, com cerca de 220 MB de memória. Ele lê uma frase por vez, então a primeira toca enquanto a seguinte é feita. Uma frase que sai com um chiado embaixo da voz é detectada medindo-a e feita de novo.

## Limites honestos

- **Ainda não foi lançado.** Tudo isso está em desenvolvimento para a 0.8.8.6.
- **Três idiomas:** espanhol, inglês e português.
- **Primeiro você baixa um modelo.** Até lá, a Fala avisa que nenhuma voz está instalada.
- **Não foi testado no Raspberry Pi** (os caminhos de código do processador dele e o driver gráfico do Pi 5).
- **Ele lê o que você seleciona; não é um leitor de tela.** Um leitor de tela, zoom e alto contraste continuam no roadmap.

## Leia o código

- [docs/VOICE.md](https://github.com/lordbasex/LordOS/blob/main/docs/VOICE.md): o projeto, os modelos, as vozes e os testes
- [server/src/doc/h_tts.c](https://github.com/lordbasex/LordOS/blob/main/server/src/doc/h_tts.c): o `lord-tts`, o motor em sandbox
- [server/src/tts/speak.c](https://github.com/lordbasex/LordOS/blob/main/server/src/tts/speak.c): como o `lord-server` inicia o `lord-say`
- [server/src/ui/js_voice.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_voice.c): a API `lord.voice`
- [apps/settings/PersonalVoice.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/settings/PersonalVoice.jsx) e [sdk/livespeech.jsx](https://github.com/lordbasex/LordOS/blob/main/sdk/livespeech.jsx): Voz pessoal e Fala ao vivo

A fala vem do Pocket TTS da Kyutai (pesos do modelo sob CC-BY-4.0), rodado pelo audio.cpp (Apache-2.0) e seu ggml (MIT).
