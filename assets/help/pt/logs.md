---
title: Registros
app: logs
section: apps
order: 45
keywords: registros, logs, journal, erros, depurar, console, diagnóstico
---
# Registros

Os registros do sistema e do LordOS. Serve para ver o que aconteceu quando
algo falha.

## Fontes

- **Meus registros:** o seu journal do systemd (seus apps e serviços), lido com as suas permissões.
- **Sistema** e **Kernel:** o journal do sistema. Os administradores (grupo sudo) o veem pelo
  assistente de administração, assim como quem estiver no grupo de leitura do journal.
- **LordOS:** os registros da sua sessão (`compositor`, `shell`, `xwayland`…; "sessão anterior" é a de
  antes deste login), e abaixo o registro de cada **app** e de cada **app web**. Ficam em
  `/var/log/lordos/users/<você>`, que só você pode ler. Cada um fica em 16 MB: ao passar disso, a sua
  parte anterior vira "<nome> (parte anterior)". **Registros antigos** são os que versões anteriores deixaram em `~/.cache/lordos`.

## Filtrar

- **Nível:** tudo, informação, avisos ou erros.
- **Tempo:** últimos 15 minutos, a última hora, 24 horas ou tudo.
- **Processo ou unidade**, e **buscar** no texto das mensagens.

Um clique em uma entrada mostra a mensagem completa com os campos (hora, processo, PID, unidade).

## Ao vivo e exportar

- **Ao vivo** acompanha as mensagens novas conforme chegam, sem recarregar.
- **Exportar** salva o que você vê (com os filtros) em um arquivo de texto na sua pasta, para
  enviar quando pedir ajuda.
