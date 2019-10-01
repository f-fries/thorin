#ifndef THORIN_WORLD_H
#define THORIN_WORLD_H

#include <cassert>
#include <iostream>
#include <functional>
#include <initializer_list>
#include <string>

#include "thorin/def.h"
#include "thorin/util.h"
#include "thorin/util/hash.h"
#include "thorin/config.h"

namespace thorin {

class Scope;
using VisitFn   = std::function<void(const Scope&)>;
using EnterFn   = std::function<bool(const Scope&)>;
using RewriteFn = std::function<const Def*(const Def*)>;

const Def* infer_width(const Def*);

/**
 * The World represents the whole program and manages creation of Thorin nodes (Def%s).
 * In particular, the following things are done by this class:
 *
 *  - @p Def unification: \n
 *      There exists only one unique @p Def.
 *      These @p Def%s are hashed into an internal map for fast access.
 *      The getters just calculate a hash and lookup the @p Def, if it is already present, or create a new one otherwise.
 *      This is corresponds to value numbering.
 *  - constant folding
 *  - canonicalization of expressions
 *  - several local optimizations like <tt>x + 0 -> x</tt>
 *
 *  Use @p cleanup to remove dead and unreachable code.
 *
 *  You can create several worlds.
 *  All worlds are completely independent from each other.
 *
 *  Note that types are also just @p Def%s and will be hashed as well.
 */
class World : public Streamable<World> {
public:
    struct SeaHash {
        static hash_t hash(const Def* def) { return def->hash(); }
        static bool eq(const Def* def1, const Def* def2) { return def1->equal(def2); }
        static const Def* sentinel() { return (const Def*)(1); }
    };

    struct BreakHash {
        static hash_t hash(size_t i) { return i; }
        static bool eq(size_t i1, size_t i2) { return i1 == i2; }
        static size_t sentinel() { return size_t(-1); }
    };

    struct ExternalsHash {
        static hash_t hash(const std::string& s) { return thorin::hash(s.c_str()); }
        static bool eq(const std::string& s1, const std::string& s2) { return s1 == s2; }
        static std::string sentinel() { return std::string(); }
    };

    using Sea         = HashSet<const Def*, SeaHash>;///< This @p HashSet contains Thorin's "sea of nodes".
    using Breakpoints = HashSet<size_t, BreakHash>;
    using Externals   = HashMap<std::string, Def*, ExternalsHash>;

    World(World&&) = delete;
    World& operator=(const World&) = delete;

    explicit World(const std::string& name = {});
    ///  Inherits the @p state_ of the @p other @p World but does @em not perform a copy.
    explicit World(const World& other)
        : World(other.name())
    {
        state_ = other.state_;
    }

