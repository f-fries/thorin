#include "thorin/def.h"
#include "thorin/fold.h"
#include "thorin/util.h"
#include "thorin/world.h"

namespace thorin {

using namespace thorin::fold;

//------------------------------------------------------------------------------

static bool is_allset(const Def* def) {
    if (auto lit = isa_lit<u64>(def)) {
        if (auto width = isa_lit<u64>(as<Tag::Int>(def->type())))
            return (*lit >> (64_u64 - *width) == u64(-1) >> (64_u64 - *width));
    }
    return false;
}

static bool is_not(const Def* def) {
    if (auto arg = isa<Tag::IOp, IOp::ixor>(def)) {
        auto [x, y] = split<2>(arg);
        if (is_allset(x)) return true;
    }
    return false;
}

//------------------------------------------------------------------------------

const Def* normalize_select(const Def* callee, const Def* arg, const Def* dbg) {
    auto& world = callee->world();
    auto [cond, a, b] = split<3>(arg);

    if (cond->isa<Bot>()) return world.bot(a->type(), dbg);
    if (auto lit = cond->isa<Lit>()) return lit->get<bool>() ? a : b;
    if (a == b) return a;
    if (is_not(cond)) std::swap(a, b);

    return world.raw_app(callee, {a, b}, dbg);
}

const Def* normalize_sizeof(const Def* callee, const Def* type, const Def* dbg) {
    auto& world = callee->world();

    const Def* width = nullptr;
    if (false) {}
    else if (auto arg = isa<Tag::Int >(type)) width = arg;
    else if (auto arg = isa<Tag::Real>(type)) width = arg;

    if (auto lit = isa_lit<u64>(width)) return world.lit_nat(*lit / 8, dbg);
    return nullptr;
}

//------------------------------------------------------------------------------

template<template<int> class F>
static const Def* fold_i(const Def* callee, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto t = a->type();
        auto w = as_lit<u64>(t->as<App>()->arg());
        Res res;
        switch (w) {
            case  1: res = F< 1>::run(la->get(), lb->get()); break;
            case  8: res = F< 8>::run(la->get(), lb->get()); break;
            case 16: res = F<16>::run(la->get(), lb->get()); break;
            case 32: res = F<32>::run(la->get(), lb->get()); break;
            case 64: res = F<64>::run(la->get(), lb->get()); break;
            default: THORIN_UNREACHABLE;
        }

        if (res) return world.lit(t, *res, dbg);
        return world.bot(t, dbg);
    }

    return nullptr;
}

template<IOp op>
const Def* normalize_IOp(const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_i<FoldIOp<op>::template Fold>(callee, a, b, dbg)) return result;

    return nullptr;
}

//------------------------------------------------------------------------------

template<template<int, bool, bool> class F>
static const Def* fold_w(const Def* callee, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto t = a->type();
        auto [ff, ww] = split<2>(callee->as<App>()->arg());
        auto f = as_lit<u64>(ff);
        auto w = as_lit<u64>(ww);
        Res res;
        switch (f) {
            case u64(WMode::none):
                switch (w) {
                    case  8: res = F< 8, false, false>::run(la->get(), lb->get()); break;
                    case 16: res = F<16, false, false>::run(la->get(), lb->get()); break;
                    case 32: res = F<32, false, false>::run(la->get(), lb->get()); break;
                    case 64: res = F<64, false, false>::run(la->get(), lb->get()); break;
                    default: THORIN_UNREACHABLE;
                }
                break;
            case u64(WMode::nsw):
                switch (w) {
                    case  8: res = F< 8,  true, false>::run(la->get(), lb->get()); break;
                    case 16: res = F<16,  true, false>::run(la->get(), lb->get()); break;
                    case 32: res = F<32,  true, false>::run(la->get(), lb->get()); break;
                    case 64: res = F<64,  true, false>::run(la->get(), lb->get()); break;
                    default: THORIN_UNREACHABLE;
                }
                break;
            case u64(WMode::nuw):
                switch (w) {
                    case  8: res = F< 8, false,  true>::run(la->get(), lb->get()); break;
                    case 16: res = F<16, false,  true>::run(la->get(), lb->get()); break;
                    case 32: res = F<32, false,  true>::run(la->get(), lb->get()); break;
                    case 64: res = F<64, false,  true>::run(la->get(), lb->get()); break;
                    default: THORIN_UNREACHABLE;
                }
                break;
            case u64(WMode::nsw | WMode::nuw):
                switch (w) {
                    case  8: res = F< 8,  true,  true>::run(la->get(), lb->get()); break;
                    case 16: res = F<16,  true,  true>::run(la->get(), lb->get()); break;
                    case 32: res = F<32,  true,  true>::run(la->get(), lb->get()); break;
                    case 64: res = F<64,  true,  true>::run(la->get(), lb->get()); break;
                    default: THORIN_UNREACHABLE;
                }
                break;
        }

        if (res) return world.lit(t, *res, dbg);
        return world.bot(t, dbg);
    }

    return nullptr;
}

