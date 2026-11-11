*Esta página substitui o antigo Aviso de cookies, que ficava em /legal/cookies/.*

## Sem cookies

O lordos.org **não usa cookies**: nem de sessão, nem de análise, nem de publicidade, nem de rastreamento, e nenhum de terceiros. Por isso o site não mostra um aviso de cookies.

## Sem serviços de análise

Nada roda no seu navegador para medir você:

- **No site:** nenhum serviço de análise, nenhum pixel de rastreamento, nenhuma impressão digital do navegador, nenhum widget de redes sociais e nenhuma fonte ou script carregado de outros servidores.
- **Na distribuição LordOS:** sem telemetria, sem estatísticas de uso, sem relatórios de falhas e sem identificadores do aparelho. As conexões de rede que o LordOS faz estão listadas na [Política de privacidade](/legal/privacy/).

## Estatísticas de visitas

Para saber como o projeto está crescendo, contamos as visitas a partir dos registros padrão de requisições da CDN do site (Amazon CloudFront). São as requisições que o seu navegador envia de qualquer forma para receber cada página: não acrescentamos nenhum código, cookie ou identificador para obtê-las.

De cada requisição guardamos:

- a página ou o arquivo pedido, e o horário;
- **o seu endereço IP**, para contar visitantes distintos;
- o país, que o CloudFront calcula por conta própria a partir do IP;
- a página de onde você veio (o referenciador), quando o seu navegador a envia;
- o agente de usuário do navegador, para saber o sistema (Windows, macOS, Linux, Android, iOS) e o tipo de aparelho (computador, celular, tablet) e para deixar os robôs fora das contagens;
- dados técnicos da resposta: o código de status, o tamanho, quanto tempo levou e qual local do CloudFront a entregou.

Não usamos esses registros para descobrir quem você é, não os cruzamos com outros dados e não os vendemos nem compartilhamos.

- **Quem as vê:** só a equipe do LordOS, num painel privado da AWS: visitas por dia, semana, mês e ano, as páginas mais vistas, as visitas por país e por sistema e aparelho. Não publicamos essas estatísticas nem as compartilhamos com ninguém.
- **O que não contamos:** as requisições que o LordOS faz a `lordos.org/apt` quando procura atualizações. O CloudFront as grava nos mesmos registros, com os mesmos dados, porque não consegue separá-las; nós as deixamos fora de todas as contagens e elas são apagadas junto com o resto.
- **Por quanto tempo:** esses registros são guardados por 13 meses e depois apagados.

## O que o site guarda no seu navegador

Duas pequenas preferências são salvas no `localStorage` do seu navegador, só no seu dispositivo:

| Chave | Valor | Para quê |
|---|---|---|
| `lordos-theme` | `light` ou `dark` | lembra o tema que você escolheu com o botão de tema; no automático nada é salvo e o site segue o seu sistema |
| `lordos-lang-seen` | `1` | mostra só uma vez a sugestão de ler o site no seu idioma |

Elas nunca são enviadas a um servidor, não identificam você e não são compartilhadas com ninguém. Se o seu navegador bloquear o armazenamento, o site continua funcionando; ele só esquece as suas escolhas. Para removê-las, apague os dados do site lordos.org nas configurações do navegador; as janelas privadas as apagam ao serem fechadas.

## Registros do servidor

O site é servido por uma rede de distribuição de conteúdo (CDN). Para entregar cada página, o seu navegador envia o seu endereço IP e os dados habituais da requisição (a página pedida, o horário, o agente de usuário do navegador). O nosso provedor de hospedagem pode guardar os registros padrão de requisições da CDN por segurança, por um tempo limitado. Esses registros do provedor, com endereços IP, são separados das estatísticas acima, e não os usamos para identificar nem traçar o perfil de ninguém.

## Contagem de downloads

Os arquivos de cada versão ficam no GitHub Releases. O GitHub mantém um contador de downloads por arquivo; ele é público, é um único total por arquivo e não diz nada sobre quem baixou. Se algum dia mencionarmos quantas vezes o LordOS foi baixado, esse número vem desses contadores.

## Sites de doação

Os botões de doação levam você ao PayPal ou ao Mercado Pago (e, mais tarde, à Binance). Esses sites usam os próprios cookies, sob as próprias políticas, quando você está neles. Nada deles é carregado no lordos.org antes de você clicar.

## Dúvidas

Veja a [Política de privacidade](/legal/privacy/), ou escreva para nós pelo [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt).