    /// @ getters
    //@{
    const std::string& name() const { return name_; }
    const Sea& defs() const { return defs_; }
    std::vector<Lam*> copy_lams() const; // TODO remove this
    //@}
    /// @name manage global identifier - a unique number for each Def
    //@{
    u32 cur_gid() const { return state_.cur_gid; }
    u32 next_gid() { return ++state_.cur_gid; }
    //@}
    /// @name Universe and Kind
    //@{
    const Universe*  universe()   { return cache_.universe_; }
    const KindMulti* kind_multi() { return cache_.kind_multi_; }
    const KindArity* kind_arity() { return cache_.kind_arity_; }
    const KindStar*  kind_star()  { return cache_.kind_star_; }
    //@}
    /// @name Param
    //@{
    const Param* param(const Def* type, Def* nominal, Debug dbg = {}) { return unify<Param>(1, type, nominal, debug(dbg)); }
    //@}
    /// @name Axiom
    //@{
    Axiom* axiom(Def::NormalizeFn normalize, const Def* type, size_t num_ops, tag_t tag, flags_t flags, Debug dbg = {});
    Axiom* axiom(const Def* type, tag_t tag, flags_t flags, Debug dbg = {}) { return axiom(nullptr, type, 0, tag, flags, dbg); }
    //@}
    /// @name Pi
    //@{
    const Pi* pi(const Def* domain, const Def* codomain, Debug dbg = {});
    const Pi* pi(Defs domain, const Def* codomain, Debug dbg = {}) { return pi(sigma(domain), codomain, dbg); }
    Pi* pi(const Def* type, Debug dbg = {}) { return insert<Pi>(2, type, debug(dbg)); } ///< @em nominal Pi.
    //@}
    /// @name Pi: continuation type, i.e., Pi type with codomain Bottom
    //@{
    const Pi* cn() { return cn(sigma()); }
    const Pi* cn(const Def* domain, Debug dbg = {}) { return pi(domain, bot_star(), dbg); }
    const Pi* cn(Defs domains, Debug dbg = {}) { return cn(sigma(domains), dbg); }
    /// Same as cn/pi but adds a mem parameter to each pi
    const Pi* cn_mem(const Def* domain, Debug dbg = {}) { return cn(sigma({ type_mem(), domain }), dbg); }
    const Pi* pi_mem(const Def* domain, const Def* codomain, Debug dbg = {}) { auto d = sigma({type_mem(), domain}); return pi(d, sigma({type_mem(), codomain}), dbg); }
    const Pi* fn_mem(const Def* domain, const Def* codomain, Debug dbg = {}) { return cn({type_mem(), domain, cn_mem(codomain)}, dbg); }
    //@}
    /// @name Lambda: nominal
    //@{
    Lam* lam(const Pi* cn, Lam::CC cc = Lam::CC::C, Lam::Intrinsic intrinsic = Lam::Intrinsic::None, Debug dbg = {}) {
        auto lam = insert<Lam>(2, cn, cc, intrinsic, debug(dbg));
        return lam;
    }
    Lam* lam(const Pi* cn, Debug dbg = {}) { return lam(cn, Lam::CC::C, Lam::Intrinsic::None, dbg); }
    //@}
    /// @name Lambda: structural
    //@{
    const Lam* lam(const Def* domain, const Def* filter, const Def* body, Debug dbg);
    const Lam* lam(const Def* domain, const Def* body, Debug dbg) { return lam(domain, lit_true(), body, dbg); }
    //@}
    /// @name App
    //@{
    const Def* app(const Def* callee, const Def* arg, Debug dbg = {});
    const Def* app(const Def* callee, Defs args, Debug dbg = {}) { return app(callee, tuple(args), dbg); }
    const Def* raw_app(const Def* callee, const Def* arg, Debug dbg = {});                                         /// Same as @p app but does @em not apply @p NormalizeFn.
    const Def* raw_app(const Def* callee, Defs args, Debug dbg = {}) { return raw_app(callee, tuple(args), dbg); } /// Same as @p app but does @em not apply @p NormalizeFn.
    //@}
    /// @name Sigma: structural
    //@{
    const Def* sigma(const Def* type, Defs ops, Debug dbg = {});
    /// a @em structural @p Sigma of type @p star
    const Def* sigma(Defs ops, Debug dbg = {}) { return sigma(kind_star(), ops, dbg); }
    const Sigma* sigma() { return cache_.sigma_; } ///< the unit type within @p kind_star()
    //@}
    /// @name Sigma: nominal
    //@{
    Sigma* sigma(const Def* type, size_t size, Debug dbg = {}) { return insert<Sigma>(size, type, size, debug(dbg)); }
    Sigma* sigma(size_t size, Debug dbg = {}) { return sigma(kind_star(), size, dbg); } ///< a @em nominal @p Sigma of type @p star
    //@}
    /// @name Union: structural
    //@{
    const Def* union_(const Def* type, Defs ops, Debug dbg = {});
    /// a @em structural @p Union of type @p star
    const Def* union_(Defs ops, Debug dbg = {}) { return union_(kind_star(), ops, dbg); }
    //@}
    /// @name Union: nominal
    //@{
    Union* union_(const Def* type, size_t size, Debug dbg = {}) { return insert<Union>(size, type, size, debug(dbg)); }
    Union* union_(size_t size, Debug dbg = {}) { return union_(kind_star(), size, dbg); } ///< a @em nominal @p Sigma of type @p star
    //@}
    /// @name Variadic
    //@{
    const Def* variadic(const Def* arity, const Def* body, Debug dbg = {});
    const Def* variadic(Defs arities, const Def* body, Debug dbg = {});
    const Def* variadic(u64 a, const Def* body, Debug dbg = {}) { return variadic(lit_arity(a), body, dbg); }
    const Def* variadic(ArrayRef<u64> a, const Def* body, Debug dbg = {}) {
        return variadic(Array<const Def*>(a.size(), [&](size_t i) { return lit_arity(a[i], dbg); }), body, dbg);
    }
    const Def* variadic_unsafe(const Def* body, Debug dbg = {}) { return variadic(top_arity(), body, dbg); }
    Variadic* variadic(const Def* type, Debug dbg = {}) { return insert<Variadic>(2, type, debug(dbg)); } ///< @em nominal Variadic.
    //@}
    /// @name Tuple
    //@{
    /// ascribes @p type to this tuple - needed for dependently typed and structural @p Sigma%s
    const Def* tuple(const Def* type, Defs ops, Debug dbg = {});
    const Def* tuple(Defs ops, Debug dbg = {});
    const Def* tuple_str(const char* s, Debug = {});
    const Def* tuple_str(const std::string& s, Debug dbg = {}) { return tuple_str(s.c_str(), dbg); }
    const Tuple* tuple() { return cache_.tuple_; } ///< the unit value of type <tt>[]</tt>
    //@}
    /// @name Variant_
    //@{
    const Def* variant_(const Def* type, const Def* index, const Def* arg, Debug dbg = {});
    /// infers the index, for @em structural unions only
    const Def* variant_(const Def* type, const Def* arg, Debug dbg = {});
    //@}
    /// @name Pack
    //@{
    const Def* pack(const Def* arity, const Def* body, Debug dbg = {});
    const Def* pack(Defs arities, const Def* body, Debug dbg = {});
    const Def* pack(u64 a, const Def* body, Debug dbg = {}) { return pack(lit_arity(a), body, dbg); }
    const Def* pack(ArrayRef<u64> a, const Def* body, Debug dbg = {}) {
        return pack(Array<const Def*>(a.size(), [&](auto i) { return lit_arity(a[i], dbg); }), body, dbg);
    }
    Pack* pack(const Def* type, Debug dbg = {}) { return insert<Pack>(1, type, debug(dbg)); } ///< @em nominal Pack.
    //@}
    /// @name Extract
    //@{
    const Def* extract(const Def* agg, const Def* i, Debug dbg = {});
    const Def* extract(const Def* agg, u64 i, Debug dbg = {}) { return extract(agg, lit_index(agg->type()->arity(), i), dbg); }
    const Def* extract(const Def* agg, u64 a, u64 i, Debug dbg = {}) { return extract(agg, lit_index(a, i), dbg); }
    const Def* extract_unsafe(const Def* agg, const Def* i, Debug dbg = {}) { return extract(agg, op_bitcast(agg->type()->arity(), i, dbg), dbg); }
    const Def* extract_unsafe(const Def* agg, u64 i, Debug dbg = {}) { return extract_unsafe(agg, lit_int(i), dbg); }
    //@}
    /// @name Insert
    //@{
    const Def* insert(const Def* agg, const Def* i, const Def* value, Debug dbg = {});
    const Def* insert(const Def* agg, u64 i, const Def* value, Debug dbg = {}) { return insert(agg, lit_index(agg->type()->arity(), i), value, dbg); }
    const Def* insert_unsafe(const Def* agg, const Def* i, const Def* value, Debug dbg = {}) { return insert(agg, op_bitcast(agg->type()->arity(), i), value, dbg); }
    const Def* insert_unsafe(const Def* agg, u64 i, const Def* value, Debug dbg = {}) { return insert_unsafe(agg, lit_int(i), value, dbg); }
    //@}
    /// @name Match_
    //@{
    const Def* match_(const Def* variant, Defs cases, Debug dbg = {});
    //@}
    /// @name Lit
    //@{
    const Lit* lit(const Def* type, u64 val, Debug dbg = {}) { return unify<Lit>(0, type, val, debug(dbg)); }
    template<class T>
    const Lit* lit(const Def* type, T val, Debug dbg = {}) { return lit(type, thorin::bitcast<u64>(val), dbg); }
    //@}
    /// @name Lit: Arity - note that this is a type
    //@{
    const Lit* lit_arity(u64 a, Debug dbg = {}) { return lit(kind_arity(), a, dbg); }
    const Lit* lit_arity_1() { return cache_.lit_arity_1_; } ///< unit arity 1ₐ
    //@}
    /// @name Lit: Index - the inhabitants of an Arity
    //@{
    const Lit* lit_index(u64 arity, u64 idx, Debug dbg = {}) { return lit_index(lit_arity(arity), idx, dbg); }
    const Lit* lit_index(const Def* arity, u64 index, Debug dbg = {});
    const Lit* lit_index_0_1() { return cache_.lit_index_0_1_; } ///< unit index 0₁ of type unit arity 1ₐ
    //@}
    /// @name Lit: Nat
    //@{
    const Lit* lit_nat(u64 a, Debug dbg = {}) { return lit(type_nat(), a, dbg); }
    //@}
    /// @name Lit: Int
    //@{
    const Lit* lit_int(nat_t width, u64 val, Debug dbg = {}) { return lit(type_int(width), (u64(-1) >> (64_u64 - width)) & val, dbg); }
    template<class I> const Lit* lit_int(I val, Debug dbg = {}) {
        static_assert(std::is_integral<I>());
        return lit(type_int(sizeof(I)*8), val, dbg);
    }
    const Lit* lit_bool(bool val) { return cache_.lit_bool_[size_t(val)]; }
    const Lit* lit_false() { return cache_.lit_bool_[0]; }
    const Lit* lit_true()  { return cache_.lit_bool_[1]; }
    //@}
    /// @name Lit: Int, Real
    //@{
    const Lit* lit_real(nat_t width, r64 val, Debug dbg = {}) {
        switch (width) {
            case 16: assert(r64(r16(val)) == val && "loosing precision"); return lit_real(r16(val), dbg);
            case 32: assert(r64(r32(val)) == val && "loosing precision"); return lit_real(r32(val), dbg);
            case 64: assert(r64(r64(val)) == val && "loosing precision"); return lit_real(r64(val), dbg);
            default: THORIN_UNREACHABLE;
        }
    }
    template<class R> const Lit* lit_real(R val, Debug dbg = {}) {
        static_assert(std::is_floating_point<R>() || std::is_same<R, r16>());
        return lit(type_real(sizeof(R)*8), val, dbg);
    }
    const Lit* lit_real_inf(nat_t w, Debug dbg = {}) {
        switch (w) {
            case 16: return lit_real(std::numeric_limits<r16>::infinity(), dbg);
            case 32: return lit_real(std::numeric_limits<r32>::infinity(), dbg);
            case 64: return lit_real(std::numeric_limits<r64>::infinity(), dbg);
            default: THORIN_UNREACHABLE;
        }
    }
    const Lit* lit_real_minus_inf(nat_t w, Debug dbg = {}) {
        switch (w) {
            case 16: return lit_real(-std::numeric_limits<r16>::infinity(), dbg);
            case 32: return lit_real(-std::numeric_limits<r32>::infinity(), dbg);
            case 64: return lit_real(-std::numeric_limits<r64>::infinity(), dbg);
            default: THORIN_UNREACHABLE;
        }
    }
    //@}
    /// @name Top/Bottom
    //@{
    const Def* bot_top(bool is_top, const Def* type, Debug dbg = {});
    const Def* bot(const Def* type, Debug dbg = {}) { return bot_top(false, type, dbg); }
    const Def* top(const Def* type, Debug dbg = {}) { return bot_top(true,  type, dbg); }
    const Def* bot_star () { return cache_.bot_star_; }
    const Def* top_star () { return cache_.top_star_; }
    const Def* top_arity() { return cache_.top_arity_; } ///< use this guy to encode an unknown arity, e.g., for unsafe arrays
    //@}
    /// @name Variant
    //@{
    const VariantType* variant_type(Defs ops, Debug dbg = {}) { return unify<VariantType>(ops.size(), kind_star(), ops, debug(dbg)); }
    const Def* variant(const VariantType* variant_type, const Def* value, Debug dbg = {}) { return unify<Variant>(1, variant_type, value, debug(dbg)); }
    //@}
    /// @name CPS2DS/DS2CPS
    //@{
    const Def* cps2ds(const Def* cps, Debug dbg = {});
    const Def* ds2cps(const Def* ds, Debug dbg = {});
    //@}
    /// @name misc types
    //@{
    const Nat* type_nat() { return cache_.type_nat_; }
    const Mem* type_mem() { return cache_.type_mem_; }
    const Axiom* type_int()  { return cache_.type_int_; }
    const Axiom* type_real() { return cache_.type_real_; }
    const Axiom* type_ptr()  { return cache_.type_ptr_; }
    const App* type_bool() { return cache_.type_bool_; }
    const App* type_int (nat_t w) { return type_int (lit_nat(w)); }
    const App* type_real(nat_t w) { return type_real(lit_nat(w)); }
    const App* type_int (const Def* w) { return app(type_int(),  w)->as<App>(); }
    const App* type_real(const Def* w) { return app(type_real(), w)->as<App>(); }
    const App* type_ptr(const Def* pointee, nat_t addr_space = AddrSpace::Generic, Debug dbg = {}) { return type_ptr(pointee, lit_nat(addr_space), dbg); }
    const App* type_ptr(const Def* pointee, const Def* addr_space, Debug dbg = {}) { return app(type_ptr(), {pointee, addr_space}, dbg)->as<App>(); }
    //@}
    /// @name IOp
    //@{
    const Axiom* op(IOp o) { return cache_.IOp_[size_t(o)]; }
    const Def* op(IOp o, const Def* a, const Def* b, Debug dbg = {}) { auto w = infer_width(a); return app(app(op(o), w), {a, b}, dbg); }
    const Def* op_IOp_inot(const Def* a, Debug dbg = {}) { auto w = get_width(a->type()); return op(IOp::ixor, lit_int(*w, u64(-1)), a, dbg); }
    //@}
    /// @name WOp
    //@{
    const Axiom* op(WOp o) { return cache_.WOp_[size_t(o)]; }
    const Def* op(WOp o, nat_t wmode, const Def* a, const Def* b, Debug dbg = {}) { return op(o, lit_nat(wmode), a, b, dbg); }
    const Def* op(WOp o, const Def* wmode, const Def* a, const Def* b, Debug dbg = {}) { auto w = infer_width(a); return app(app(op(o), {wmode, w}), {a, b}, dbg); }
    const Def* op_WOp_minus(nat_t wmode, const Def* a, Debug dbg = {}) { auto w = get_width(a->type()); return op(WOp::sub, wmode, lit_int(*w, 0), a, dbg); }
    //@}
    /// @name ZOp
    //@{
    const Axiom* op(ZOp o) { return cache_.ZOp_[size_t(o)]; }
    const Def* op(ZOp o, const Def* mem, const Def* a, const Def* b, Debug dbg = {}) { auto w = infer_width(a); return app(app(op(o), w), {mem, a, b}, dbg); }
    //@}
    /// @name ROp
    //@{
    const Axiom* op(ROp o) { return cache_.ROp_[size_t(o)]; }
    const Def* op(ROp o, const Def* a, const Def* b, Debug dbg = {}) { return op(o, RMode::none, a, b, dbg); }
    const Def* op(ROp o, nat_t rmode, const Def* a, const Def* b, Debug dbg = {}) { return op(o, lit_nat(rmode), a, b, dbg); }
    const Def* op(ROp o, const Def* rmode, const Def* a, const Def* b, Debug dbg = {}) { auto w = infer_width(a); return app(app(op(o), {rmode, w}), {a, b}, dbg); }
    const Def* op_ROp_minus(const Def* rmode, const Def* a, Debug dbg = {}) { auto w = get_width(a->type()); return op(ROp::sub, rmode, lit_real(*w, -0.0), a, dbg); }
    const Def* op_ROp_minus(nat_t rmode, const Def* a, Debug dbg = {}) { return op_ROp_minus(lit_nat(rmode), a, dbg); }
    const Def* op_ROp_minus(const Def* a, Debug dbg = {}) { return op_ROp_minus(RMode::none, a, dbg); }
    //@}
    /// @name ICmp
    //@{
    const Axiom* op(ICmp o) { return cache_.ICmp_[size_t(o)]; }
    const Def* op(ICmp o, const Def* a, const Def* b, Debug dbg = {}) { auto w = infer_width(a); return app(app(op(o), w), {a, b}, dbg); }
    //@}
    /// @name RCmp
    //@{
    const Axiom* op(RCmp o) { return cache_.RCmp_[size_t(o)]; }
    const Def* op(RCmp o, const Def* a, const Def* b, Debug dbg = {}) { return op(o, RMode::none, a, b, dbg); }
    const Def* op(RCmp o, nat_t rmode, const Def* a, const Def* b, Debug dbg = {}) { return op(o, lit_nat(rmode), a, b, dbg); }
    const Def* op(RCmp o, const Def* rmode, const Def* a, const Def* b, Debug dbg = {}) { auto w = infer_width(a); return app(app(op(o), {rmode, w}), {a, b}, dbg); }
    //@}
    /// @name Conv
    //@{
    const Axiom* op(Conv o) { return cache_.Conv_[size_t(o)]; }
    const Def* op(Conv o, const Def* dst_type, const Def* src, Debug dbg = {}) {
        auto d = dst_type   ->as<App>()->arg();
        auto s = src->type()->as<App>()->arg();
        return app(app(op(o), {d, s}), src, dbg);
    }
    //@}
    /// @name memory-related operations
    //@{
    const Def* op_load()  { return cache_.op_load_;  }
    const Def* op_store() { return cache_.op_store_; }
    const Def* op_slot()  { return cache_.op_slot_;  }
    const Def* op_alloc() { return cache_.op_alloc_; }
    const Def* op_load (const Def* mem, const Def* ptr, Debug dbg = {})                 { auto [T, a] = as<Tag::Ptr>(ptr->type())->args<2>(); return app(app(op_load (), {T, a}), {mem, ptr},      dbg); }
    const Def* op_store(const Def* mem, const Def* ptr, const Def* val, Debug dbg = {}) { auto [T, a] = as<Tag::Ptr>(ptr->type())->args<2>(); return app(app(op_store(), {T, a}), {mem, ptr, val}, dbg); }
    const Def* op_alloc(const Def* type, const Def* mem, Debug dbg = {}) { return app(app(op_alloc(), {type, lit_nat(0)}), mem, dbg); }
    const Def* op_slot (const Def* type, const Def* mem, Debug dbg = {}) { return app(app(op_slot (), {type, lit_nat(0)}), mem, dbg); }
    const Def* global(const Def* id, const Def* init, bool is_mutable = true, Debug dbg = {});
    const Def* global(const Def* init, bool is_mutable = true, Debug dbg = {}) { return global(lit_nat(state_.cur_gid), init, is_mutable, debug(dbg)); }
    const Def* global_immutable_string(const std::string& str, Debug dbg = {});
    //@}
    /// @name PE - partial evaluation related operations
    //@{
    const Def* op(PE o) { return cache_.PE_[size_t(o)]; }
    const Def* op(PE o, const Def* def, Debug dbg = {}) { return app(app(op(o), def->type()), def, debug(dbg)); }
    //@}
    /// @name Analyze - used internally for Pass%es
    //@{
    const Analyze* analyze(const Def* type, Defs ops, fields_t index, Debug dbg = {}) { return unify<Analyze>(ops.size(), type, ops, index, debug(dbg)); }
    //@}
    /// @name misc operations
    //@{
    const Axiom* op_bitcast() const { return cache_.op_bitcast_; }
    const Axiom* op_lea()     const { return cache_.op_lea_;     }
    const Axiom* op_select()  const { return cache_.op_select_;  }
    const Axiom* op_sizeof()  const { return cache_.op_sizeof_;  }
    const Def* op_bitcast(const Def* dst_type, const Def* src, Debug dbg = {}) { return app(app(op_bitcast(), {dst_type, src->type()}), src, dbg); }
    const Def* op_lea(const Def* ptr, const Def* index, Debug dbg = {});
    const Def* op_lea_unsafe(const Def* ptr, const Def* i, Debug dbg = {}) { return op_lea(ptr, op_bitcast(as<Tag::Ptr>(ptr->type())->arg(0)->arity(), i), dbg); }
    const Def* op_lea_unsafe(const Def* ptr, u64 i, Debug dbg = {}) { return op_lea_unsafe(ptr, lit_int(i), dbg); }
    const Def* op_select(const Def* cond, const Def* t, const Def* f, Debug dbg = {}) { return app(app(cache_.op_select_, t->type()), {cond, t, f}, dbg); }
    const Def* op_sizeof(const Def* type, Debug dbg = {}) { return app(op_sizeof(), type, dbg); }
    Lam* match(const Def* type, size_t num_patterns);
    Axiom* op_end() const { return cache_.op_end_; }
    //@}
    /// @name helpers for optional/variant arguments
    //@{
    const Def* name2def(Name n) {
        if (auto s = std::get_if<const char*>(&n)) return tuple_str(*s);
        if (auto s = std::get_if<std::string>(&n)) return tuple_str(s->c_str());
        return std::get<const Def*>(n);
    }
    const Def* debug(Debug dbg) {
        if (auto d = std::get_if<0>(&*dbg)) {
            auto n = name2def(std::get<0>(*d));
            auto f = name2def(std::get<1>(*d));
            auto l = tuple({
                lit_nat(std::get<2>(*d)),
                lit_nat(std::get<3>(*d)),
                lit_nat(std::get<4>(*d)),
                lit_nat(std::get<5>(*d))
            });
            auto m = std::get<6>(*d);
            return tuple({n, f, l, m ? m : bot(bot_star()) });
        }
        return std::get<const Def*>(*dbg);
    }
    //@}
    /// @name modify state
    //@{
    void mark_pe_done(bool flag = true) { state_.pe_done = flag; }
    bool is_pe_done() const { return state_.pe_done; }
    void do_tuple2pack(bool flag = true) { state_.tuple2pack = flag; }
    bool tuple2pack() const { return state_.tuple2pack; }
    //@}
    /// @name manage externals
    //@{
    bool empty() { return externals_.empty(); }
    const Externals& externals() const { return externals_; }
    void make_external(Def* def) { externals_.emplace(def->name(), def); }
    void make_internal(Def* def) { externals_.erase(def->name()); }
    bool is_external(const Def* def) { return externals_.contains(def->name()); }
    Def* lookup(const std::string& name) { return externals_.lookup(name).value_or(nullptr); }
    //@}
    /// @name visit and rewrite
    //@{
    /**
     * Transitively visits all @em reachable Scope%s in this @p World that do not have free variables.
     * We call these Scope%s @em top-level Scope%s.
     * Select with @p elide_empty whether you want to visit trivial @p Scope%s of @em nominals without body.
     */
    template<bool elide_empty = true> void visit(VisitFn) const;
    /**
     * Rewrites the whole world by @p visit%ing each @p Def with all @em top-level @p Scope%s.
     * Every time, we enter a new scope @p enter_fn will be invoked.
     * Return @c true, if you are interested in this @p Scope.
     * Return @c false, if you want to skip this @p Scope.
     * For each @p Def in the current @p Scope, @p rewrite_fn will be invoked.
     */
    void rewrite(const std::string& info, EnterFn enter_fn, RewriteFn rewrite_fn);
    //@}
#if THORIN_ENABLE_CHECKS
    /// @name debugging features
    //@{
    void breakpoint(size_t number) { state_.breakpoints.insert(number); }
    const Breakpoints& breakpoints() const { return state_.breakpoints; }
    bool track_history() const { return state_.track_history; }
    void enable_history(bool flag = true) { state_.track_history = flag; }
    const Def* lookup_by_gid(u32 gid);
    //@}
#endif
    Stream& stream(Stream&) const;

