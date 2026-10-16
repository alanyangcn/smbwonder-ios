#!/bin/bash
# 从你自己的游戏包生成本项目需要的全部本地输入（work/ 下），并按 verified-inputs.json 逐项核对哈希。
# Builds every local input this project needs (under work/) from your own game dump and checks each against
# verified-inputs.json.
#
# 用法 / usage:
#   tools/import_game.sh <本体 v0 .nsz|.nsp> <更新 v262144 (1.2.0) .nsz|.nsp> <prod.keys> [--keep-intermediate] [--no-romfs-dir]
#
# 产物 / outputs (all under work/, never in Git):
#   game/update-262144-nstool/0/{main,sdk,rtld,main.npdm}   更新后的 ExeFS（叠在本体上）
#   game/update-262144-nstool/1/...                          合并后的 RomFS（--no-romfs-dir 时打包后删除，省 3.7 GB）
#   game/control-update/                                     更新的 control 分区（control.nacp）
#   build/cpu-native-262144/{main,sdk}.*                     CPU 改写产物（patch_nso.py）
#   analysis/update-262144/*-structure.log, *-equivalence-fixed.log   改写核对证据（构建时校验）
#   build/wonder-romfs.wrom                                  上传到手机 Documents 的 RomFS 归档
# 中间文件（NSP 解压、NCA）默认做完即删；磁盘需要约 20 GB 空闲。
# 需要先运行 tools/setup_env.sh。密钥只在本机读取，不复制进仓库。
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WONDER_WORK_ROOT:-$REPO/work}
TOOLS=${WONDER_TOOLS:-$WORK/tools}
PY=$TOOLS/venv/bin/python3
HACTOOL=$TOOLS/hactool/hactool
NSTOOL=$TOOLS/nstool/nstool
LLVM_BIN=${LLVM_BIN:-$(dirname "$(xcrun -f llvm-objdump)")}
export LLVM_BIN
die() { echo "✗ $*" >&2; exit 1; }
say() { echo "[$(date +%T)] $*"; }
[ $# -ge 3 ] || { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
BASE_PKG=$1; UPD_PKG=$2; KEYS=$3; shift 3
KEEP=0; ROMFS_DIR=1
for a in "$@"; do case "$a" in --keep-intermediate) KEEP=1;; --no-romfs-dir) ROMFS_DIR=0;; *) die "未知参数 $a";; esac; done
for f in "$BASE_PKG" "$UPD_PKG" "$KEYS"; do [ -f "$f" ] || die "找不到文件：$f"; done
for t in "$PY" "$HACTOOL" "$NSTOOL"; do [ -x "$t" ] || die "缺少 ${t}：先运行 tools/setup_env.sh"; done
[ -x "$LLVM_BIN/llvm-objdump" ] || die "找不到 llvm-objdump（Xcode 自带）；可设 LLVM_BIN=<目录>"
EXEFS=$WORK/game/update-262144-nstool
[ ! -e "$EXEFS/0/main" ] || die "已存在 ${EXEFS}（不覆盖；要重做先移走 work/game/update-262144-nstool）"

TMP=$WORK/tmp/import.$$
mkdir -p "$TMP/keys" "$TMP/pkg" "$TMP/nca/base" "$TMP/nca/update"
cleanup() { if [ "$KEEP" = 0 ]; then rm -rf "$TMP"; else echo "中间文件保留在 $TMP"; fi; }
trap cleanup EXIT
# nsz 只从 ~/.switch 或 --keys 读密钥；hactool/nstool 用 -k。hactool 遇到长度不是 16/32 字节的条目（新版 prod.keys
# 里有 17 字节、256/528 字节的项）会直接退出，所以给它一份只含 32/64 位十六进制值的副本。
cp "$KEYS" "$TMP/keys/prod.keys"
awk -F' *= *' 'NF==2 && (length($2)==32 || length($2)==64)' "$KEYS" > "$TMP/keys/hactool.keys"

