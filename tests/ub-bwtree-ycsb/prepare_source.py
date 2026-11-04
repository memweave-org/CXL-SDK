#!/usr/bin/env python3
"""Apply isolated benchmark adaptations to a copied BwTree header, never the source tree."""
from pathlib import Path
import sys


def prepare(source, target):
    text = Path(source).read_text()
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise RuntimeError('BwTree source changed; cannot apply adaptation: ' + old[:80])
        text = text.replace(old, new)
    # NodeIDs are never returned to the freelist by the original implementation.
    # Avoid its unused 128-bit atomic on UB.
    replace('auto ret_pair = free_node_id_list.Pop();',
            'auto ret_pair = std::make_pair(false, NodeID{});')
    replace('  AtomicStack<NodeID, MAPPING_TABLE_SIZE> free_node_id_list;',
            '  struct UnusedFreelist {} free_node_id_list;')
    # Only B installs nodes, so all reclamation stays with their owner.
    begin = text.index('    bool ForwardRemoteEpochGCIfNeeded(')
    end = text.index('\n#endif', begin)
    text = text[:begin] + '    bool ForwardRemoteEpochGCIfNeeded(const BaseNode*) { return false; }\n' + text[end:]
    Path(target).write_text(text)


if __name__ == '__main__':
    prepare(*sys.argv[1:])
