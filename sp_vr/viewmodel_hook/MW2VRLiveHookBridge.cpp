#include <cstdint>

// Compile the known Phase 3D live hook in this translation unit, but rename
// its installer so this bridge can provide a more tolerant locator for retail
// SP executable layouts. All of the proven patching, camera, and transform
// code remains the same.
#define gOriginalAddPlayerWeapon gOriginalAddPlayerWeapon_MW2VRShared
#define MW2VR_InstallHook MW2VR_InstallHook_OriginalStrict
#include "MW2VRLiveHook.cpp"
#undef MW2VR_InstallHook
#undef gOriginalAddPlayerWeapon

namespace
{
    struct TaggedDvarRef
    {
        std::uintptr_t address = 0;
        unsigned tag = 0;
    };

    struct DvarWindow
    {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::uintptr_t span = std::numeric_limits<std::uintptr_t>::max();
        bool valid = false;
    };

    DvarWindow FindSmallestAllDvarWindow(
        const std::vector<std::uintptr_t>& drawRefs,
        const std::vector<std::uintptr_t>& xRefs,
        const std::vector<std::uintptr_t>& yRefs,
        const std::vector<std::uintptr_t>& zRefs)
    {
        std::vector<TaggedDvarRef> refs;
        refs.reserve(drawRefs.size() + xRefs.size() + yRefs.size() + zRefs.size());

        for (const auto address : drawRefs)
        {
            refs.push_back({address, 0});
        }
        for (const auto address : xRefs)
        {
            refs.push_back({address, 1});
        }
        for (const auto address : yRefs)
        {
            refs.push_back({address, 2});
        }
        for (const auto address : zRefs)
        {
            refs.push_back({address, 3});
        }

        std::sort(refs.begin(), refs.end(),
            [](const TaggedDvarRef& a, const TaggedDvarRef& b)
            {
                if (a.address != b.address)
                {
                    return a.address < b.address;
                }
                return a.tag < b.tag;
            });

        std::array<unsigned, 4> counts{};
        unsigned present = 0;
        std::size_t left = 0;
        DvarWindow best{};

        for (std::size_t right = 0; right < refs.size(); ++right)
        {
            if (counts[refs[right].tag]++ == 0)
            {
                ++present;
            }

            while (present == 4 && left <= right)
            {
                const std::uintptr_t begin = refs[left].address;
                const std::uintptr_t end = refs[right].address;
                const std::uintptr_t span = end - begin;
                if (!best.valid || span < best.span)
                {
                    best = {begin, end, span, true};
                }

                if (--counts[refs[left].tag] == 0)
                {
                    --present;
                }
                ++left;
            }
        }

        return best;
    }