    friend void swap(World& w1, World& w2) {
        using std::swap;
        swap(w1.name_,      w2.name_);
        swap(w1.externals_, w2.externals_);
        swap(w1.defs_,      w2.defs_);
        swap(w1.arena_, w2.arena_);
        swap(w1.state_, w2.state_);
        swap(w1.cache_, w2.cache_);
        swap(w1.cache_.universe_->world_, w2.cache_.universe_->world_);
        assert(&w1.universe()->world() == &w1);
        assert(&w2.universe()->world() == &w2);
    }

private:
    /// @name put into sea of nodes
    //@{
    template<class T, class... Args>
    const T* unify(size_t num_ops, Args&&... args) {
        auto def = arena_.allocate<T>(num_ops, args...);
#ifndef NDEBUG
        if (state_.breakpoints.contains(def->gid())) THORIN_BREAK;
#endif
        assert(!def->isa_nominal());
        auto [i, success] = defs_.emplace(def);
        if (success) {
            def->finalize();
            return def;
        }

        arena_.deallocate<T>(def);
        return static_cast<const T*>(*i);
    }

    template<class T, class... Args>
    T* insert(size_t num_ops, Args&&... args) {
        auto def = arena_.allocate<T>(num_ops, args...);
#ifndef NDEBUG
        if (state_.breakpoints.contains(def->gid())) THORIN_BREAK;
#endif
        auto p = defs_.emplace(def);
        assert_unused(p.second);
        return def;
    }

