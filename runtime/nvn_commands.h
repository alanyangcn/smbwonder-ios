#pragma once
#include "nvn_device.h"
#define WONDER_NVN_COMMAND_MAGIC UINT32_C(0x57434d44)
struct wonder_nvn_command_header { uint32_t magic, size, opcode, reserved; };
enum { WONDER_NVN_DRAW_ELEMENTS = 1 };
struct wonder_nvn_draw_elements { int32_t primitive, index_type, count, reserved; uint64_t index_address; };
// The callback consumes an immutable-by-contract snapshot for this submission; record bytes live in
// caller-owned NVN command memory. Returns no success for unknown commands or native inline packets.
typedef void (*wonder_nvn_command_visitor)(unsigned opcode, const void *payload, size_t size, void *context);
void wonder_nvn_command_walk(uint64_t handle, wonder_nvn_command_visitor visitor, void *context);
