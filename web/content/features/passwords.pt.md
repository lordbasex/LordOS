## Em poucas palavras

Senhas é o gerenciador de senhas do LordOS, e a mesma engrenagem é o **chaveiro do sistema**: o lugar onde os apps guardam segredos como senhas de Wi-Fi, credenciais de VPN e senhas de contas de e-mail. Tudo fica num arquivo de cofre criptografado, e tudo o que toca seus segredos em texto puro roda num único processo pequeno e trancado, o `lord-vault`.

[[shot:0]]

Pense na sala de cofres de um banco. A janela de Senhas é o balcão onde você conversa; o `lord-vault` é o cofre atrás da parede. O atendente (`lord-server`) passa os documentos por uma fresta, mas nunca segura a chave. Só sai o que você pede para ver, um item por vez.

[[diagram:vault]]

## O que ele faz

- **Itens para tudo:** sites, cartões, Wi-Fi (com um QR code para entrar pelo celular), servidores e chaves SSH, licenças com seus arquivos, documentos de identidade com fotos, carteiras de cripto com um aviso extra, notas seguras. Dezesseis categorias de fábrica, mais as suas, em árvore, com tags.
- **Códigos 2FA** (TOTP e HOTP) com contagem regressiva ao vivo. Dá para adicionar um a partir de uma imagem de QR; o segredo nunca sai do `lord-vault`, e a janela só recebe o código atual.
- **Um gerador** de senhas e de frases-senha a partir de uma lista de 7.776 palavras.
- **Um relatório de segurança** com senhas fracas, reutilizadas, antigas e vazadas. "Vazada" é verificado **só offline**: contra uma lista incluída de 10.000 senhas comuns e, se você fornecer, um arquivo local de hashes de senhas vazadas. Nada é consultado online; o `lord-vault` não tem rede nenhuma.
- **Importação** de arquivos KDBX (versões 3.1 e 4), de exportações CSV de navegadores e de exportações CSV e JSON de outros gerenciadores, com leitores próprios do LordOS. **Exportação** para uma cópia criptografada, ou para CSV depois de um aviso em vermelho.

[[shot:3]]

## Como você desbloqueia

O cofre tem uma chave aleatória de 32 bytes. Ela nunca é derivada da sua senha. Em vez disso, cada forma de desbloqueio embrulha essa mesma chave:

- **uma senha mestra**, reforçada com Argon2id, com o custo medido na sua máquina para levar cerca de 0,7 s;
- **uma chave de segurança FIDO2** com a extensão `hmac-secret`, pelo cliente CTAP2 próprio do LordOS;
- **um arquivo-chave**, por exemplo num pendrive;
- **uma senha mais um arquivo-chave**, como dois fatores;
- **uma chave de recuperação imprimível** de 40 caracteres.

Vários métodos podem conviver. Por padrão, o cofre se tranca sozinho depois de 5 minutos sem uso, quando você bloqueia a tela e quando troca de usuário.

[[shot:1]]

## Como ele protege seus segredos

**O arquivo.** Um único arquivo `.lvault`, criptografado com XChaCha20-Poly1305. O cabeçalho é autenticado junto com o corpo, então alterar qualquer byte, trocar um método de desbloqueio ou truncar o arquivo é detectado. A suíte de testes inverte cada byte de um cofre e confere se cada alteração é pega: 1.165 de 1.165. O salvamento é atômico, com um `.bak` da versão anterior.

**O processo.** O `lord-vault` é iniciado pelo `lord-server` a partir de um caminho fixo e sem argumentos, e se tranca antes de ler qualquer coisa: sem rede, sem sockets novos, sem executar programas, sem arquivos além da pasta do cofre que ele recebe já aberta. Ele não pode ser depurado nem ter a memória despejada por outros processos do seu usuário, e os segredos ficam em memória travada (`mlock`), excluída dos core dumps e apagada ao ser liberada.

**A janela.** A própria janela de Senhas roda como um processo em sandbox próprio (`lord-server --app passwords`), fora do JavaScript do desktop. Os valores revelados nunca entram na memória do desktop. As listas mostram títulos e nomes de usuário, nunca segredos; um valor só trafega quando você clica em revelar, copiar ou editar.