    static inline size_t align(size_t n) { return (n + (sizeof(void*) - 1)) & ~(sizeof(void*)-1); }

    template<class T> static inline size_t num_bytes_of(size_t num_ops) {
        size_t result = sizeof(Def) + sizeof(const Def*)*num_ops;
        return align(result);
    }
    //@}

    class Arena {
    public:
        Arena()
            : root_zone_(new Zone) // don't use 'new Zone()' - we keep the allocated Zone uninitialized
            , cur_zone_(root_zone_.get())
        {}

        struct Zone {
            static const size_t Size = 1024 * 1024 - sizeof(std::unique_ptr<int>); // 1MB - sizeof(next)
            char buffer[Size];
            std::unique_ptr<Zone> next;
        };

#ifndef NDEBUG
        struct Lock {
            Lock() { assert((guard_ = !guard_) && "you are not allowed to recursively invoke allocate"); }
            ~Lock() { guard_ = !guard_; }
            static bool guard_;
        };
#else
        struct Lock { ~Lock() {} };
#endif
        template<class T, class... Args>
        T* allocate(size_t num_ops, Args&&... args) {
            static_assert(sizeof(Def) == sizeof(T), "you are not allowed to introduce any additional data in subclasses of Def");
            Lock lock;
            size_t num_bytes = num_bytes_of<T>(num_ops);
            num_bytes = align(num_bytes);
            assert(num_bytes < Zone::Size);

            if (buffer_index_ + num_bytes >= Zone::Size) {
                auto zone = new Zone;
                cur_zone_->next.reset(zone);
                cur_zone_ = zone;
                buffer_index_ = 0;
            }

            auto result = new (cur_zone_->buffer + buffer_index_) T(args...);
            assert(result->num_ops() == num_ops);
            buffer_index_ += num_bytes;
            assert(buffer_index_ % alignof(T) == 0);

            return result;
        }

