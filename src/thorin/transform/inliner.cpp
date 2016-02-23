#include "thorin/lambda.h"
#include "thorin/world.h"
#include "thorin/analyses/cfg.h"
#include "thorin/analyses/scope.h"
#include "thorin/analyses/verify.h"
#include "thorin/transform/mangle.h"

namespace thorin {

void inliner(World& world) {
#if 0
    Scope::for_each(world, [] (const Scope& scope) {
        for (auto n : scope.f_cfg().post_order()) {
            auto lambda = n->lambda();
            if (auto to_lambda = lambda->to()->isa_lambda()) {
                if (!to_lambda->empty() && to_lambda->num_uses() <= 2 && !scope.outer_contains(to_lambda)) {
                    Scope to_scope(to_lambda);
                    lambda->jump(drop(to_scope, lambda->type_args(), lambda->args()), {}, {}, lambda->jump_loc());
                }
            }
        }
    });
#endif

    debug_verify(world);
}

}