template<WOp op>
const Def* normalize_WOp(const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_w<FoldWOp<op>::template Fold>(callee, a, b, dbg)) return result;

    return nullptr;
}

//------------------------------------------------------------------------------

template<template<int> class F>
static const Def* fold_ZOp(const Def* callee, const Def* m, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto t = a->type();
        auto w = as_lit<u64>(t->as<App>()->arg());
        Res res;
        switch (w) {
            case  8: res = F< 8>::run(la->get(), lb->get()); break;
            case 16: res = F<16>::run(la->get(), lb->get()); break;
            case 32: res = F<32>::run(la->get(), lb->get()); break;
            case 64: res = F<64>::run(la->get(), lb->get()); break;
            default: THORIN_UNREACHABLE;
        }

        if (res) return world.tuple({m, world.lit(t, *res, dbg)}, dbg);
        return world.tuple({m, world.bot(t, dbg)}, dbg);
    }

    return nullptr;
}

template<ZOp op>
const Def* normalize_ZOp(const Def* callee, const Def* arg, const Def* dbg) {
    auto [m, a, b] = split<3>(arg);
    if (auto result = fold_ZOp<FoldZOp<op>::template Fold>(callee, m, a, b, dbg)) return result;

    return nullptr;
}

//------------------------------------------------------------------------------

template<template<int> class F>
static const Def* fold_r(const Def* callee, const Def* a, const Def* b, const Def* dbg) {
    auto& world = callee->world();
    auto la = a->isa<Lit>(), lb = b->isa<Lit>();
    if (la && lb) {
        auto t = a->type();
        auto w = as_lit<u64>(t->as<App>()->arg());
        Res res;
        switch (w) {
            case 16: res = F<16>::run(la->get(), lb->get()); break;
            case 32: res = F<32>::run(la->get(), lb->get()); break;
            case 64: res = F<64>::run(la->get(), lb->get()); break;
            default: THORIN_UNREACHABLE;
        }

        if (res) return world.lit(t, *res, dbg);
        return world.bot(t, dbg);
    }

    return nullptr;
}

template<ROp op>
const Def* normalize_ROp(const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_r<FoldROp<op>::template Fold>(callee, a, b, dbg)) return result;

    return nullptr;
}

//------------------------------------------------------------------------------

template<ICmp op>
const Def* normalize_ICmp(const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_i<FoldICmp<op>::template Fold>(callee, a, b, dbg)) return result;

    return nullptr;
}

//------------------------------------------------------------------------------

template<RCmp op>
const Def* normalize_RCmp(const Def* callee, const Def* arg, const Def* dbg) {
    auto [a, b] = split<2>(arg);
    if (auto result = fold_r<FoldRCmp<op>::template Fold>(callee, a, b, dbg)) return result;

    return nullptr;
}

//------------------------------------------------------------------------------

template<Cast op>
const Def* normalize_Cast(const Def*, const Def*, const Def*) {
    return nullptr;
}

//------------------------------------------------------------------------------

// instantiate templates
#define CODE(T, o) template const Def* normalize_ ## T<T::o>(const Def*, const Def*, const Def*);
THORIN_W_OP (CODE)
THORIN_Z_OP (CODE)
THORIN_I_OP (CODE)
THORIN_R_OP (CODE)
THORIN_I_CMP(CODE)
THORIN_R_CMP(CODE)
THORIN_CAST(CODE)
#undef CODE

}