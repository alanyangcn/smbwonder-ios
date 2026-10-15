# Wonder First Game Frame Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Render and present the first actual Wonder game frame on FlyMe, preserving game commands/resources and retaining strict failure boundaries.
**Architecture:** Preserve SDK 55 command object fields used inline by main. Record typed host commands in guest-supplied command memory; detect native inline command words explicitly and handle them before accepting a submission. Replay supported commands into a persistent Metal 2 queue, with real resource/texture/shader bindings and completion/presentation evidence.
**Tech Stack:** C11, Objective-C, AArch64 signed offline-rewritten modules, Metal 2 / MSL 2.1, Python, XcodeGen/Xcode.
**Spec:** METAL2-RENDER-PLAN.md and METAL2-STATUS.md.

## Global Constraints

- Wonder 1.2.0 / update 262144, existing verified main/sdk identities and rewrite artifacts.
- iPhone 8 Plus / A11 / iOS 16.7.16; no JIT guest CPU code, no Metal 3 prerequisite.
- Missing service/command/shader/resource semantics stop with evidence; no generic success stubs.
- Game data, derived shaders and device evidence stay in ignored work/.
- A diagnostic image or a clear-only frame does not satisfy actual game first-frame acceptance.

## Review Focus

- SDK 55 inline writes must not collide with host metadata or silently disappear.
- Recording snapshots mutable arguments; submitted buffers/resources outlive GPU use.
- Virtual pool physical aliases and texture uploads cannot read PROT_NONE token addresses.
- Unknown shader facts/bindings remain unvalidated; compilation alone is insufficient.
- Fresh launch identity, GPU completion, image capture and presentation must refer to the same run.

## Tasks

### 1. SDK 55 recording lifecycle
Files: runtime/nvn_buffer.c, tests/nvn_device_test.c; split command recorder into runtime/nvn_commands.c/.h when command bodies grow.
- [ ] Decode target SDK Begin/End and command-memory callback/control records; preserve evidence in work/analysis/first-frame/.
- [ ] Add failing tests for begin/end state, bounds/canaries, copied arguments and segment changes.
- [ ] Implement recorder/lifecycle with unsupported native packets rejected before submission.
- [ ] Run targeted tests, build, install and identify the next real command on FlyMe.

### 2. First submission and visible clear
Files: runtime/nvn_commands.c/.h, runtime/host_graphics.m/.h, runtime/nvn_swapchain.c; tests for recorder/replay.
- [ ] Implement actually encountered commands, queue completion, fences and swapchain presentation.
- [ ] Validate real GPU clear/readback/presentation without calling it game first frame.
- [ ] Review command lifetimes, native inline packets and failure paths.

### 3. Game resources and actual startup shaders
Files: runtime/nvn_texture.c/.h, runtime/nvn_device.c, tools/*shader*.py, Metal resource/shader bridge.
- [ ] Capture actual program code/control and binding facts at ProgramSetShaders.
- [ ] Translate and validate those exact variants offline; compile MSL on A11 and bind by verified reflection.
- [ ] Implement actual startup texture/buffer/view uploads and alias/coherence rules.
- [ ] Exercise GPU resource bindings with meaningful failure controls.

### 4. First game draw and acceptance
- [ ] Implement required PSO/vertex/index/draw states observed at startup.
- [ ] Present and capture a game-generated frame with draw count, resource/shader identities and GPU completion.
- [ ] Inspect image, retain raw run logs and hashes, update status and commit verified changes.
