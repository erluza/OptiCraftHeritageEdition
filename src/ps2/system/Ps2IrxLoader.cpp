#ifdef PS2_PLATFORM

#include "platform/Log.h"
#include "ps2/system/Ps2IrxLoader.h"
#include "ps2/storage/assets/Ps2Assets.h"

#include <cstdio>
#include <cstdlib>

#include <loadfile.h>
#include <sbv_patches.h>

namespace Ps2IrxLoader
{

int load(const char* assetKey, const char* fallbackPath)
{
    if (assetKey && assetKey[0] != '\0')
    {
        unsigned int size = 0;
        unsigned char* data = Ps2Assets::loadAsset(assetKey, &size);
        if (data && size > 0)
        {
            // Ps2Assets returns a 64-byte aligned buffer. LMB copies the module
            // to the IOP before returning, so the EE-side buffer can be freed.
            sbv_patch_enable_lmb();
            int mod_res = 0;
            const int result = SifExecModuleBuffer(data, static_cast<int>(size), 0, nullptr, &mod_res);
            std::free(data);
            MC_LOG_INFO("platform", "[PS2][irx] load %s -> result=%d, mod_res=%d\n", assetKey, result, mod_res);
            if (result < 0 || mod_res == 1)
                return -1;
            return result;
        }
        std::free(data);
    }

    if (fallbackPath && fallbackPath[0] != '\0')
    {
        int mod_res = 0;
        const int result = SifLoadStartModule(fallbackPath, 0, nullptr, &mod_res);
        MC_LOG_INFO("platform", "[PS2][irx] load %s -> result=%d, mod_res=%d\n", fallbackPath, result, mod_res);
        if (result < 0 || mod_res == 1)
            return -1;
        return result;
    }

    MC_LOG_INFO("platform", "[PS2][irx] module unavailable: %s\n", assetKey ? assetKey : "(null)");
    return -1;
}

} // namespace Ps2IrxLoader

#endif // PS2_PLATFORM