**A tela.** A janela de Senhas é protegida: sai preta em capturas de tela, gravações de tela e na área de trabalho remota.

**A área de transferência.** Um segredo copiado fica fora do histórico da área de transferência, é marcado como sensível para os apps Linux que respeitam essa convenção e é apagado depois de 30 segundos (configurável), se ainda estiver lá.

[[shot:2]]

## O chaveiro do sistema

- **Aberto no login.** O `lord-sessiond` confere sua senha de login, entrega-a uma única vez ao `lord-vault` para abrir o chaveiro da sessão e apaga a própria cópia.
- **Os apps do LordOS** guardam seus segredos com o `keyring` do SDK. Cada app vê só os próprios itens. Para os apps `.lpk` instalados, o `lord-server` define o dono a partir da identidade verificada do app, então um app não consegue se passar por outro. Se um app pede um item de outro app, o sistema pergunta a você antes.
- **Os apps Linux** usam o Secret Service padrão do freedesktop sobre D-Bus, servido por código próprio do LordOS. O Chromium foi testado de ponta a ponta: salvou um login, reiniciou, buscou a chave dele no chaveiro do LordOS e preencheu o login. Desde a 0.8.5 uma sessão iniciada com o computador espera o seu barramento de sessão, então com o login automático o chaveiro do LordOS tem o Secret Service desde o início, e se um app traz o gnome-keyring, só a parte PKCS#11 dele roda; na VM de teste o Chrome então pediu pelo aviso do chaveiro do próprio LordOS.

[[shot:4]]

## As perguntas do leitor cético

**Quem pode ler o chaveiro da sessão?** Dentro do LordOS, cada app só os próprios itens, a menos que você compartilhe algum. Pelo Secret Service, a regra de acesso é a do freedesktop: qualquer app Linux da sua sessão que alcance o barramento da sessão pode ler a coleção desbloqueada, como acontece com outros chaveiros de desktop. Os apps `.lpk` isolados não alcançam o barramento.

**O app Senhas pode ler a chave de segurança diretamente?** Não. Usar uma chave de segurança passa pelo aviso da permissão `usb`; depois o `lord-server` abre o dispositivo e o entrega direto ao `lord-vault`. Nem a janela do app nem o JavaScript dela chegam a segurá-lo.

## Limites honestos

- **Chaves de segurança reais não foram testadas.** O caminho FIDO2 foi testado só contra um autenticador em software. Chaves que verificam apenas com leitor de digital embutido, e o protocolo de PIN 2, mais novo, não são suportados.
- **O Secret Service é mínimo:** uma coleção, sem regras de acesso por app, sem avisos de exclusão.
- **A grande área de trabalho do Argon2 não pode ser travada na memória** com o limite padrão do Debian; ela é excluída dos dumps e apagada após cada uso.
- **Algumas variantes de KDBX não são suportadas** (Twofish e Argon2 versão 0x10).

## Leia o código

- [server/src/vault/vault.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vault.c): o processo `lord-vault` e seu sandbox
- [server/src/vault/vfile.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/vfile.c): o formato `.lvault` e os métodos de desbloqueio
- [server/src/vault/smem.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/smem.c): a área de memória travada e apagada
- [server/src/vault/keyring.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/keyring.c) e [server/src/vault/secretsvc.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/secretsvc.c): o chaveiro e o Secret Service
- [server/src/vault/ctap2.c](https://github.com/lordbasex/LordOS/blob/main/server/src/vault/ctap2.c): o cliente FIDO2
- [server/src/sec/brokerkr.c](https://github.com/lordbasex/LordOS/blob/main/server/src/sec/brokerkr.c): o chaveiro para apps isolados
- [apps/passwords](https://github.com/lordbasex/LordOS/tree/main/apps/passwords): o app
- [docs/PASSWORDS.md](https://github.com/lordbasex/LordOS/blob/main/docs/PASSWORDS.md)
