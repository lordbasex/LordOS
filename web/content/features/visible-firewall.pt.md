## Em poucas palavras

A maioria dos firewalls pensa em portas e endereços. O firewall do LordOS pensa em **apps**: você decide quais dos seus apps podem usar a rede, e quanto dela. E, como ele está integrado ao Monitor, você vê o que cada app está fazendo antes de decidir.

[[shot:0]]

Imagine um prédio com um porteiro na porta de cada apartamento, em vez de um só na entrada da rua. Quando um app tenta abrir uma conexão, o porteiro da porta dele confere a lista numa fração de microssegundo e deixa passar ou responde "não permitido". O porteiro fica dentro do kernel Linux, então um app insistente não consegue simplesmente dar a volta nele.

[[diagram:path]]

## O que você vê e decide

- **Monitor → Rede** lista os apps com conexões, quanto cada um enviou e recebeu, e um mapa-múndi com um arco até cada país com que eles falam. Cada app tem um botão de **Regra**: desligue e o app perde a rede na hora, com contadores de conexões permitidas e negadas vindos direto do kernel. Abra um app e cada nome que ele alcançou (ou o endereço, quando o resolvedor do sistema não deu nome) tem seu próprio botão para permitir ou bloquear, que diz o que decide aquilo agora. O gráfico se afasta do minuto ao vivo para 1 hora, 24 horas, 7 dias e 30 dias, a partir de um pequeno histórico guardado na sua pasta pessoal: bytes por minuto e por hora, nunca um endereço, um nome ou um país.
- **Ajustes → Privacidade → Rede dos apps** tem um modo e duas listas:
  - **Silencioso** (o padrão): o que não tem regra é permitido e fica anotado, para você revisar no Monitor. **Desligado**: só valem as suas negações explícitas e as restrições. **Negar em silêncio** (desde a 0.8.5): só se conecta o que uma regra permite, e todo o resto é negado e anotado, enquanto os serviços do próprio computador continuam funcionando.
  - **Apps confiáveis**: permitidos sem anotação. Uma negação explícita que você definiu continua valendo.
  - **Apps restritos**: marque o que um app não pode fazer. As restrições de rede são **Sem internet** (só LAN), **Sem LAN**, **Sem rede** nenhuma e **Sem conexões de entrada**. As outras tiram dispositivos e dados (veja [Além da rede](#alem-da-rede)).

[[shot:1]]

[[shot:2]]

## Como funciona

**1. Todo app tem um nome que o kernel enxerga.** Quando você abre um app Linux pelo lançador de apps, o `lord-server` pede ao root que o coloque num cgroup próprio, `app-lordos-<id>.scope`. Os apps `.lpk` instalados ganham `app-lordos-lpk-<id>.scope`. Esses escopos são criados pelo root e pertencem a ele, então um app não consegue se mudar para outro.

**2. Um pequeno serviço root aplica as regras.** O `lord-netperm` carrega seus programas eBPF no cgroup do serviço do LordOS, que contém todas as sessões:

| Gancho do kernel | O que confere |
|---|---|
| `connect4`, `connect6` | TCP e UDP conectado |
| `sendmsg4`, `sendmsg6` | UDP enviado com endereço |
| `bind4`, `bind6` | entrada: "sem conexões de entrada" e regras de entrada |
| `sock_create` | "sem rede": o app nem consegue criar um socket de internet |
| `cgroup/dev` | os dispositivos de câmera, USB, captura ALSA e framebuffer de um app Linux, no `open()` |
| LSM `socket_create` | os sockets Bluetooth de um app Linux |

Cada programa descobre a qual app pertence o processo que chamou (pelo ancestral do cgroup no nível do escopo, então processos filhos contam como o mesmo app), decide e devolve permitir ou negar. Um app negado recebe `EPERM`, "Operation not permitted".

[[shot:4]]

**3. Suas regras viram mapas do kernel.** Você muda as regras na sessão; o `lord-server` as salva em `~/.config/lordos/firewall.json` e manda ao `lord-netperm` uma única mensagem sem dados: `RELOAD`. O serviço então lê o arquivo por conta própria, com validação estrita, e compila as regras em mapas hash e de correspondência pelo prefixo mais longo. As regras nunca trafegam pelo socket.

**4. Decidido ao conectar, aplicado em cada pacote.** A decisão é tomada quando uma conexão é aberta ou uma porta é vinculada: cerca de **0,5 µs** por connect UDP numa VM de teste, TCP dentro do ruído. Desde a 0.8.3 a mesma decisão também é aplicada a cada pacote de um app cujas regras negam algo, então uma regra nova corta as conexões que já estão abertas (veja a próxima seção). Isso custa cerca de 150 ns por pacote, 1,6 % de um núcleo a 104.000 pacotes por segundo, e cerca de 30 ns para um app sem regras de negação; a vazão TCP não muda além do ruído (`docs/FIREWALL.md`).

## Bloquear um app corta o que já está aberto

Desde a 0.8.3, desligar a regra de um app, adicionar uma regra que nega um destino ou uma restrição como "sem internet" não só impede conexões novas: um vídeo que já estava tocando também para. O kernel descarta os pacotes das conexões que a regra nova nega, e o `lord-netperm` fecha esses sockets, então o app vê a conexão terminar em vez de ficar travado. Só é cortado o que a regra nova nega: outros destinos, a rede local com "sem internet" e os clientes de um servidor que o app roda continuam funcionando. Voltar a permitir funciona na hora, e o Monitor mostra os bytes descartados como "Tráfego bloqueado".

No Raspberry Pi 5, o fechamento precisa do kernel do LordOS compilado com a destruição de sockets (`6.18.57-lordos.3`); com um anterior os pacotes continuam sendo descartados, mas o app só vê a conexão terminar quando expira o seu próprio tempo limite.

## Funciona por domínio?

O motor funciona, com cuidado sobre a origem dos nomes. Uma conexão só carrega um endereço IP, então, para aplicar uma regra como "permitir só `example.com`", o firewall precisa saber a quais nomes um endereço pertence. Ele aprende isso **só com o resolvedor do sistema**: assina o monitor do systemd-resolved (ou, sem ele, lê as respostas da porta 53 dos servidores em `/etc/resolv.conf`). Ele nunca confia em respostas DNS que um app recebeu nos próprios sockets, que um app malicioso poderia forjar.

[[diagram:dns]]

Na VM de teste, "negar tudo exceto example.com" deixou passar `https://example.com` e negou `https://www.debian.org`, 20 de 20 vezes, mesmo com o cache do resolvedor vazio. Um app que usa seu próprio DNS sobre HTTPS obtém endereços que o firewall nunca aprendeu, então, sob uma regra dessas, eles são negados.

**Onde escrevê-las.** Ajustes → Privacidade → **Regras de rede por app** mostra o padrão de cada app ("Permitir tudo" ou "Bloquear tudo exceto as regras") e suas regras na ordem em que vencem, já que vence a mais específica, com quando cada uma coincidiu pela última vez. A folha que adiciona uma regra aceita um domínio exato, `*.domínio`, um endereço ou uma rede, com porta e protocolo opcionais, e confere o que você digita enquanto digita. Em **Monitor → Rede**, "Permitir este domínio" ou "Bloquear este domínio" num destino de um app abre a mesma folha, preenchida com o nome que o resolvedor do sistema deu para aquele endereço.

**A pegadinha, dita com honestidade: DNS criptografado.** Um navegador que resolve nomes por conta própria com DNS sobre HTTPS nunca pergunta ao resolvedor do sistema, então os nomes dele não são aprendidos. "Permitir só example.com" continua valendo (os endereços que ele obtém nunca foram aprendidos, então são bloqueados), mas "Bloquear example.com" não o impede. As regras por endereço e por rede, o padrão do app e suas restrições continuam valendo, e os Ajustes dizem isso abaixo das regras.

## Além da rede

A mesma lista de apps restritos também pode tirar a câmera, o microfone, a tela, o USB, os seus arquivos, a área de transferência, o Bluetooth e as notificações. Uma restrição vence as permissões que você deu antes: o app é recusado sem aviso, e restringir um app em execução tira dele a câmera e o microfone na hora. Cada uma é aplicada onde o recurso é entregue:

[[diagram:restrictions]]

- **Os apps isolados do LordOS** (apps `.lpk`, Senhas e, desde a 0.8.2, Mail, Calendário, Contatos e Lembretes): as oito, no broker que já intermedeia tudo o que eles recebem.
- **Os apps incluídos** (a linha "LordOS"): câmera, microfone, tela e notificações. Essa linha é o próprio desktop, então arquivos, USB, área de transferência e Bluetooth são restringidos por app.
- **Os apps Linux abertos pelo lançador de apps:** os dispositivos de câmera, USB, captura ALSA e framebuffer são recusados no kernel (`cgroup/dev`); os sockets Bluetooth, por um programa BPF LSM; o microfone e as câmeras do PipeWire, pelo guarda do PipeWire do LordOS, ao vivo, identificando cada cliente pelas credenciais do seu socket; as notificações, pelo servidor de notificações do LordOS, que pergunta ao barramento quem está chamando e descarta a notificação se não conseguir saber; e "sem acesso aos seus arquivos" abre o app, a partir da próxima abertura, em namespaces próprios com uma pasta pessoal vazia e Landlock. A captura de tela: não há protocolo de captura, e os apps X11 (pelo Xwayland desde a 0.8.3) só veem uns aos outros, nunca os apps Wayland nem a tela do LordOS; desde a 0.8.8.8 um app pode pedir pelo portal ScreenCast do LordOS, que compartilha só o que você escolher. Com "sem captura de tela" o portal o recusa sem perguntar e o guarda do PipeWire esconde dele toda tela compartilhada; o dispositivo de framebuffer é negado no kernel.

## Novo na 0.8.5

Isto chegou com o LordOS 0.8.5-beta, publicada em 27 de outubro de 2026.

- **Monitor → Rede, segunda geração.** A lista de apps agora é uma árvore: cada app se abre nos sites e endereços com que fala, e cada endereço nas suas portas, com o que foi enviado e recebido, um pequeno medidor ao vivo e um botão para permitir ou bloquear em cada destino. Um único botão agrupa a vista por app, domínio, endereço, porta ou país e a filtra por direção, protocolo e regra; a busca transforma `app:`, `domain:`, `ip:`, `port:`, `proto:` e `country:` em etiquetas que você pode combinar.
- **Os sockets de ping, raw e de pacote aparecem** sob o app que os abriu. Os sockets IP raw e de pacote podem ver ou forjar tráfego, e a linha diz isso, com o protocolo ou o ethertype e a interface.
- **As partes do próprio LordOS têm suas próprias linhas e regras.** O Mail e a sincronização do calendário, a Loja e as atualizações, impressoras e scanners, o Terminal (com os nomes dos programas que você roda nele) e as atualizações das listas de bloqueio rodam cada um num processo próprio, no seu próprio escopo, então você pode vê-los e bloqueá-los separadamente, e o kernel garante isso. Bloquear a Loja pergunta antes, porque as atualizações de segurança param. A resolução de nomes, a sincronização da hora, o serviço de rede e os seus túneis VPN também aparecem, e nunca são bloqueados. Na 0.8.7, a procura na rede de Arquivos tem sua própria linha. O tráfego de um compartilhamento de rede montado não tem: o cliente do kernel abre seus próprios sockets, fora de qualquer app.
- **Listas de bloqueio.** Ajustes → Privacidade → Listas de bloqueio oferece listas públicas de sites de publicidade, rastreadores, malware e ataques, cada uma com sua fonte e sua licença. Ative uma e o LordOS a baixa por HTTPS uma vez por dia, com um baixador isolado que só recebe um socket já conectado, confere cada linha duas vezes e bloqueia esses destinos para todos os apps. Uma regra que você escreveu para um site sempre vence uma lista, e um endereço só é bloqueado quando todos os nomes que o resolvedor do sistema deu para ele estão na lista, assim um servidor compartilhado não é cortado por acidente.
- **Negar em silêncio**, o modo acima, também por um novo item da barra de menus.
- **Atividade de rede na barra de menus** (opcional, desligada por padrão): as taxas de envio e recebimento, e um painel com os últimos cinco minutos, os apps que estão movendo dados agora, o que foi negado há pouco, o modo do firewall e as listas de bloqueio.

## As perguntas do leitor cético

**Vale também para os apps Linux?** Sim. Esse é um dos pontos principais: Chromium, Firefox ou qualquer app aberto pelo lançador de apps tem seu próprio escopo e suas próprias regras. Os apps incluídos no LordOS e os programas iniciados pelo Terminal contam juntos como "LordOS"; desde a 0.8.5 o Terminal e as partes de rede do próprio LordOS têm linhas próprias.

**Um app consegue escapar do seu escopo?** Isso foi testado. Um app Linux não consegue se inscrever em outro cgroup, porque todo arquivo `cgroup.procs` da árvore da sessão pertence ao root. Ele ainda pode pedir ao gerenciador systemd do usuário que inicie um novo processo fora da árvore (`systemd-run --user`). O LordOS fecha isso também: o `lord-netperm` anexa seus programas também ao `user@<uid>.service` de cada usuário, e qualquer processo ali fica sujeito à união mais estrita das restrições dos apps restritos daquele usuário que estão rodando. Testado com `systemd-run --user --scope`, um serviço transitório e `setsid`: todos negados.

[[shot:3]]

**Posso permitir uma conexão só uma vez, ou ser perguntado?** O motor de regras conhece três durações (para sempre, até o app fechar, só uma vez) e um modo "perguntar", mas o aviso de pergunta ainda não foi construído: hoje "perguntar" se comporta como silencioso. Ele foi projetado como um aviso de nova tentativa: a primeira conexão é negada, você recebe *Permitir uma vez / Sempre / Negar*, e a nova tentativa do app passa.

**O que acontece se o serviço parar?** Os vínculos dele são fechados e tudo fica permitido. O desktop não depende dele. Na ISO final, o serviço estava rodando com seus sete programas de rede anexados.

**O próprio serviço é perigoso?** Ele é mantido pequeno de propósito: não executa programas, não aceita argumentos, não tem sockets de internet próprios (sua unit só permite sockets Unix, de pacote e netlink; pelo netlink ele fecha as conexões que uma regra nega) e aceita um único tipo de mensagem. Ele usa um carregador próprio sobre a chamada de sistema `bpf()`, com os programas compilados dentro do binário; sem libbpf nem bcc.

## Limites honestos

- **ICMP** (ping) só é bloqueado por "sem rede". `listen` sem `bind` é decidido como uma conexão de saída. (Conexões de entrada numa porta que já está aberta são decididas por pacote desde a 0.8.3.)
- **A união do `user@` é grosseira:** enquanto um app restrito roda, todo processo no gerenciador systemd daquele usuário fica sujeito à união das restrições.
- **Um escopo recém-criado** é registrado em microssegundos, mas uma conexão feita antes disso passaria pelas regras padrão.
- **O Monitor mostra os nomes que o resolvedor do sistema deu** para cada destino, ou o endereço quando ele não deu nenhum. Ele nunca faz DNS reverso, então nenhum endereço sai da máquina só porque você olhou para ele.
- **As regras por domínio não veem apps que usam o próprio DNS criptografado** (veja acima).
- **O que as restrições não cobrem para apps Linux:** o Bluetooth através do BlueZ no barramento do sistema (só os sockets Bluetooth diretos são recusados), o barramento do sistema em geral, a área de transferência e programas iniciados fora do lançador de apps. Um cliente novo de PipeWire de um app restrito tem alguns milissegundos antes de o guarda restringi-lo, e enquanto roda um app com a câmera ou o Bluetooth restritos, os seus próprios serviços de áudio também não conseguem abrir uma câmera nem um socket Bluetooth novos.
- **O arquivo de regras pode ser editado por apps Linux sem sandbox**, já que eles rodam como você, a menos que estejam restritos com "sem acesso aos seus arquivos". Os apps `.lpk` instalados não o alcançam.

## Leia o código

- [server/src/netperm/np.bpf.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/np.bpf.c): os programas eBPF de cgroup e LSM
- [server/src/netperm/netperm.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/netperm.c): o serviço root
- [server/src/netperm/npcompile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/npcompile.c): regras em mapas do kernel
- [server/src/netperm/npdns.c](https://github.com/lordbasex/LordOS/blob/main/server/src/netperm/npdns.c): nomes vindos do resolvedor do sistema
- [server/src/fw/rules.c](https://github.com/lordbasex/LordOS/blob/main/server/src/fw/rules.c): regras, precedência e o leitor estrito do arquivo
- [server/src/ui/js_fw.c](https://github.com/lordbasex/LordOS/blob/main/server/src/ui/js_fw.c): `lord.fw`, a API da sessão
- [apps/monitor/Firewall.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/monitor/Firewall.jsx) e [apps/monitor/FwRules.jsx](https://github.com/lordbasex/LordOS/blob/main/apps/monitor/FwRules.jsx): as seções de Ajustes
- [server/src/sec/restrict.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/restrict.c) e [server/src/sec/pwguard.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/pwguard.c): as restrições no broker e o guarda do PipeWire
- [server/tests/netperm_integration.sh](https://github.com/lordbasex/LordOS/blob/main/server/tests/netperm_integration.sh): o teste com o kernel, 18 de 18
- [docs/FIREWALL.md](https://github.com/lordbasex/LordOS/blob/main/docs/FIREWALL.md) e [docs/MONITOR.md](https://github.com/lordbasex/LordOS/blob/main/docs/MONITOR.md)
