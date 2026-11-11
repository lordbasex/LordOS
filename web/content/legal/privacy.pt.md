O LordOS não tem contas, publicidade nem rastreamento, nem no site nem na distribuição. Ninguém vende nem compartilha os seus dados. O único dado pessoal que guardamos é o endereço IP nos registros de requisições do site, usado para estatísticas de visitas que só a equipe do LordOS vê. Esta política tem duas partes: o **site lordos.org** e a **distribuição LordOS** que você instala no seu computador.

## Parte 1: o site

### O que o seu navegador guarda

O site é um conjunto de páginas estáticas. Ele **não define cookies** e guarda apenas dois valores no `localStorage` do seu navegador, no seu dispositivo:

| Chave | O que contém | Para quê |
|---|---|---|
| `lordos-theme` | `light` ou `dark` (nada quando você usa o automático) | para mostrar o tema que você escolheu |
| `lordos-lang-seen` | `1` | para que a sugestão "esta página também está no seu idioma" apareça só uma vez |

Esses valores nunca saem do seu navegador e nós não podemos lê-los. Apague-os quando quiser nas configurações de dados de sites do navegador. Veja o [Cookies e estatísticas](/legal/cookies-and-statistics/).

### Sem serviços de análise, sem anúncios, sem código de terceiros

O site não carrega nenhum serviço de análise, publicidade, widgets de redes sociais nem fontes ou scripts de outros servidores: tudo o que ele precisa é servido por lordos.org.

### Registros do servidor

O site é hospedado na Amazon Web Services (S3 e CloudFront). Como em qualquer site, o seu navegador envia o seu endereço IP e os dados normais de uma requisição (a página pedida, o horário, o user agent do navegador) para receber a página. O nosso provedor de hospedagem pode manter registros padrão de requisições da CDN por motivos de segurança, por um tempo limitado. A partir desses registros contamos as visitas para saber como o projeto cresce: páginas vistas, visitantes, países, sistemas (Windows, macOS, Linux…) e aparelhos, que só a equipe do LordOS vê, com registros que incluem o seu endereço IP e são apagados após 13 meses. Não os usamos para identificar nem traçar o perfil de visitantes. Os detalhes estão em [Cookies e estatísticas](/legal/cookies-and-statistics/).

### Downloads no GitHub

