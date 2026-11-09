# lord-fish.fish — LordOS Terminal shell integration for fish (the same escape sequences popular terminal emulators use).
# La terminal lanza `fish -l --init-command 'source …/lord-fish.fish'`: corre después de la
# configuración del usuario. Mismas secuencias que bash/zsh: OSC 133 A/B/C/D, OSC 7 y
# OSC 1337 RemoteHost.

if status is-interactive; and test "$TERM_PROGRAM" = LordTerminal; and not set -q LORD_TERM_INTEGRATION_LOADED
    set -g LORD_TERM_INTEGRATION_LOADED 1
    set -g __lord_ran 0

    function __lord_prompt_start --on-event fish_prompt
        set -l s $status
        test $__lord_ran = 1; and printf '\e]133;D;%s\a' $s
        set -g __lord_ran 0
        printf '\e]7;file://%s%s\a' (prompt_hostname) "$PWD"
        printf '\e]1337;RemoteHost=%s@%s\a' "$USER" (prompt_hostname)
        printf '\e]133;A\a'
    end

    function __lord_preexec --on-event fish_preexec
        set -g __lord_ran 1
        printf '\e]133;C\a'
    end

    # fin del prompt (133;B) después del prompt del usuario o del tema
    if functions -q fish_prompt
        functions -c fish_prompt __lord_user_prompt
        function fish_prompt
            __lord_user_prompt
            printf '\e]133;B\a'
        end
    end

    function lord_imgcat
        set -l w auto
        set -l h auto
        while set -q argv[1]
            switch $argv[1]
                case -w
                    set w $argv[2]; set -e argv[1..2]
                case -h
                    set h $argv[2]; set -e argv[1..2]
                case '*'
                    break
            end
        end
        for f in $argv
            if not test -r $f
                switch "$LANGUAGE$LC_ALL$LC_MESSAGES$LANG"
                    case 'es*'
                        echo "lord_imgcat: no se puede leer $f" >&2
                    case 'pt*'
                        echo "lord_imgcat: não é possível ler $f" >&2
                    case '*'
                        echo "lord_imgcat: cannot read $f" >&2
                end
                continue
            end
            printf '\e]1337;File=name=%s;width=%s;height=%s;inline=1:' (printf %s $f | base64 -w0) $w $h
            base64 -w0 < $f
            printf '\a\n'
        end
    end
    command -q imgcat; or alias imgcat=lord_imgcat

    function lord_badge
        printf '\e]1337;SetBadgeFormat=%s\a' (printf %s "$argv" | base64 -w0)
    end
    # ssh: shareable for Files mode, as in bash (one socket per shell in the terminal's 0700 folder). LORD_SSH_MUX=0 turns it off.
    function ssh --wraps ssh
        if test -n "$LORD_SSH_MUX_DIR"; and test -d "$LORD_SSH_MUX_DIR"; and test "$LORD_SSH_MUX" != 0
            if string match -qr -- '(ControlPath|ControlMaster|^-S$|^-O$|^-M$)' $argv
                command ssh $argv
            else
                command ssh -o ControlMaster=auto -o "ControlPath=$LORD_SSH_MUX_DIR/%C-$fish_pid" -o ControlPersist=no $argv
            end
        else
            command ssh $argv
        end
    end
end
