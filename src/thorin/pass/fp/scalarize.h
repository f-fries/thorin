#ifndef THORIN_PASS_FP_SCALARIZE_H
#define THORIN_PASS_FP_SCALARIZE_H

#include "thorin/pass/pass.h"

namespace thorin {

class Scalerize : public FPPass<Scalerize> {
public:
    Scalerize(PassMan& man, size_t index)
        : FPPass(man, "scalerize", index)
    {}

    const Def* rewrite(Def*, const Def*) override;
    undo_t analyze(Def*, const Def*) override;

    using Data = std::tuple<LamSet>;

private:
    DefSet keep_;
    Lam2Lam tup2sca_;
};

}

#endif
