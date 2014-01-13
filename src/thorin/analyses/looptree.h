#ifndef ANALYSES_LOOPS_H
#define ANALYSES_LOOPS_H

#include <vector>

#include "thorin/analyses/scope_analysis.h"
#include "thorin/util/array.h"
#include "thorin/util/autoptr.h"

namespace thorin {

class Lambda;
class Scope;
class World;

struct Edge {
    Edge() {}
    Edge(Lambda* src, Lambda* dst) 
        : src_(src)
        , dst_(dst)
    {}

    Lambda* src() const { return src_; }
    Lambda* dst() const { return dst_; }

private:
    Lambda* src_;
    Lambda* dst_;
};

class LoopHeader;

/**
 * Represents a node of a loop nesting forest.
 * Please refer to G. Ramalingam, "On Loops, Dominators, and Dominance Frontiers", 1999
 * for an introduction to loop nesting forests.
 * A \p LoopNode consists of a set of header \p Lambda%s.
 * The root node is a \p LoopHeader without any lambdas but further \p LoopNode children and \p depth_ -1.
 * Thus, the forest is pooled into a tree.
 */
class LoopNode : public MagicCast<LoopNode> {
public:
    LoopNode(LoopHeader* parent, int depth, const std::vector<Lambda*>& lambdas);

    int depth() const { return depth_; }
    const LoopHeader* parent() const { return parent_; }
    ArrayRef<Lambda*> lambdas() const { return lambdas_; }
    size_t num_lambdas() const { return lambdas().size(); }

protected:
    LoopHeader* parent_;
    int depth_;
    std::vector<Lambda*> lambdas_;
};

/// A LoopHeader owns further \p LoopNode%s as children.
class LoopHeader : public LoopNode {
public:
    explicit LoopHeader(LoopHeader* parent, int depth, const std::vector<Lambda*>& lambdas)
        : LoopNode(parent, depth, lambdas)
        , dfs_begin_(0)
        , dfs_end_(-1)
    {}

    ArrayRef<LoopNode*> children() const { return children_; }
    const LoopNode* child(size_t i) const { return children_[i]; }
    size_t num_children() const { return children().size(); }
    const std::vector<Edge>& entries() const { return entries_; }
    const std::vector<Edge>& exits() const { return exits_; }
    const std::vector<Edge>& backedges() const { return backedges_; }
    bool is_root() const { return parent_ == 0; }
    size_t dfs_begin() const { return dfs_begin_; };
    size_t dfs_end() const { return dfs_end_; }

private:
    size_t dfs_begin_;
    size_t dfs_end_;
    AutoVector<LoopNode*> children_;
    std::vector<Edge> entries_;
    std::vector<Edge> exits_;
    std::vector<Edge> backedges_;

    friend class LoopNode;
    friend class LoopTreeBuilder;
};

class LoopLeaf : public LoopNode {
public:
    explicit LoopLeaf(size_t dfs_index, LoopHeader* parent, int depth, const std::vector<Lambda*>& lambdas)
        : LoopNode(parent, depth, lambdas)
        , dfs_index_(dfs_index)
    {
        assert(num_lambdas() == 1);
    }

    Lambda* lambda() const { return lambdas().front(); }
    size_t dfs_index() const { return dfs_index_; }

private:
    size_t dfs_index_;
};

/**
 * Calculates a loop nesting forest rooted at \p root_.
 * The implementation uses Steensgard's algorithm.
 * Check out G. Ramalingam, "On Loops, Dominators, and Dominance Frontiers", 1999, for more information.
 */
class LoopTree : public ScopeAnalysis<LoopLeaf, true, false /*do not auto-destroy nodes*/> {
public:
    typedef ScopeAnalysis<LoopLeaf, true, false> Super;

    explicit LoopTree(const Scope& scope);

    const LoopHeader* root() const { return root_; }
    int depth(Lambda* lambda) const { return Super::lookup(lambda)->depth(); }
    size_t lambda2dfs(Lambda* lambda) const { return Super::lookup(lambda)->dfs_index(); }
    bool contains(const LoopHeader* header, Lambda* lambda) const {
        if (!scope().contains(lambda)) return false;
        size_t dfs = lambda2dfs(lambda);
        return header->dfs_begin() <= dfs && dfs < header->dfs_end();
    }
    ArrayRef<const LoopLeaf*> loop(const LoopHeader* header) {
        return ArrayRef<const LoopLeaf*>(dfs_leaves_.data() + header->dfs_begin(), header->dfs_end() - header->dfs_begin());
    }
    Array<Lambda*> loop_lambdas(const LoopHeader* header);
    Array<Lambda*> loop_lambdas_in_rpo(const LoopHeader* header);
    void dump() const;

private:
    Array<LoopLeaf*> dfs_leaves_;
    AutoPtr<LoopHeader> root_;

    friend class LoopTreeBuilder;
};

std::ostream& operator << (std::ostream& o, const LoopNode* node);

} // namespace thorin

#endif // ANALYSES_LOOPS_H
