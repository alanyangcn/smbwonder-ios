#!/bin/bash
# 一轮 FlyMe 真机验证：宿主测试 → 构建 → 签名 → 安装 → 启动（默认附加 LLDB）→ 取回日志。
# One FlyMe round: host tests, build, sign, install, launch (LLDB by default), fetch the log.
# Usage: device_round.sh <round-name> [--fetch-only]   (results in work/analysis/<round-name>/)
set -euo pipefail
ROUND=${1:?round name, e.g. nvn-round1}
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WONDER_WORK_ROOT:-$REPO/work}
OUT=$WORK/analysis/$ROUND
DEVICE=${WONDER_DEVICE:-0930f8c1ac0291e34476f925f1369499819a5a8f}
TEAM=${WONDER_TEAM:-5H8VE3NN7D}   # paid team (kernel memory entitlements); MFXJ9AKFM5 was a free personal team
BUNDLE=two.local.smbwonder.loaderprobe   # new App ID under the paid team (old: local.smbwonder.loaderprobe)
DEPLOY=$WORK/tools/ios-deploy/build/Release/ios-deploy
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode-26.6.0.app/Contents/Developer}
mkdir -p "$OUT"
step(){ echo "== $1"; echo "$(date '+%F %T') $1" >> "$OUT/steps.log"; }
DEBUG_PID=""
cleanup(){ if [ -n "${DEBUG_PID:-}" ]; then kill "$DEBUG_PID" 2>/dev/null || true; fi; }
fetch(){
  step "download"
  for f in loader-probe.log loader-probe.previous.log metal2-probe.json; do
    "$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --download="/Documents/$f" --to "$OUT" >> "$OUT/download.log" 2>&1 || true
  done
  # Crash reports for this app (the guest stops cleanly; a crash here is a host defect).
  mkdir -p "$OUT/crashes"
  if command -v idevicecrashreport >/dev/null; then idevicecrashreport -u "$DEVICE" -k -f WonderLoaderProbe "$OUT/crashes" >> "$OUT/download.log" 2>&1 || true; fi
  find "$HOME/Library/Logs/CrashReporter/MobileDevice" -name 'WonderLoaderProbe*' -newer "$OUT/steps.log" -exec cp {} "$OUT/crashes/" \; 2>/dev/null || true
  cleanup; DEBUG_PID=""
  grep -nE "stop reason|EXC_|signal SIG|frame #" "$OUT"/launch-*.log 2>/dev/null | head -20 || true
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
python3 tools/build_loader_probe.py --stage main --graphics-backend metal2 \
  --xcodegen "$WORK/tools/xcodegen/xcodegen/bin/xcodegen" > "$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; step "FAILED build"; exit 1; }
step "xcodebuild"
xcodebuild -project "$WORK/build/loader-probe/project/WonderLoaderProbe.xcodeproj" -scheme WonderLoaderProbe \
  -configuration Debug -destination "generic/platform=iOS" -derivedDataPath "$WORK/build/loader-probe/DerivedData" \
  -allowProvisioningUpdates DEVELOPMENT_TEAM=$TEAM build > "$OUT/xcode.log" 2>&1 || { grep -E "error:|BUILD FAILED" "$OUT/xcode.log" | head -40; step "FAILED xcodebuild"; exit 1; }
APP=$WORK/build/loader-probe/DerivedData/Build/Products/Debug-iphoneos/WonderLoaderProbe.app
codesign --verify --deep --strict "$APP" > "$OUT/codesign.log" 2>&1
shasum -a 256 "$APP/WonderLoaderProbe" > "$OUT/app.sha256"
grep -a -o 'com.apple.developer.kernel[a-z.-]*' "$APP/WonderLoaderProbe" | sort -u > "$OUT/entitlements.txt" || true
step "signed entitlements: $(tr '\n' ' ' < "$OUT/entitlements.txt")"
step "install"
if ! "$DEPLOY" --id "$DEVICE" --bundle "$APP" > "$OUT/install.log" 2>&1; then
  # Replacing a differently signed app must not silently delete Documents (including future saves).
  tail -20 "$OUT/install.log"
  step "FAILED install; installed app and Documents preserved"
  exit 1
fi
# The RomFS archive (3.7 GB) lives in the app's Documents. A marker holding the archive's sha256 is uploaded
# only after a complete upload; the archive is re-sent only when the marker is missing or different.
# (ios-deploy --list prints through writeToFile:/dev/stdout, which is unreliable through pipes/redirection.)
ROMFS=$WORK/build/wonder-romfs.wrom
ROMFS_SHA=$(python3 -c "import json;print(json.load(open('$ROMFS.json'))['sha256'])")
mkdir -p "$OUT/romfs-marker"
"$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --download=/Documents/wonder-romfs.ok --to "$OUT/romfs-marker" > "$OUT/romfs-marker.log" 2>&1 || true
if [ "${WONDER_SKIP_ROMFS:-0}" = 1 ]; then
  step "RomFS upload skipped (WONDER_SKIP_ROMFS=1)"
elif [ "$(cat "$OUT/romfs-marker/Documents/wonder-romfs.ok" 2>/dev/null)" = "$ROMFS_SHA" ]; then
  step "RomFS archive already on device (marker sha256 matches)"
else
  step "uploading RomFS archive (3.7 GB, ~2-3 min)"
  "$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --upload "$ROMFS" --to /Documents/wonder-romfs.wrom > "$OUT/romfs-upload.log" 2>&1 || { tail -5 "$OUT/romfs-upload.log"; step "FAILED RomFS upload"; exit 1; }
  printf '%s' "$ROMFS_SHA" > "$OUT/wonder-romfs.ok"
  "$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --upload "$OUT/wonder-romfs.ok" --to /Documents/wonder-romfs.ok >> "$OUT/romfs-upload.log" 2>&1 || { step "FAILED RomFS marker upload"; exit 1; }
  step "RomFS archive uploaded and marked"
fi
# Baseline: the previous run's id, so a stale log on the device is never mistaken for this run.
mkdir -p "$OUT/baseline"
"$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --download=/Documents/loader-probe.log --to "$OUT/baseline" >> "$OUT/download.log" 2>&1 || true
OLD_RUN=$(head -1 "$OUT/baseline/Documents/loader-probe.log" 2>/dev/null | grep -o 'PROBE_RUN id=[0-9A-F-]*' || true)
step "previous run ${OLD_RUN:-none}"
echo "!! 请保持 FlyMe 解锁、屏幕常亮：锁屏时 app 在后台启动，CAMetalLayer/GPU 提交不可信"
# Launch mode. debug (default): ios-deploy keeps LLDB attached in the background for the whole round, so
# the app stays in the foreground (a --justlaunch detach sends it to the Home screen ~1.6 s after it
# becomes active, HOST_LIFECYCLE in nvn-round3b) and any host crash is caught with a backtrace.
# cold: WONDER_LAUNCH=cold uses --justlaunch (no debugger; racy on iOS 16, retried).
NEW_RUN=""; DEBUG_PID=""
trap cleanup EXIT
for attempt in 1 2 3; do
  if [ "${WONDER_LAUNCH:-debug}" = cold ]; then
    step "launch attempt $attempt: cold (--justlaunch, no debugger)"
    "$DEPLOY" --id "$DEVICE" --bundle "$APP" --justlaunch --noinstall > "$OUT/launch-$attempt.log" 2>&1 && rc=0 || rc=$?
    step "ios-deploy justlaunch rc=$rc (LLDB state at detach, not app exit)"
  else
    step "launch attempt $attempt: LLDB attached in background (app stays foreground)"
    "$DEPLOY" --id "$DEVICE" --bundle "$APP" --debug --noinstall --noninteractive > "$OUT/launch-$attempt.log" 2>&1 &
    DEBUG_PID=$!
  fi
  for i in $(seq 1 6); do
    sleep 5
    "$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --download=/Documents/loader-probe.log --to "$OUT" >> "$OUT/download.log" 2>&1 || true
    NEW_RUN=$(head -1 "$OUT/Documents/loader-probe.log" 2>/dev/null | grep -o 'PROBE_RUN id=[0-9A-F-]*' || true)
    [ -n "$NEW_RUN" ] && [ "$NEW_RUN" != "$OLD_RUN" ] && break 2
  done
  cleanup; DEBUG_PID=""
  step "no new run after 30s; device locked or launch lost — retrying"
done
if [ -z "$NEW_RUN" ] || [ "$NEW_RUN" = "$OLD_RUN" ]; then
  step "FAILED: app never started a new run (unlock FlyMe, keep screen on)"; exit 1
fi
step "new run $NEW_RUN; waiting for stop marker (max 120s)"
for i in $(seq 1 24); do
  if grep -qE 'NVN_GAP|BOOT_BLOCKED|FATAL|NNMAIN_RETURNED|GAME_RENDERER_PENDING|GUEST_FAULT|SDK_HOST_ABORT_OBSERVER' "$OUT/Documents/loader-probe.log"; then step "stop marker after ~$((i*5))s"; break; fi
  sleep 5
  "$DEPLOY" --id "$DEVICE" --bundle_id "$BUNDLE" --download=/Documents/loader-probe.log --to "$OUT" >> "$OUT/download.log" 2>&1 || true
done
fetch
