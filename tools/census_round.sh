#!/bin/bash
# 着色器普查循环的一步：取回上一轮写出的 DrawCensus → 加进 work/build/census-dirs.txt → 离线重建着色器 →
# 以交互模式启动下一轮（App 保持运行，实时日志在 console-1.log）。
# One step of the shader census loop: fetch the previous round's DrawCensus, add it to census-dirs.txt, rebuild the
# shaders, launch the next round interactively. Signing / device variables are passed through to device_round.sh.
# Usage: census_round.sh <previous round> <next round>
set -euo pipefail
PREV=${1:?previous round, e.g. first-frame-round117}
NEXT=${2:?next round, e.g. first-frame-round118}
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WONDER_WORK_ROOT:-$REPO/work}
cd "$REPO"
tools/device_round.sh "$PREV" --fetch-only > /dev/null 2>&1 || true
CENSUS=$WORK/analysis/$PREV/Documents/DrawCensus
LIST=$WORK/build/census-dirs.txt
if [ ! -s "$CENSUS/index.txt" ]; then echo "  $PREV 没有新的 DrawCensus（无缺失着色器）"; else
python3 - "${CENSUS#$REPO/}" "$LIST" <<'PY'
import os, sys
census, path = sys.argv[1], sys.argv[2]
have = open(path).read().split()
for key in open(os.path.join(census, 'index.txt')).read().split():
    d = f'{census}/{key}/'
    files = os.listdir(d) if os.path.isdir(d) else []
    if 'draw.json' not in files: sys.exit(f'✗ {d} is incomplete: {files}')
    if d not in have: have.append(d); print('added', d)
open(path, 'w').write(' '.join(have) + '\n')
PY
fi
# Every program in RomFS too (tools/extract_shaders.py, round153), after the real censuses so their bound types win.
OFFLINE=$WORK/build/offline-shaders/census-dirs.txt
[ -s "$OFFLINE" ] || "$WORK/tools/venv/bin/python3" tools/extract_shaders.py "$WORK/build/wonder-romfs.wrom" --out "$WORK/build/offline-shaders" | tail -1
"$WORK/tools/venv/bin/python3" tools/wonder_shader_build.py $(cat "$LIST") $(cat "$OFFLINE") --out "$WORK/build/wonder-shaders" 2>&1 | grep -m1 '^built'

WONDER_INTERACTIVE=1 tools/device_round.sh "$NEXT" 2>&1 | tail -1
