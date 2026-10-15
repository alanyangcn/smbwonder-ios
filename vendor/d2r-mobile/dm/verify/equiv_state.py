# 差分测试的可观察内存写入：仅排除改写 ABI 实际使用的完整 TSD 槽写入。
# Observable differential-test writes: exclude only full TSD-slot writes actually used by the rewrite ABI.

def guest_writes(writes, patched, tsd_base, key_slots):
    """保留其余写入的地址、宽度、值与顺序。 | Preserve address, width, value and order of every other write."""
    if not patched:
        return list(writes)
    slots = {tsd_base + 8 * key for key in key_slots}
    return [write for write in writes if not (write[0] in slots and write[1] == 8)]
