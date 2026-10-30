Encontrou algo que não funciona, ou tem uma ideia para melhorar o LordOS? Esta página explica onde nos contar e o que incluir, para podermos agir rápido.

## Onde relatar

- **Por enquanto** o repositório do LordOS no GitHub é privado, então os relatos de bugs e os pedidos de recursos vão pelo [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt). Comece o assunto com "Bug" ou "Feature request".
- **Quando o repositório for público**, use o [GitHub Issues](https://github.com/lordbasex/LordOS/issues). Escolha o modelo de relato de bug ou de pedido de recurso; as issues em branco ficam desativadas para que cada relato traga as informações de que precisamos.

Antes de escrever, veja as [perguntas frequentes](/faq/) e, quando forem públicas, as issues que já existem: talvez alguém já tenha relatado.

## Problemas de segurança são diferentes

**Nunca relate uma vulnerabilidade numa issue pública.** Siga a [Política de segurança](/legal/security/) e use o [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt) com "Security" no assunto. Issues públicas sobre problemas de segurança serão ocultadas e tratadas em privado.

## O que incluir num relato de bug

- **A versão do LordOS**, como aparece no app Sobre o LordOS (por exemplo "0.8.6 · Beta").
- **Como você instalou:** a ISO (live ou instalada), a imagem para Raspberry Pi 5 ou o `.deb` no Debian 13.
- **O seu hardware:** o modelo do computador ou da placa, a placa de vídeo, a quantidade de memória e qualquer dispositivo envolvido (impressora, touchpad, adaptador Wi-Fi, monitor).
- **Passos para reproduzir:** o que você fez, um passo por linha, a partir de uma inicialização limpa se possível.
- **O que você esperava** e **o que aconteceu**, com o texto exato de qualquer mensagem de erro.
- **Registros:** abra o app Registros, encontre as entradas do momento do problema e copie-as no relato. Capturas de tela ou uma gravação curta da tela também ajudam.

Um problema por relato, por favor. Se encontrar vários, envie vários relatos.

## O que não incluir

Registros e capturas de tela podem conter informações privadas. Antes de enviar, remova ou esconda:

- senhas, frases-senha, códigos de recuperação, códigos 2FA e chaves privadas;
- dados pessoais: nomes, endereços, telefones, e-mails, nomes de contas;
- o conteúdo de e-mails, documentos ou mensagens privadas;
- endereços IP, nomes de redes Wi-Fi e dados de VPN que você não queira compartilhar.

Nunca vamos pedir uma senha.

## Pedidos de recursos

Conte o que você quer fazer e por quê, não só a solução que tem em mente: o problema nos ajuda a encontrar o design certo. Diga se você precisa disso no dia a dia e se outros desktops ou apps resolvem isso de um jeito que você gosta (descreva o comportamento; não é preciso citar produtos). Não podemos prometer fazer todos os pedidos, mas lemos todos e eles orientam o roteiro.

## O que acontece depois

Somos uma equipe pequena, então estes são objetivos, não garantias:

- confirmamos os relatos enviados pelo formulário de contato em cerca de **5 dias úteis**;
- fazemos a triagem de cada relato: tentamos reproduzi-lo, pedimos mais informações se necessário e atribuímos rótulos e uma prioridade;
- as correções saem na próxima versão que puder incluí-las, e as notas de versão no [GitHub Releases](https://github.com/lordbasex/LordOS/releases) as mencionam.

## Rótulos

Quando as issues forem públicas, estes são os rótulos que usamos:

| Rótulo | Significado |
|---|---|
| `bug` | algo não funciona como está documentado |
| `enhancement` | um recurso novo ou uma melhoria |
| `documentation` | documentação, páginas de ajuda ou textos do site |
| `hardware` | depende de um computador, placa ou dispositivo específico |
| `needs-info` | precisamos de mais detalhes de quem relatou para continuar |
| `good first issue` | um bom ponto de partida para quem está começando a contribuir |
| `duplicate` | já foi relatado em outro lugar (colocamos o link do original) |
| `wontfix` | decidimos não mudar, e explicamos o motivo |

Quer corrigir você mesmo? Veja [Como contribuir](/legal/contributing/).
