#include <functional>

#include "thorin/check.h"
#include "thorin/transform/lower_typed_closures.h"

namespace thorin {

void LowerTypedClosures::run() {
    auto externals = std::vector(world().externals().begin(), world().externals().end());
    for (auto [_, n]: externals)
        rewrite(n);
    while (!worklist_.empty()) {
        auto [lvm, lcm, lam] = worklist_.front();
        worklist_.pop();
        lcm_ = lcm;
        lvm_ = lvm;
        world().DLOG("in {} (lvm={}, lcm={})", lam, lvm_, lcm_);
        if (lam->is_set()) {
            lam->set_body(rewrite(lam->body()));
            lam->set_filter(rewrite(lam->filter()));
        }
    }
}

// After scalarize, the :mem paramter can be basically anywhere
static const Def* get_mem_var(Lam *lam) {
    for (size_t i = 0; i < lam->num_doms(); i++)
        if (thorin::isa<Tag::Mem>(lam->dom(i)))
            return lam->var(i);
    assert(false && "continuation \\wo :mem paramter");
}

Lam *LowerTypedClosures::make_stub(Lam* lam, bool unbox_env) {
    assert(lam && "make_stub: not a lam");
    if (auto new_lam = old2new_.lookup(lam); new_lam && (*new_lam)->isa_nom<Lam>())
        return (*new_lam)->as_nom<Lam>();
    auto& w = world();
    auto new_type = w.cn(Array<const Def*>(lam->num_doms(), [&](auto i) {
        auto new_dom = rewrite(lam->dom(i));
        return (i == CLOSURE_ENV_PARAM && !unbox_env) ? w.type_ptr(new_dom) : new_dom;
    }));
    auto new_lam = lam->stub(w, new_type, w.dbg("uc" + lam->name()));
    w.DLOG("stub {} ~> {}", lam, new_lam);
    new_lam->set_name(lam->name());
    new_lam->set_body(lam->body());
    new_lam->set_filter(lam->filter());
    if (lam->is_external()) {
        lam->make_internal();
        new_lam->make_external();
    }
    auto mem_var = get_mem_var(lam);
    const Def* lcm = get_mem_var(new_lam);
    const Def* env = new_lam->var(CLOSURE_ENV_PARAM);
    if (!unbox_env) {
        auto env_mem = w.op_load(lcm, env);
        lcm = w.extract(env_mem, 0_u64);
        env = w.extract(env_mem, 1_u64, w.dbg("env"));
    }
    auto new_args = w.tuple(Array<const Def*>(lam->num_doms(), [&](auto i) {
        return (i == CLOSURE_ENV_PARAM) ? env
             : (lam->var(i) == mem_var) ? lcm
             : new_lam->var(i);
    }));
    map(lam->var(), new_args);
    worklist_.emplace(mem_var, lcm, new_lam);
    return map<Lam>(lam, new_lam);
}

const Def* LowerTypedClosures::make_stub(ClosureLit& closure, bool unbox_env) {
    auto& w = world();
    if (auto fnc = closure.fnc_as_lam())
        return make_stub(fnc, unbox_env);
    auto [idx, lams] = closure.fnc_as_folded();
    assert(idx && lams && "closure should be lam or folded branch");
    auto new_lams = DefArray(lams->num_ops(), [&](auto i) {
        const Def* lam = lams->op(i);
        return make_stub(lam->isa_nom<Lam>(), unbox_env);
    });
    return w.extract(w.tuple(new_lams), idx);
}

// TODO: Handle ptr, cn's?
static size_t repr_size(const Def* type, size_t inf) {
    if (auto size = thorin::isa_sized_type(type)) {
        if (auto sz = isa_lit(size))
            return *sz;
        else
            return inf;
    } else if (auto sigma = type->isa<Sigma>()) {
        auto size = 0;
        for (size_t i = 0; i < sigma->num_ops(); i++)
            size += repr_size(sigma->op(0), inf);
        return size;
    } else if (auto arr = type->isa<Arr>(); arr && isa_lit(arr->shape())) {
        return as_lit(arr->shape()) * repr_size(arr->body(), inf);
    } else {
        return inf;
    }
}

bool LowerTypedClosures::unbox_env(const Def* type) {
    return repr_size(type, 64 * 2) <= 64;
}

const Def* LowerTypedClosures::rewrite(const Def* def) {
    switch(def->node()) {
        case Node::Bot:
        case Node::Top:
        case Node::Kind:
        case Node::Space:
        case Node::Nat:
            return def;
    }

    auto& w = world();

    if (auto new_def = old2new_.lookup(def))
        return *new_def;

    auto new_type = rewrite(def->type());
    auto new_dbg = def->dbg() ? rewrite(def->dbg()) : nullptr;

    if (auto ct = isa_ctype(def)) {
        return map(def, w.sigma({rewrite(ct->op(1)), rewrite(ct->op(2))}));
    } else if (auto proj = def->isa<Extract>()) {
        auto tuple = proj->tuple();
        auto idx = isa_lit(proj->index());
        if (isa_ctype(tuple->type())) {
            assert (idx && idx <= 2 && "unknown proj from closure tuple");
            if (*idx == 0)
                return map(def, env_type());
            else
                return map(def, rewrite(tuple)->proj(*idx - 1));
        } else if (auto var = tuple->isa<Var>(); var && isa_ctype(var->nom())) {
            assert(false && "proj fst type form closure type");
        }
    }

    if (auto c = isa_closure_lit(def)) {
        auto env = rewrite(c.env());
        auto unbox = unbox_env(env);
        auto fn = make_stub(c, unbox);
        const Def* lwd_clos;
        if (!unbox) {
            auto mem_ptr = (c.is_escaping()) 
                ? w.op_slot(env->type(), lcm_)
                : w.op_alloc(env->type(), lcm_);
            auto mem = w.extract(mem_ptr, 0_u64);
            auto env_ptr = mem_ptr->proj(1_u64, w.dbg(fn->name() + "_env"));
            lcm_ = w.op_store(mem, env_ptr, env);
            map(lvm_, lcm_);
            lwd_clos = w.tuple({env_ptr, fn});
        } else {
            lwd_clos = w.tuple({env, fn});
        }
        return w.op_bitcast(new_type, lwd_clos);
    } else if (auto lam = def->isa_nom<Lam>()) {
        // Lam's in callee pos are scalarized (unpacked env)
        // or external in which case their env is []
        return make_stub(lam, true);
    } else if (auto nom = def->isa_nom()) {
        assert(!isa_ctype(nom));
        auto new_nom = nom->stub(w, new_type, new_dbg);
        map(nom, new_nom);
        for (size_t i = 0; i < nom->num_ops(); i++)
            if (nom->op(i))
                new_nom->set(i, rewrite(nom->op(i)));
        if (Checker(w).equiv(nom, new_nom))
            return map(nom, nom);
        if (auto restruct = new_nom->restructure())
            return map(nom, restruct);
        return new_nom;
    } else {
        auto new_ops = Array<const Def*>(def->num_ops(), [&](auto i) {
            return rewrite(def->op(i));
        });
        
        auto new_def = def->rebuild(w, new_type, new_ops, new_dbg);

        // We may need to update the mem token after all ops have been rewritten:
        // F (m, a1, ..., (env, f):pct)
        // ~>
        // let [m', env_ptr] = :alloc T m' 
        // let m'' = :store env_ptr env
        // F (m, a1', ..., (env_ptr, f'))
        // ~>
        // let ... 
        // F (m'', a1', ..., (env_ptr, f'))
        for (size_t i = 0; i < new_def->num_ops(); i++)
            if (new_def->op(i)->type() == w.type_mem())
                new_def = new_def->refine(i, lcm_);

        if (new_type == w.type_mem()) { // :store
            lcm_ = new_def;
            lvm_ = def;
        } else if (new_type->isa<Sigma>()) {  // :alloc, :slot, ...
            for (size_t i = 0; i < new_type->num_ops(); i++) {
                if (new_type->op(i) == w.type_mem()) {
                    lcm_ = w.extract(new_def, i);
                    lvm_ = w.extract(def, i);
                    break;
                }
            }
        }

        return map(def, new_def);
    }
}

}