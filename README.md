# Axia DE

[English](#english) | [Português](#portugues) | [Español](#espanol)

<a id="english"></a>

## English

Axia DE is a fork of [LordOS](https://github.com/lordbasex/LordOS) focused on extracting and developing its desktop environment as a project independent of the distribution. Fedora is this fork’s first integration target.

The C Wayland compositor, graphics engine, React runtime, themes, and native applications remain the desktop components. The host distribution provides the kernel, login, networking, audio, and system package management.

**Status: experimental.** Builds and headless startup can be verified in a Fedora container. Physical sessions with logind/seatd still need hardware validation, including TTY switching, GPU support, suspend, and SELinux.

### Build on Fedora

```sh
sudo dnf install gcc make binutils python3 pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel libxkbcommon-devel \
  libxcrypt-devel libseat-devel openssl-devel nodejs npm
npm ci
make -j4
make check
```

Node.js and npm are only used to build the themes. The desktop does not require them at runtime. The default build uses CPU composition when the Mesa development libraries are unavailable. GPU composition remains optional:

```sh
sudo dnf install mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel
make -j4
```

Package documentation: [Wayland](https://packages.fedoraproject.org/pkgs/wayland/wayland-devel/), [libxkbcommon](https://packages.fedoraproject.org/pkgs/libxkbcommon/libxkbcommon/), [libseat](https://packages.fedoraproject.org/pkgs/seatd/libseat-devel/), [Node.js](https://developer.fedoraproject.org/tech/languages/nodejs/nodejs.html).

### Install and run

To inspect the installation without changing your system:

```sh
make install PREFIX=/usr DESTDIR="$PWD/build/stage"
```

For a local installation with an entry in your login manager:

```sh
sudo make install PREFIX=/usr
```

Installation adds `axia-session`, the files under `/usr/libexec/axia`, and the **Axia DE** session. It does not enable services or replace your login manager. Log out and select this session at login, or run `axia-session` from an available local TTY. Run the desktop as your regular user, **without sudo**. DRM and evdev access goes through libseat with logind or seatd.

To check headless startup from the checkout:

```sh
cd server
./build/axia-server --headless 800x600 --timeout 2
```

This mode does not open a window inside GNOME/KDE. `Ctrl+Alt+Backspace` ends the physical session; `Ctrl+Alt+F1` through `F10` requests a TTY switch from the session provider.

### Local RPM

```sh
sudo dnf install rpm-build
sh tools/desktop/dist.sh
mkdir -p build/rpmbuild/SOURCES
cp dist/axia-de-*.tar.gz build/rpmbuild/SOURCES/
rpmbuild -ba --define "_topdir $PWD/build/rpmbuild" packaging/rpm/axia-de.spec
```

The source archive includes themes built from the lockfile; the RPM build compiles the C code without network access. This is an experimental local package, not yet prepared for submission to the official Fedora repositories. The RPM uses CPU composition to keep its dependencies small.

### Desktop and OS separation

The Debian package manager, the OS installer and Imager, the privileged administration helper, `lord-netd` and `lord-sessiond` have been removed from this fork. `axia-netperm` and `axia-fand` stay in the source but are not built or installed. The store manages `.lpk` applications and offers to open the distribution’s software manager when available. RPM/DNF packages are not treated as Debian packages.

Networking and VPNs, users, SSH, fans, disk formatting, and other administrative operations previously handled by the helper are left to the host system. Pages that still depend on that helper have limited functionality. Audio uses the PulseAudio protocol, including PipeWire; Bluetooth and other existing integrations continue to use D-Bus.

The upstream distribution code (Debian packaging, APT repository, ISO and Raspberry Pi images) is no longer in this repository, and the build has a single profile. The `upstream` remote keeps that history for selective cherry-picks.

See [the extraction architecture and next steps](docs/STANDALONE.md) (in Portuguese). The [original documentation](docs/README.md) describes upstream LordOS; its ISO, installation, and administration commands do not automatically apply to this fork.

### Repository layout

- `server/`: compositor, graphics engine, application helpers, and the `src/platform/` layer.
- `runtime/`, `sdk/`, `themes/`, `apps/`: interface and native applications.
- `tools/desktop/`: installation, verification, and source archive preparation for packaging.
- `packaging/desktop/`, `packaging/rpm/`: session integration and Fedora package.
- `tools/fedora-live/`: experimental Fedora 44 live ISO with Axia DE as the default session; see [FEDORA-LIVE.md](docs/FEDORA-LIVE.md) (in Portuguese).

### Origin and licenses

The code derives from LordOS, developed by iPERFEX. Original credits and copyright notices are preserved. See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md) for the code and its components. This fork is not an official LordOS release, and the LordOS name, crown logo and Lumo mascot are not used in it, as the LordOS trademark policy asks of forks.

[English](#english) | [Português](#portugues) | [Español](#espanol)

<a id="portugues"></a>

## Português

O Axia DE é um fork do [LordOS](https://github.com/lordbasex/LordOS) dedicado a extrair e desenvolver seu ambiente de desktop como um projeto independente da distribuição. Fedora é a primeira plataforma de integração deste fork.

O compositor Wayland em C, o motor gráfico, o runtime React, os temas e os aplicativos nativos continuam sendo os componentes do desktop. A distribuição fornece o kernel, o login, a rede, o áudio e o gerenciamento dos pacotes do sistema.

**Estado: experimental.** A compilação e a inicialização sem monitor são verificáveis em contêiner Fedora. A sessão física com logind/seatd ainda precisa ser validada em hardware, incluindo troca de TTY, GPU, suspensão e SELinux.

### Compilar no Fedora

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

### Instalar e executar

Para inspecionar a instalação sem alterar o sistema:

```sh
make install PREFIX=/usr DESTDIR="$PWD/build/stage"
```

Para uma instalação local com entrada no gerenciador de login:

```sh
sudo make install PREFIX=/usr
```

A instalação adiciona `axia-session`, os arquivos em `/usr/libexec/axia` e a sessão **Axia DE**. Não habilita serviços nem substitui o gerenciador de login. Encerre a sessão atual e selecione essa entrada no login, ou execute `axia-session` em uma TTY local livre. Execute o desktop como seu usuário normal, **sem sudo**. O acesso a DRM e evdev passa por libseat com logind ou seatd.

Para verificar a inicialização sem monitor, dentro do checkout:

```sh
cd server
./build/axia-server --headless 800x600 --timeout 2
```

Esse modo não abre uma janela dentro do GNOME/KDE. `Ctrl+Alt+Backspace` encerra a sessão física; `Ctrl+Alt+F1` a `F10` solicita a troca de TTY ao provedor de sessão.

### RPM local

```sh
sudo dnf install rpm-build
sh tools/desktop/dist.sh
mkdir -p build/rpmbuild/SOURCES
cp dist/axia-de-*.tar.gz build/rpmbuild/SOURCES/
rpmbuild -ba --define "_topdir $PWD/build/rpmbuild" packaging/rpm/axia-de.spec
```

O arquivo fonte inclui os temas compilados a partir do lockfile; o build do RPM compila o C sem acessar a rede. É um pacote local experimental, ainda sem preparação para submissão aos repositórios oficiais do Fedora. O RPM usa a composição por CPU para manter suas dependências pequenas.

### Separação entre desktop e SO

O gerenciador de pacotes Debian, o instalador do SO e o Imager, o helper administrativo privilegiado, `lord-netd` e `lord-sessiond` foram removidos deste fork. `axia-netperm` e `axia-fand` continuam no código-fonte, mas não são compilados nem instalados. A loja administra aplicativos `.lpk` e oferece abrir o gerenciador de software da distribuição quando disponível. Pacotes RPM/DNF não são tratados como pacotes Debian.

Rede e VPN, usuários, SSH, ventoinhas, formatação de discos e outras operações administrativas do antigo helper ficam com o sistema hospedeiro. As páginas que ainda dependem dele têm funcionalidades limitadas. Áudio usa o protocolo PulseAudio, incluindo PipeWire; Bluetooth e outras integrações existentes continuam usando D-Bus.

O código de distribuição do upstream (empacotamento Debian, repositório APT, imagens ISO e Raspberry Pi) não está mais neste repositório, e o build tem um único perfil. O remoto `upstream` guarda esse histórico para `cherry-pick` seletivo.

Veja [a arquitetura da extração e os próximos passos](docs/STANDALONE.md). A [documentação original](docs/README.md) descreve o LordOS upstream; seus comandos de ISO, instalação e administração não se aplicam automaticamente a este fork.

### Estrutura

- `server/`: compositor, motor gráfico, helpers de aplicativos e camada `src/platform/`.
- `runtime/`, `sdk/`, `themes/`, `apps/`: interface e aplicativos nativos.
- `tools/desktop/`: instalação, verificação e arquivo fonte para empacotamento.
- `packaging/desktop/`, `packaging/rpm/`: integração da sessão e pacote Fedora.
- `tools/fedora-live/`: ISO live experimental do Fedora 44 com Axia DE como sessão padrão; veja [FEDORA-LIVE.md](docs/FEDORA-LIVE.md).

### Origem e licenças

O código deriva do LordOS, desenvolvido pela iPERFEX. Créditos e avisos de copyright originais são preservados. Consulte [LICENSE](LICENSE) e [THIRD_PARTY.md](THIRD_PARTY.md) para o código e seus componentes. Este fork não é uma versão oficial do LordOS e não usa o nome LordOS, o logo da coroa nem o mascote Lumo, como a política de marca do LordOS pede aos forks.

[English](#english) | [Português](#portugues) | [Español](#espanol)

<a id="espanol"></a>

## Español

Axia DE es un fork de [LordOS](https://github.com/lordbasex/LordOS) dedicado a extraer y desarrollar su entorno de escritorio como un proyecto independiente de la distribución. Fedora es la primera plataforma de integración de este fork.

El compositor Wayland en C, el motor gráfico, el runtime de React, los temas y las aplicaciones nativas siguen siendo los componentes del escritorio. La distribución proporciona el kernel, el inicio de sesión, la red, el audio y la gestión de los paquetes del sistema.

**Estado: experimental.** La compilación y el inicio sin monitor se pueden verificar en un contenedor Fedora. La sesión física con logind/seatd todavía necesita validación en hardware, incluidos el cambio de TTY, la GPU, la suspensión y SELinux.

### Compilar en Fedora

```sh
sudo dnf install gcc make binutils python3 pkgconf-pkg-config \
  wayland-devel wayland-protocols-devel libxkbcommon-devel \
  libxcrypt-devel libseat-devel openssl-devel nodejs npm
npm ci
make -j4
make check
```

Node.js y npm solo se utilizan para compilar los temas. El escritorio no depende de ellos durante su ejecución. La compilación predeterminada utiliza composición por CPU cuando las bibliotecas de desarrollo de Mesa no están disponibles. La composición por GPU sigue siendo opcional:

```sh
sudo dnf install mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel
make -j4
```

Documentación de los paquetes: [Wayland](https://packages.fedoraproject.org/pkgs/wayland/wayland-devel/), [libxkbcommon](https://packages.fedoraproject.org/pkgs/libxkbcommon/libxkbcommon/), [libseat](https://packages.fedoraproject.org/pkgs/seatd/libseat-devel/), [Node.js](https://developer.fedoraproject.org/tech/languages/nodejs/nodejs.html).

### Instalar y ejecutar

Para inspeccionar la instalación sin modificar el sistema:

```sh
make install PREFIX=/usr DESTDIR="$PWD/build/stage"
```

Para una instalación local con una entrada en el gestor de inicio de sesión:

```sh
sudo make install PREFIX=/usr
```

La instalación añade `axia-session`, los archivos en `/usr/libexec/axia` y la sesión **Axia DE**. No habilita servicios ni sustituye el gestor de inicio de sesión. Cierra la sesión actual y selecciona esta entrada al iniciar sesión, o ejecuta `axia-session` desde una TTY local disponible. Ejecuta el escritorio con tu usuario habitual, **sin sudo**. El acceso a DRM y evdev se realiza mediante libseat con logind o seatd.

Para comprobar el inicio sin monitor desde el checkout:

```sh
cd server
./build/axia-server --headless 800x600 --timeout 2
```

Este modo no abre una ventana dentro de GNOME/KDE. `Ctrl+Alt+Backspace` termina la sesión física; `Ctrl+Alt+F1` a `F10` solicita el cambio de TTY al proveedor de sesión.

### RPM local

```sh
sudo dnf install rpm-build
sh tools/desktop/dist.sh
mkdir -p build/rpmbuild/SOURCES
cp dist/axia-de-*.tar.gz build/rpmbuild/SOURCES/
rpmbuild -ba --define "_topdir $PWD/build/rpmbuild" packaging/rpm/axia-de.spec
```

El archivo fuente incluye los temas compilados a partir del lockfile; la compilación del RPM compila el código C sin acceder a la red. Es un paquete local experimental, todavía sin preparar para su inclusión en los repositorios oficiales de Fedora. El RPM utiliza composición por CPU para mantener pocas dependencias.

### Separación entre escritorio y SO

El gestor de paquetes Debian, el instalador del SO y el Imager, el helper de administración privilegiado, `lord-netd` y `lord-sessiond` se eliminaron de este fork. `axia-netperm` y `axia-fand` siguen en el código fuente, pero no se compilan ni se instalan. La tienda gestiona aplicaciones `.lpk` y ofrece abrir el gestor de software de la distribución cuando está disponible. Los paquetes RPM/DNF no se tratan como paquetes Debian.

La red y las VPN, los usuarios, SSH, los ventiladores, el formateo de discos y otras operaciones administrativas del antiguo helper quedan a cargo del sistema anfitrión. Las páginas que todavía dependen de él tienen funcionalidades limitadas. El audio utiliza el protocolo PulseAudio, incluido PipeWire; Bluetooth y otras integraciones existentes siguen utilizando D-Bus.

El código de distribución upstream (empaquetado Debian, repositorio APT, imágenes ISO y Raspberry Pi) ya no está en este repositorio, y el build tiene un único perfil. El remoto `upstream` conserva ese historial para `cherry-pick` selectivo.

Consulta [la arquitectura de la extracción y los próximos pasos](docs/STANDALONE.md) (en portugués). La [documentación original](docs/README.md) describe LordOS upstream; sus comandos de ISO, instalación y administración no se aplican automáticamente a este fork.

### Estructura del repositorio

- `server/`: compositor, motor gráfico, helpers de aplicaciones y capa `src/platform/`.
- `runtime/`, `sdk/`, `themes/`, `apps/`: interfaz y aplicaciones nativas.
- `tools/desktop/`: instalación, verificación y preparación del archivo fuente para empaquetado.
- `packaging/desktop/`, `packaging/rpm/`: integración de la sesión y paquete Fedora.
- `tools/fedora-live/`: ISO live experimental de Fedora 44 con Axia DE como sesión predeterminada; consulta [FEDORA-LIVE.md](docs/FEDORA-LIVE.md) (en portugués).

### Origen y licencias

El código deriva de LordOS, desarrollado por iPERFEX. Se conservan los créditos y avisos de copyright originales. Consulta [LICENSE](LICENSE) y [THIRD_PARTY.md](THIRD_PARTY.md) para el código y sus componentes. Este fork no es una versión oficial de LordOS y no usa el nombre LordOS, el logo de la corona ni la mascota Lumo, como pide a los forks la política de marca de LordOS.

[English](#english) | [Português](#portugues) | [Español](#espanol)
