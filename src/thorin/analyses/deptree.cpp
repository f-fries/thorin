#include "thorin/analyses/deptree.h"

#include "thorin/world.h"

namespace thorin {

static void merge(ParamSet& params, ParamSet&& other) {
    params.insert(other.begin(), other.end());
}

void DepTree::run() {
    for (const auto& [_, nom] : world().externals()) run(nom);
    adjust_depth(root_.get(), 0);
}

ParamSet DepTree::run(Def* nom) {
    auto [i, success] = nom2node_.emplace(nom, std::unique_ptr<DepNode>());
    if (!success) {
        if (auto params = def2params_.lookup(nom))
            return *params;
        else
            return {};
    }

    i->second = std::make_unique<DepNode>(nom, stack_.size() + 1);
    auto node = i->second.get();
    stack_.push_back(node);

    auto result = run(nom, nom);
    auto parent = root_.get();
    for (auto param : result) {
        auto n = nom2node_[param->nominal()].get();
        parent = n->depth() > parent->depth() ? n : parent;
    }
    node->set_parent(parent);

    stack_.pop_back();
    return result;
}

ParamSet DepTree::run(Def* cur_nom, const Def* def) {
    if (def->is_const())                                         return {};
    if (auto params = def2params_.lookup(def))                   return *params;
    if (auto nom    = def->isa_nominal(); nom && cur_nom != nom) return run(nom);

    ParamSet result;
    if (auto param = def->isa<Param>()) {
        result.emplace(param);
    } else {
        for (auto op : def->extended_ops())
            merge(result, run(cur_nom, op));

        if (cur_nom == def) result.erase(cur_nom->param());
    }

    return def2params_[def] = result;
}

void DepTree::adjust_depth(DepNode* node, size_t depth) {
    node->depth_ = depth;

    for (const auto& child : node->children())
        adjust_depth(child, depth + 1);
}

bool DepTree::depends(Def* a, Def* b) const {
    auto n = nom2node(a);
    auto m = nom2node(b);

    if (n->depth() < m->depth()) return false;

    auto i = n;
    for (size_t e = m->depth(); i->depth() != e; i = i->parent()) {}

    return i == m;
}

}