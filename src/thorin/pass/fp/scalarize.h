#ifndef THORIN_PASS_FP_SCALARIZE_H
#define THORIN_PASS_FP_SCALARIZE_H

#include "thorin/world.h"
#include "thorin/pass/pass.h"

namespace thorin {

/// Perform Scalarization (= Argument simplification), i.e.:
/// <code> f := λ (x_1:[T_1, T_2], .., x_n:T_n).E </code> will be transformed to
/// <code> f' := λ (y_1:T_1, y_2:T2, .. y_n:T_n).E[x_1\(y_1, y2); ..; x_n\y_n]</code> if
/// <code>f</code> appears in callee position only, see @p EtaExp. 
/// It will not flatten nominal @p Sigma#s or @p Arr#s.

class Scalerize : public FPPass<Scalerize, Lam> {
public:
    Scalerize(PassMan& man)
        : FPPass(man, "scalerize")
    {}

    const Def* rewrite(const Def*) override;

    undo_t analyze(const Def*) override;

    using Data = DefSet; // Expanded Lams

private:

    bool should_expand(Lam *lam);
    Lam* make_scalar(Lam *lam);

    DefSet keep_;               // Should not be expanded
    Lam2Lam tup2sca_;
    NomMap<Def2Def> sca_args;   // rewrite args in body
};

}

#endif
