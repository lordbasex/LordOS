# lord-bash.sh — LordOS Terminal shell integration for bash (the same escape sequences popular terminal emulators use).
#
# La terminal lanza `bash --rcfile lord-bash.sh -i`. Este archivo primero carga lo mismo
# que un shell de login (/etc/profile y ~/.bash_profile, ~/.bash_login o ~/.profile) y
# después agrega secuencias de escape estándar:
#   OSC 133;A / B   comienzo y fin del prompt (marcas para saltar entre comandos)
#   OSC 133;C       empieza la salida del comando
#   OSC 133;D;n     terminó el comando con código n (marca azul o roja)
#   OSC 7           directorio actual (pestañas nuevas abren ahí)
#   OSC 1337;RemoteHost=usuario@host   para el cambio automático de perfil por host
# y una función ssh: la conexión queda compartible (ControlMaster) con su socket en la carpeta de la terminal,
# así el modo Archivos abre SFTP sobre la misma sesión, sin otro login (docs/TERMINAL.md, "Files mode").

[ -r /etc/profile ] && . /etc/profile
if [ -r ~/.bash_profile ]; then . ~/.bash_profile
elif [ -r ~/.bash_login ]; then . ~/.bash_login
elif [ -r ~/.profile ]; then . ~/.profile
fi

if [ -n "$LORD_TERM_INTEGRATION_LOADED" ] || [ "$TERM_PROGRAM" != LordTerminal ]; then return 0; fi
LORD_TERM_INTEGRATION_LOADED=1

__lord_status() {
    local s=$?
    printf '\033]133;D;%s\007' "$s"
    printf '\033]7;file://%s%s\007' "${HOSTNAME:-localhost}" "$PWD"
    printf '\033]1337;RemoteHost=%s@%s\007' "$USER" "${HOSTNAME:-localhost}"
    return $s
}

__lord_wrap_ps1() {
    case "$PS1" in
        *'133;A'*) ;;
        *) PS1='\[\033]133;A\007\]'"$PS1"'\[\033]133;B\007\]' ;;
    esac
}

PS0=$'\033]133;C\007'"${PS0:-}"
if [[ "$(declare -p PROMPT_COMMAND 2>/dev/null)" == "declare -a"* ]]; then
    PROMPT_COMMAND=(__lord_status "${PROMPT_COMMAND[@]}" __lord_wrap_ps1)
else
    PROMPT_COMMAND="__lord_status${PROMPT_COMMAND:+; $PROMPT_COMMAND}; __lord_wrap_ps1"
fi

# Mensaje de error en el idioma de la región (LANGUAGE / LANG).
__lord_msg() {
    case "${LANGUAGE:-${LC_ALL:-${LC_MESSAGES:-$LANG}}}" in
        es*) echo "lord_imgcat: no se puede leer $1" ;;
        pt*) echo "lord_imgcat: não é possível ler $1" ;;
        *)   echo "lord_imgcat: cannot read $1" ;;
    esac
}

# imgcat: shows an image in the terminal (the OSC 1337 inline-image protocol).
#   imgcat [-w ancho] [-h alto] archivo...   (ancho/alto en celdas, "Npx", "N%" o auto)
lord_imgcat() {
    local f w=auto h=auto
    while [ $# -gt 0 ]; do
        case "$1" in
            -w) w=$2; shift 2 ;;
            -h) h=$2; shift 2 ;;
            *) break ;;
        esac
    done
    for f in "$@"; do
        [ -r "$f" ] || { __lord_msg "$f" >&2; continue; }
        printf '\033]1337;File=name=%s;width=%s;height=%s;inline=1:' "$(printf %s "$f" | base64 -w0)" "$w" "$h"
        base64 -w0 < "$f"
        printf '\007\n'
    done
}
command -v imgcat >/dev/null 2>&1 || alias imgcat=lord_imgcat

# Insignia (badge) en la esquina de la terminal: lord_badge "texto"
lord_badge() { printf '\033]1337;SetBadgeFormat=%s\007' "$(printf %s "$*" | base64 -w0)"; }

# ssh: la conexión de esta pestaña, compartible por el modo Archivos (un socket por shell, en la carpeta 0700 de
# la terminal; ControlPersist=no: se cierra con la sesión). Si ya pasás tus propias opciones de multiplexado,
# no se toca nada. LORD_SSH_MUX=0 lo apaga.
ssh() {
    if [ -n "$LORD_SSH_MUX_DIR" ] && [ -d "$LORD_SSH_MUX_DIR" ] && [ "${LORD_SSH_MUX:-1}" != 0 ]; then
        case " $* " in
            *ControlPath*|*ControlMaster*|*" -S "*|*" -O "*|*" -M "*) command ssh "$@"; return ;;
        esac
        command ssh -o ControlMaster=auto -o "ControlPath=$LORD_SSH_MUX_DIR/%C-$$" -o ControlPersist=no "$@"
    else
        command ssh "$@"
    fi
}
