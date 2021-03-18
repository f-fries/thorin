#include "thorin/be/spirv/spirv.h"
#include "thorin/analyses/scope.h"
#include "thorin/analyses/schedule.h"

#include "thorin/transform/cleanup_world.h"

#include <iostream>

namespace thorin::spirv {

CodeGen::CodeGen(thorin::World& world, Cont2Config&, bool debug)
    : thorin::CodeGen(world, debug)
{}

void CodeGen::emit(std::ostream& out) {
    builder::SpvFileBuilder builder;
    builder_ = &builder;
    builder_->capability(spv::Capability::CapabilityShader);
    builder_->capability(spv::Capability::CapabilityLinkage);

    structure_loops();
    structure_flow();
    // cleanup_world(world());

    Scope::for_each(world(), [&](const Scope& scope) { emit(scope); });

    builder_->finish(out);
    builder_ = nullptr;
}

SpvType CodeGen::convert(const Type* type) {
    if (auto spv_type = types_.lookup(type)) return *spv_type;

    assert(!type->isa<MemType>());
    SpvType spv_type;
    switch (type->tag()) {
        // Boolean types are typically packed intelligently when declaring in local variables, however with vanilla Vulkan 1.0 they can only be represented via 32-bit integers
        // Using extensions, we could use 16 or 8-bit ints instead
        // We can also pack them inside structures using bit-twiddling tricks, if the need arises
        case PrimType_bool:                                                             spv_type.id = builder_->declare_bool_type(); spv_type.size = 4; spv_type.alignment = 4; break;
        case PrimType_ps8:  case PrimType_qs8:  case PrimType_pu8:  case PrimType_qu8:  assert(false && "TODO: look into capabilities to enable this");
        case PrimType_ps16: case PrimType_qs16: case PrimType_pu16: case PrimType_qu16: assert(false && "TODO: look into capabilities to enable this");
        case PrimType_ps32: case PrimType_qs32:                                         spv_type.id = builder_->declare_int_type(32, true ); spv_type.size = 4; spv_type.alignment = 4; break;
        case PrimType_pu32: case PrimType_qu32:                                         spv_type.id = builder_->declare_int_type(32, false); spv_type.size = 4; spv_type.alignment = 4; break;
        case PrimType_ps64: case PrimType_qs64: case PrimType_pu64: case PrimType_qu64: assert(false && "TODO: look into capabilities to enable this");
        case PrimType_pf16: case PrimType_qf16:                                         assert(false && "TODO: look into capabilities to enable this");
        case PrimType_pf32: case PrimType_qf32:                                         spv_type.id = builder_->declare_float_type(32); spv_type.size = 4; spv_type.alignment = 4; break;
        case PrimType_pf64: case PrimType_qf64:                                         assert(false && "TODO: look into capabilities to enable this");
        case Node_PtrType: {
            auto ptr = type->as<PtrType>();
            assert(false && "TODO");
            break;
        }
        case Node_IndefiniteArrayType: {
            assert(false && "TODO");
            auto array = type->as<IndefiniteArrayType>();
            //return types_[type] = spv_type;
        }
        case Node_DefiniteArrayType: {
            auto array = type->as<DefiniteArrayType>();
            SpvType element = convert(array->elem_type());
            SpvId size = builder_->constant(convert(world().type_pu32()).id, { (uint32_t) array->dim() });
            spv_type.id = builder_->declare_array_type(element.id, size);
            spv_type.size = element.size * array->dim();
            spv_type.alignment = element.alignment;
            break;
        }

        case Node_ClosureType:
        case Node_FnType: {
            // extract "return" type, collect all other types
            auto fn = type->as<FnType>();
            std::unique_ptr<SpvType> ret;
            std::vector<SpvId> ops;
            for (auto op : fn->ops()) {
                if (op->isa<MemType>() || op == world().unit()) continue;
                auto fn = op->isa<FnType>();
                if (fn && !op->isa<ClosureType>()) {
                    assert(!ret && "only one 'return' supported");
                    std::vector<SpvType> ret_types;
                    for (auto fn_op : fn->ops()) {
                        if (fn_op->isa<MemType>() || fn_op == world().unit()) continue;
                        ret_types.push_back(convert(fn_op));
                    }
                    if (ret_types.empty())          ret = std::make_unique<SpvType>( SpvType { { builder_->void_type }, 0, 1} );
                    else if (ret_types.size() == 1) ret = std::make_unique<SpvType>(ret_types.back());
                    else                            assert(false && "Didn't we refactor this out yet by making functions single-argument ?");
                } else
                    ops.push_back(convert(op).id);
            }
            assert(ret);

            if (type->tag() == Node_FnType) {
                return types_[type] = { builder_->declare_fn_type(ops, ret->id), 0, 0 };
            }

            assert(false && "TODO: handle closure mess");
            break;
        }

        case Node_StructType: {
            std::vector<SpvId> types;
            for (auto elem : type->as<StructType>()->ops()) {
                auto member_type = convert(elem);
                types.push_back(member_type.id);
                spv_type.size += member_type.size;

                // TODO handle alignment for real
                assert(member_type.alignment == 4 || (member_type.size == 0 && member_type.alignment == 1));
                spv_type.alignment = 4;
            }
            if (spv_type.size == 0)
                spv_type.alignment = 1;
            spv_type.id = builder_->declare_struct_type(types);
            builder_->name(spv_type.id, type->to_string());
            break;
        }

        case Node_TupleType: {
            std::vector<SpvId> types;
            for (auto elem : type->as<TupleType>()->ops()){
                auto member_type = convert(elem);
                types.push_back(member_type.id);
                spv_type.size += member_type.size;

                // TODO handle alignment for real
                assert(member_type.alignment == 4 || (member_type.size == 0 && member_type.alignment == 1));
                spv_type.alignment = 4;
            }
            if (spv_type.size == 0)
                spv_type.alignment = 1;
            spv_type.id = builder_->declare_struct_type(types);
            builder_->name(spv_type.id, type->to_string());
            break;
        }

        case Node_VariantType: {
            std::vector<SpvId> types;
            types.push_back(convert(world().type_pu32()).id);
            for (auto elem : type->as<VariantType>()->ops()){
                auto member_type = convert(elem);
                spv_type.size = std::max(spv_type.size, member_type.size);

                // TODO handle alignment for real
                assert(member_type.alignment == 4 || (member_type.size == 0 && member_type.alignment == 1));
                spv_type.alignment = 4;
            }
            types.push_back(convert(world().definite_array_type(world().type_pu32(), (spv_type.size + 3) / 4)).id);
            if (spv_type.size == 0)
                spv_type.alignment = 1;
            spv_type.id = builder_->declare_struct_type(types);
            builder_->name(spv_type.id, type->to_string());
            break;
        }

        default:
            THORIN_UNREACHABLE;
    }

    return types_[type] = spv_type;
}

void CodeGen::emit(const thorin::Scope& scope) {
    entry_ = scope.entry();
    assert(entry_->is_returning());

    FnBuilder fn;
    fn.fn_type = convert(entry_->type()).id;
    fn.fn_ret_type = get_codom_type(entry_);

    current_fn_ = &fn;

    auto conts = schedule(scope);

    fn.bbs.reserve(conts.size());
    std::vector<BasicBlockBuilder> bbs;
    bbs.reserve(conts.size());

    for (auto cont : conts) {
        if (cont->intrinsic() == Intrinsic::EndScope) continue;

        BasicBlockBuilder* bb = &bbs.emplace_back(BasicBlockBuilder(*builder_));
        fn.bbs.emplace_back(bb);
        auto [i, b] = fn.bbs_map.emplace(cont, bb);
        assert(b);

        bb->label = builder_->generate_fresh_id();

        if (debug())
            builder_->name(bb->label, cont->name().c_str());
        fn.labels.emplace(cont, bb->label);

        if (entry_ == cont) {
            for (auto param : entry_->params()) {
                if (is_mem(param) || is_unit(param)) {
                    // Nothing
                } else if (param->order() == 0) {
                    auto param_t = convert(param->type());
                    fn.header.op(spv::Op::OpFunctionParameter, 3);
                    auto id = builder_->generate_fresh_id();
                    fn.header.ref_id(param_t.id);
                    fn.header.ref_id(id);
                    fn.params[param] = id;
                }
            }
        } else {
            for (auto param : cont->params()) {
                if (is_mem(param) || is_unit(param)) {
                    // Nothing
                } else {
                    // OpPhi requires the full list of predecessors (values, labels)
                    // We don't have that yet! But we will need the Phi node identifier to build the basic blocks ...
                    // To solve this we generate an id for the phi node now, but defer emission of it to a later stage
                    bb->phis[param] = { convert(param->type()).id, builder_->generate_fresh_id(), {} };
                }
            }
        }
    }

    Scheduler new_scheduler(scope);
    swap(scheduler_, new_scheduler);

    for (auto cont : conts) {
        if (cont->intrinsic() == Intrinsic::EndScope) continue;
        assert(cont == entry_ || cont->is_basicblock());
        emit_epilogue(cont, fn.bbs_map[cont]);
    }
    
    builder_->define_function(fn);
}

SpvId CodeGen::get_codom_type(const Continuation* fn) {
    auto ret_cont_type = fn->ret_param()->type();
    std::vector<SpvId> types;
    for (auto& op : ret_cont_type->ops()) {
        if (op->isa<MemType>() || is_type_unit(op))
            continue;
        assert(op->order() == 0);
        types.push_back(convert(op).id);
    }
    if (types.empty())
        return builder_->void_type;
    if (types.size() == 1)
        return types[0];
    return builder_->declare_struct_type(types);
}

void CodeGen::emit_epilogue(Continuation* continuation, BasicBlockBuilder* bb) {
    if (continuation->callee() == entry_->ret_param()) {
        std::vector<SpvId> values;

        for (auto arg : continuation->args()) {
            assert(arg->order() == 0);
            if (is_mem(arg) || is_unit(arg))
                continue;
            auto val = emit(arg, bb);
            values.emplace_back(val);
        }

        switch (values.size()) {
            case 0:  bb->return_void();      break;
            case 1:  bb->return_value(values[0]); break;
            default: bb->return_value(bb->composite(current_fn_->fn_ret_type, values));
        }
    }
    else if (continuation->callee() == world().branch()) {
        auto cond = emit(continuation->arg(0), bb);
        bb->args[continuation->arg(0)] = cond;
        auto tbb = *current_fn_->labels[continuation->arg(1)->as_continuation()];
        auto fbb = *current_fn_->labels[continuation->arg(2)->as_continuation()];
        bb->branch_conditional(cond, tbb, fbb);
    } /*else if (continuation->callee()->isa<Continuation>() &&
               continuation->callee()->as<Continuation>()->intrinsic() == Intrinsic::Match) {
        auto val = emit(continuation->arg(0));
        auto otherwise_bb = cont2bb(continuation->arg(1)->as_continuation());
        auto match = irbuilder.CreateSwitch(val, otherwise_bb, continuation->num_args() - 2);
        for (size_t i = 2; i < continuation->num_args(); i++) {
            auto arg = continuation->arg(i)->as<Tuple>();
            auto case_const = llvm::cast<llvm::ConstantInt>(emit(arg->op(0)));
            auto case_bb    = cont2bb(arg->op(1)->as_continuation());
            match->addCase(case_const, case_bb);
        }
    } else if (continuation->callee()->isa<Bottom>()) {
        irbuilder.CreateUnreachable();
    } */
    else if (auto callee = continuation->callee()->isa_continuation(); callee && callee->is_basicblock()) { // ordinary jump
        int index = -1;
        for (auto& arg : continuation->args()) {
            index++;
            if (is_mem(arg) || is_unit(arg)) continue;
            bb->args[arg] = emit(arg, bb);
            auto* param = callee->param(index);
            auto& phi = current_fn_->bbs_map[callee]->phis[param];
            phi.preds.emplace_back(*bb->args[arg], *current_fn_->labels[continuation]);
        }
        bb->branch(*current_fn_->labels[callee]);
    } /*else if (auto callee = continuation->callee()->isa_continuation(); callee && callee->is_intrinsic()) {
        auto ret_continuation = emit_intrinsic(irbuilder, continuation);
        irbuilder.CreateBr(cont2bb(ret_continuation));
    } else { // function/closure call
        // put all first-order args into an array
        std::vector<llvm::Value*> args;
        const Def* ret_arg = nullptr;
        for (auto arg : continuation->args()) {
            if (arg->order() == 0) {
                if (auto val = emit_unsafe(arg))
                    args.push_back(val);
            } else {
                assert(!ret_arg);
                ret_arg = arg;
            }
        }

        llvm::CallInst* call = nullptr;
        if (auto callee = continuation->callee()->isa_continuation()) {
            call = irbuilder.CreateCall(emit(callee), args);
            if (callee->is_exported())
                call->setCallingConv(kernel_calling_convention_);
            else if (callee->cc() == CC::Device)
                call->setCallingConv(device_calling_convention_);
            else
                call->setCallingConv(function_calling_convention_);
        } else {
            // must be a closure
            auto closure = emit(callee);
            args.push_back(irbuilder.CreateExtractValue(closure, 1));
            call = irbuilder.CreateCall(irbuilder.CreateExtractValue(closure, 0), args);
        }

        // must be call + continuation --- call + return has been removed by codegen_prepare
        auto succ = ret_arg->as_continuation();

        size_t n = 0;
        const Param* last_param = nullptr;
        for (auto param : succ->params()) {
            if (is_mem(param) || is_unit(param))
                continue;
            last_param = param;
            n++;
        }

        if (n == 0) {
            irbuilder.CreateBr(cont2bb(succ));
        } else if (n == 1) {
            irbuilder.CreateBr(cont2bb(succ));
            emit_phi_arg(irbuilder, last_param, call);
        } else {
            Array<llvm::Value*> extracts(n);
            for (size_t i = 0, j = 0; i != succ->num_params(); ++i) {
                auto param = succ->param(i);
                if (is_mem(param) || is_unit(param))
                    continue;
                extracts[j] = irbuilder.CreateExtractValue(call, unsigned(j));
                j++;
            }

            irbuilder.CreateBr(cont2bb(succ));

            for (size_t i = 0, j = 0; i != succ->num_params(); ++i) {
                auto param = succ->param(i);
                if (is_mem(param) || is_unit(param))
                    continue;
                emit_phi_arg(irbuilder, param, extracts[j]);
                j++;
            }
        }
    }*/
    else {
        assert(false && "epilogue not implemented for this");
    }
}

SpvId CodeGen::emit(const Def* def, BasicBlockBuilder* bb) {
    if (auto bin = def->isa<BinOp>()) {
        SpvId lhs = emit(bin->lhs(), bb);
        SpvId rhs = emit(bin->rhs(), bb);
        SpvType result_types = convert(def->type());
        SpvId result_type = result_types.id;

        if (auto cmp = bin->isa<Cmp>()) {
            auto type = cmp->lhs()->type();
            if (is_type_s(type)) {
                switch (cmp->cmp_tag()) {
                    case Cmp_eq: return bb->binop(spv::Op::OpIEqual            , result_type, lhs, rhs);
                    case Cmp_ne: return bb->binop(spv::Op::OpINotEqual         , result_type, lhs, rhs);
                    case Cmp_gt: return bb->binop(spv::Op::OpSGreaterThan      , result_type, lhs, rhs);
                    case Cmp_ge: return bb->binop(spv::Op::OpSGreaterThanEqual , result_type, lhs, rhs);
                    case Cmp_lt: return bb->binop(spv::Op::OpSLessThan         , result_type, lhs, rhs);
                    case Cmp_le: return bb->binop(spv::Op::OpSLessThanEqual    , result_type, lhs, rhs);
                }
            } else if (is_type_u(type)) {
                switch (cmp->cmp_tag()) {
                    case Cmp_eq: return bb->binop(spv::Op::OpIEqual            , result_type, lhs, rhs);
                    case Cmp_ne: return bb->binop(spv::Op::OpINotEqual         , result_type, lhs, rhs);
                    case Cmp_gt: return bb->binop(spv::Op::OpUGreaterThan      , result_type, lhs, rhs);
                    case Cmp_ge: return bb->binop(spv::Op::OpUGreaterThanEqual , result_type, lhs, rhs);
                    case Cmp_lt: return bb->binop(spv::Op::OpULessThan         , result_type, lhs, rhs);
                    case Cmp_le: return bb->binop(spv::Op::OpULessThanEqual    , result_type, lhs, rhs);
                }
            } else if (is_type_f(type)) {
                switch (cmp->cmp_tag()) {
                    // TODO look into the NaN story
                    case Cmp_eq: return bb->binop(spv::Op::OpFOrdEqual            , result_type, lhs, rhs);
                    case Cmp_ne: return bb->binop(spv::Op::OpFOrdNotEqual         , result_type, lhs, rhs);
                    case Cmp_gt: return bb->binop(spv::Op::OpFOrdGreaterThan      , result_type, lhs, rhs);
                    case Cmp_ge: return bb->binop(spv::Op::OpFOrdGreaterThanEqual , result_type, lhs, rhs);
                    case Cmp_lt: return bb->binop(spv::Op::OpFOrdLessThan         , result_type, lhs, rhs);
                    case Cmp_le: return bb->binop(spv::Op::OpFOrdLessThanEqual    , result_type, lhs, rhs);
                }
            } else if (type->isa<PtrType>()) {
                assertf(false, "Physical pointers are unsupported");
            } else if(is_type_bool(type)) {
                switch (cmp->cmp_tag()) {
                    // TODO look into the NaN story
                    case Cmp_eq: return bb->binop(spv::Op::OpLogicalEqual    , result_type, lhs, rhs);
                    case Cmp_ne: return bb->binop(spv::Op::OpLogicalNotEqual , result_type, lhs, rhs);
                    default: THORIN_UNREACHABLE;
                }
                assertf(false, "TODO: should we emulate the other comparison ops ?");
            }
        }

        if (auto arithop = bin->isa<ArithOp>()) {
            auto type = arithop->type();

            if (is_type_f(type)) {
                switch (arithop->arithop_tag()) {
                    case ArithOp_add: return bb->binop(spv::Op::OpFAdd, result_type, lhs, rhs);
                    case ArithOp_sub: return bb->binop(spv::Op::OpFSub, result_type, lhs, rhs);
                    case ArithOp_mul: return bb->binop(spv::Op::OpFMul, result_type, lhs, rhs);
                    case ArithOp_div: return bb->binop(spv::Op::OpFDiv, result_type, lhs, rhs);
                    case ArithOp_rem: return bb->binop(spv::Op::OpFRem, result_type, lhs, rhs);
                    case ArithOp_and:
                    case ArithOp_or:
                    case ArithOp_xor:
                    case ArithOp_shl:
                    case ArithOp_shr: THORIN_UNREACHABLE;
                }
            }

            if (is_type_s(type)) {
                switch (arithop->arithop_tag()) {
                    case ArithOp_add: return bb->binop(spv::Op::OpIAdd                 , result_type, lhs, rhs);
                    case ArithOp_sub: return bb->binop(spv::Op::OpISub                 , result_type, lhs, rhs);
                    case ArithOp_mul: return bb->binop(spv::Op::OpIMul                 , result_type, lhs, rhs);
                    case ArithOp_div: return bb->binop(spv::Op::OpSDiv                 , result_type, lhs, rhs);
                    case ArithOp_rem: return bb->binop(spv::Op::OpSRem                 , result_type, lhs, rhs);
                    case ArithOp_and: return bb->binop(spv::Op::OpBitwiseAnd           , result_type, lhs, rhs);
                    case ArithOp_or:  return bb->binop(spv::Op::OpBitwiseOr            , result_type, lhs, rhs);
                    case ArithOp_xor: return bb->binop(spv::Op::OpBitwiseXor           , result_type, lhs, rhs);
                    case ArithOp_shl: return bb->binop(spv::Op::OpShiftLeftLogical     , result_type, lhs, rhs);
                    case ArithOp_shr: return bb->binop(spv::Op::OpShiftRightArithmetic , result_type, lhs, rhs);
                }
            } else if (is_type_u(type)) {
                switch (arithop->arithop_tag()) {
                    case ArithOp_add: return bb->binop(spv::Op::OpIAdd              , result_type, lhs, rhs);
                    case ArithOp_sub: return bb->binop(spv::Op::OpISub              , result_type, lhs, rhs);
                    case ArithOp_mul: return bb->binop(spv::Op::OpIMul              , result_type, lhs, rhs);
                    case ArithOp_div: return bb->binop(spv::Op::OpUDiv              , result_type, lhs, rhs);
                    case ArithOp_rem: return bb->binop(spv::Op::OpUMod              , result_type, lhs, rhs);
                    case ArithOp_and: return bb->binop(spv::Op::OpBitwiseAnd        , result_type, lhs, rhs);
                    case ArithOp_or:  return bb->binop(spv::Op::OpBitwiseOr         , result_type, lhs, rhs);
                    case ArithOp_xor: return bb->binop(spv::Op::OpBitwiseXor        , result_type, lhs, rhs);
                    case ArithOp_shl: return bb->binop(spv::Op::OpShiftLeftLogical  , result_type, lhs, rhs);
                    case ArithOp_shr: return bb->binop(spv::Op::OpShiftRightLogical , result_type, lhs, rhs);
                }
            } else if(is_type_bool(type)) {
                switch (arithop->arithop_tag()) {
                    case ArithOp_and: return bb->binop(spv::Op::OpLogicalAnd      , result_type, lhs, rhs);
                    case ArithOp_or:  return bb->binop(spv::Op::OpLogicalOr       , result_type, lhs, rhs);
                    // Note: there is no OpLogicalXor
                    case ArithOp_xor: return bb->binop(spv::Op::OpLogicalNotEqual , result_type, lhs, rhs);
                    default: THORIN_UNREACHABLE;
                }
            }
            THORIN_UNREACHABLE;
        }
    } else if (auto primlit = def->isa<PrimLit>()) {
        Box box = primlit->value();
        auto type = convert(def->type()).id;
        SpvId constant;
        switch (primlit->primtype_tag()) {
            case PrimType_bool:                     constant = bb->file_builder.bool_constant(type, box.get_bool()); break;
            case PrimType_ps8:  case PrimType_qs8:  assertf(false, "not implemented yet");
            case PrimType_pu8:  case PrimType_qu8:  assertf(false, "not implemented yet");
            case PrimType_ps16: case PrimType_qs16: assertf(false, "not implemented yet");
            case PrimType_pu16: case PrimType_qu16: assertf(false, "not implemented yet");
            case PrimType_ps32: case PrimType_qs32: constant = bb->file_builder.constant(type, { static_cast<unsigned int>(box.get_s32()) }); break;
            case PrimType_pu32: case PrimType_qu32: constant = bb->file_builder.constant(type, { static_cast<unsigned int>(box.get_u32()) }); break;
            case PrimType_ps64: case PrimType_qs64: assertf(false, "not implemented yet");
            case PrimType_pu64: case PrimType_qu64: assertf(false, "not implemented yet");
            case PrimType_pf16: case PrimType_qf16: assertf(false, "not implemented yet");
            case PrimType_pf32: case PrimType_qf32: assertf(false, "not implemented yet");
            case PrimType_pf64: case PrimType_qf64: assertf(false, "not implemented yet");
        }
        return constant;
    } else if(auto param = def->isa<Param>()) {
        if (auto param_id = current_fn_->params.lookup(param)) {
            assert((*param_id).id != 0);
            return *param_id;
        } else {
            auto val = (*current_fn_->bbs_map[param->continuation()]).phis[param].value;
            assert(val.id != 0);
            return val;
        }
    }
    assertf(false, "Incomplete emit(def) definition");
}

}
