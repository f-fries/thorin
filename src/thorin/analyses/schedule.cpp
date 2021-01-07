#include "thorin/analyses/schedule.h"

#include "thorin/config.h"
#include "thorin/continuation.h"
#include "thorin/primop.h"
#include "thorin/world.h"
#include "thorin/analyses/cfg.h"
#include "thorin/analyses/domtree.h"
#include "thorin/analyses/looptree.h"
#include "thorin/analyses/scope.h"
#include "thorin/util/log.h"

namespace thorin {

Schedule::Schedule(const Scope& scope)
    : scope_(scope)
    , cfg_(scope.f_cfg())
    , domtree_(cfg().domtree())
{
    compute_def2uses();
}

void Schedule::compute_def2uses() {
    std::queue<const Def*> queue;
    DefSet done;

    auto enqueue = [&](const Def* def, size_t i, const Def* op) {
        if (scope_.contains(op)) {
            auto p1 = def2uses_[op].emplace(i, def);
            assert_unused(p1.second);
            auto p2 = done.emplace(op);
            if (p2.second)
                queue.push(op);
        }
    };

    for (auto n : cfg().reverse_post_order()) {
        queue.push(n->continuation());
        auto p = done.emplace(n->continuation());
        assert_unused(p.second);
    }

    while (!queue.empty()) {
        auto def = pop(queue);
        for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
            // all reachable continuations have already been registered above
            // NOTE we might still see references to unreachable continuations in the schedule
            if (!def->op(i)->isa<Continuation>())
                enqueue(def, i, def->op(i));
        }
    }
}

Continuation* Schedule::schedule_early(const Def* def) {
    if (auto cont = early_.lookup(def)) return *cont;
    if (auto param = def->isa<Param>()) return early_[def] = param->continuation();

    auto result = scope().entry();
    for (auto op : def->as<PrimOp>()->ops()) {
        if (!op->isa_continuation() && def2uses_.find(op) != def2uses_.end()) {
            auto cont = schedule_early(op);
            if (domtree_.depth(cfg(cont)) > domtree_.depth(cfg(result)))
                result = cont;
        }
    }

    return early_[def] = result;
}

Continuation* Schedule::schedule_late(const Def* def) {
    if (auto cont = late_.lookup(def)) return *cont;

    Continuation* result = nullptr;
    if (auto continuation = def->isa_continuation()) {
        result = continuation;
    } else if (auto param = def->isa<Param>()) {
        result = param->continuation();
    } else {
        for (auto use : uses(def)) {
            auto cont = schedule_late(use);
            result = result ? domtree_.least_common_ancestor(cfg(result), cfg(cont))->continuation() : cont;
        }
    }

    return late_[def] = result;
}

Continuation* Schedule::schedule_smart(const Def* def) {
    if (auto cont = smart_.lookup(def)) return *cont;

    auto early = cfg(schedule_early(def));
    auto late  = cfg(schedule_late (def));

    auto result = late;
    int depth = cfg().looptree()[late]->depth();
    for (auto i = late; i != early;) {
        auto idom = domtree_.idom(i);
        assert(i != idom);
        i = idom;

        // HACK this should actually never occur
        if (i == nullptr) {
            WLOG("don't know where to put {}", def);
            result = late;
            break;
        }

        int cur_depth = cfg().looptree()[i]->depth();
        if (cur_depth < depth) {
            result = i;
            depth = cur_depth;
        }
    }

    return smart_[def] = result->continuation();
}

BlockSchedule block_schedule(const Scope& scope) {
    // until we have sth better simply use the RPO of the CFG
    BlockSchedule result;
    for (auto n : scope.f_cfg().reverse_post_order())
        result.emplace_back(n->continuation());

    return result;
}

#if 0
void Schedule::verify() {
#if THORIN_ENABLE_CHECKS
    bool ok = true;
    auto& domtree = cfg().domtree();
    Schedule::Map<const Def*> block2mem(*this);

    for (auto& block : *this) {
        const Def* mem = block.continuation()->mem_param();
        auto idom = block.continuation() != scope().entry() ? domtree.idom(block.node()) : block.node();
        mem = mem ? mem : block2mem[(*this)[idom]];
        for (auto primop : block) {
            if (auto memop = primop->isa<MemOp>()) {
                if (memop->mem() != mem) {
                    WLOG("incorrect schedule: {} @ '{}'; current mem is {} @ '{}') - scope entry: {}", memop, memop->location(), mem, mem->location(), scope_.entry());
                    ok = false;
                }
                mem = memop->out_mem();
            }
        }
        block2mem[block] = mem;
    }

    assert(ok && "incorrectly wired or scheduled memory operations");
#endif
}

std::ostream& Schedule::stream(std::ostream& os) const {
    for (auto& block : *this) {
        auto continuation = block.continuation();
        if (continuation->intrinsic() != Intrinsic::EndScope) {
            bool indent = continuation != scope().entry();
            if (indent)
                os << up;
            os << endl;
            continuation->stream_head(os) << up_endl;
            for (auto primop : block)
                primop->stream_assignment(os);

            continuation->stream_jump(os) << down_endl;
            if (indent)
                os << down;
        }
    }
    return os << endl;
}

void Schedule::write_thorin(const char* filename) const { std::ofstream file(filename); stream(file); }

void Schedule::thorin() const {
    auto filename = world().name() + "_" + scope().entry()->unique_name() + ".thorin";
    write_thorin(filename.c_str());
}

//------------------------------------------------------------------------------

void verify_mem(World& world) {
    Scope::for_each(world, [&](const Scope& scope) { schedule(scope); });
}
#endif

}
