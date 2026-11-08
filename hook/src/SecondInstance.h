#ifndef HOOK_SECONDINSTANCE_H
#define HOOK_SECONDINSTANCE_H

namespace Dynamite {
    // Test-only, see InstanceName.h. Call before the game's own code runs (from the hook constructor).
    void SetupSecondInstance();
} // namespace Dynamite

#endif // HOOK_SECONDINSTANCE_H
