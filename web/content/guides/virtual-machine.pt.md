O LordOS vem como uma máquina virtual pronta para amd64: `lordos-<versão>-amd64.ova`. Ela já está instalada, então não há instalador: você a abre no seu hipervisor, liga e cria sua conta.

## O que você precisa

- Um PC de 64 bits (amd64) com a virtualização por hardware ativada no firmware. A OVA não roda em computadores com processador ARM: para esses, use a ISO arm64.
- **VirtualBox 7** (testado no 7.2) ou **VMware** Fusion ou Workstation.
- Memória e processadores para a máquina: **pelo menos 4 CPUs e 4 GB de memória**. Com 2 CPUs o desempenho não é bom. Com muitos apps abertos, **o ideal são 16 GB**. O computador que a roda precisa disso livre, além do que já usa.
- Disco: o download tem cerca de 1,3 GB. O disco da máquina é de 32 GB e cresce conforme o uso; no início ocupa poucos GB.

Todos os números de cada imagem estão em [Requisitos do sistema](/pt/requirements/).

## 1. Baixe e confira

Baixe `lordos-<versão>-amd64.ova`, `SHA256SUMS` e `SHA256SUMS.sig` da [versão mais recente](https://github.com/lordbasex/LordOS/releases). Na pasta com os arquivos:

```sh
shasum -a 256 -c SHA256SUMS --ignore-missing
```

"OK" ao lado do nome quer dizer que é o arquivo que publicamos. Como conferir também a assinatura está em [Assinatura de código](/pt/legal/code-signing/).

## 2. Importe

### VirtualBox

Arquivo → Importar appliance, escolha o `.ova` e clique em Concluir. Em Configurações você vê o que a máquina traz: 4 CPUs, 4 GB de memória e a tela já configurada. As capturas estão em inglês.

![A janela de importação do VirtualBox](docs/vm/virtualbox-import.png "VirtualBox: importar o appliance, com as configurações da máquina")

### VMware Fusion

Arquivo → Importar, escolha o `.ova` e clique em Continuar. Dê um nome e uma pasta à máquina e clique em Salvar. Quando a importação termina, Concluir a liga.

![VMware Fusion: escolher a OVA](docs/vm/vmware-choose.png "VMware Fusion: escolha o arquivo e clique em Continuar")

![VMware Fusion: o resumo depois de importar](docs/vm/vmware-finish.png "VMware Fusion: a máquina importada, 4 núcleos e 4 GB de memória")

Na primeira vez que liga, o Fusion pode perguntar se você quer atualizar a máquina virtual. Qualquer resposta funciona; atualizar está bem, a menos que você também a abra com uma versão anterior do VMware.

![O VMware Fusion pergunta se deve atualizar a máquina](docs/vm/vmware-upgrade.png "VMware Fusion: atualizar ou não, as duas funcionam")

No **VMware Workstation**: Arquivo → Abrir, escolha o `.ova`. Se o VMware disser que o arquivo não segue a especificação OVF, clique em Tentar novamente.

## 3. Crie sua conta

Ligue a máquina. Ela abre sozinha **Configurar o LordOS** (nas capturas, em inglês):

![Configurar o LordOS na primeira inicialização](docs/vm/virtualbox-welcome.png "A primeira inicialização: a configuração abre sozinha")

1. **Idioma e região** e **Teclado**.
2. **Usuário**: seu nome, o nome de usuário, a senha e o nome do computador.

![O passo Usuário](docs/vm/virtualbox-user.png "Usuário: seu nome, o nome de usuário e a senha")

3. **Resumo**, depois **Criar conta**.

![O passo Resumo](docs/vm/virtualbox-summary.png "Resumo, depois Criar conta")

4. **Reiniciar**. Aparece a tela de login: entre com a senha que você escolheu.

![Sua conta está pronta](docs/vm/virtualbox-ready.png "A conta está criada: reinicie para terminar")

![A tela de login depois de reiniciar](docs/vm/vmware-login.png "Depois de reiniciar, a tela de login (aqui no VMware Fusion)")

É igual no VirtualBox e no VMware. Até a conta existir, a máquina roda uma sessão de configuração sem senha, acessível só pela própria tela. Termine este passo antes de deixá-la ligada.

## Ajustar a máquina

Com a máquina desligada, nas configurações dela:

- **Processadores e memória:** aumente se o computador tiver espaço; com muitos apps, o ideal são 16 GB de memória.
- **Tela:** no VirtualBox vem VBoxSVGA; VMSVGA também funciona.
- **Rede:** o VirtualBox usa NAT, que basta para navegar e atualizar; o VMware Fusion escolhe uma conexão em ponte, que coloca a máquina na sua rede. Para que outros computadores cheguem até ela (com a área de trabalho remota, por exemplo), use um adaptador em ponte.

## Se algo der errado

- **O VirtualBox diz `VERR_ALREADY_EXISTS` ao importar:** ficou em `VirtualBox VMs` a pasta de uma máquina anterior com o mesmo nome. Remover uma máquina com "Apenas remover" mantém os arquivos. Mude o nome na tela de importação ou apague essa pasta antiga.
- **Tela preta ou janela pequena:** verifique o controlador de tela (acima) e dê à máquina pelo menos 4 GB de memória.
- **Está lenta:** confira se ela tem 4 CPUs ou mais e se a virtualização por hardware está ativada no firmware do computador.

Para relatar um erro, rode `lordos-issue` no Terminal da máquina e anexe o arquivo que ele grava ao [issue](https://github.com/lordbasex/LordOS/issues).
