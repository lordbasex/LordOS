# Fedora 44 Live com Axia DE

Remix experimental da ISO Fedora Workstation 44 x86_64, com Axia DE 0.8.7 como sessão padrão. O GDM continua como tela de login, com os componentes GNOME necessários para seu greeter. As sessões de desktop GNOME ficam ocultas.

## Gerar a imagem

Use o RPM da DE e uma ISO Fedora 44 Workstation x86_64. O script usa Podman sem sudo; extração, DNF e reconstrução do sistema de arquivos rodam em um contêiner Fedora 44. Reserve aproximadamente 25 GB para os arquivos de trabalho, além da imagem de saída.

```sh
bash tools/fedora-live/build.sh \
  "$HOME/Downloads/Fedora-Workstation-Live-44-1.7.x86_64.iso" \
  dist/axia-de-0.8.7-2.fc44.x86_64.rpm \
  dist/Fedora-Axia-DE-Live-44-0.8.7.x86_64.iso
```

A saída deve ser um arquivo novo. A ISO de origem entra no contêiner somente para leitura. A pasta de trabalho fica em `build/fedora-live.*`; um quarto argumento permite escolher outra pasta vazia. Os downloads vêm dos repositórios Fedora configurados na imagem; as versões instaladas podem variar com a data da geração.

Os arquivos extraídos pertencem ao root do namespace do Podman. Para apagar uma pasta de trabalho, use `podman unshare rm -rf build/fedora-live.XXXXXX`; um `rm` comum não tem permissão.

O resultado inclui a ISO, um arquivo `.iso.sha256` e um `.iso.json` com os hashes da base, do RPM e do resultado. O sistema de arquivos live é EROFS, embora o nome usado pela imagem Fedora seja `LiveOS/squashfs.img`. Os rótulos SELinux são gerados com a política da própria imagem; as capabilities dos arquivos são restauradas a partir do banco RPM. O xorriso conserva a estrutura de boot BIOS/UEFI e o checksum de mídia é recalculado com isomd5sum.

## Sessão e instalação

O livesys do Fedora cria o usuário live e faz login automático no Axia DE. Ao iniciar, a sessão abre **Instalar o Axia DE** (`tools/fedora-live/axia-install.py`, com o auxiliar de root `axia-install-root` liberado por polkit só para o usuário live), que também fica no launcher:

- confere o pendrive com a soma de verificação gravada na ISO (`checkisomd5`), o que detecta uma gravação corrompida antes da cópia dos arquivos;
- mostra a memória e os discos, com aviso abaixo de 2 GB de RAM ou 20 GB de disco;
- abre o instalador do Fedora (`liveinst`, Anaconda) e, quando ele fecha, lê os logs do Anaconda: numa falha, diz a causa provável (leitura do pendrive, falta de espaço ou outra), mostra as linhas do erro e salva um relatório em `~/axia-instalacao-*.tar.gz`.

O facilitador e seu atalho são removidos do sistema instalado pelo script pós-instalação.

A configuração do Anaconda volta a mostrar a criação de usuário e a escolha de data/hora. O GNOME Initial Setup é removido: a conta real deve ser criada no instalador. Um script pós-instalação restaura a configuração normal do GDM, com Axia DE como sessão padrão e autenticação normal.

A sessão importa seus endereços Wayland e Xwayland para o barramento do usuário e inicia o agente MATE de autenticação polkit. O Firefox usado pelo instalador roda por Xwayland: no teste desta versão, o caminho Wayland do Firefox exibia a interface, mas não respondia corretamente aos cliques. O instalador usa um perfil temporário com o tema do Anaconda e abre em tela cheia, mantendo os botões de navegação fora da área de interação do Dock.

A configuração de rede usa os serviços Fedora; `nm-connection-editor` e `nmtui` estão disponíveis. A composição deste RPM é por CPU, como definido em `packaging/rpm/axia-de.spec`.

Esta imagem mantém as limitações da DE independente descritas em [STANDALONE.md](STANDALONE.md). Uma inicialização em VM verifica essa configuração virtual; GPUs, periféricos, suspensão e SELinux em outras máquinas ainda dependem de testes em hardware.

## Referências

- [Estrutura e EROFS da mídia live Fedora](https://fedoraproject.org/wiki/Changes/EROFSforLiveMedia).
- [Construção de sistemas de arquivos EROFS](https://erofs.docs.kernel.org/en/latest/mkfs.html).
- [Personalização do Anaconda e páginas de criação de conta](https://anaconda-installer.readthedocs.io/en/latest/anaconda-webui/docs/customization.html).
- [Configuração e papel do GDM](https://github.com/GNOME/gdm/blob/main/README.md).
- [xorriso: preservação da estrutura de boot com replay](https://manpages.debian.org/unstable/xorriso/xorriso.1.en.html).
