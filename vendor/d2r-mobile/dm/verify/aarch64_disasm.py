# 独立 LLVM 反汇编：有 objcopy 时使用 ELF，否则用 clang 封装 Mach-O；不依赖改写器的解码器。
# Independent LLVM disassembly: use ELF with objcopy, otherwise wrap Mach-O with clang; never use the rewriter's decoder.
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def relevant_instructions(blob, base, llvm_bin):
    """返回相关指令的原映像地址与文本。 | Return original-image addresses and text for relevant instructions."""
    if len(blob) % 4:
        raise ValueError('AArch64 text size must be a multiple of four')
    if not blob:
        return {}
    objdump = Path(llvm_bin) / 'llvm-objdump'
    objcopy = Path(llvm_bin) / 'llvm-objcopy'
    with tempfile.TemporaryDirectory() as directory:
        raw = Path(directory) / 'text.bin'
        obj = Path(directory) / 'text.o'
        raw.write_bytes(blob)
        if objcopy.is_file():
            subprocess.run([str(objcopy), '-I', 'binary', '-O', 'elf64-littleaarch64',
                            '--rename-section', '.data=.text,code', str(raw), str(obj)], check=True)
        else:
            # Xcode 缺 objcopy；汇编器仅封装原始字节，不翻译或修正任何客体指令。
            # Xcode lacks objcopy; the assembler only wraps raw bytes, translating or repairing no guest instructions.
            clang = os.environ.get('DM_CLANG') or shutil.which('clang')
            if not clang:
                raise FileNotFoundError('Need llvm-objcopy or clang for independent disassembly')
            asm = Path(directory) / 'text.S'
            incbin = str(raw).replace('\\', '\\\\').replace('"', '\\"')
            asm.write_text('.section __TEXT,__text,regular,pure_instructions\n'
                           '.p2align 2\n.incbin "' + incbin + '"\n')
            subprocess.run([clang, '-target', 'arm64-apple-macos11.0', '-c', str(asm), '-o', str(obj)], check=True)
        # 将数百万行的筛选交给 grep；保留 LLVM 作为独立解码器。
        # Filter millions of lines with grep, keeping LLVM as the independent decoder.
        process = subprocess.Popen([str(objdump), '-d', '--no-show-raw-insn', str(obj)], stdout=subprocess.PIPE)
        try:
            filtered = subprocess.run(['/usr/bin/grep', '-iE', r'[xw]18|tpidr|svc|cnt[pv]ct'],
                                      stdin=process.stdout, capture_output=True, text=True)
        finally:
            process.stdout.close()
            status = process.wait()
        if status:
            raise subprocess.CalledProcessError(status, process.args)
        if filtered.returncode not in (0, 1):
            raise subprocess.CalledProcessError(filtered.returncode, filtered.args, stderr=filtered.stderr)
    result = {}
    for line in filtered.stdout.splitlines():
        match = re.match(r'\s*([0-9a-f]+):\s+(.*)$', line)
        if match:
            result[base + int(match.group(1), 16)] = match.group(2).split('//')[0].split('<')[0].strip().lower()
    return result
