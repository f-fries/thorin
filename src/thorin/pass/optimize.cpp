#include "thorin/pass/fp/beta_red.h"
#include "thorin/pass/fp/copy_prop.h"
#include "thorin/pass/fp/dce.h"
#include "thorin/pass/fp/eta_exp.h"
#include "thorin/pass/fp/eta_red.h"
#include "thorin/pass/fp/ssa_constr.h"
#include "thorin/pass/rw/bound_elim.h"
#include "thorin/pass/rw/partial_eval.h"
#include "thorin/pass/rw/ret_wrap.h"
#include "thorin/pass/rw/scalarize.h"

// old stuff
#include "thorin/transform/cleanup_world.h"
#include "thorin/transform/partial_evaluation.h"
#include "thorin/transform/closure_conv.h"
#include "thorin/transform/untype_closures.h"

namespace thorin {

void optimize(World& world) {
    PassMan opt(world);
    // opt.add<PartialEval>();
    // opt.add<BetaRed>();
    auto er = opt.add<EtaRed>();
    auto ee = opt.add<EtaExp>(er);
    // opt.add<SSAConstr>(ee);
    // opt.add<CopyProp>();
    // opt.add<Scalerize>();
    // opt.add<AutoDiff>();
    opt.run();

    ClosureConv(world).run();
    auto cc = PassMan(world);
    cc.add<Scalerize>();
    cc.run();
    world.debug_stream();

    UntypeClosures(world).run();

    // while (partial_evaluation(world, true)); // lower2cff
    // flatten_tuples(world);

    // PassMan codgen_prepare(world);
    //codgen_prepare.add<BoundElim>();
    // codgen_prepare.add<RetWrap>();
    // codgen_prepare.run();
}

}
