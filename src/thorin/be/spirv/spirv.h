#ifndef THORIN_SPIRV_H
#define THORIN_SPIRV_H

#include "thorin/be/spirv/spirv_builder.hpp"
#include "thorin/be/codegen.h"

namespace thorin::spirv {

using SpvId = builder::SpvId;

class CodeGen;
struct Datatype;

struct ConvertedType {
    CodeGen* code_gen;
    SpvId type_id { 0 };
    std::unique_ptr<Datatype> datatype;

    ConvertedType(CodeGen* cg) : code_gen(cg) {}
    bool is_known_size() { return datatype != nullptr; }
};

struct FnBuilder;

struct BasicBlockBuilder : public builder::SpvBasicBlockBuilder {
    explicit BasicBlockBuilder(FnBuilder& fn_builder);

    std::unordered_map<const Param*, Phi> phis_map;
    DefMap<SpvId> args;
};

struct FnBuilder : public builder::SpvFnBuilder {
    const Scope* scope;
    builder::SpvFileBuilder* file_builder;
    std::vector<std::unique_ptr<BasicBlockBuilder>> bbs;
    std::unordered_map<Continuation*, BasicBlockBuilder*> bbs_map;
    ContinuationMap<SpvId> labels;
    DefMap<SpvId> params;
};

class CodeGen : public thorin::CodeGen {
public:
    CodeGen(World&, Cont2Config&, bool debug);

    void emit_stream(std::ostream& stream) override;
    const char* file_ext() const override { return ".spv"; }

    ConvertedType* convert(const Type*);
protected:
    void structure_loops();
    void structure_flow();

    void emit(const Scope& scope);
    void emit_epilogue(Continuation*, BasicBlockBuilder* bb);
    SpvId emit(const Def* def, BasicBlockBuilder* bb);

    SpvId get_codom_type(const Continuation* fn);

    builder::SpvFileBuilder* builder_ = nullptr;
    Continuation* entry_ = nullptr;
    FnBuilder* current_fn_ = nullptr;
    TypeMap<std::unique_ptr<ConvertedType>> types_;
    DefMap<SpvId> defs_;

};

/// Thorin data types are mapped to SPIR-V in non-trivial ways, this interface is used by the emission code to abstract over
/// potentially different mappings, depending on the capabilities of the target platform. The serdes code deals with pointers
/// in arrays of unsigned 32 bit words, and is there to get around the limitation of not being able to bitcast pointers in the
/// logical addressing mode.
struct Datatype {
public:
    ConvertedType* type;
    Datatype(ConvertedType* type) : type(type) {}

    virtual size_t serialized_size() = 0;
    virtual void emit_serialization(BasicBlockBuilder& bb, SpvId output, SpvId data) = 0;
    virtual SpvId emit_deserialization(BasicBlockBuilder& bb, SpvId input) = 0;
};

/// For scalar datatypes
struct ScalarDatatype : public Datatype {
    int type_tag;
    size_t size_in_bytes;
    size_t alignment;
    ScalarDatatype(ConvertedType* type, int type_tag, size_t size_in_bytes, size_t alignment_in_bytes);

    size_t serialized_size() override { return size_in_bytes / 4; };
    SpvId emit_deserialization(BasicBlockBuilder& bb, SpvId input) override;
    void emit_serialization(BasicBlockBuilder& bb, SpvId output, SpvId data) override;
};

struct DefiniteArrayDatatype : public Datatype {
    ConvertedType* element_type;
    size_t length;

    DefiniteArrayDatatype(ConvertedType* type, ConvertedType* element_type, size_t length);

    size_t serialized_size() override { return element_type->datatype->serialized_size(); };
    SpvId emit_deserialization(BasicBlockBuilder& bb, SpvId input) override;
    void emit_serialization(BasicBlockBuilder& bb, SpvId output, SpvId data) override;
};

struct ProductDatatype : public Datatype {
    std::vector<ConvertedType*> elements_types;
    size_t total_size = 0;

    ProductDatatype(ConvertedType* type, const std::vector<ConvertedType*>&& elements_types);

    size_t serialized_size() override { return total_size; };
    SpvId emit_deserialization(BasicBlockBuilder& bb, SpvId input) override;
    void emit_serialization(BasicBlockBuilder& bb, SpvId output, SpvId data) override;
};

}

#endif //THORIN_SPIRV_H