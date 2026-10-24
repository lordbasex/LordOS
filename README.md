# Lord DE

Fork do [LordOS](https://github.com/lordbasex/LordOS) dedicado a extrair e desenvolver seu ambiente de desktop como um projeto independente da distribuição. Fedora é a primeira plataforma de integração deste fork.

O compositor Wayland em C, o motor gráfico, o runtime React, os temas e os aplicativos nativos continuam sendo os componentes do desktop. A distribuição fornece o kernel, o login, a rede, o áudio e o gerenciamento dos pacotes do sistema.

**Estado: experimental.** A compilação e a inicialização sem monitor são verificáveis em contêiner Fedora. A sessão física com logind/seatd ainda precisa ser validada em hardware, incluindo troca de TTY, GPU, suspensão e SELinux.

## Compilar no Fedora

```sh
sudo dnf install gcc make binutils python3 pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel libxkbcommon-devel \
  libxcrypt-devel libseat-devel openssl-devel nodejs npm
npm ci
make -j4
make check
```

Node.js e npm são usados apenas para compilar os temas. A execução do desktop não depende deles. O caminho padrão usa a composição por CPU quando as bibliotecas Mesa de desenvolvimento estão ausentes. A composição por GPU continua opcional:

```sh
sudo dnf install mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel
make -j4
```

Documentação dos pacotes: [Wayland](https://packages.fedoraproject.org/pkgs/wayland/wayland-devel/), [libxkbcommon](https://packages.fedoraproject.org/pkgs/libxkbcommon/libxkbcommon/), [libseat](https://packages.fedoraproject.org/pkgs/seatd/libseat-devel/), [Node.js](https://developer.fedoraproject.org/tech/languages/nodejs/nodejs.html).

## Instalar e executar

Para inspecionar a instalação sem alterar o sistema:

```sh
make install PREFIX=/usr DESTDIR="$PWD/build/stage"
```

Para uma instalação local com entrada no gerenciador de login:

```sh
sudo make install PREFIX=/usr
```

A instalação adiciona `lord-de-session`, os arquivos em `/usr/libexec/lord-de` e a sessão **Lord DE (experimental)**. Não habilita serviços nem substitui o gerenciador de login. Encerre a sessão atual e selecione essa entrada no login, ou execute `lord-de-session` em uma TTY local livre. Execute o desktop como seu usuário normal, **sem sudo**. O acesso a DRM e evdev passa por libseat com logind ou seatd.

Para verificar a inicialização sem monitor, dentro do checkout:

```sh
cd server
./build/lord-server --headless 800x600 --timeout 2
```

Esse modo não abre uma janela dentro do GNOME/KDE. `Ctrl+Alt+Backspace` encerra a sessão física; `Ctrl+Alt+F1` a `F10` solicita a troca de TTY ao provedor de sessão.

## RPM local

```sh
sudo dnf install rpm-build
sh tools/desktop/dist.sh
mkdir -p build/rpmbuild/SOURCES
cp dist/lord-de-*.tar.gz build/rpmbuild/SOURCES/
rpmbuild -ba --define "_topdir $PWD/build/rpmbuild" packaging/rpm/lord-de.spec
```

O arquivo fonte inclui os temas compilados a partir do lockfile; o build do RPM compila o C sem acessar a rede. É um pacote local experimental, ainda sem preparação para submissão aos repositórios oficiais do Fedora. O RPM usa a composição por CPU para manter suas dependências pequenas.

## Separação entre desktop e SO

O perfil padrão `desktop` não compila o gerenciador Debian, o instalador do SO, o helper administrativo privilegiado, `lord-netd`, `lord-sessiond`, `lord-netperm` ou `lord-fand`. A loja administra aplicativos `.lpk` e oferece abrir o gerenciador de software da distribuição quando disponível. Pacotes RPM/DNF não são tratados como pacotes Debian.

Rede e VPN, usuários, SSH, ventoinhas, formatação de discos e outras operações administrativas do antigo helper ficam com o sistema hospedeiro. As páginas que ainda dependem dele têm funcionalidades limitadas. Áudio usa o protocolo PulseAudio, incluindo PipeWire; Bluetooth e outras integrações existentes continuam usando D-Bus.

O código e os scripts de distribuição do upstream permanecem no repositório como legado, fora da instalação da DE. Para trabalhar explicitamente no perfil antigo: `make -C server PROFILE=os` e `LORD_PROFILE=os npm run build`. Esse perfil continua específico do LordOS/Debian e não foi convertido em uma distribuição Fedora.

Veja [a arquitetura da extração e os próximos passos](docs/STANDALONE.md). A [documentação original](docs/README.md) descreve o LordOS upstream; seus comandos de ISO, instalação e administração não se aplicam automaticamente a este fork.

## Estrutura

- `server/`: compositor, motor gráfico, helpers de aplicativos e camada `src/platform/`.
- `runtime/`, `sdk/`, `themes/`, `apps/`: interface e aplicativos nativos.
- `tools/desktop/`: instalação, verificação e arquivo fonte para empacotamento.
- `packaging/desktop/`, `packaging/rpm/`: integração da sessão e pacote Fedora.
- `tools/iso/`, `tools/rpi/`, `packaging/debian/`: legado da distribuição original.

## Origem e licenças

O código deriva do LordOS, desenvolvido pela iPERFEX. Créditos e avisos de copyright originais são preservados. Consulte [LICENSE](LICENSE) e [THIRD_PARTY.md](THIRD_PARTY.md) para o código e seus componentes. Este fork não é uma versão oficial do LordOS.
