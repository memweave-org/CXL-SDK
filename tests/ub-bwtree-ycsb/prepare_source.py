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
    # This test never reclaims nodes. Avoid the freelist's 128-bit atomic on UB.
    replace('auto ret_pair = free_node_id_list.Pop();',
            'auto ret_pair = std::make_pair(false, NodeID{});')
    replace('  AtomicStack<NodeID, MAPPING_TABLE_SIZE> free_node_id_list;',
            '  struct UnusedFreelist {} free_node_id_list;')
    begin = text.index('    bool ForwardRemoteEpochGCIfNeeded(')
    end = text.index('\n#endif', begin)
    text = text[:begin] + '    bool ForwardRemoteEpochGCIfNeeded(const BaseNode*) { return false; }\n' + text[end:]
    replace('  void PerformGC(int thread_id) {',
            '  void PerformGC(int thread_id) { return; // benchmark: retain every node until both hosts exit')
    # All readers use TraverseReadOptimized directly, with no epoch writes.
    # Published nodes are immutable and retained until both hosts exit.
    # Refresh before dereferencing a remotely published node (including deltas).
    replace('    inline NodeType GetType() const { return metadata.type; }', '''
    inline NodeType GetType() const {
      if (ub_read_only_thread) {
        clflush(this, sizeof(BaseNode));
        BwTree::RefreshRemoteNode(this, metadata.type);
        if (metadata.low_key_p) clflush(metadata.low_key_p, sizeof(KeyNodeIDPair));
        if (metadata.high_key_p) clflush(metadata.high_key_p, sizeof(KeyNodeIDPair));
      }
      return metadata.type;
    }
''')
    # A node can be reached through a low/high-key pointer before GetType().
    # Refresh metadata accessors too; never write to the shared mapping.
    replace('    inline const NodeMetaData &GetNodeMetaData() const { return metadata; }',
            '    inline const NodeMetaData &GetNodeMetaData() const { (void)GetType(); return metadata; }')
    marker='  inline void TakeNodeSnapshotReadOptimized(NodeID node_id,'
    replace(marker, '''  static void RefreshRemoteNode(const BaseNode* p, NodeType type) {
    // Fixed-size deltas fit in this union; elastic base nodes carry an array.
    if (type == NodeType::LeafType) {
      auto* node=static_cast<const LeafNode*>(p);
      clflush(node, sizeof(LeafNode));
      clflush(node, sizeof(LeafNode) + sizeof(KeyValuePair)*node->GetSize());
    } else if (type == NodeType::InnerType) {
      auto* node=static_cast<const InnerNode*>(p);
      clflush(node, sizeof(InnerNode));
      clflush(node, sizeof(InnerNode) + sizeof(KeyNodeIDPair)*node->GetSize());
    } else {
      clflush(p, sizeof(DeltaNodeUnion));
    }
  }

''' + marker)
    Path(target).write_text(text)


if __name__ == '__main__':
    prepare(*sys.argv[1:])
