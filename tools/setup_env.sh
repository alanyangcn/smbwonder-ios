#!/bin/bash
# 在新 Mac 上把 work/tools 里的工具链装好（可重复执行，已装的跳过）。不需要游戏文件；游戏导入见 tools/import_game.sh。
# Installs the toolchain under work/tools on a fresh Mac (idempotent: installed pieces are skipped). Needs no game
# files; game import is tools/import_game.sh.
#
# 前置条件 / prerequisites (manual):
#   - Xcode（含 iOS SDK；默认路径 /Applications/Xcode-26.6.0.app，可用 DEVELOPER_DIR 指定）、命令行工具（clang、git、make、python3）
#   - cmake 不需要；网络可访问 github.com、dot.net、pypi.org
#
# 安装内容与版本（与已校验产物一致）/ what gets installed (versions of the verified artifacts):
#   venv            python3 -m venv + requirements.txt
#   hactool         SciresM/hactool @ 1d64a83450e025622f3468c28fc4164dad2c5ef6（make）
#   nstool          jakcron/nstool v1.9.2 macOS release（x86_64；Apple 芯片需 Rosetta）
#   xcodegen        yonaskolb/XcodeGen 2.46.0 release
#   ios-deploy      ios-control/ios-deploy @ 83f7d9ef97d03af42777965e80079f05332cc580 + tools/patches/ios-deploy-streaming-upload.patch
#   dotnet          .NET SDK 10.0.402（dotnet-install.sh，装在 work/tools/dotnet，不动系统）
#   wonder_xlate    tools/wonder_xlate（Ryujinx 着色器前端，源码在 vendor/）dotnet publish
#   spirv-cross     KhronosGroup/SPIRV-Cross @ 300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1（make）
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WONDER_WORK_ROOT:-$REPO/work}
TOOLS=${WONDER_TOOLS:-$WORK/tools}
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode-26.6.0.app/Contents/Developer}
mkdir -p "$TOOLS"
say() { echo "== $*"; }
die() { echo "✗ $*" >&2; exit 1; }
have() { [ -e "$1" ] && echo "   已有 $1"; }

[ -d "$DEVELOPER_DIR" ] || die "没有 Xcode：${DEVELOPER_DIR}（装好后用 DEVELOPER_DIR=<Xcode.app>/Contents/Developer 指定）"
xcrun --sdk iphoneos --show-sdk-path > /dev/null || die "Xcode 里没有 iOS SDK"
for t in git make clang python3 curl unzip; do command -v $t > /dev/null || die "缺少命令 ${t}（装 Xcode 命令行工具：xcode-select --install）"; done

clone() {   # clone <url> <dir> <commit>
  if [ ! -d "$2/.git" ]; then git clone --quiet "$1" "$2"; fi
  git -C "$2" fetch --quiet origin "$3" 2>/dev/null || true
  git -C "$2" -c advice.detachedHead=false checkout --quiet "$3"
}

say "Python venv"
if ! have "$TOOLS/venv/bin/python3"; then python3 -m venv "$TOOLS/venv"; fi
"$TOOLS/venv/bin/python3" -m pip install --quiet --upgrade pip
"$TOOLS/venv/bin/python3" -m pip install --quiet -r "$REPO/requirements.txt"

say "hactool"
if ! have "$TOOLS/hactool/hactool"; then
  clone https://github.com/SciresM/hactool.git "$TOOLS/hactool" 1d64a83450e025622f3468c28fc4164dad2c5ef6
  cp "$TOOLS/hactool/config.mk.template" "$TOOLS/hactool/config.mk"
  make -C "$TOOLS/hactool" > "$TOOLS/hactool-build.log" 2>&1 || { tail -20 "$TOOLS/hactool-build.log"; die "hactool 编译失败"; }
fi

say "nstool"
if ! have "$TOOLS/nstool/nstool"; then
  mkdir -p "$TOOLS/nstool"
  curl -fsSL -o "$TOOLS/nstool/nstool-v1.9.2-macos_x86_64.zip" https://github.com/jakcron/nstool/releases/download/v1.9.2/nstool-v1.9.2-macos_x86_64.zip
  unzip -q -o "$TOOLS/nstool/nstool-v1.9.2-macos_x86_64.zip" -d "$TOOLS/nstool"
  chmod +x "$TOOLS/nstool/nstool"
fi

say "XcodeGen"
if ! have "$TOOLS/xcodegen/xcodegen/bin/xcodegen"; then
  mkdir -p "$TOOLS/xcodegen"
  curl -fsSL -o "$TOOLS/xcodegen/xcodegen.zip" https://github.com/yonaskolb/XcodeGen/releases/download/2.46.0/xcodegen.zip
  unzip -q -o "$TOOLS/xcodegen/xcodegen.zip" -d "$TOOLS/xcodegen"
fi

say "ios-deploy（带流式上传补丁：3.7 GB RomFS 归档不能整个读进内存）"
if ! have "$TOOLS/ios-deploy/build/Release/ios-deploy"; then
  clone https://github.com/ios-control/ios-deploy.git "$TOOLS/ios-deploy" 83f7d9ef97d03af42777965e80079f05332cc580
  git -C "$TOOLS/ios-deploy" apply --check "$REPO/tools/patches/ios-deploy-streaming-upload.patch" 2>/dev/null \
    && git -C "$TOOLS/ios-deploy" apply "$REPO/tools/patches/ios-deploy-streaming-upload.patch"
  (cd "$TOOLS/ios-deploy" && xcodebuild -quiet -target ios-deploy -configuration Release SYMROOT=build > "$TOOLS/ios-deploy-build.log" 2>&1) \
    || { tail -20 "$TOOLS/ios-deploy-build.log"; die "ios-deploy 编译失败"; }
fi

say ".NET SDK 10.0.402"
if ! have "$TOOLS/dotnet/dotnet"; then
  curl -fsSL -o "$TOOLS/dotnet-install.sh" https://dot.net/v1/dotnet-install.sh
  bash "$TOOLS/dotnet-install.sh" --version 10.0.402 --install-dir "$TOOLS/dotnet" --no-path > "$TOOLS/dotnet-install.log" 2>&1 \
    || { tail -20 "$TOOLS/dotnet-install.log"; die ".NET 安装失败"; }
fi

say "wonder_xlate（着色器翻译前端）"
if ! have "$TOOLS/wonder_xlate/wonder_xlate"; then
  DOTNET_ROOT="$TOOLS/dotnet" DOTNET_CLI_HOME="$TOOLS/dotnet-home" NUGET_PACKAGES="$TOOLS/nuget" DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    "$TOOLS/dotnet/dotnet" publish "$REPO/tools/wonder_xlate/wonder_xlate.csproj" -c Release -o "$TOOLS/wonder_xlate" > "$TOOLS/wonder_xlate-build.log" 2>&1 \
    || { tail -20 "$TOOLS/wonder_xlate-build.log"; die "wonder_xlate 编译失败"; }
fi

say "SPIRV-Cross"
if ! have "$TOOLS/spirv-cross/spirv-cross"; then
  clone https://github.com/KhronosGroup/SPIRV-Cross.git "$TOOLS/spirv-cross" 300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1
  make -C "$TOOLS/spirv-cross" -j"$(sysctl -n hw.ncpu)" > "$TOOLS/spirv-cross-build.log" 2>&1 || { tail -20 "$TOOLS/spirv-cross-build.log"; die "SPIRV-Cross 编译失败"; }
fi

say "完成。下一步：tools/import_game.sh <本体 .nsz/.nsp> <1.2.0 更新 .nsz/.nsp> <prod.keys>"
