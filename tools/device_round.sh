#!/bin/bash
# 一轮真机验证（iPhone 18 Pro，iOS 27）：宿主测试 → 构建 → 签名 → 安装 → 启动（devicectl --console 抓 stdout/stderr）→ 取回日志。
# One device round: host tests, build, sign, install, launch (devicectl --console captures the app's stdout/stderr), fetch logs.
# Usage: device_round.sh <round-name> [--fetch-only]   (results in work/analysis/<round-name>/)
# WONDER_ENVS="KEY=VALUE ..." passes diagnostics to the app (e.g. WONDER_SUBMIT_SYNC=1).
# WONDER_INTERACTIVE=1 leaves the app running after it starts (for play testing): the live log keeps streaming into
#   console-<attempt>.log; fetch later with --fetch-only (which stops the app first).
set -euo pipefail
ROUND=${1:?round name, e.g. nvn-round1}
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WONDER_WORK_ROOT:-$REPO/work}
OUT=$WORK/analysis/$ROUND
DEVICE=${WONDER_DEVICE:-00008160-001E68D40421400A}   # iPhone 18 Pro (iPhone19,2)
# Signing: paid team 5H8VE3NN7D grants both kernel memory entitlements; the personal team MFXJ9AKFM5 with
# WONDER_BUNDLE=local.smbwonder.loaderprobe grants increased-memory-limit only (no extended-virtual-addressing).
TEAM=${WONDER_TEAM:-5H8VE3NN7D}
BUNDLE=${WONDER_BUNDLE:-two.local.smbwonder.loaderprobe}
# WONDER_EXTENDED_VA=0 builds without extended-virtual-addressing (required for the personal team).
EXTRA_BUILD=""; [ "${WONDER_EXTENDED_VA:-1}" = 0 ] && EXTRA_BUILD=--no-extended-va
export DEVELOPER_DIR=${DEVELOPER_DIR:-$(xcode-select -p)}
mkdir -p "$OUT"
step(){ echo "== $1"; echo "$(date '+%F %T') $1" >> "$OUT/steps.log"; }
dctl(){ xcrun devicectl "$@" --device "$DEVICE"; }
limit(){ local s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }   # a stalled devicectl transfer must not hang the round
# pull <path under the app container> -> $OUT/<same path>; non-zero when the file is missing.
pull(){ mkdir -p "$OUT/$(dirname "$1")"; limit 180 xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE" --source "$1" --destination "$OUT/$1" >> "$OUT/download.log" 2>&1; }
push(){ dctl device copy to --domain-type appDataContainer --domain-identifier "$BUNDLE" --source "$1" --destination "$2" >> "$OUT/upload.log" 2>&1; }
CONSOLE_PID=""
cleanup(){ if [ -n "${CONSOLE_PID:-}" ]; then kill "$CONSOLE_PID" 2>/dev/null || true; fi; }
stop_app(){
  # The growing log cannot be copied while the game writes it: stop the app (its console session) first.
  cleanup; CONSOLE_PID=""
  pkill -f "devicectl device process launch --device $DEVICE" 2>/dev/null || true
  local pid; pid=$(limit 30 xcrun devicectl device info processes --device "$DEVICE" 2>/dev/null | awk '/WonderLoaderProbe.app\/WonderLoaderProbe/{print $1; exit}')
  [ -n "$pid" ] && limit 30 xcrun devicectl device process terminate --device "$DEVICE" --pid "$pid" > /dev/null 2>&1 || true
}
fetch(){
  step "download"
  stop_app
  for f in loader-probe.log loader-probe.previous.log metal3-probe.json; do pull "Documents/$f" || true; done
  if [ ! -s "$OUT/Documents/loader-probe.log" ]; then
    latest=$(ls -t "$OUT"/console-*.log 2>/dev/null | head -1)
    [ -n "$latest" ] && { mkdir -p "$OUT/Documents"; cp "$latest" "$OUT/Documents/loader-probe.log"; step "log taken from the live console ($(basename "$latest"))"; }
  fi
  if grep -q 'NVN_SHADER_CAPTURE.*complete=1' "$OUT/Documents/loader-probe.log" 2>/dev/null; then
    for f in manifest.json stage-{0..5}.{code,control}.bin; do
      pull "Documents/ShaderCapture/$f" || true
    done
  fi
  if grep -q 'NVN_DRAW_CENSUS wrote' "$OUT/Documents/loader-probe.log" 2>/dev/null; then
    # One directory per program (<vs>-<fs>), listed in DrawCensus/index.txt.
    pull Documents/DrawCensus/index.txt || true
    while read -r dir; do
      [ -n "$dir" ] || continue
      for f in draw.json stage-{0..1}.{code,control}.bin; do
        pull "Documents/DrawCensus/$dir/$f" || true
      done
    done < "$OUT/Documents/DrawCensus/index.txt"
  fi
  if grep -q 'HOST_FIRST_DRAW_CAPTURE' "$OUT/Documents/loader-probe.log" 2>/dev/null; then
    pull Documents/FirstDraw/target-0.png || true
  fi
  if grep -q 'Documents/Present/present-' "$OUT/Documents/loader-probe.log" 2>/dev/null; then
    for f in $(grep -o 'Present/present-[0-9]*\.png' "$OUT/Documents/loader-probe.log" | sort -u); do
      pull "Documents/$f" || true
    done
  fi
  # Crash reports for this app written during the round (the guest stops cleanly; a crash here is a host defect).
  mkdir -p "$OUT/crashes"
  if dctl device info files --domain-type systemCrashLogs --json-output "$OUT/crashlogs.json" > /dev/null 2>&1; then
    python3 - "$OUT/crashlogs.json" "$(head -1 "$OUT/steps.log" | cut -c1-19)" <<'PY' | while read -r f; do
import json, sys, datetime
start = datetime.datetime.strptime(sys.argv[2], '%Y-%m-%d %H:%M:%S').astimezone(datetime.timezone.utc)
for f in json.load(open(sys.argv[1]))['result']['files']:
    name = f.get('relativePath', '')
    when = f.get('lastModDate') or f.get('creationDate') or ''
    try: t = datetime.datetime.fromisoformat(when.replace('Z', '+00:00'))
    except ValueError: continue
    if 'WonderLoaderProbe' in name and t >= start: print(name)
PY
      dctl device copy from --domain-type systemCrashLogs --source "$f" --destination "$OUT/crashes/$(basename "$f")" >> "$OUT/download.log" 2>&1 || true
    done
  fi
  grep -nE "EXC_|signal SIG|Terminating app|crashed" "$OUT"/console-*.log 2>/dev/null | head -20 || true
  step "done"
  echo "---- run: $(head -1 "$OUT/Documents/loader-probe.log" 2>/dev/null) ----"
  grep -h HOST_LIFECYCLE "$OUT/Documents/loader-probe.log" 2>/dev/null || true
  echo "---- log tail ----"
  tail -40 "$OUT/Documents/loader-probe.log" 2>/dev/null || echo "no log downloaded"
  ls "$OUT/crashes"
}
if [ "${2:-}" = "--fetch-only" ]; then fetch; exit 0; fi
cd "$REPO"
step "host tests"
python3 -m unittest discover -s tests -q > "$OUT/tests.log" 2>&1 || { tail -40 "$OUT/tests.log"; step "FAILED tests"; exit 1; }
tail -3 "$OUT/tests.log"
step "build"
python3 tools/build_loader_probe.py --stage main --bundle-id "$BUNDLE" $EXTRA_BUILD \
  --xcodegen "$WORK/tools/xcodegen/xcodegen/bin/xcodegen" > "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; step "FAILED build"; exit 1; }
step "xcodebuild"
xcodebuild -project "$WORK/build/loader-probe/project/WonderLoaderProbe.xcodeproj" -scheme WonderLoaderProbe \
  -configuration Debug -destination "generic/platform=iOS" -derivedDataPath "$WORK/build/loader-probe/DerivedData" \
  -allowProvisioningUpdates DEVELOPMENT_TEAM=$TEAM PRODUCT_BUNDLE_IDENTIFIER=$BUNDLE build > "$OUT/xcode.log" 2>&1 || { grep -E "error:|BUILD FAILED" "$OUT/xcode.log" | head -40; step "FAILED xcodebuild"; exit 1; }
APP=$WORK/build/loader-probe/DerivedData/Build/Products/Debug-iphoneos/WonderLoaderProbe.app
codesign --verify --deep --strict "$APP" > "$OUT/codesign.log" 2>&1
shasum -a 256 "$APP/WonderLoaderProbe" > "$OUT/app.sha256"
grep -a -o 'com.apple.developer.kernel[a-z.-]*' "$APP/WonderLoaderProbe" | sort -u > "$OUT/entitlements.txt" || true
step "signed entitlements: $(tr '\n' ' ' < "$OUT/entitlements.txt")"
step "install"
if ! dctl device install app "$APP" > "$OUT/install.log" 2>&1; then
  # Replacing a differently signed app must not silently delete Documents (including future saves).
  tail -20 "$OUT/install.log"
  step "FAILED install; installed app and Documents preserved"
  exit 1
fi
# The RomFS archive (3.7 GB) lives in the app's Documents. A marker holding the archive's sha256 is uploaded
# only after a complete upload; the archive is re-sent only when the marker is missing or different.
ROMFS=$WORK/build/wonder-romfs.wrom
ROMFS_SHA=$(python3 -c "import json;print(json.load(open('$ROMFS.json'))['sha256'])")
rm -f "$OUT/Documents/wonder-romfs.ok"; pull Documents/wonder-romfs.ok || true
if [ "${WONDER_SKIP_ROMFS:-0}" = 1 ]; then
  step "RomFS upload skipped (WONDER_SKIP_ROMFS=1)"
elif [ "$(cat "$OUT/Documents/wonder-romfs.ok" 2>/dev/null)" = "$ROMFS_SHA" ]; then
  step "RomFS archive already on device (marker sha256 matches)"
else
  step "uploading RomFS archive (3.7 GB)"
  push "$ROMFS" Documents/wonder-romfs.wrom || { tail -5 "$OUT/upload.log"; step "FAILED RomFS upload"; exit 1; }
  printf '%s' "$ROMFS_SHA" > "$OUT/wonder-romfs.ok"
  push "$OUT/wonder-romfs.ok" Documents/wonder-romfs.ok || { step "FAILED RomFS marker upload"; exit 1; }
  step "RomFS archive uploaded and marked"
fi
echo "!! 请保持 iPhone 解锁、屏幕常亮：锁屏时 app 在后台启动，CAMetalLayer/GPU 提交不可信"
# devicectl --console keeps the app attached (foreground) for the whole round and records its stdout/stderr (the host's
# fprintf(stderr) diagnostics) in console-<attempt>.log. Environment for the app: DEVICECTL_CHILD_<KEY>.
NEW_RUN=""; CONSOLE_PID=""
trap cleanup EXIT
for attempt in 1 2 3; do
  step "launch attempt $attempt: devicectl --console"
  CONSOLE=$OUT/console-$attempt.log
  ( for kv in ${WONDER_ENVS:-}; do export "DEVICECTL_CHILD_$kv"; done
    exec xcrun devicectl device process launch --device "$DEVICE" --terminate-existing --console "$BUNDLE" ) > "$CONSOLE" 2>&1 &
  CONSOLE_PID=$!
  for i in $(seq 1 30); do
    sleep 1
    NEW_RUN=$(grep -o -m1 'PROBE_RUN id=[0-9A-F-]*' "$CONSOLE" 2>/dev/null || true)
    [ -n "$NEW_RUN" ] && break 2
    kill -0 "$CONSOLE_PID" 2>/dev/null || break
  done
  cleanup; CONSOLE_PID=""
  step "no new run in the console after 30s; device locked or launch lost — retrying"
done
if [ -z "$NEW_RUN" ]; then
  step "FAILED: app never started a new run (unlock the iPhone, keep screen on)"; exit 1
fi
if [ "${WONDER_INTERACTIVE:-0}" = 1 ]; then
  step "new run $NEW_RUN; interactive: app left running, live log in $(basename "$CONSOLE"); fetch with --fetch-only"
  CONSOLE_PID=""; exit 0
fi
step "new run $NEW_RUN; waiting for stop marker (max ${WONDER_WAIT:-120}s)"
for i in $(seq 1 $(( ${WONDER_WAIT:-120} / 5 ))); do
  if grep -qE 'NVN_GAP|BOOT_BLOCKED|FATAL|NNMAIN_RETURNED|GUEST_FAULT|SDK_HOST_ABORT_OBSERVER' "$CONSOLE"; then step "stop marker after ~$((i*5))s"; break; fi
  if ! kill -0 "$CONSOLE_PID" 2>/dev/null; then step "app exited (console closed) after ~$((i*5))s"; break; fi
  sleep 5
done
fetch
