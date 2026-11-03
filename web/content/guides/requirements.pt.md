O que cada imagem do LordOS precisa. Se um número desta página não bater com outra, é esta que mantemos atualizada.

## Resumo

| | PC (ISO) | Máquina virtual | Raspberry Pi 5 | Pacote para Debian 13 |
|---|---|---|---|---|
| Arquivo | `lordos-live-<versão>-amd64.iso` | `lordos-<versão>-amd64.ova`, ou uma ISO | `lordos-<versão>-rpi5-arm64.img.xz` | `install.sh` |
| Processador | Intel ou AMD de 64 bits (amd64) | pelo menos 4 CPUs | Raspberry Pi 5 | amd64 ou arm64 |
| Memória | 4 GB mínimo, 8 GB recomendado | 4 GB mínimo, ideal 16 GB | 4 GB funciona, 8 ou 16 GB recomendado | o mesmo que num PC |
| Armazenamento | disco de 8 GB mínimo, 32 GB recomendado | disco de 32 GB (o da OVA cresce com o uso) | cartão SD de 16 GB (8 GB mínimo) ou NVMe | 2 GB livres em `/var` |
| Para instalar | um pendrive de 2 GB ou mais | o arquivo OVA | um cartão SD ou um disco NVMe | um sistema com Debian 13 |

## PC

- **Processador:** qualquer Intel ou AMD de 64 bits. Não há versão de 32 bits.
- **Memória:** 4 GB mínimo; 8 GB recomendado, 16 GB com muitos apps abertos.
- **Armazenamento:** instalar precisa de um disco de pelo menos 8 GB (o limite do instalador é 8 GiB); 32 GB ou mais é o recomendado. Instalar ao lado de outro sistema precisa de um disco GPT com partição EFI, o computador iniciado em modo UEFI e pelo menos 8 GB livres. Essa opção ainda nunca foi executada até o fim, então faça um backup antes.
- **Firmware:** UEFI ou BIOS. Desative a inicialização segura (Secure Boot).
- **Gráficos:** qualquer um. Com uma placa de vídeo compatível (via Mesa: AMD, Intel e outras) a área de trabalho é composta na GPU; sem ela, o caminho pela CPU sempre funciona.
- **Para iniciar:** um pendrive de 2 GB ou mais; a ISO tem cerca de 1,1 GB. O sistema ao vivo também pode ser copiado para a memória ao iniciar (há uma opção no menu de inicialização); para isso, dê ao computador 4 GB ou mais.

## ISO arm64

`lordos-live-<versão>-arm64.iso` é para máquinas e máquinas virtuais arm64 que iniciam com UEFI. Até agora só rodou no QEMU emulado, não em hardware real. Os números de memória e armazenamento são os do PC.

## Máquina virtual

- **Processadores e memória:** pelo menos 4 CPUs e 4 GB de memória. Com 2 CPUs o desempenho não é bom. Com muitos apps abertos, o ideal são 16 GB.
- **Hipervisor:** a OVA abre no VirtualBox 7 e no VMware; a ISO funciona neles e em outros, como o Proxmox. Para a ISO, recomenda-se UEFI, com o tipo de sistema Debian 64 bits.
- **O computador que a roda:** um PC de 64 bits com a virtualização por hardware ativada no firmware, e a memória da máquina livre além do que ele já usa.
- **Armazenamento:** o disco da OVA é de 32 GB e cresce conforme o uso.

Passo a passo: [Máquina virtual (OVA)](/pt/virtual-machine/).

## Raspberry Pi 5

- **Placa:** Raspberry Pi 5 de 4, 8 ou 16 GB. 4 GB funciona; 8 ou 16 GB é o recomendado. Os outros modelos de Raspberry Pi não são suportados.
- **Armazenamento:** um cartão SD de 16 GB ou mais (8 GB mínimo), ou um disco NVMe no M.2 HAT+, que é mais rápido.
- **Alimentação e refrigeração:** recomendam-se a fonte oficial de 27 W (5 V, 5 A) e refrigeração ativa.

O [LordOS Imager](/pt/imager/) grava a imagem e prepara a primeira inicialização.

## Num Debian 13 que você já tem

`curl -fsSL https://lordos.org/install.sh | sh` adiciona o LordOS ao Debian 13 (trixie), amd64 ou arm64, iniciado com systemd. Precisa de root ou `sudo` e 2 GB livres em `/var`. Outras distribuições baseadas no Debian podem funcionar, com um aviso.

## Não suportado

- Processadores de 32 bits.
- Iniciar com o Secure Boot ativado.
- Modelos de Raspberry Pi que não sejam o Pi 5.
