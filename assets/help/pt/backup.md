---
title: Backups
app: backup
section: apps
order: 44
keywords: backup, cópia de segurança, restaurar, time machine, recuperar arquivos
---
# Backups

Cópias automáticas da sua pasta pessoal.

## Começar

1. Em **Ajustes** do app escolha o **destino**: um disco externo (de preferência ext4) ou uma
   pasta. As cópias são guardadas em `Axia DE-Copias`.
2. Escolha a **frequência**: a cada hora, a cada dia, a cada semana ou manual.
3. **Copiar agora** faz a primeira.

## Como funciona

- Cada cópia é uma foto completa da sua pasta pessoal naquele momento, mas **só ocupa o que mudou**:
  os arquivos iguais aos da cópia anterior não são guardados de novo.
- **São mantidas** todas as cópias das últimas 24 horas, uma por dia do último mês e uma por semana
  antes disso. Se o disco encher, as mais antigas são apagadas.
- **Não deixa o desktop lento:** roda com a menor prioridade de processador e de disco, e pausa
  sozinha quando o computador está na bateria (dá para desligar).
- Se for interrompida (o disco é desconectado, o computador desliga), a próxima continua de onde
  parou.
- **Exclusões:** pastas que você não quer copiar. O cache e a Lixeira nunca são copiados.
- Roda com o seu usuário: não precisa de senha de administrador.

## Restaurar

Em **Cópias** escolha a data e navegue como no Arquivos. Você pode **restaurar** um arquivo, uma
pasta ou **tudo**. Se o arquivo já existir, os dois são mantidos (com “(restaurado)” no nome), a
menos que você escolha substituir.

Ao terminar, um [aviso](help:notifications) conta como foi. A criptografia das cópias ainda não
está disponível: se precisar, use um disco criptografado.
