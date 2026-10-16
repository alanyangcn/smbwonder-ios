#include "tbdr_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { C = 1, D = 2, P = 3, V = 4 };   // texture identities (V: a view of C)
static struct tbdr_att att(uintptr_t object, uintptr_t root, int level, int plane, int load) {
    return (struct tbdr_att){.object = object, .root = root, .level = level, .plane = plane, .load = load};
}
static struct tbdr_op pass1(struct tbdr_att a) { struct tbdr_op o = {.kind = TBDR_OP_PASS, .natt = 1}; o.att[0] = a; return o; }
static struct tbdr_op clear1(struct tbdr_att a) { a.load = TBDR_LOAD_CLEAR; struct tbdr_op o = {.kind = TBDR_OP_CLEAR, .natt = 1}; o.att[0] = a; return o; }
static const uintptr_t reads_c[] = {C}, reads_p[] = {P};
int main(void) {
    struct tbdr_att col = att(C, C, 0, TBDR_PLANE_COLOR, 0), dep = att(D, D, 0, TBDR_PLANE_DEPTH, 0), sten = att(D, D, 0, TBDR_PLANE_STENCIL, 0);
    {   // clear C, pass C, then C is read (present): clear folded into the pass, which stores.
        struct tbdr_op o[3] = {clear1(col), pass1(col), {.kind = TBDR_OP_READS, .nreads = 1, .reads = reads_c}};
        struct tbdr_stats s = {0}; tbdr_plan(o, 3, &s);
        assert(o[0].dropped && o[1].att[0].load == TBDR_LOAD_CLEAR && o[1].att[0].clear_op == 0 && o[1].att[0].clear_att == 0);
        assert(o[1].att[0].store == TBDR_STORE_STORE && s.folded_clears == 1 && s.clear_loads == 1 && s.stores == 1 && s.dontcare_stores == 0);
    }
    {   // scene (C, D) -> post samples C into P -> clear D -> pass (P, D): scene depth DontCare (next touch overwrites it), the
        // depth clear folds into the last pass; C and P keep Store (read later / end of the queue).
        struct tbdr_op scene = {.kind = TBDR_OP_PASS, .natt = 2}; scene.att[0] = col; scene.att[1] = dep;
        struct tbdr_op post = pass1(att(P, P, 0, TBDR_PLANE_COLOR, 0)); post.nreads = 1; post.reads = reads_c;
        struct tbdr_op last = {.kind = TBDR_OP_PASS, .natt = 2}; last.att[0] = att(P, P, 0, TBDR_PLANE_COLOR, 0); last.att[1] = dep;
        struct tbdr_op o[4] = {scene, post, clear1(dep), last};
        tbdr_plan(o, 4, NULL);
        assert(o[0].att[0].store == TBDR_STORE_STORE && o[0].att[1].store == TBDR_STORE_DONTCARE);
        assert(o[2].dropped && o[3].att[1].load == TBDR_LOAD_CLEAR && o[3].att[0].load == TBDR_LOAD_LOAD);
        assert(o[1].att[0].store == TBDR_STORE_STORE && o[3].att[0].store == TBDR_STORE_STORE && o[3].att[1].store == TBDR_STORE_STORE);
    }
    {   // the pass samples the cleared texture: no fold (the sample would see the uncleared texture); the clear stores.
        struct tbdr_op p = pass1(col); p.nreads = 1; p.reads = reads_c;
        struct tbdr_op o[2] = {clear1(col), p}; tbdr_plan(o, 2, NULL);
        assert(!o[0].dropped && o[0].att[0].store == TBDR_STORE_STORE && o[1].att[0].load == TBDR_LOAD_LOAD);
    }
    {   // a pass sampling C between the pass and the clear keeps Store.
        struct tbdr_op r = pass1(att(P, P, 0, TBDR_PLANE_COLOR, 0)); r.nreads = 1; r.reads = reads_c;
        struct tbdr_op o[3] = {pass1(col), r, clear1(col)}; tbdr_plan(o, 3, NULL);
        assert(o[0].att[0].store == TBDR_STORE_STORE && !o[2].dropped);
    }
    {   // another view object of the same texture: not comparable, Store; and a clear is not folded through a view.
        struct tbdr_op o[2] = {pass1(att(V, C, 0, TBDR_PLANE_COLOR, 0)), clear1(col)}; tbdr_plan(o, 2, NULL);
        assert(o[0].att[0].store == TBDR_STORE_STORE);
        struct tbdr_op q[2] = {clear1(col), pass1(att(V, C, 0, TBDR_PLANE_COLOR, 0))}; tbdr_plan(q, 2, NULL);
        assert(!q[0].dropped && q[1].att[0].load == TBDR_LOAD_LOAD);
    }
    {   // another level of the same object does not touch level 0.
        struct tbdr_op o[3] = {pass1(col), pass1(att(C, C, 1, TBDR_PLANE_COLOR, 0)), clear1(col)}; tbdr_plan(o, 3, NULL);
        assert(o[0].att[0].store == TBDR_STORE_DONTCARE && o[1].att[0].store == TBDR_STORE_STORE);
    }
    {   // a slice is a different subresource too; a Load attachment of the same subresource is a read.
        struct tbdr_att s1 = col; s1.slice = 1;
        struct tbdr_op o[3] = {pass1(col), pass1(s1), clear1(col)}; tbdr_plan(o, 3, NULL);
        assert(o[0].att[0].store == TBDR_STORE_DONTCARE);
        struct tbdr_op q[3] = {pass1(col), pass1(col), clear1(col)}; tbdr_plan(q, 3, NULL);
        assert(q[0].att[0].store == TBDR_STORE_STORE && q[1].att[0].store == TBDR_STORE_DONTCARE);
    }
    {   // an operation that may read any texture (compute, copy, readback) and the end of the queue keep Store;
        // operations without textures (buffer updates) are transparent.
        struct tbdr_op o[3] = {pass1(col), {.kind = TBDR_OP_READS_ALL}, clear1(col)}; tbdr_plan(o, 3, NULL);
        assert(o[0].att[0].store == TBDR_STORE_STORE && !o[2].dropped && o[2].att[0].store == TBDR_STORE_STORE);
        struct tbdr_op q[3] = {clear1(col), {.kind = TBDR_OP_NEUTRAL}, pass1(col)}; tbdr_plan(q, 3, NULL);
        assert(q[0].dropped && q[2].att[0].load == TBDR_LOAD_CLEAR);
        struct tbdr_op r[2] = {pass1(col), {.kind = TBDR_OP_READS, .nreads = 1, .reads = reads_p}}; tbdr_plan(r, 2, NULL);
        assert(r[0].att[0].store == TBDR_STORE_STORE);
    }
    {   // two clears in a row: the first has no effect (dropped), the second folds.
        struct tbdr_op o[3] = {clear1(col), clear1(col), pass1(col)}; struct tbdr_stats s = {0}; tbdr_plan(o, 3, &s);
        assert(o[0].dropped && o[1].dropped && o[2].att[0].load == TBDR_LOAD_CLEAR && o[2].att[0].clear_op == 1);
        assert(s.folded_clears == 1 && s.dropped_clears == 1);
    }
    {   // planes are separate: depth-only clear (stencil attached with Load) after a depth-stencil pass.
        struct tbdr_op a = {.kind = TBDR_OP_PASS, .natt = 2}; a.att[0] = dep; a.att[1] = sten;
        struct tbdr_op c = {.kind = TBDR_OP_CLEAR, .natt = 2}; c.att[0] = dep; c.att[0].load = TBDR_LOAD_CLEAR; c.att[1] = sten; c.att[1].load = TBDR_LOAD_LOAD;
        struct tbdr_op o[3] = {a, c, a}; tbdr_plan(o, 3, NULL);
        assert(o[1].dropped && o[2].att[0].load == TBDR_LOAD_CLEAR && o[2].att[1].load == TBDR_LOAD_LOAD);
        assert(o[0].att[0].store == TBDR_STORE_DONTCARE && o[0].att[1].store == TBDR_STORE_STORE);
        // depth and stencil cleared, the next pass attaches only depth: the stencil clear has no folding target, nothing folds.
        struct tbdr_op c2 = c; c2.att[1].load = TBDR_LOAD_CLEAR;
        struct tbdr_op q[2] = {c2, pass1(dep)}; tbdr_plan(q, 2, NULL);
        assert(!q[0].dropped && q[1].att[0].load == TBDR_LOAD_LOAD && q[0].att[0].store == TBDR_STORE_STORE);
    }
    {   // replanning resets outputs (a queue planned twice gives the same result).
        struct tbdr_op o[2] = {clear1(col), pass1(col)}; tbdr_plan(o, 2, NULL); tbdr_plan(o, 2, NULL);
        assert(o[0].dropped && o[1].att[0].load == TBDR_LOAD_CLEAR);
    }
    puts("TBDR_PLAN_OK");
}
