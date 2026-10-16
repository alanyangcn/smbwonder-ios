#pragma once
// GPU submission gate for the iOS app lifecycle. iOS refuses GPU work from a backgrounded app
// (kIOGPUCommandBufferCallbackErrorBackgroundExecutionNotPermitted, round154b: the player left the app and the next
// replay command buffer failed, stopping the game). The scene closes the gate when it resigns active (GPU work is still
// allowed until it enters the background, ~0.8 s later) and opens it when it is active again; guest threads entering
// nvnQueueSubmitCommands / nvnQueuePresentTexture wait while it is closed, as a Switch title is suspended on HOME.
#include <stdint.h>
void wonder_gpu_gate_set(int open);        // scene lifecycle (LoaderProbe.m); starts open
int wonder_gpu_gate_is_open(void);
uint64_t wonder_gpu_gate_wait(void);       // blocks while closed; returns the nanoseconds waited (0 = was open)
