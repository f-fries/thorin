#ifndef THORIN_PASS_CLOSURE_CONV_H
#define THORIN_PASS_CLOSURE_CONV_H

#include "thorin/pass/pass.h"

namespace thorin {

class ClosureConv : public RWPass {
public:
    ClosureConv(PassMan& man)
        : RWPass(man, "closure_conv")
    {}

    const Def* rewrite(Def*, const Def*) override;

private:
    const Tuple* convert(Lam*);
    const Sigma* convert(const Pi*);

    GIDMap<const Pi*, const Sigma*> pi2closure_;
    LamMap<const Tuple*> lam2closure_;
};

}

#endif
