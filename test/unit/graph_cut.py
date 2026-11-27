#!/usr/bin/env python3
"""Edit the neighbor tuples of a pgvector hnsw index file (the cluster must be stopped) so that one element C loses every incoming edge and
a second element E keeps only the incoming edge from C and points to C itself: C is then not reachable from the entry point, and neither is E, while each lists the other as a neighbor.

usage: graph_cut.py <index file> <C heap block> <C heap offset> <E heap block> <E heap offset>

Element and neighbor tuple layout as in src/cuvs_cover_reach.h: an element tuple is type 1 with ten heap TIDs at offset 4 and the TID of its
neighbor tuple at offset 64; a neighbor tuple is type 2 with a count at offset 2 and the TIDs of its neighbors from offset 4. A removed edge is
overwritten with the invalid TID (block 0xFFFFFFFF), which the walk and pgvector skip.
Prints "cut C=<n edges removed> E=<n edges removed> added=<0|1>".
"""
import struct
import sys

PAGE = 8192
INVALID = (0xFFFF, 0xFFFF, 0)


def items(page):
    lower, upper, special = struct.unpack_from('<HHH', page, 12)
    if special != PAGE - 8 or lower < 24 or lower > upper:
        return
    for i in range((lower - 24) // 4):
        lp, = struct.unpack_from('<I', page, 24 + 4 * i)
        off, flags, ln = lp & 0x7FFF, (lp >> 15) & 3, lp >> 17
        if flags == 1 and off + ln <= PAGE - 8 and ln >= 4:
            yield i + 1, off, ln


def tid(page, at):
    hi, lo, off = struct.unpack_from('<HHH', page, at)
    return (hi << 16) | lo, off


def main():
    path = sys.argv[1]
    cb, co, eb, eo = (int(x) for x in sys.argv[2:6])
    data = bytearray(open(path, 'rb').read())
    nblocks = len(data) // PAGE
    elements = {}
    neighbors = []
    for blk in range(1, nblocks):
        page = data[blk * PAGE:(blk + 1) * PAGE]
        for no, off, ln in items(page):
            t = page[off]
            if t == 1 and ln >= 72:
                heaps = [tid(page, off + 4 + 6 * k) for k in range(10)]
                elements[(blk, no)] = (heaps, tid(page, off + 64))
            elif t == 2:
                neighbors.append((blk, no, off, ln))
    ec = [k for k, (h, _) in elements.items() if (cb, co) in h]
    ee = [k for k, (h, _) in elements.items() if (eb, eo) in h]
    if len(ec) != 1 or len(ee) != 1:
        sys.exit(f'element of C: {ec}, of E: {ee}')
    ec, ee = ec[0], ee[0]
    c_neighbor_tuple = elements[ec][1]
    e_neighbor_tuple = elements[ee][1]
    removed_c = removed_e = added = 0
    for blk, no, off, ln in neighbors:
        base = blk * PAGE + off
        n = struct.unpack_from('<H', data, base + 2)[0]
        n = min(n, (ln - 4) // 6)
        for k in range(n):
            at = base + 4 + 6 * k
            t = tid(data, at)
            if t == ec and (blk, no) != e_neighbor_tuple:
                struct.pack_into('<HHH', data, at, *INVALID)
                removed_c += 1
            elif t == ee and (blk, no) != c_neighbor_tuple:
                struct.pack_into('<HHH', data, at, *INVALID)
                removed_e += 1
    # the edge C -> E: into the neighbor tuple of C, in place of its last entry if E is not among them
    for blk, no, off, ln in neighbors:
        if (blk, no) == c_neighbor_tuple:
            base = blk * PAGE + off
            n = min(struct.unpack_from('<H', data, base + 2)[0], (ln - 4) // 6)
            present = any(tid(data, base + 4 + 6 * k) == ee for k in range(n))
            if not present and n > 0:
                struct.pack_into('<HHH', data, base + 4 + 6 * (n - 1), (ee[0] >> 16) & 0xFFFF, ee[0] & 0xFFFF, ee[1])
                added = 1
    # the edge E -> C: into the neighbor tuple of E, in place of its last entry if C is not among them
    for blk, no, off, ln in neighbors:
        if (blk, no) == e_neighbor_tuple:
            base = blk * PAGE + off
            n = min(struct.unpack_from('<H', data, base + 2)[0], (ln - 4) // 6)
            present = any(tid(data, base + 4 + 6 * k) == ec for k in range(n))
            if not present and n > 0:
                struct.pack_into('<HHH', data, base + 4 + 6 * (n - 1), (ec[0] >> 16) & 0xFFFF, ec[0] & 0xFFFF, ec[1])
    open(path, 'wb').write(data)
    print(f'cut C={removed_c} E={removed_e} added={added}')


if __name__ == '__main__':
    main()