    std::uint8_t* LocateAddPlayerWeaponCallsite_OrderIndependent(
        const ModuleLayout& layout)
    {
        const std::uintptr_t drawGlobal = ResolveDvarGlobal(layout, "cg_drawGun");
        const std::uintptr_t gunXGlobal = ResolveDvarGlobal(layout, "cg_gun_x");
        const std::uintptr_t gunYGlobal = ResolveDvarGlobal(layout, "cg_gun_y");
        const std::uintptr_t gunZGlobal = ResolveDvarGlobal(layout, "cg_gun_z");
        if (!drawGlobal || !gunXGlobal || !gunYGlobal || !gunZGlobal)
        {
            return nullptr;
        }

        Log("[MW2VR] dvar globals draw=%08X x=%08X y=%08X z=%08X.\r\n",
            static_cast<unsigned>(drawGlobal),
            static_cast<unsigned>(gunXGlobal),
            static_cast<unsigned>(gunYGlobal),
            static_cast<unsigned>(gunZGlobal));

        const auto drawRefs = FindGlobalReads(layout, drawGlobal);
        const auto xRefs = FindGlobalReads(layout, gunXGlobal);
        const auto yRefs = FindGlobalReads(layout, gunYGlobal);
        const auto zRefs = FindGlobalReads(layout, gunZGlobal);

        Log("[MW2VR] dvar read counts draw=%u x=%u y=%u z=%u.\r\n",
            static_cast<unsigned>(drawRefs.size()),
            static_cast<unsigned>(xRefs.size()),
            static_cast<unsigned>(yRefs.size()),
            static_cast<unsigned>(zRefs.size()));

        if (drawRefs.empty() || xRefs.empty() || yRefs.empty() || zRefs.empty())
        {
            Log("[MW2VR] at least one required viewmodel dvar has no code references; refusing hook.\r\n");
            return nullptr;
        }

        const DvarWindow window =
            FindSmallestAllDvarWindow(drawRefs, xRefs, yRefs, zRefs);
        if (!window.valid)
        {
            Log("[MW2VR] could not form an all-dvar viewmodel code window.\r\n");
            return nullptr;
        }

        Log("[MW2VR] smallest all-dvar window %08X..%08X span=0x%X.\r\n",
            static_cast<unsigned>(window.begin),
            static_cast<unsigned>(window.end),
            static_cast<unsigned>(window.span));

        // Retail SP builds may access cg_drawGun before OR after cg_gun_x/y/z,
        // so do not assume an ordering. A true CG_AddViewWeapon region should
        // still keep all four reads reasonably close together.
        if (window.span > 0x3000u)
        {
            Log("[MW2VR] all-dvar window is too wide for one viewmodel routine; refusing hook.\r\n");
            return nullptr;
        }

        const std::uintptr_t textBegin =
            reinterpret_cast<std::uintptr_t>(layout.text.begin);
        const std::uintptr_t textEnd = textBegin + layout.text.size;

        auto findCalls = [&](std::uintptr_t scanBegin, std::uintptr_t scanEnd)
        {
            std::vector<std::uint8_t*> calls;
            scanBegin = std::max(scanBegin, textBegin);
            scanEnd = std::min(scanEnd, textEnd);

            for (std::uintptr_t address = scanBegin;
                 address + 8 < scanEnd;
                 ++address)
            {
                auto* call = reinterpret_cast<std::uint8_t*>(address);
                if (call[0] != 0xE8 || !HasCallerCleanup20(call, layout.text))
                {
                    continue;
                }

                std::int32_t relative = 0;
                std::memcpy(&relative, call + 1, sizeof(relative));
                const std::uintptr_t target = address + 5 + relative;
                if (!AddressInside(layout.text, target))
                {
                    continue;
                }

                const std::uintptr_t pushStart =
                    address > textBegin + 112 ? address - 112 : textBegin;
                if (CountLikelyPushes(
                        reinterpret_cast<const std::uint8_t*>(pushStart), call) >= 4)
                {
                    calls.push_back(call);
                }
            }
            return calls;
        };

        // Preferred path: the five-argument CG_AddPlayerWeapon submit is
        // expected shortly after the last of the four viewmodel-dvar reads.
        auto calls = findCalls(window.end, window.end + 0x1400u);

        // Some compiler/layout variants interleave helper calls and place the
        // final submit slightly before the last dvar access. Use a broader
        // function-local window only when the preferred path found nothing.
        if (calls.empty())
        {
            const std::uintptr_t fallbackBegin =
                window.begin > 0x500u ? window.begin - 0x500u : textBegin;
            calls = findCalls(fallbackBegin, window.end + 0x1800u);
        }

        // De-duplicate in case overlapping scans or byte-wise scanning saw the
        // same call more than once.
        std::sort(calls.begin(), calls.end());
        calls.erase(std::unique(calls.begin(), calls.end()), calls.end());

        if (calls.size() != 1)
        {
            Log("[MW2VR] order-independent locator expected one five-argument viewmodel submit call, found %u; refusing hook.\r\n",
                static_cast<unsigned>(calls.size()));
            for (std::size_t index = 0; index < std::min<std::size_t>(calls.size(), 8); ++index)
            {
                Log("[MW2VR] candidate submit[%u]=%08X.\r\n",
                    static_cast<unsigned>(index),
                    static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(calls[index])));
            }
            return nullptr;
        }

        Log("[MW2VR] verified order-independent CG_AddPlayerWeapon callsite at %08X.\r\n",
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(calls.front())));
        return calls.front();
    }
}

extern "C" __declspec(dllexport)
DWORD WINAPI MW2VR_InstallHook(LPVOID)
{
    std::lock_guard<std::mutex> lock(gInstallMutex);
    if (gHookInstalled.load(std::memory_order_acquire))
    {
        return 1;
    }

    ModuleLayout layout{};
    if (!BuildModuleLayout(layout))
    {
        Log("[MW2VR] failed to parse the 32-bit iw4sp.exe PE image.\r\n");
        return 0;
    }

    std::uint8_t* callsite =
        LocateAddPlayerWeaponCallsite_OrderIndependent(layout);
    if (!callsite)
    {
        Log("[MW2VR] live viewmodel hook NOT installed; executable validation failed safely.\r\n");
        return 0;
    }

    if (!PatchCallsite(callsite))
    {
        Log("[MW2VR] live viewmodel call patch failed; game code left unmodified.\r\n");
        return 0;
    }

    gHookInstalled.store(true, std::memory_order_release);
    MW2VR_ResetWeaponBaselines();

    HANDLE resolver = CreateThread(
        nullptr, 0, CameraResolverThread, nullptr, 0, nullptr);
    if (resolver)
    {
        CloseHandle(resolver);
    }

    Log("[MW2VR] LIVE HOOK INSTALLED: order-independent SP locator accepted CG_AddPlayerWeapon and the viewmodel now passes through the VR transform.\r\n");
    return 1;
}

extern "C" __declspec(dllexport)
std::uintptr_t __cdecl MW2VR_GetOriginalAddPlayerWeaponAddress()
{
    return reinterpret_cast<std::uintptr_t>(
        gOriginalAddPlayerWeapon_MW2VRShared);
}
