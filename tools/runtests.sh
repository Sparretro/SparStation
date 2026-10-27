#!/bin/sh
# Pasa los ps1-tests y compara la VRAM con la captura de la PS1 real cuando la hay.
# uso: tools/runtests.sh [filtro] [frames]
cd "$(dirname "$0")/.." || exit 1
filter=${1:-.}
frames=${2:-300}
mkdir -p out/tests
find tests_dl/ps1-tests -name '*.exe' | grep -v /tools/ | grep -E "$filter" | sort | while read -r t; do
	name=$(echo "${t#tests_dl/ps1-tests/}" | sed 's#/#_#g; s#\.exe$##')
	dir=$(dirname "$t")
	ref=""
	[ -f "$dir/vram.bmp" ] && ref="--ref $dir/vram.bmp"
	timeout 120 out/psxpc.exe bios/openbios.bin "$t" "$frames" "out/tests/$name" --vram $ref > "out/tests/$name.log" 2>&1
	pass=$(grep -ciE '^\s*(pass|ok)\b' "out/tests/$name.log")
	fail=$(grep -ciE '^\s*fail' "out/tests/$name.log")
	diff=$(grep -oE 'VRAM_DIFF -?[0-9]+' "out/tests/$name.log" | cut -d' ' -f2)
	printf '%-45s pass %3s  fail %3s  %s\n' "$name" "$pass" "$fail" "${diff:+vram_diff=$diff}"
done
