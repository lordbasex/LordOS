# /etc/profile.d/lordos-fallback.sh — LordOS's fallback commands (fastfetch and neofetch run lordos-fetch) at the
# END of PATH, so the real package, once installed, comes first (packaging/fallback-bin/fastfetch). Login shells
# read this; the desktop session gets the same PATH from lord-sessiond (src/remote/sess.c).
__lordos_fallback=/usr/lib/lordos/fallback-bin
if [ -d "$__lordos_fallback" ]; then
    case ":$PATH:" in
        *":$__lordos_fallback:"*) ;;
        *) PATH="${PATH:+$PATH:}$__lordos_fallback"; export PATH ;;
    esac
fi
unset __lordos_fallback
