# lord-zsh.zsh — LordOS Terminal shell integration for zsh (the same escape sequences popular terminal emulators use).
# Mismas secuencias que lord-bash.sh: OSC 133 A/B/C/D (marcas de prompt y comandos),
# OSC 7 (directorio) y OSC 1337 RemoteHost (usuario@host, para el cambio automático de perfil).

[[ "$TERM_PROGRAM" == LordTerminal && -z "$LORD_TERM_INTEGRATION_LOADED" ]] || return 0
typeset -g LORD_TERM_INTEGRATION_LOADED=1
typeset -g __lord_ran=0

__lord_precmd() {
    local s=$?
    (( __lord_ran )) && printf '\033]133;D;%s\007' "$s"
    __lord_ran=0
    printf '\033]7;file://%s%s\007' "${HOST:-localhost}" "$PWD"
    printf '\033]1337;RemoteHost=%s@%s\007' "$USER" "${HOST:-localhost}"
    printf '\033]133;A\007'
    # fin del prompt: se agrega a PS1 si el tema lo reemplazó
    [[ "$PS1" == *'133;B'* ]] || PS1="$PS1"$'%{\e]133;B\a%}'
    # que este hook quede último aunque .zshrc agregue otros (temas como p10k)
    precmd_functions=(${precmd_functions:#__lord_precmd} __lord_precmd)
}

__lord_preexec() {
    __lord_ran=1
    printf '\033]133;C\007'
}

autoload -Uz add-zsh-hook
add-zsh-hook precmd __lord_precmd
add-zsh-hook preexec __lord_preexec

# Mensaje de error en el idioma de la región (LANGUAGE / LANG).
__lord_msg() {
    case "${LANGUAGE:-${LC_ALL:-${LC_MESSAGES:-$LANG}}}" in
        es*) echo "lord_imgcat: no se puede leer $1" ;;
        pt*) echo "lord_imgcat: não é possível ler $1" ;;
        *)   echo "lord_imgcat: cannot read $1" ;;
    esac
}

# imgcat y lord_badge, como en bash
lord_imgcat() {
    local f w=auto h=auto
    while (( $# )); do
        case "$1" in
            -w) w=$2; shift 2 ;;
            -h) h=$2; shift 2 ;;
            *) break ;;
        esac
    done
    for f in "$@"; do
        [[ -r "$f" ]] || { __lord_msg "$f" >&2; continue; }
        printf '\033]1337;File=name=%s;width=%s;height=%s;inline=1:' "$(printf %s "$f" | base64 -w0)" "$w" "$h"
        base64 -w0 < "$f"
        printf '\007\n'
    done
}
(( $+commands[imgcat] )) || alias imgcat=lord_imgcat
lord_badge() { printf '\033]1337;SetBadgeFormat=%s\007' "$(printf %s "$*" | base64 -w0)"; }

# ssh: shareable for Files mode, as in bash (one socket per shell in the terminal's 0700 folder). LORD_SSH_MUX=0 turns it off.
ssh() {
    if [[ -n "$LORD_SSH_MUX_DIR" && -d "$LORD_SSH_MUX_DIR" && "${LORD_SSH_MUX:-1}" != 0 ]]; then
        case " $* " in
            *ControlPath*|*ControlMaster*|*" -S "*|*" -O "*|*" -M "*) command ssh "$@"; return ;;
        esac
        command ssh -o ControlMaster=auto -o "ControlPath=$LORD_SSH_MUX_DIR/%C-$$" -o ControlPersist=no "$@"
    else
        command ssh "$@"
    fi
}
