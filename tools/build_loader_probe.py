#!/usr/bin/env python3
"""构建输入受哈希约束的无签名 SDK 诊断 App；不生成可玩游戏包。
Build a hash-bound, unsigned SDK-diagnostic App, not a playable game package.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import struct

REPOSITORY = Path(__file__).resolve().parents[1]


# Event/VI/host-graphics layer and strict NVN bootstrap; kept in sync with ios-probe/project.yml.
RUNTIME_GRAPHICS = tuple('runtime/' + name for name in (
    'kernel_threads.c', 'account_state.c', 'account_hooks.c', 'nfp_hooks.c', 'audio_platform.c', 'time_hooks.c', 'hid_state.c', 'hid_hooks.c', 'crash_report.c', 'vi_state.c', 'vi_hooks.c', 'host_graphics.m',
    'shared_font.c', 'host_fonts.m', 'font_hooks.c', 'nvn_device.c', 'nvn_swapchain.c', 'nvn_buffer.c', 'nvn_commands.c', 'nvn_queue.c', 'nvn_texture.c', 'nvn_state.c', 'nvn_bootstrap.c', 'nvn_unimpl.S')) + (
    'vendor/d2r-mobile/dm/gfx/nvn/nvn_tex.c',      # shared TX1 block-linear layout calculator (deko3d-derived)
    'vendor/d2r-mobile/dm/gfx/tex/nvn_fmt_mtl.c',  # NVN format -> Metal pixel format (pure C)
    'vendor/d2r-mobile/dm/gfx/tex/nvn_bcdec.c',    # BC decode for GPUs without BC (A11)
    'vendor/d2r-mobile/dm/gfx/tex/nvn_deswizzle.c')  # TX1 block-linear <-> linear


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_inputs(work, profile):
    if 'npdm' in profile:
        record=profile['npdm'];blob=(work/record['path']).read_bytes()
        if hashlib.sha256(blob).hexdigest()!=record['sha256'] or len(blob)<0x20 or blob[:4]!=b'META':
            raise ValueError('NPDM identity mismatch')
        if struct.unpack_from('<I',blob,0x1c)[0]!=record['main_thread_stack_size']:
            raise ValueError('NPDM main thread stack mismatch')
        # sdk_kernel.c reports this as GetInfo(16); it enables the SDK's VammManager.
        if 'system_resource_size' in record and struct.unpack_from('<I',blob,0x14)[0]!=record['system_resource_size']:
            raise ValueError('NPDM system resource size mismatch')
    for name, module in profile['modules'].items():
        for key in ('nso', 'patched'):
            record = module[key]
            if digest(work / record['path']) != record['sha256']:
                raise ValueError(f'{name}: {key} hash differs from the verified input')
        for record in module.get('auxiliary', []):
            if digest(work / record['path']) != record['sha256']:
                raise ValueError(f'{name}: layout/gate metadata differs from verified input')
        original = (work / module['nso']['path']).read_bytes()
        if original[:4] != b'NSO0' or original[0x40:0x60].hex() != module['module_id']:
            raise ValueError(f'{name}: module identity mismatch')
    for check in profile['checks']:
        path = work / check['path']
        if digest(path) != check['sha256'] or check['marker'] not in path.read_text():
            raise ValueError('Verification evidence changed or failed: ' + check['path'])


def build(work, profile_path, bundle_id, xcodegen=None, stage="sdk", graphics_backend="metal2"):
    profile = json.loads(profile_path.read_text())
    validate_inputs(work, profile)
    if profile.get('npdm', {}).get('main_thread_stack_size') != 0x100000:
        raise ValueError('This host requires the verified 1 MiB NPDM stack')
    if profile.get('npdm', {}).get('system_resource_size') != 0x1000000:
        raise ValueError('sdk_kernel.c reports a 16 MiB system resource (WONDER_SYSTEM_RESOURCE_SIZE); profile must match')
    for name, module in profile['modules'].items():
        expected = {profile['patched_directory'] + '/' + name + '.' + suffix for suffix in ('fixups.tsv', 'gates.tsv')}
        if {record['path'] for record in module.get('auxiliary', [])} != expected:
            raise ValueError('Verified profile must bind both layout and gates: ' + name)
    if set(profile['modules']) != {'main', 'sdk'}:
        raise ValueError('This probe supports exactly the verified main/sdk pair')
    output = work / 'build/loader-probe'
    output.mkdir(parents=True, exist_ok=True)
    app = output / 'WonderLoaderProbe.app'
    app.mkdir(exist_ok=True)
    resources = app / 'dm'
    resources.mkdir(exist_ok=True)
    patch_dir = work / profile['patched_directory']
    # 所有产物留在外部工作区；不复制密钥、存档或 RomFS。
    # Keep all artifacts in the external work area; copy no keys, saves, or RomFS.
    for name in ('main', 'sdk'):
        for suffix in ('fixups.tsv', 'gates.tsv'):
            shutil.copy2(patch_dir / f'{name}.{suffix}', resources / f'{name}.{suffix}')
    embed = output / 'embed.S'
    flags = subprocess.check_output([sys.executable, str(REPOSITORY / 'vendor/d2r-mobile/dm/ios/tools/gen_embed.py'),
                                     str(patch_dir), str(embed), 'main', 'sdk'], text=True).split()
    env = os.environ.copy()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    command = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios16.0', '-isysroot', sdk,
               '-DSMBW_STRICT_IMPORTS=1', '-O1', '-g', '-fobjc-arc', '-Wall', '-Wno-unused-variable', '-I' + str(REPOSITORY / 'vendor/d2r-mobile/dm/host'), '-I' + str(REPOSITORY / 'vendor/d2r-mobile/dm/gfx/nvn'), '-I' + str(REPOSITORY / 'vendor/d2r-mobile/dm/gfx/tex')]
    command += [str(REPOSITORY / p) for p in ('ios-probe/LoaderProbe.m',
                'vendor/d2r-mobile/dm/host/loader.c', 'vendor/d2r-mobile/dm/host/hle.c', 'vendor/d2r-mobile/dm/host/entry.S',
                'runtime/sdk_kernel.c', 'runtime/sdk_startup.c',
                'runtime/sdk_services.c', 'runtime/sdk_service_hooks.c',
                'runtime/game_thread.c', 'runtime/module_inits.c',
                'runtime/applet_state.c', 'runtime/applet_hooks.c',
                'runtime/romfs.c', 'runtime/romfs_hooks.c',
                'runtime/savedata.c', 'runtime/fs_save_hooks.c', 'runtime/online_hooks.c',
                'runtime/ngc_service.c', 'runtime/ngc_hooks.c',
                'runtime/nvn_draw_census.c', 'runtime/settings_service.c', 'runtime/settings_hooks.c', 'runtime/os_hooks.c',
                'runtime/graphics_gate.c', 'runtime/metal2_probe.m',
                'runtime/metal2_pixels.c', 'runtime/metal2_shader_check.m',
                *RUNTIME_GRAPHICS)]
    command += [str(embed), '-framework', 'UIKit', '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'CoreText', '-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'ImageIO', *flags, '-o', str(app / 'LoaderProbe')]
    subprocess.run(command, check=True, env=env)
    info = {'CFBundleIdentifier': bundle_id, 'CFBundleName':'Wonder Loader Probe',
            'CFBundleExecutable':'LoaderProbe', 'CFBundlePackageType':'APPL',
            'CFBundleShortVersionString':'0.1', 'CFBundleVersion':'4', 'WonderBootStage':stage, 'WonderGraphicsBackend':graphics_backend,
            'MinimumOSVersion':'16.0', 'LSRequiresIPhoneOS':True, 'UIDeviceFamily':[1, 2],
            'UIFileSharingEnabled':True, 'LSSupportsOpeningDocumentsInPlace':True,
            'UILaunchScreen':{}, 'UISupportedInterfaceOrientations':['UIInterfaceOrientationPortrait',
            'UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight']}
    with (app / 'Info.plist').open('wb') as stream:
        plistlib.dump(info, stream)
    if xcodegen:
        project = output / 'project'
        project.mkdir(exist_ok=True)
        xcode_info = dict(info, CFBundleExecutable='$(EXECUTABLE_NAME)')
        with (project / 'ProbeInfo.plist').open('wb') as stream:
            plistlib.dump(xcode_info, stream)
        spec = (REPOSITORY / 'ios-probe/project.yml').read_text()
        spec = spec.replace('${SMBW_REFERENCE}', str(REPOSITORY)).replace('${SMBW_WORK}', str(work))
        (project / 'project.yml').write_text(spec)
        subprocess.run([str(xcodegen), 'generate', '--spec', str(project / 'project.yml')], check=True)
    (output / 'build-result.json').write_text(json.dumps({'app':str(app), 'executable_sha256':digest(app / 'LoaderProbe'),
        'scope':{'sdk':'SDK initialization only','constructors':'SDK initialization and module DT_INIT','main':'SDK, module DT_INIT and nnMain entry; subsequent missing services stop the guest'}[stage], 'stage':stage, 'graphics_backend':graphics_backend, 'signing':'not provisioned; clang may add ad-hoc signature',
        'profile_sha256':digest(profile_path)}, indent=2) + '\n')
    print(app)
    print('Diagnostic only; not a playable game. Device provisioning and execution remain to be verified.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-root', type=Path, default=REPOSITORY / 'work')
    parser.add_argument('--profile', type=Path, default=REPOSITORY / 'verified-inputs.json')
    parser.add_argument('--bundle-id', default='two.local.smbwonder.loaderprobe')
    parser.add_argument('--xcodegen', type=Path, help='Also generate an automatic-signing Xcode project in the external work directory')
    parser.add_argument('--stage', choices=['sdk','constructors','main'], default='sdk')
    parser.add_argument('--graphics-backend', choices=['metal2','metal3'], default='metal2')
    args = parser.parse_args()
    build(args.work_root.resolve(), args.profile.resolve(), args.bundle_id, args.xcodegen, args.stage, args.graphics_backend)
