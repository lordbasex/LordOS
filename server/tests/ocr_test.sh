#!/bin/sh
# ocr_test.sh — lord-ocr (src/doc/h_ocr.c), without lord-server: it reads a picture LordOS rendered (accents
# included), rejects bad language names and missing data, and its confinement blocks what lord-doc's does.
# Needs build/lord-ocr, build/doc-test and the Tesseract eng and spa data.
cd "$(dirname "$0")/.." || exit 2
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
build/doc-test build/lord-ocr ocr tests/ocr-sample.pgm spa+eng > "$T/read.out" 2>/dev/null
check "reads the Spanish line, accents included" 'grep -q "Configuración rápida: año, niño, acción" "$T/read.out"'
check "reads the version" 'grep -q "0\.8\.8\.6 OCR" "$T/read.out"'
build/doc-test build/lord-ocr ocr tests/ocr-sample.pgm 'eng;x' > "$T/bad.out" 2>/dev/null
check "a bad language name is refused" 'grep -q "\"bad-request\"" "$T/bad.out"'
build/doc-test build/lord-ocr ocr tests/ocr-sample.pgm zzz > "$T/none.out" 2>/dev/null
check "missing language data is an error" 'grep -q "\"no-language\"" "$T/none.out"'
build/doc-test build/lord-ocr selftest-ocr > "$T/self.out" 2>/dev/null
check "cannot read ~/.ssh" 'grep -q "\"ssh\":[1-9]" "$T/self.out"'
check "cannot read the home folder" 'grep -q "\"home_file\":[1-9]" "$T/self.out"'
check "cannot write /tmp" 'grep -q "\"write_tmp\":[1-9]" "$T/self.out"'
check "no network (AF_INET)" 'grep -q "\"inet\":[1-9]" "$T/self.out"'
check "no sockets" 'grep -q "\"unix_socket\":[1-9]" "$T/self.out"'
check "no fork" 'grep -q "\"fork\":[1-9]" "$T/self.out"'
[ "$fails" = 0 ] && echo "ocr_test: all passed" || echo "ocr_test: $fails failed"
[ "$fails" = 0 ]