A imagem ISO, a imagem para Raspberry Pi, o pacote `.deb` e as assinaturas deles ficam hospedados no GitHub Releases. Quando você os baixa, ou quando o script de instalação para Debian os busca, o GitHub recebe a sua requisição conforme a [declaração de privacidade do GitHub](https://docs.github.com/pt/site-policy/privacy-policies/github-general-privacy-statement). O GitHub conta quantas vezes cada arquivo de uma versão é baixado e mostra esses totais publicamente; são números agregados, sem informação sobre quem baixou. São os únicos números de downloads que olhamos.

### Doações

Os botões de doação levam você ao PayPal ou ao Mercado Pago (e, mais adiante, à Binance). O pagamento acontece inteiramente nos sites deles: nunca vemos nem guardamos os dados do seu cartão ou da sua conta. O processador nos informa o valor, a data e o nome ou e-mail que você forneceu a ele, que guardamos apenas para a nossa contabilidade. Valem as políticas deles: [PayPal](https://www.paypal.com/myaccount/privacy/privacyhub), [Mercado Pago](https://www.mercadopago.com.ar/privacidad). Veja os [Termos das doações](/legal/donations/).

### Formulário de contato

Se você nos escrever pelo [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt), no site da iPERFEX, guardamos a sua mensagem e os dados de contato que você informar apenas para responder e acompanhar o assunto.

## Parte 2: a distribuição LordOS

### Sem telemetria

O LordOS **não envia telemetria**: nada de estatísticas de uso, relatórios de falhas, identificadores do dispositivo, verificações que "ligam para casa" ou login em contas. Nada sobre você ou o seu computador é enviado para nós.

### A que o LordOS se conecta por padrão

Estas são as únicas conexões de rede que uma instalação nova do LordOS faz sem que você peça algo:

| Conexão | Quando | Por quê | Como desligar |
|---|---|---|---|
| **DHCP e DNS** na sua rede local | quando um cabo de rede ou uma rede Wi-Fi está conectado | para obter um endereço e resolver nomes; o DNS vai para os servidores que a sua rede fornece ou que você define | desconecte, ou defina endereço e DNS manuais em Ajustes → Rede |
| **Hora pela rede (NTP)** via `systemd-timesyncd`, para o pool público de NTP do Debian | quando o computador está on-line | para manter o relógio certo | Ajustes → Data e hora → desligue a hora automática |
| **mDNS** (`avahi-daemon`, nas ISOs e na imagem para Raspberry Pi, com `libnss-mdns` para que os nomes `.local` sejam resolvidos) | apenas na rede local | anuncia o nome do computador para que outros dispositivos da sua rede o encontrem, e responde a consultas de nomes locais | `sudo systemctl disable --now avahi-daemon` |

O fuso horário nunca é deduzido pela rede. Nada nesta tabela vai para o projeto LordOS.

### Só quando você pede

Estas conexões acontecem só depois de uma ação sua:

- **Loja e atualizações:** as listas de pacotes e os pacotes vêm dos espelhos do Debian configurados nas suas fontes do apt (`deb.debian.org` e `security.debian.org` por padrão; no Raspberry Pi, o kernel e o firmware vêm de `archive.raspberrypi.com`). A Loja só as atualiza quando você toca em **Procurar atualizações** (na Loja ou em Ajustes → Atualizações) ou instala algo, e confere cada lista com as assinaturas do Debian. As atualizações do LordOS vêm do repositório assinado do LordOS em `lordos.org/apt`, consultado ao mesmo tempo. Desde a 0.8.5 esse repositório também traz o banner em destaque da Loja (só aceito de uma fonte cujo índice assinado diz que é do LordOS, e conferido com seus hashes) e a lista de apps de Loja → Inteligência Artificial; as fontes `https://` são baixadas por HTTPS. Os apps `.lpk` do LordOS são instalados a partir de arquivos que você escolhe; não existe um catálogo on-line de apps `.lpk`. A CDN do site registra essas requisições como qualquer outra, com o seu endereço IP, por 13 meses; elas não são usadas para estatísticas (veja [Cookies e estatísticas](/legal/cookies-and-statistics/)).
- **Apps dos repositórios dos fabricantes** (desde a 0.8.5): instalar um app de Loja → Inteligência Artificial ou Loja → Comerciais cujo fabricante tem repositório próprio primeiro diz de quem é o repositório que será adicionado e a impressão digital da chave, e depois o adiciona, então a partir daí a Loja e o apt também baixam as listas de pacotes desse fabricante. Remover o app remove esse repositório, a menos que outro app instalado o use. Ao que o próprio app se conecta depende do fabricante.
- **Pacotes de idioma:** adicionar um idioma em Ajustes pode instalar os dicionários e as fontes dele a partir dos mesmos espelhos do Debian.
- **Impressoras e scanners:** Ajustes → Impressoras e scanners pergunta à rede local (mDNS) quais impressoras e scanners existem nela, e só fala com endereços locais.
- **Mail, Calendário e Contatos:** conectam-se apenas aos servidores das contas que você adiciona (IMAP, SMTP, CalDAV, CardDAV). As imagens remotas das mensagens ficam bloqueadas até você tocar em **Carregar imagens**, e os pixels de rastreamento são removidos.
- **Listas de bloqueio** (desde a 0.8.5, todas desligadas por padrão): cada lista que você liga em Ajustes → Privacidade → Listas de bloqueio, ou adiciona pelo endereço https, é baixada por HTTPS uma vez por dia a partir do endereço https de quem a publica, por um baixador isolado. Nada sobre você é enviado no pedido além do que qualquer download leva.
- **Arquivos → Rede e servidores** (desde a 0.8.7): Arquivos → Rede fica desligado até você abrir essa página. Então faz uma passada limitada pela rede local, com mDNS e tentando as portas SMB e NFS nas redes privadas deste computador, e só fala com endereços locais. Conectar ao servidor e o Remote Desk só se conectam aos servidores que você indicar.
- **Fundos em movimento** (desde a 0.8.7): um clipe é baixado do repositório do projeto em `raw.githubusercontent.com` quando você o escolhe, e é conferido com um SHA-256 do pacote. O GitHub vê esse pedido como vê qualquer download.
- **Relatórios de hardware** (desde a 0.8.7): o LordOS não envia nada por conta própria. Compartilhar no GitHub salva um relatório e uma página no seu computador; se você enviar o formulário do GitHub, com a sua própria conta, o relatório fica público lá. Ele não tem campos para números de série, endereços nem nomes de computador, de usuário ou de rede.
- **Wi-Fi, Bluetooth e VPNs:** apenas as redes, os dispositivos e os servidores VPN que você escolhe.
- **A área de trabalho remota** vem **desligada** por padrão (porta 5673 fechada). Ela só escuta depois que você a liga em Ajustes → Compartilhar. No Raspberry Pi, a área de trabalho remota e o servidor SSH também ficam desligados, a menos que você coloque um arquivo de ativação `remote-desktop` ou `ssh` na partição de boot.
- **Navegadores e outros apps:** o Firefox ESR, o Chromium e qualquer app Linux que você instalar são programas separados, de seus próprios projetos. Eles fazem as conexões que as configurações deles permitem, sob as políticas de privacidade deles. Os apps `.lpk` isolados só acessam a rede se você der a permissão, e o firewall por app mostra e controla a que cada app se conecta.
- **O script de instalação para Debian** (`curl -fsSL https://lordos.org/install.sh | sh`) contata apenas `api.github.com` (para encontrar a última versão), `github.com` (para baixá-la com a assinatura) e os seus espelhos do apt. Ele não envia telemetria.

### O que fica no seu computador

- **O mapa-múndi do Monitor** usa um banco de dados offline de IP para país (DB-IP Lite) que vem com o LordOS. Descobrir para onde vai uma conexão nunca consulta um servidor.
- **O mapa de armazenamento da Limpeza** guarda nomes e tamanhos de pastas entre sessões só neste computador, legível só pelo seu usuário. Você pode desligá-lo nos Ajustes da Limpeza ou apagá-lo na Atividade dele.
- **Senhas:** o cofre fica criptografado no seu disco e o processo dele não tem acesso à rede. A verificação de "senha vazada" é apenas offline.
- **Configurações, e-mails, arquivos e registros** ficam no seu computador. Os registros não são enviados para lugar nenhum.

## Parte 3: seus direitos, contato e alterações

### Seus direitos

A distribuição não coleta dados pessoais. O site guarda os endereços IP nos seus registros de requisições por 13 meses, como explicado acima. Para o que guardamos (esses registros, um e-mail que você nos enviou, um registro de doação), você pode pedir para ver, corrigir ou apagar, respeitados os registros que a lei nos obriga a manter. No Brasil, a Lei Geral de Proteção de Dados (LGPD, Lei 13.709/2018) garante esses direitos, e você pode apresentar reclamação à Autoridade Nacional de Proteção de Dados (ANPD). Na Argentina, a Lei 25.326 de proteção de dados pessoais dá esses direitos, com reclamação perante a Agência de Acesso à Informação Pública (AAIP). Na União Europeia ou em outro país com leis semelhantes (como o GDPR), você tem os direitos que essas leis lhe dão.

### Contato

Dúvidas ou pedidos sobre privacidade: [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt).

### Alterações

Se o LordOS ou o site passar a fazer um novo tipo de conexão, ou se esta política mudar de qualquer outra forma, atualizaremos esta página antes e mudaremos a data no topo.
