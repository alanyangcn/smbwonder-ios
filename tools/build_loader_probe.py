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


def copy_shaders(source, destination):
    """Copy a tools/wonder_shader_build.py output into the app (dm/shaders): manifest.json plus each key's .metal and .refl,
    every file checked against the manifest's sha256. No build = no shaders (draws then stop with NVN_SHADER_MISSING)."""
    source, destination = Path(source), Path(destination)
    if destination.exists(): shutil.rmtree(destination)
    if not (source / 'manifest.json').exists(): return 0
    keys = json.loads((source / 'manifest.json').read_text())['keys']
    destination.mkdir(parents=True)
    for key, entry in keys.items():
        for suffix, field in (('metal', 'msl_sha256'), ('refl', 'refl_sha256')):
            name = f'{key}.{suffix}'
            if digest(source / name) != entry[field]: raise ValueError(f'{name} does not match the shader build manifest')
            shutil.copy2(source / name, destination / name)
    shutil.copy2(source / 'manifest.json', destination / 'manifest.json')
    return len(keys)


def build(work, profile_path, bundle_id, xcodegen=None, stage="sdk", extended_va=True):
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
    shaders = copy_shaders(work / 'build/wonder-shaders', resources / 'shaders')
    print(f'translated shader stages bundled: {shaders}')
    # Shared font type 5 stand-in (button icons, runtime/host_fonts.m): drawn by tools/make_icon_font.py into work/.
    icons = work / 'build/icon-font/wonder-icons.ttf'
    if not icons.exists():
        icons.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(work / 'tools/venv/bin/python3'), str(REPOSITORY / 'tools/make_icon_font.py'), '--out', str(icons)], check=True)
    (resources / 'fonts').mkdir(exist_ok=True)
    shutil.copy2(icons, resources / 'fonts/wonder-icons.ttf')
    embed = output / 'embed.S'
    flags = subprocess.check_output([sys.executable, str(REPOSITORY / 'vendor/d2r-mobile/dm/ios/tools/gen_embed.py'),
                                     str(patch_dir), str(embed), 'main', 'sdk'], text=True).split()
    env = os.environ.copy()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    command = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios18.0', '-isysroot', sdk,
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
                'runtime/nvn_draw_census.c', 'runtime/shader_library.c', 'runtime/metal3_draw.m', 'runtime/tbdr_plan.c', 'runtime/rt_declare.c', 'runtime/profile_image.c', 'runtime/texture_storage.c', 'runtime/settings_service.c', 'runtime/settings_hooks.c', 'runtime/os_hooks.c',
                'runtime/graphics_gate.c', 'runtime/metal3_probe.m', 'runtime/host_input.m', 'runtime/host_audio.m', 'runtime/audio_sink.c', 'runtime/page_guard.c', 'runtime/gpu_gate.c', 'runtime/access_watch.c', 'runtime/log_throttle.c', 'runtime/command_store.c', 'runtime/input_script.c',
                'runtime/metal3_pixels.c', 'runtime/metal3_shader_check.m',
                *RUNTIME_GRAPHICS)]
    command += [str(embed), '-framework', 'UIKit', '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'CoreText', '-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'ImageIO', '-framework', 'GameController', '-framework', 'AudioToolbox', '-framework', 'AVFAudio', *flags, '-o', str(app / 'LoaderProbe')]
    subprocess.run(command, check=True, env=env)
    info = {'CFBundleIdentifier': bundle_id, 'CFBundleName':'Wonder Loader Probe',
            'CFBundleExecutable':'LoaderProbe', 'CFBundlePackageType':'APPL',
            'CFBundleShortVersionString':'0.1', 'CFBundleVersion':'4', 'WonderBootStage':stage,
            'MinimumOSVersion':'18.0', 'LSRequiresIPhoneOS':True, 'UIDeviceFamily':[1, 2],
            'UIFileSharingEnabled':True, 'LSSupportsOpeningDocumentsInPlace':True,
            'UILaunchScreen':{},
            # iOS 27 stops apps without the UIScene lifecycle (LoaderProbe.m ProbeSceneDelegate).
            'UIApplicationSceneManifest':{'UIApplicationSupportsMultipleScenes':False,'UISceneConfigurations':{'UIWindowSceneSessionRoleApplication':[
                {'UISceneConfigurationName':'Default','UISceneDelegateClassName':'ProbeSceneDelegate'}]}}, 'UISupportedInterfaceOrientations':['UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight'],
            'UIRequiresFullScreen':True, 'UIStatusBarHidden':True}
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
        if not extended_va:   # personal teams cannot provision it; the address-space need is then checked on the device
            line = '        com.apple.developer.kernel.extended-virtual-addressing: true\n'
            if line not in spec: raise ValueError('project.yml no longer declares extended-virtual-addressing')
            spec = spec.replace(line, '')
        (project / 'project.yml').write_text(spec)
        subprocess.run([str(xcodegen), 'generate', '--spec', str(project / 'project.yml')], check=True)
    (output / 'build-result.json').write_text(json.dumps({'app':str(app), 'executable_sha256':digest(app / 'LoaderProbe'),
        'scope':{'sdk':'SDK initialization only','constructors':'SDK initialization and module DT_INIT','main':'SDK, module DT_INIT and nnMain entry; subsequent missing services stop the guest'}[stage], 'stage':stage, 'graphics_backend':'metal3', 'signing':'not provisioned; clang may add ad-hoc signature',
        'profile_sha256':digest(profile_path)}, indent=2) + '\n')
    print(app)
    print('Diagnostic only; not a playable game. Device provisioning and execution remain to be verified.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-root', type=Path, default=REPOSITORY / 'work')
    parser.add_argument('--profile', type=Path, default=REPOSITORY / 'verified-inputs.json')
    parser.add_argument('--bundle-id', default='two.local.smbwonder.loaderprobe')
    parser.add_argument('--no-extended-va', action='store_true', help='omit com.apple.developer.kernel.extended-virtual-addressing (personal team)')
    parser.add_argument('--xcodegen', type=Path, help='Also generate an automatic-signing Xcode project in the external work directory')
    parser.add_argument('--stage', choices=['sdk','constructors','main'], default='sdk')
    args = parser.parse_args()
    build(args.work_root.resolve(), args.profile.resolve(), args.bundle_id, args.xcodegen, args.stage, not args.no_extended_va)
