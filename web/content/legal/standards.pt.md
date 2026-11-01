O LordOS é feito por uma equipe pequena, e tentamos trabalhar como fazem os projetos de código aberto sérios. Esta página explica, com honestidade, com quais normas internacionais nossos processos estão alinhados, o que implementa cada uma e o que ainda falta.

**O LordOS e a iPERFEX não têm nenhuma certificação ISO.** "Alinhado com" significa que seguimos as principais práticas de uma norma e publicamos as evidências abaixo; não significa que um auditor nos avaliou ou certificou, nem que cumprimos todos os requisitos.

## Divulgação e tratamento de vulnerabilidades: ISO/IEC 29147 e ISO/IEC 30111

**O que fazemos:** uma [Política de segurança](/legal/security/) pública com canal de relato, escopo, porto seguro e prazos de resposta (confirmação em 3 dias úteis, avaliação em 10, correção de problemas graves em geral em até 30, divulgação coordenada em 90 dias); um [security.txt](/.well-known/security.txt) legível por máquinas; créditos e avisos para as vulnerabilidades corrigidas; versões assinadas para que as correções sejam confiáveis.

**O que falta:** um processo interno documentado de tratamento (triagem, pontuação de gravidade, etapas de correção e publicação); uma página pública de avisos de segurança; identificadores CVE, por meio de uma CVE Numbering Authority ou do GitHub; o relato privado de vulnerabilidades do GitHub, que ainda não está ativado.

## Conformidade de licenças de código aberto: ISO/IEC 5230 (OpenChain)

**O que fazemos:** um inventário de cada componente de terceiros incluído e sua licença no [THIRD_PARTY.md](https://github.com/lordbasex/LordOS/blob/main/THIRD_PARTY.md); avisos de licença na página de [Licenças](/legal/licenses/), no repositório e no arquivo de copyright de cada pacote do Debian; uma lista de materiais de software (SBOM) em SPDX 2.3, gerada por `tools/release/make-sbom.sh` e anexada a cada versão a partir da 0.8.0 beta; o processo está documentado em [docs/COMPLIANCE.md](https://github.com/lordbasex/LordOS/blob/main/docs/COMPLIANCE.md). As contribuições entram sob a mesma licença MIT com uma assinatura de conformidade (veja [Contribuir](/legal/contributing/)).

**O que falta:** uma política de código aberto por escrito; uma pessoa designada como responsável pela conformidade; treinamento para quem contribui; uma autocertificação OpenChain.

## Garantia de segurança do código aberto: ISO/IEC 18974 (OpenChain)

**O que fazemos:** a base Debian recebe as atualizações de segurança do Debian, instaladas pela Loja ou pelo apt; o SBOM relaciona os componentes e as versões de cada versão; os arquivos de cada versão e os pacotes `.lpk` são assinados; a política de segurança define como recebemos e tratamos os relatos.

**O que falta:** um acompanhamento sistemático de vulnerabilidades conhecidas (CVEs) contra o SBOM de cada versão; um processo documentado para isso; uma autocertificação de conformidade OpenChain.

## Práticas de segurança da informação (no estilo ISO/IEC 27001)

Aplicamos algumas práticas descritas pela ISO/IEC 27001, só onde elas são reais:

- **Privilégio mínimo:** os apps rodam no próprio sandbox (Landlock, seccomp, cgroups, sem capacidades), e os processos auxiliares que lidam com dados não confiáveis (e-mail, documentos, mídia, o cofre de senhas) ficam sem privilégios e sem acesso à rede, como documenta o [docs/PRIVACY.md](https://github.com/lordbasex/LordOS/blob/main/docs/PRIVACY.md). As credenciais de publicação do site só podem gravar num bucket de armazenamento e atualizar uma distribuição da CDN.
- **Versões assinadas:** cada arquivo de uma versão aparece num `SHA256SUMS` assinado (veja [Assinatura de código](/legal/code-signing/)).
- **Custódia de chaves:** a chave de assinatura de versões fica só na máquina do mantenedor, nunca no repositório nem numa máquina de compilação, com uma cópia de segurança offline.

**O que falta:** um sistema de gestão de segurança da informação (SGSI), uma avaliação de riscos formal, controles documentados e auditorias internas ou externas. **Não** temos a certificação ISO/IEC 27001.

## Acessibilidade do site: WCAG 2.2 AA

**O que fazemos:** o site busca o nível AA da WCAG 2.2: acesso por teclado com contorno de foco visível, um link para pular ao conteúdo, movimento reduzido, temas claro e escuro, regiões e textos alternativos, como descreve a [Declaração de acessibilidade](/legal/accessibility/).

**O que falta:** uma auditoria completa contra a WCAG 2.2 AA, com testes com leitores de tela; do lado da distribuição, um leitor de tela, a lupa e um tema de alto contraste.

## security.txt: RFC 9116

**O que fazemos:** `/.well-known/security.txt` com os campos Contact, Expires, Preferred-Languages, Canonical e Policy.

**O que falta:** uma assinatura OpenPGP do arquivo.

## Dúvidas

Se você vir uma lacuna que não listamos, ou quiser ajudar a fechar uma, escreva pelo [nosso formulário de contato](https://www.iperfex.com/contacto/?lang=pt).