        template<class T>
        void deallocate(const T* def) {
            size_t num_bytes = num_bytes_of<T>(def->num_ops());
            num_bytes = align(num_bytes);
            def->~T();
            if (ptrdiff_t(buffer_index_ - num_bytes) > 0) // don't care otherwise
                buffer_index_-= num_bytes;
            assert(buffer_index_ % alignof(T) == 0);
        }

    private:
        std::unique_ptr<Zone> root_zone_;
        Zone* cur_zone_;
        size_t buffer_index_ = 0;
    } arena_;

    struct State {
        u32 cur_gid = 0;
        bool pe_done = false;
        bool tuple2pack = true;
#if THORIN_ENABLE_CHECKS
        bool track_history = false;
        Breakpoints breakpoints;
#endif
    } state_;

    struct Cache {
        Universe* universe_;
        const KindMulti* kind_multi_;
        const KindArity* kind_arity_;
        const KindStar*  kind_star_;
        const Bot* bot_star_;
        const Top* top_star_;
        const Top* top_arity_;
        const Sigma* sigma_;
        const Tuple* tuple_;
        const Nat* type_nat_;
        const Mem* type_mem_;
        std::array<const Lit*, 2> lit_bool_;
        const Lit* lit_arity_1_;
        const Lit* lit_index_0_1_;
        std::array<Axiom*, Num<IOp>>    IOp_;
        std::array<Axiom*, Num<WOp>>    WOp_;
        std::array<Axiom*, Num<ZOp>>    ZOp_;
        std::array<Axiom*, Num<ROp>>    ROp_;
        std::array<Axiom*, Num<ICmp>>   ICmp_;
        std::array<Axiom*, Num<RCmp>>   RCmp_;
        std::array<Axiom*, Num<Conv>>   Conv_;
        std::array<Axiom*, Num<PE>>     PE_;
        Axiom* op_end_;
        Axiom* type_int_;
        Axiom* type_real_;
        Axiom* type_ptr_;
        const App* type_bool_;
        Axiom* op_bitcast_;
        Axiom* op_lea_;
        Axiom* op_select_;
        Axiom* op_sizeof_;
        Axiom* op_alloc_;
        Axiom* op_slot_;
        Axiom* op_load_;
        Axiom* op_store_;
    } cache_;

    std::string name_;
    Externals externals_;
    Sea defs_;

    friend class Cleaner;
    friend void Def::replace(Tracker) const;
};

}

#endif
