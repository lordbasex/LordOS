O LordOS é software livre e as contribuições são bem-vindas: código, documentação, traduções, testes em hardware real e bons relatos de bugs. Esta página explica como participar e as poucas regras que toda contribuição segue.

## Como participar

O repositório é público em [github.com/lordbasex/LordOS](https://github.com/lordbasex/LordOS). Você pode:

- experimentar o LordOS e [relatar problemas](/legal/issues/) no [GitHub Issues](https://github.com/lordbasex/LordOS/issues);
- testá-lo no seu hardware e nos contar o que funciona e o que não funciona;
- **acompanhar** (watch) o repositório ou dar uma **estrela** para saber das versões.

## Fork e pull request

1. Faça um **fork** do repositório no GitHub e crie um branch para a sua mudança.
2. Faça a mudança, com testes quando fizer sentido, e rode as suítes de testes (veja abaixo).
3. Faça o commit com a assinatura de conformidade (`git commit -s`, explicado abaixo).
4. Abra um **pull request contra o `main`**, explicando o que muda e por quê, e colocando o link da issue que ele resolve, se houver.
5. Um mantenedor revisa. Podemos pedir mudanças; quando estiver pronto, é integrado.

Para mudanças grandes (um app novo, um serviço do sistema novo, uma dependência nova), abra primeiro uma issue para combinarmos o design antes de você dedicar tempo a isso.

## Licença das contribuições

O LordOS usa licenciamento **inbound = outbound** (a mesma licença na entrada e na saída): ao contribuir, você licencia a sua contribuição sob a mesma **licença MIT** do resto do projeto ([LICENSE](https://github.com/lordbasex/LordOS/blob/main/LICENSE)). O copyright do seu trabalho continua sendo seu; não há cessão de direitos nem um acordo de contribuição separado para assinar.

## Certificado de Origem do Desenvolvedor (sign-off)

Todo commit precisa ter a assinatura de conformidade, uma linha no final da mensagem do commit:

```
Signed-off-by: Seu Nome <voce@example.com>
```

O `git commit -s` adiciona essa linha para você, com o nome e o e-mail da sua configuração do Git. Ao adicioná-la, você certifica o [Developer Certificate of Origin 1.1](https://developercertificate.org/) (Certificado de Origem do Desenvolvedor), que diz, em resumo, que:

- você escreveu a contribuição, ou tem o direito de enviá-la sob a licença de código aberto do projeto;
- se ela se baseia num trabalho anterior, esse trabalho está sob uma licença de código aberto compatível que permite que você o envie;
- você entende que a contribuição e a sua assinatura são públicas e ficam guardadas para sempre no histórico do projeto.

Use o seu nome real (ou o nome pelo qual você é conhecido publicamente). Pull requests com commits sem a assinatura de conformidade não podem ser integrados.

## Regras do projeto

- **Só em inglês.** Todo o código, os identificadores, os comentários, as mensagens de commit e a documentação em Markdown são escritos em inglês. A exceção são os textos que o usuário vê: eles ficam nos catálogos de idioma (`assets/locales/<idioma>/`) e passam pela tradução, com o inglês como idioma padrão.
- **Os testes precisam passar.** Rode `make -C server test` para as suítes autônomas (e `make -C server test-full` quando mexer nos processos auxiliares ou no sandbox), e `node tools/i18n-check.mjs` quando mexer em textos que o usuário vê. Adicione um teste para cada bug corrigido e cada recurso adicionado, quando possível.
- **C bem escrito.** C11 legível e modular, que compile sem avisos com `-Wall -Wextra`. Respeite a política de dependências do [docs/ARCHITECTURE.md](https://github.com/lordbasex/LordOS/blob/main/docs/ARCHITECTURE.md).
- **Não afirme compatibilidade sem testar.** Se você diz que algo funciona com um hardware ou um app, é porque testou.
- **Regras de marca.** Siga o [kit de marca](https://github.com/lordbasex/LordOS/blob/main/docs/LORDOS-BRAND.md) para o logo, as cores e a grafia "LordOS". Use nomes genéricos para os recursos e deixe fora dos textos públicos as marcas e os nomes de produtos de outras empresas; o `node tools/brand-check.mjs` verifica isso.
- **Nada de segredos.** Nunca faça commit de chaves privadas, senhas ou dados pessoais.

## Traduções

O LordOS vem em inglês, espanhol e português. Outros idiomas entram como **pacotes de idioma** montados a partir dos catálogos de tradução; veja o [docs/LANGUAGES.md](https://github.com/lordbasex/LordOS/blob/main/docs/LANGUAGES.md). Correções nas traduções existentes também são bem-vindas.

## Código de conduta

Todas as pessoas que participam do LordOS seguem o nosso [Código de conduta](/legal/code-of-conduct/). Problemas de segurança nunca são relatados em público: veja a [Política de segurança](/legal/security/).
