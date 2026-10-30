Levamos a segurança do LordOS a sério e agradecemos a quem nos ajuda a encontrar e corrigir problemas. Esta página explica como relatar uma vulnerabilidade e o que você pode esperar de nós.

## Como relatar uma vulnerabilidade

Escreva pelo **[nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt)**, com "Security" no assunto. Quando o repositório for público, o relato privado de vulnerabilidades do GitHub também estará disponível. Por favor, não abra uma issue pública, não publique em redes sociais nem divulgue detalhes até que uma correção esteja disponível.

Inclua, na medida do possível:

- a versão do LordOS (no app Sobre o LordOS, por exemplo "0.8.6 · Beta") e se é a ISO, a imagem para Raspberry Pi ou o pacote do Debian;
- o componente afetado (por exemplo o compositor, o sandbox de apps, o firewall por app, a área de trabalho remota, o cofre de senhas, a Loja, o script de instalação para Debian ou o site);
- os passos para reproduzir, uma prova de conceito se você tiver, e o impacto que você vê;
- como você gostaria de ser creditado, se quiser.

Você pode escrever em inglês, espanhol ou português.

## Escopo

**Dentro do escopo:**

- o código próprio do LordOS: `lord-server` (o servidor gráfico e compositor Wayland), `lord-sessiond` e a tela de login, o sandbox de apps e o intermediário de permissões, `lord-netperm`, `lord-vault`, os auxiliares de e-mail, documentos e mídia, o motor de pacotes, os apps nativos e o SDK;
- os arquivos de release e as suas assinaturas, e o script de instalação para Debian (`lordos.org/install.sh`);
- a configuração que o LordOS aplica ao sistema (por exemplo os serviços que ele ativa);
- o site lordos.org.

**Fora do escopo** (relate-os aos seus próprios projetos):

- vulnerabilidades em pacotes do Debian, no kernel Linux, no Mesa, em navegadores ou em outro software de terceiros que também afetam o Debian puro ([equipe de segurança do Debian](https://www.debian.org/security/)); avise-nos também se o LordOS piorar o problema;
- apps `.lpk` de terceiros (relate ao autor; avise-nos se algum escapar do sandbox);
- ataques que exigem acesso físico a um computador desbloqueado, engenharia social ou negação de serviço por inundação;
- relatórios de scanners automáticos sem impacto demonstrado, e cabeçalhos de boas práticas ausentes sem consequência de segurança.

## Porto seguro

Se você agir de boa-fé e seguir esta política, consideraremos a sua pesquisa autorizada, não tomaremos medidas legais contra você e diremos isso se outra pessoa o fizer. Boa-fé significa que você:

- testa apenas nos seus próprios computadores, máquinas virtuais e contas;
- não acessa, altera nem apaga dados de outras pessoas, e para assim que chegar a algum;
- não prejudica o site nem os sistemas de outros usuários;
- nos dá um prazo razoável para corrigir o problema antes de divulgá-lo.

## O que esperar

Somos uma equipe pequena, então estas são metas, não garantias:

| Etapa | Meta |
|---|---|
| Confirmamos o recebimento do seu relato | em até 3 dias úteis |
| Confirmamos o problema e dizemos qual gravidade achamos que ele tem | em até 10 dias úteis |
| Corrigimos problemas críticos e de alta gravidade | o mais rápido possível, normalmente em até 30 dias |
| Divulgação pública coordenada | quando a correção for publicada, ou após 90 dias, o que vier primeiro, salvo acordo diferente |

Manteremos você informado, daremos o crédito nas notas de versão se você quiser e publicaremos um aviso para as vulnerabilidades corrigidas. No momento não oferecemos recompensas pagas por falhas.

## Verificar as releases

Cada arquivo de release (a ISO, a imagem para Raspberry Pi e o `.deb`) aparece com o seu SHA-256 em `SHA256SUMS`, e `SHA256SUMS` é assinado com a chave de release do LordOS (Ed25519). O script de instalação para Debian se recusa a instalar quando a assinatura está ausente ou é inválida.

- Chave pública: [lordos-release.pub.pem](/lordos-release.pub.pem)
- Impressão digital (SHA-256 da chave pública em DER):
  `c7155b8e6bbbb180f907381994d06af76d333016cd6f3facf67cadd053ea108e`

Para conferir um download manualmente:

```
base64 -d SHA256SUMS.sig > SHA256SUMS.sig.bin
openssl pkeyutl -verify -pubin -inkey lordos-release.pub.pem -rawin -in SHA256SUMS -sigfile SHA256SUMS.sig.bin
sha256sum -c --ignore-missing SHA256SUMS
```

Se a impressão digital que você vê não for igual à de cima, não instale o arquivo e avise-nos. Como assinamos as versões, os apps e os pacotes, e como protegemos as chaves, está explicado em [Assinatura de código](/legal/code-signing/).

## security.txt

Esta política também é publicada em formato legível por máquinas em [/.well-known/security.txt](/.well-known/security.txt) (RFC 9116).
