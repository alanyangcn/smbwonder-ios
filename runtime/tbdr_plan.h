#pragma once
// Load/store actions for the render passes of one Metal command buffer (metal3_draw.m defers its passes, full clears and
// buffer updates into a queue and plans them here when the queue is encoded). Pure logic, no Metal: textures are opaque
// identities.
//
// Proof rules (only what the queued operations themselves show; anything else keeps Load / Store):
//  - A full clear is folded into the next operation touching that texture when it is a pass attaching exactly the same
//    subresource and plane with Load and not sampling the texture: that attachment loads with Clear and the clear pass
//    is dropped (Metal's Clear load is the same overwrite, done in tile memory).
//  - An attachment stores DontCare when the next operation touching its texture overwrites exactly that subresource and
//    plane (a full clear, or a pass attachment loading with Clear) and nothing in between reads it. A sample of the
//    texture, an attachment through another view object, a Load attachment, an operation reading textures it does not
//    name (TBDR_OP_READS_ALL), or the end of the queue (later command buffers are unknown) keep Store.
//  - A clear whose every plane ends up DontCare does nothing and is dropped.
#include <stdint.h>
enum { TBDR_PLANE_COLOR, TBDR_PLANE_DEPTH, TBDR_PLANE_STENCIL };
enum { TBDR_LOAD_LOAD, TBDR_LOAD_CLEAR };
enum { TBDR_STORE_STORE, TBDR_STORE_DONTCARE };
// PASS: draws into atts (load in: Load, out: Clear when a clear was folded), sampling `reads` roots.
// CLEAR: full clear of atts (load Clear). NEUTRAL: touches no texture (buffer fills / updates).
// READS: reads (and may write) the `reads` roots. READS_ALL: may read any texture.
enum { TBDR_OP_PASS, TBDR_OP_CLEAR, TBDR_OP_NEUTRAL, TBDR_OP_READS, TBDR_OP_READS_ALL };
#define TBDR_MAX_ATT 10
struct tbdr_att {
    uintptr_t object, root;   // the attached texture object and the texture it views (itself when not a view)
    int level, slice, plane;
    int load, store;          // load: in/out; store: out
    int clear_op, clear_att;  // out: the folded clear (op index, attachment index), -1 if none
};
struct tbdr_op {
    int kind, natt, nreads;
    struct tbdr_att att[TBDR_MAX_ATT];
    const uintptr_t *reads;   // texture roots
    int dropped;              // out: a clear folded into a pass or with no effect
};
struct tbdr_stats { unsigned folded_clears, dropped_clears, dontcare_stores, stores, clear_loads; };
// Plans ops[0..n) in queue order. Every output field is (re)initialised; stats (optional) are added to.
void tbdr_plan(struct tbdr_op *ops, int n, struct tbdr_stats *stats);
