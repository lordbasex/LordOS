#!/bin/sh
# migration_shmem_huge_test.sh — packaging/migrations/011-0.8.9.6-shmem-huge-pages.sh over fake system trees
# (LORDOS_MIGRATION_ROOT): "never" becomes "advise", every other value an administrator chose stays, a kernel
# without transparent huge pages is left alone, and a second run changes nothing. Run from server/ by
# tests/run_suites.sh. (The kernel's file reads "always within_size [advise] never deny force" and takes one word;
# the fake file just records what was written.)
set -u
M=$(cd "$(dirname "$0")/../.." && pwd)/packaging/migrations/011-0.8.9.6-shmem-huge-pages.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/lord-mig011.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
fails=0 n=0
F=sys/kernel/mm/transparent_hugepage/shmem_enabled

# case NAME VALUE EXPECTED: a tree whose file holds VALUE ("" = no file), the migration run twice, the file after
case_() {
    n=$((n + 1))
    R="$T/$1"
    mkdir -p "$R/sys/kernel/mm"
    [ -z "$2" ] || { mkdir -p "$R/sys/kernel/mm/transparent_hugepage"; printf '%s\n' "$2" > "$R/$F"; }
    for run in 1 2; do
        LORDOS_MIGRATION_ROOT=$R sh -eu "$M" || { echo "FAIL $1: run $run exited with $?"; fails=$((fails + 1)); return; }
    done
    if [ -z "$3" ]; then
        [ ! -e "$R/$F" ] && return
        echo "FAIL $1: the file was made"; fails=$((fails + 1)); return
    fi
    got=$(cat "$R/$F")
    [ "$got" = "$3" ] && return
    echo "FAIL $1: '$got', expected '$3'"; fails=$((fails + 1))
}

case_ never "always within_size advise [never] deny force" advise
case_ advise "always within_size [advise] never deny force" "always within_size [advise] never deny force"
case_ always "[always] within_size advise never deny force" "[always] within_size advise never deny force"
case_ within_size "always [within_size] advise never deny force" "always [within_size] advise never deny force"
case_ deny "always within_size advise never [deny] force" "always within_size advise never [deny] force"
case_ force "always within_size advise never deny [force]" "always within_size advise never deny [force]"
case_ no-thp "" ""
case_ junk "never" "never"

if [ "$fails" -gt 0 ]; then echo "migration 011: $fails of $n failed"; exit 1; fi
echo "migration 011: $n cases passed"
