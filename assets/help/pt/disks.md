---
title: Discos
app: disks
section: apps
order: 43
keywords: discos, pendrive, usb, formatar, partições, smart, saúde, montar, ejetar, iso
---
# Discos

Para ver e preparar discos e pendrives, como o GNOME Discos.

## Ver

- Os discos internos, os externos e as imagens, com as partições, quanto ocupam e onde estão
  montadas.
- **Saúde (SMART):** “Verificar saúde” lê o que o disco informa: estado, temperatura, horas ligado,
  setores realocados ou pendentes e, nos NVMe, a vida usada. Discos virtuais e muitos cartões não
  informam nada.

## Fazer

- **Montar**, **desmontar** e **ejetar** um pendrive antes de tirá-lo.
- **Formatar** um pendrive ou uma partição em **FAT32** (funciona em todo lugar, arquivos de até
  4 GB), **exFAT** (pendrives grandes, sem esse limite), **ext4** (Linux) ou **NTFS** (para discos compartilhados
  com outros sistemas operacionais, se o programa dele estiver instalado).
- **Renomear** o volume (desmontado).
- **Apagar:** rápido (a tabela e as assinaturas) ou escrevendo zeros em tudo.
- **Criar um USB de boot** com uma imagem `.iso` ou `.img`: ela é copiada como está e depois
  **verificada** lendo o disco.

## Segurança

Antes de apagar qualquer coisa, uma caixa de diálogo mostra exatamente o que será perdido (disco,
partições, onde estão montadas e quantos dados têm) e é preciso digitar o nome do disco. **O disco
do sistema não pode ser formatado nem apagado** por aqui. Os discos internos só podem
ser alterados por um administrador; os pendrives, por qualquer usuário.

Um administrador pode montar uma **partição livre do disco do sistema**, como a de outro sistema
operacional instalado no mesmo disco. Nunca são oferecidas as partições em uso (montadas,
criptografadas, swap) nem as que o sistema reserva para si (EFI, inicialização, reservadas ou de
recuperação). Se o outro sistema operacional não desligou por completo (inicialização rápida ou
hibernação), a partição dele é montada somente leitura: desligue-o por completo para gravar nela.
