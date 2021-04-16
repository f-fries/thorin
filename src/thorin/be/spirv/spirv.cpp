#include "thorin/be/spirv/spirv.h"

#include "thorin/analyses/scope.h"
#include "thorin/analyses/schedule.h"
#include "thorin/analyses/domtree.h"
#include "thorin/transform/cleanup_world.h"

#include <iostream>

namespace thorin {
    void dump_dot(thorin::World &world);
}

namespace thorin::spirv {

CodeGen::CodeGen(thorin::World& world, Cont2Config&, bool debug)
    : thorin::CodeGen(world, debug)
{}

void CodeGen::emit_stream(std::ostream& out) {
    builder::SpvFileBuilder builder;
    builder_ = &builder;
    builder_->capability(spv::Capability::CapabilityShader);
    builder_->capability(spv::Capability::CapabilityLinkage);
    builder_->capability(spv::Capability::CapabilityVariablePointers);
    builder_->capability(spv::Capability::CapabilityPhysicalStorageBufferAddresses);

    structure_loops();
    structure_flow();
    // cleanup_world(world());
    dump_dot(world());

    Scope::for_each(world(), [&](const Scope& scope) { emit(scope); });

    for (auto& cont : world().continuations()) {
        if (cont->is_exported()) {
            // TODO create entry point
        }
    }

    builder_->finish(out);
    builder_ = nullptr;
}

void CodeGen::emit(const thorin::Scope& scope) {
    entry_ = scope.entry();
    assert(entry_->is_returning());

    FnBuilder fn;
    fn.scope = &scope;
    fn.file_builder = builder_;
    fn.fn_type = convert(entry_->type())->type_id;
    fn.fn_ret_type = get_codom_type(entry_);

    current_fn_ = &fn;

    auto conts = schedule(scope);

    fn.bbs_to_emit.reserve(conts.size());
    fn.bbs.reserve(conts.size());
    auto& bbs = fn.bbs;

    for (auto cont : conts) {
        if (cont->intrinsic() == Intrinsic::EndScope) continue;

        BasicBlockBuilder* bb = bbs.emplace_back(std::make_unique<BasicBlockBuilder>(fn)).get();
        fn.bbs_to_emit.emplace_back(bb);
        auto [i, b] = fn.bbs_map.emplace(cont, bb);
        assert(b);

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
                    fn.header.ref_id(param_t->type_id);
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
                    bb->phis_map[param] = {convert(param->type())->type_id, builder_->generate_fresh_id(), {} };
                }
            }
        }
    }

    for (auto cont : conts) {
        if (cont->intrinsic() == Intrinsic::EndScope) continue;
        assert(cont == entry_ || cont->is_basicblock());
        emit_epilogue(cont, fn.bbs_map[cont]);
    }

    for(auto& bb : fn.bbs) {
        for (auto& [param, phi] : bb->phis_map) {
            bb->phis.emplace_back(&phi);
        }
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
        types.push_back(convert(op)->type_id);
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
        auto& domtree = current_fn_->scope->b_cfg().domtree();
        auto merge_cont = domtree.idom(current_fn_->scope->f_cfg().operator[](continuation))->continuation();
        SpvId merge_bb;
        if (merge_cont == current_fn_->scope->exit()) {
            BasicBlockBuilder* unreachable_merge_bb = current_fn_->bbs.emplace_back(std::make_unique<BasicBlockBuilder>(*current_fn_)).get();
            current_fn_->bbs_to_emit.emplace_back(unreachable_merge_bb);
            builder_->name(unreachable_merge_bb->label, "merge_unreachable" + continuation->name());
            unreachable_merge_bb->unreachable();
            merge_bb = unreachable_merge_bb->label;
        } else {
            // TODO create a dedicated merge bb if this one is the merge blocks for more than 1 selection construct
            merge_bb = current_fn_->labels[merge_cont];
        }

        auto cond = emit(continuation->arg(0), bb);
        bb->args.emplace(continuation->arg(0), cond);
        auto tbb = current_fn_->labels[continuation->arg(1)->as_continuation()];
        auto fbb = current_fn_->labels[continuation->arg(2)->as_continuation()];
        bb->selection_merge(merge_bb,spv::SelectionControlMaskNone);
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
    else if (continuation->intrinsic() == Intrinsic::SCFLoopHeader) {
        auto merge_label = current_fn_->bbs_map[const_cast<Continuation*>(continuation->attributes_.scf_metadata.loop_header.merge_target)]->label;
        auto continue_label = current_fn_->bbs_map[const_cast<Continuation*>(continuation->attributes_.scf_metadata.loop_header.continue_target)]->label;
        bb->loop_merge(merge_label, continue_label, spv::LoopControlMaskNone, {});

        BasicBlockBuilder* dispatch_bb = current_fn_->bbs.emplace_back(std::make_unique<BasicBlockBuilder>(*current_fn_)).get();

        auto header_bb_location = std::find(current_fn_->bbs_to_emit.begin(), current_fn_->bbs_to_emit.end(), bb);

        current_fn_->bbs_to_emit.emplace(header_bb_location + 1, dispatch_bb);
        builder_->name(dispatch_bb->label, "dispatch_" + continuation->name());
        bb->branch(dispatch_bb->label);
        int targets = continuation->num_ops();
        assert(targets > 0);

        assert(targets == 1);
        auto callee = continuation->op(0)->as_continuation();
        // Extract the relevant variant & expand the tuple if necessary
        auto arg = world().variant_extract(continuation->param(0), 0);
        auto extracted = emit(arg, dispatch_bb);

        if (callee->param(0)->type()->equal(arg->type())) {
            auto* param = callee->param(0);
            auto& phi = current_fn_->bbs_map[callee]->phis_map[param];
            phi.preds.emplace_back(extracted, dispatch_bb->label);
        } else {
            assert(false && "TODO destructure argument");
        }

        dispatch_bb->branch(current_fn_->bbs_map[callee]->label);

    } else if (continuation->intrinsic() == Intrinsic::SCFLoopContinue) {
        auto loop_header = continuation->op(0)->as_continuation();
        auto header_label = current_fn_->bbs_map[loop_header]->label;

        auto arg = continuation->param(0);
        bb->args[arg] = emit(arg, bb);
        auto* param = loop_header->param(0);
        auto& phi = current_fn_->bbs_map[loop_header]->phis_map[param];
        phi.preds.emplace_back(bb->args[arg], current_fn_->labels[continuation]);

        bb->branch(header_label);
    } else if (continuation->intrinsic() == Intrinsic::SCFLoopMerge) {
        // auto header_cont = continuation->op(0)->as_continuation();

        int targets = continuation->num_ops();
        assert(targets > 0);

        assert(targets == 1);
        auto callee = continuation->op(0)->as_continuation();
        // TODO phis
        bb->branch(current_fn_->bbs_map[callee]->label);
    } else if (auto callee = continuation->callee()->isa_continuation(); callee && callee->is_basicblock()) { // ordinary jump
        int index = -1;
        for (auto& arg : continuation->args()) {
            index++;
            if (is_mem(arg) || is_unit(arg)) continue;
            bb->args[arg] = emit(arg, bb);
            auto* param = callee->param(index);
            auto& phi = current_fn_->bbs_map[callee]->phis_map[param];
            phi.preds.emplace_back(bb->args[arg], current_fn_->labels[continuation]);
        }
        bb->branch(current_fn_->labels[callee]);
    }
    /*else if (auto callee = continuation->callee()->isa_continuation(); callee && callee->is_intrinsic()) {
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
        ConvertedType* result_types = convert(def->type());
        SpvId result_type = result_types->type_id;

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
        auto type = convert(def->type())->type_id;
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
    } else if (auto param = def->isa<Param>()) {
        if (auto param_id = current_fn_->params.lookup(param)) {
            assert((*param_id).id != 0);
            return *param_id;
        } else {
            auto val = (*current_fn_->bbs_map[param->continuation()]).phis_map[param].value;
            assert(val.id != 0);
            return val;
        }
    } else if (auto variant = def->isa<Variant>()) {
        auto variant_type = def->type()->as<VariantType>();
        auto variant_datatype = (ProductDatatype*) convert(variant_type)->datatype.get();

        auto payload_arr = bb->variable(variant_datatype->elements_types[1]->type_id, spv::StorageClassFunction);
        auto converted_payload_type = convert(variant_type->op(variant->index()));
        converted_payload_type->datatype->emit_serialization(*bb, payload_arr, emit(variant->value(), bb));
        auto payload = bb->load(variant_datatype->elements_types[1]->type_id, payload_arr);

        auto tag = builder_->constant(convert(world().type_pu32())->type_id, {static_cast<uint32_t>(variant->index()) });
        std::vector<SpvId> with_tag = { tag, payload };
        return bb->composite(convert(variant->type())->type_id, with_tag);
    } else if (auto vextract = def->isa<VariantExtract>()) {
        auto variant_type = vextract->value()->type()->as<VariantType>();
        auto variant_datatype = (ProductDatatype*) convert(variant_type)->datatype.get();

        auto target_type = convert(def->type());

        auto payload_arr = bb->variable(variant_datatype->elements_types[1]->type_id, spv::StorageClassFunction);
        auto payload = bb->extract(variant_datatype->elements_types[1]->type_id, emit(vextract->value(), bb), {1});
        bb->store(payload, payload_arr);

        return target_type->datatype->emit_deserialization(*bb, payload_arr);
    } else if (auto vindex = def->isa<VariantIndex>()) {
        auto value = emit(vindex->op(0), bb);
        return bb->extract(convert(world().type_pu32())->type_id, value, { 0 });
    } else if (auto tuple = def->isa<Tuple>()) {
        std::vector<SpvId> elements;
        elements.resize(tuple->num_ops());
        size_t x = 0;
        for (auto& e : tuple->ops()) {
            elements[x++] = emit(e, bb);
        }
        return bb->composite(convert(tuple->type())->type_id, elements);
    } else if (auto structagg = def->isa<StructAgg>()) {
        std::vector<SpvId> elements;
        elements.resize(structagg->num_ops());
        size_t x = 0;
        for (auto& e : structagg->ops()) {
            elements[x++] = emit(e, bb);
        }
        return bb->composite(convert(structagg->type())->type_id, elements);
    }
    assertf(false, "Incomplete emit(def) definition");
}

BasicBlockBuilder::BasicBlockBuilder(FnBuilder& fn_builder)
: builder::SpvBasicBlockBuilder(*fn_builder.file_builder) {
    label = file_builder.generate_fresh_id();
}

}