nsp_of() {   # <package> <label> -> echoes an NSP path
  case "$1" in
    *.nsz|*.NSZ) say "解压 NSZ：$(basename "$1")" >&2
      mkdir -p "$TMP/pkg/$2"; "$TOOLS/venv/bin/nsz" -D --keys "$TMP/keys/prod.keys" -o "$TMP/pkg/$2" "$1" > "$TMP/$2-decompress.log" 2>&1 || { tail -5 "$TMP/$2-decompress.log" >&2; die "NSZ 解压失败"; }
      ls "$TMP/pkg/$2"/*.nsp;;
    *.nsp|*.NSP) echo "$1";;
    *) die "只认 .nsz 或 .nsp：$1";;
  esac
}
BASE_NSP=$(nsp_of "$BASE_PKG" base); UPD_NSP=$(nsp_of "$UPD_PKG" update)

say "拆 PFS0"
"$HACTOOL" -k "$TMP/keys/hactool.keys" --disablekeywarns -t pfs0 --outdir="$TMP/nca/base" "$BASE_NSP" > "$TMP/base-pfs.log" 2>&1 || { tail -5 "$TMP/base-pfs.log"; die "本体包拆不开（密钥不对或文件损坏）"; }
"$HACTOOL" -k "$TMP/keys/hactool.keys" --disablekeywarns -t pfs0 --outdir="$TMP/nca/update" "$UPD_NSP" > "$TMP/update-pfs.log" 2>&1 || { tail -5 "$TMP/update-pfs.log"; die "更新包拆不开（密钥不对或文件损坏）"; }
[ "$KEEP" = 1 ] || rm -rf "$TMP/pkg"

find_nca() {   # <dir> <content type> [program id] -> NCA reporting that content type (and program id)
  # nstool, not hactool: this hactool cannot read key generation 17 headers (the 1.2.0 update program NCA).
  for f in "$1"/*.nca; do
    case "$f" in *.cnmt.nca) continue;; esac
    info=$("$NSTOOL" -k "$KEYS" -t nca "$f" 2>&1 || true)
    echo "$info" | grep -q "Content Type: *$2" || continue
    [ -z "${3:-}" ] || echo "$info" | grep -qi "ProgID: *0x$3" || continue
    echo "$f"; return 0
  done
  return 1
}
BASE_PROGRAM=$(find_nca "$TMP/nca/base" Program 010015100B514000) || die "本体包里没有《超级马里奥 惊奇》(010015100B514000) 的程序内容"
UPD_PROGRAM=$(find_nca "$TMP/nca/update" Program 010015100B514000) || die "更新包里没有程序内容"
UPD_CONTROL=$(find_nca "$TMP/nca/update" Control) || die "更新包里没有 control 内容"
UPD_TIK=$(ls "$TMP/nca/update"/*.tik | head -1); BASE_TIK=$(ls "$TMP/nca/base"/*.tik | head -1)
# The update RomFS is a BKTR patch over the base RomFS, and the base NCA has its own ticket: nstool takes one --tik,
# so the base title key goes in as --titlekey (the encrypted key at 0x180 of a common ticket: title key type at 0x281 = 0).
for t in "$UPD_TIK" "$BASE_TIK"; do [ "$(xxd -p -s 0x281 -l 1 "$t")" = 00 ] || die "$(basename "$t") 不是 common ticket（个人化 ticket 不支持）"; done
BASE_TITLEKEY=$(xxd -p -s 0x180 -l 16 "$BASE_TIK")

say "取更新后的 ExeFS / RomFS（叠在本体上，RomFS 约 3.7 GB）"
mkdir -p "$EXEFS.part"
"$NSTOOL" -k "$KEYS" --tik "$UPD_TIK" --basenca "$BASE_PROGRAM" -x /0 "$EXEFS.part/0" "$UPD_PROGRAM" > "$TMP/exefs.log" 2>&1 || die "ExeFS 取不出：$(tail -1 "$TMP/exefs.log")"
"$NSTOOL" -k "$KEYS" --tik "$UPD_TIK" --titlekey "$BASE_TITLEKEY" --basenca "$BASE_PROGRAM" -x /1 "$EXEFS.part/1" "$UPD_PROGRAM" > "$TMP/romfs.log" 2>&1 || die "RomFS 取不出：$(tail -1 "$TMP/romfs.log")"
# nstool exits 0 when a partition is unreadable; require the merged RomFS to be there.
[ -d "$EXEFS.part/1" ] && [ "$(find "$EXEFS.part/1" -type f | head -1)" ] || { grep WARNING "$TMP/romfs.log" | head -5; die "RomFS 为空（本体 NCA 解不开）"; }
mkdir -p "$WORK/game"
rm -rf "$WORK/game/control-update"
"$NSTOOL" -k "$KEYS" --tik "$UPD_TIK" -x /0 "$WORK/game/control-update" "$UPD_CONTROL" > "$TMP/control.log" 2>&1 || die "control 取不出：$(tail -1 "$TMP/control.log")"
[ "$KEEP" = 1 ] || rm -rf "$TMP/nca"
mv "$EXEFS.part" "$EXEFS"

say "核对 ExeFS 与 NPDM（verified-inputs.json）"
"$PY" - "$REPO/verified-inputs.json" "$WORK" <<'EOF'
import hashlib, json, sys
from pathlib import Path
profile, work = json.loads(Path(sys.argv[1]).read_text()), Path(sys.argv[2])
records = [profile['npdm']] + [m['nso'] for m in profile['modules'].values()]
for r in records:
    got = hashlib.sha256((work / r['path']).read_bytes()).hexdigest()
    print(('  ✓ ' if got == r['sha256'] else '  ✗ ') + r['path'])
    if got != r['sha256']: sys.exit('版本不对：不是 1.2.0 (262144) 的程序文件')
EOF

say "CPU 改写（patch_nso.py）与核对（structck / equiv）"
PATCHED=$WORK/build/cpu-native-262144; ANALYSIS=$WORK/analysis/update-262144
mkdir -p "$PATCHED" "$ANALYSIS"
for m in main sdk; do
  "$PY" "$REPO/vendor/d2r-mobile/dm/tools/patch_nso.py" "$EXEFS/0/$m" "$PATCHED" "$m" > "$PATCHED/$m.patch.log" 2>&1 || { tail -5 "$PATCHED/$m.patch.log"; die "$m 改写失败"; }
  "$PY" "$REPO/vendor/d2r-mobile/dm/verify/structck.py" "$PATCHED" "$m" "$EXEFS/0/$m" > "$ANALYSIS/$m-structure.log" 2>&1 || { tail -3 "$ANALYSIS/$m-structure.log"; die "$m 结构核对失败"; }
  "$PY" "$REPO/vendor/d2r-mobile/dm/verify/equiv.py" "$PATCHED" "$m" "$EXEFS/0/$m" --seed 1 --mutate 300 --tsv "$ANALYSIS/$m-equivalence-fixed.tsv" \
    > "$ANALYSIS/$m-equivalence-fixed.log" 2>&1 || { tail -3 "$ANALYSIS/$m-equivalence-fixed.log"; die "$m 等价核对失败"; }
  echo "  ✓ $m"
done
"$PY" - "$REPO/verified-inputs.json" "$WORK" <<'EOF'
import hashlib, json, sys
from pathlib import Path
profile, work = json.loads(Path(sys.argv[1]).read_text()), Path(sys.argv[2])
bad = []
for m in profile['modules'].values():
    for r in [m['patched']] + m.get('auxiliary', []):
        if hashlib.sha256((work / r['path']).read_bytes()).hexdigest() != r['sha256']: bad.append(r['path'])
for c in profile['checks']:
    p = work / c['path']
    if hashlib.sha256(p.read_bytes()).hexdigest() != c['sha256'] or c['marker'] not in p.read_text(): bad.append(c['path'])
if bad: sys.exit('与已校验产物不一致：' + ', '.join(bad))
print('  ✓ 改写产物与核对证据与 verified-inputs.json 一致')
EOF

say "打包 RomFS 归档（build/wonder-romfs.wrom，约 3.7 GB）"
"$PY" "$REPO/tools/pack_romfs.py" "$EXEFS/1" "$WORK/build/wonder-romfs.wrom" > /dev/null
"$PY" - "$WORK/build/wonder-romfs.wrom.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
want = 'adbbd1b5147480ad078e7eee80955929e56b065c3ef41670e7e15a79aa7cface'
print(('  ✓ ' if r['sha256'] == want else '  ✗ ') + f"wonder-romfs.wrom {r['files']} files sha256 {r['sha256'][:16]}…")
if r['sha256'] != want: sys.exit('RomFS 归档与已校验的不同（文件不全或版本不对）')
EOF
[ "$ROMFS_DIR" = 1 ] || rm -rf "$EXEFS/1"

say "完成。着色器：没有普查数据时第一次运行会按 NVN_SHADER_MISSING 停下并写普查；可用 tools/local_state.sh import 导入另一台机器的普查与着色器构建。"
