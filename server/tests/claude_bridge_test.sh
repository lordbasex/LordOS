#!/bin/sh
# claude_bridge_test.sh — lord-claude-status (src/term/claudebridge.c): keeps Claude Code's status JSON for the
# Terminal's footer and passes the same input to the user's own status line. Needs build/lord-claude-status.
cd "$(dirname "$0")/.." || exit 2
B=$PWD/build/lord-claude-status
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
J='{"session_id":"e9be29c2-84cd-4d07-b0e2-348c94977132","model":{"display_name":"Opus 5.5"},"context_window":{"used_percentage":24},"rate_limits":{"five_hour":{"used_percentage":37,"resets_at":1794110000}}}'
D="$T/h1/.cache/lordos/claude"
mkdir -p "$T/h1"
out=$(printf '%s' "$J" | HOME="$T/h1" LANG=es_AR.UTF-8 "$B")
check "no command: LordOS's own status line" '[ "$out" = "Opus 5.5 | ctx 24% | 5h 37%" ]'
check "keeps the JSON by session" '[ "$(cat "$D/e9be29c2-84cd-4d07-b0e2-348c94977132.json")" = "$J" ]'
check "only the owner reads it" '[ "$(stat -c %a "$D/e9be29c2-84cd-4d07-b0e2-348c94977132.json")" = 600 ] && [ "$(stat -c %a "$D")" = 700 ]'
check "no temporary file left" '[ "$(ls -A "$D" | wc -l)" = 1 ]'
out=$(printf '%s' "$J" | HOME="$T/h1" "$B" sh -c 'cat | wc -c')
check "the user status line gets the same input" '[ "$out" = "$(printf "%s" "$J" | wc -c)" ]'
out=$(printf '%s' "$J" | HOME="$T/h1" "$B" sh -c 'echo "Opus | LordOS | 76%"')
check "and its output is what Claude Code shows" '[ "$out" = "Opus | LordOS | 76%" ]'
printf '%s' "$J" | HOME="$T/h1" "$B" sh -c 'exit 3'
check "and its exit status" '[ $? = 3 ]'
mkdir -p "$T/repo/.git" "$T/repo/a/b" "$T/wt"
echo 'ref: refs/heads/fix/x' > "$T/repo/.git/HEAD"
echo "gitdir: $T/repo/.git" > "$T/wt/.git"
line() { printf '%s' "$1" | HOME="$T/h1" LANG="$2" "$B"; }
out=$(line '{"model":{"display_name":"Opus 5.5"},"workspace":{"current_dir":"'"$T"'/repo/a/b"},"context_window":{"used_percentage":10.6},"rate_limits":{"seven_day":{"used_percentage":4.6}}}' es_AR.UTF-8)
check "the folder, its branch from a folder above, 5h without the window, the week in Spanish" '[ "$out" = "Opus 5.5 | b | fix/x | ctx 11% | 5h – | sem 5%" ]'
out=$(line '{"workspace":{"current_dir":"'"$T"'/wt"},"rate_limits":{"five_hour":{"used_percentage":37},"seven_day":{"used_percentage":61}}}' it_IT.UTF-8)
check "a worktree's branch, the week in Italian" '[ "$out" = "Claude Code | wt | fix/x | 5h 37% | sett 61%" ]'
out=$(line "$(printf '{"model":{"display_name":"A\\u001b]0;x\\u0007|B"}}')" C)
check "no escape sequences or separators from the input" '[ "$out" = "A]0;xB" ]'
out=$(line '{"model":{"display_name":"X"},"context_window":{"used_percentage":24}}' C)
check "no limits (an API key account): none shown" '[ "$out" = "X | ctx 24%" ]'
mkdir -p "$T/h2"
printf '{"session_id":"../../evil"}' | HOME="$T/h2" "$B" >/dev/null
printf '{"session_id":"a/b"}' | HOME="$T/h2" "$B" >/dev/null
printf 'not json' | HOME="$T/h2" "$B" >/dev/null
check "bad session ids and bad JSON are not kept" '[ ! -e "$T/h2/.cache/lordos/claude" ] || [ -z "$(ls -A "$T/h2/.cache/lordos/claude")" ]'
check "nothing written outside the folder" '[ ! -e "$T/evil.json" ] && [ ! -e "$T/h2/evil.json" ]'
mkdir -p "$T/h3/.cache/lordos" "$T/elsewhere"
ln -s "$T/elsewhere" "$T/h3/.cache/lordos/claude"
printf '%s' "$J" | HOME="$T/h3" "$B" >/dev/null
check "a link in place of the folder is not followed" '[ -z "$(ls -A "$T/elsewhere")" ]'
mkdir -p "$T/h1/.cache/lordos/claude" && touch -d '10 days ago' "$D/old-session.json"
printf '%s' "$J" | HOME="$T/h1" "$B" >/dev/null
check "snapshots older than a week are removed" '[ ! -e "$D/old-session.json" ]'
head -c 2000000 /dev/zero | tr '\0' a | HOME="$T/h1" "$B" sh -c 'exit 0'
check "an input over 1 MB is drained, not kept" '[ $? = 0 ]'
[ "$fails" = 0 ] && echo "claude_bridge_test: all passed" || echo "claude_bridge_test: $fails failed"
[ "$fails" = 0 ]
