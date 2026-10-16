#!/bin/bash
# 在两台机器之间私下搬运"真机跑出来、Git 里没有"的本地状态：着色器普查（DrawCensus，含游戏着色器二进制）与
# 离线着色器构建结果。它们源自游戏数据，不能进 GitHub；没有它们时第一次运行会在每个新程序处按 NVN_SHADER_MISSING
# 停下重新普查（要几十轮）。
# Moves the device-derived local state that is not in Git between machines: draw censuses (game shader binaries)
# and the offline shader build. Both derive from game data and must not go to GitHub.
#
# 用法 / usage:
#   tools/local_state.sh export <文件.tar.gz>    打包 work/build/census-dirs.txt 列出的普查目录 + work/build/wonder-shaders
#   tools/local_state.sh import <文件.tar.gz>    解到 work/ 下，再用本机工具链重建 wonder-shaders 并与包内结果比对
#   tools/local_state.sh rebuild                 只用现有普查重建 work/build/wonder-shaders
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WONDER_WORK_ROOT:-$REPO/work}
die() { echo "✗ $*" >&2; exit 1; }
LIST=$WORK/build/census-dirs.txt

rebuild() {
  [ -s "$LIST" ] || die "没有 $LIST"
  [ -x "$WORK/tools/wonder_xlate/wonder_xlate" ] && [ -x "$WORK/tools/spirv-cross/spirv-cross" ] || die "先运行 tools/setup_env.sh"
  # census-dirs.txt holds paths relative to the repository (work/analysis/...).
  (cd "$REPO" && "$WORK/tools/venv/bin/python3" tools/wonder_shader_build.py $(cat "$LIST") --out "$1")
}

case "${1:-}" in
  export)
    OUT=${2:?输出文件，例如 ~/wonder-local-state.tar.gz}
    [ -s "$LIST" ] || die "没有 $LIST"
    (cd "$REPO" && tar -czf "$OUT" "${LIST#$REPO/}" work/build/wonder-shaders $(cat "$LIST"))
    echo "已写 ${OUT}（$(du -h "$OUT" | cut -f1)）。内含游戏着色器数据：只放私人存储，不要传到公开位置。";;
  import)
    IN=${2:?输入文件}
    [ -f "$IN" ] || die "没有 $IN"
    (cd "$REPO" && tar -xzf "$IN")
    CHECK=$WORK/tmp/shader-rebuild.$$; rm -rf "$CHECK"
    rebuild "$CHECK" > "$WORK/tmp/shader-rebuild.log" 2>&1 || { tail -20 "$WORK/tmp/shader-rebuild.log"; die "本机重建着色器失败"; }
    # manifest.json records absolute tool paths, so compare the generated stages themselves.
    differ=0
    for f in "$WORK/build/wonder-shaders"/*.metal "$WORK/build/wonder-shaders"/*.refl; do cmp -s "$f" "$CHECK/$(basename "$f")" || { echo "  不同：$(basename "$f")"; differ=1; }; done
    if [ "$differ" = 0 ]; then echo "✓ 本机重建的着色器与导入的逐字节一致"; rm -rf "$CHECK"
    else echo "! 本机重建结果与导入的不同（工具版本不同？）；构建仍使用导入的 work/build/wonder-shaders，重建结果在 ${CHECK}"; fi;;
  rebuild)
    rebuild "$WORK/build/wonder-shaders";;
  *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2;;
esac
