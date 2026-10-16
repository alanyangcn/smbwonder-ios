#include "tbdr_plan.h"
enum { TOUCH_END, TOUCH_READ, TOUCH_ATTACH };
static int reads_root(const struct tbdr_op *o, uintptr_t root) {
    for (int i = 0; i < o->nreads; i++) if (o->reads[i] == root) return 1;
    return 0;
}
// The first operation after `from` that touches a's subresource and plane: TOUCH_ATTACH (*op, *att: an attachment of exactly
// that subresource and plane, no other view of the texture in that op, texture not sampled there), TOUCH_READ (anything
// else that may read it), TOUCH_END (no operation in the queue touches it).
static int next_touch(const struct tbdr_op *ops, int n, int from, const struct tbdr_att *a, int *op, int *att) {
    for (int j = from; j < n; j++) {
        const struct tbdr_op *o = &ops[j];
        if (o->dropped || o->kind == TBDR_OP_NEUTRAL) continue;
        if (o->kind == TBDR_OP_READS_ALL || reads_root(o, a->root)) return TOUCH_READ;
        if (o->kind == TBDR_OP_READS) continue;
        int exact = -1;
        for (int k = 0; k < o->natt; k++) {
            const struct tbdr_att *b = &o->att[k];
            if (b->root != a->root) continue;
            if (b->object != a->object) return TOUCH_READ;   // another view of the texture: subresources not comparable
            if (b->level == a->level && b->slice == a->slice && b->plane == a->plane) exact = k;
        }
        if (exact >= 0) { *op = j; *att = exact; return TOUCH_ATTACH; }
    }
    return TOUCH_END;
}
void tbdr_plan(struct tbdr_op *ops, int n, struct tbdr_stats *stats) {
    struct tbdr_stats s = {0};
    for (int i = 0; i < n; i++) {
        ops[i].dropped = 0;
        for (int k = 0; k < ops[i].natt; k++) {
            struct tbdr_att *a = &ops[i].att[k];
            if (ops[i].kind == TBDR_OP_PASS) a->load = TBDR_LOAD_LOAD;
            a->store = TBDR_STORE_STORE; a->clear_op = a->clear_att = -1;
        }
    }
    // Fold full clears into the pass that next attaches all their cleared planes.
    for (int i = 0; i < n; i++) {
        struct tbdr_op *c = &ops[i];
        if (c->kind != TBDR_OP_CLEAR) continue;
        int pass = -1, pa[TBDR_MAX_ATT], cleared = 0, ok = 1;
        for (int k = 0; k < c->natt && ok; k++) {
            if (c->att[k].load != TBDR_LOAD_CLEAR) continue;
            int p, a; cleared++;
            if (next_touch(ops, n, i + 1, &c->att[k], &p, &a) != TOUCH_ATTACH || ops[p].kind != TBDR_OP_PASS || ops[p].att[a].load != TBDR_LOAD_LOAD || (pass >= 0 && p != pass)) ok = 0;
            else { pass = p; pa[k] = a; }
        }
        if (!ok || !cleared) continue;
        for (int k = 0; k < c->natt; k++) if (c->att[k].load == TBDR_LOAD_CLEAR) {
            struct tbdr_att *b = &ops[pass].att[pa[k]];
            b->load = TBDR_LOAD_CLEAR; b->clear_op = i; b->clear_att = k; s.clear_loads++;
        }
        c->dropped = 1; s.folded_clears++;
    }
    // Stores: DontCare when the next touch overwrites the same subresource and plane.
    for (int i = 0; i < n; i++) {
        struct tbdr_op *o = &ops[i];
        if (o->dropped || (o->kind != TBDR_OP_PASS && o->kind != TBDR_OP_CLEAR)) continue;
        int all = o->natt > 0;
        for (int k = 0; k < o->natt; k++) {
            int p, a;
            if (next_touch(ops, n, i + 1, &o->att[k], &p, &a) == TOUCH_ATTACH && ops[p].att[a].load == TBDR_LOAD_CLEAR) { o->att[k].store = TBDR_STORE_DONTCARE; s.dontcare_stores++; }
            else { all = 0; s.stores++; }
        }
        if (o->kind == TBDR_OP_CLEAR && all) { o->dropped = 1; s.dropped_clears++; }
    }
    if (stats) {
        stats->folded_clears += s.folded_clears; stats->dropped_clears += s.dropped_clears; stats->dontcare_stores += s.dontcare_stores;
        stats->stores += s.stores; stats->clear_loads += s.clear_loads;
    }
}
