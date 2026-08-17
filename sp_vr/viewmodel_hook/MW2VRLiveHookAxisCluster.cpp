#include <cstdint>

// Reuse the proven Phase 3D hook implementation, but replace only its
// executable locator. The user's retail SP build resolves the dvars correctly
// yet accesses cg_drawGun far away from cg_gun_x/y/z, so drawGun cannot be a
// mandatory member of the same code cluster.
#define gOriginalAddPlayerWeapon gOriginalAddPlayerWeapon_MW2VRShared
#define MW2VR_InstallHook MW2VR_InstallHook_OriginalStrict
#include "MW2VRLiveHook.cpp"
#undef MW2VR_InstallHook
#undef gOriginalAddPlayerWeapon

namespace
{
    struct TaggedAxisRef
    {
        std::uintptr_t address = 0;
        unsigned tag = 0;
    };

    struct AxisWindow
    {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::uintptr_t span = std::numeric_limits<std::uintptr_t>::max();
        bool valid = false;
    };

    AxisWindow FindSmallestAxisWindow(
        const std::vector<std::uintptr_t>& xRefs,
        const std::vector<std::uintptr_t>& yRefs,
        const std::vector<std::uintptr_t>& zRefs)
    {
        std::vector<TaggedAxisRef> refs;
        refs.reserve(xRefs.size() + yRefs.size() + zRefs.size());

        for (const auto address : xRefs)
        {
            refs.push_back({address, 0});
        }
        for (const auto address : yRefs)
        {
            refs.push_back({address, 1});
        }
        for (const auto address : zRefs)
        {
            refs.push_back({address, 2});
        }

        std::sort(refs.begin(), refs.end(),
            [](const TaggedAxisRef& a, const TaggedAxisRef& b)
            {
                if (a.address != b.address)
                {
                    return a.address < b.address;
                }
                return a.tag < b.tag;
            });

        std::array<unsigned, 3> counts{};
        unsigned present = 0;
        std::size_t left = 0;
        AxisWindow best{};

        for (std::size_t right = 0; right < refs.size(); ++right)
        {
            if (counts[refs[right].tag]++ == 0)
            {
                ++present;
            }

            while (present == 3 && left <= right)
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

    std::uintptr_t Distance(std::uintptr_t a, std::uintptr_t b)
    {
        return a >= b ? a - b : b - a;
    }

    std::uintptr_t NearestRefDistance(
        const std::vector<std::uintptr_t>& refs,
        std::uintptr_t address)
    {
        if (refs.empty())
        {
            return std::numeric_limits<std::uintptr_t>::max();
        }

        const auto it = std::lower_bound(refs.begin(), refs.end(), address);
        std::uintptr_t best = std::numeric_limits<std::uintptr_t>::max();
        if (it != refs.end())
        {
            best = Distance(*it, address);
        }
        if (it != refs.begin())
        {
            best = std::min(best, Distance(*std::prev(it), address));
        }
        return best;
    }

    struct SubmitCandidate
    {
        std::uint8_t* call = nullptr;
        std::uintptr_t target = 0;
        std::uintptr_t axisDistance = 0;
        std::uintptr_t drawDistance = 0;
        bool afterAxisCluster = false;
    };

    std::vector<SubmitCandidate> FindSubmitCandidates(
        const ModuleLayout& layout,
        const AxisWindow& window,
        const std::vector<std::uintptr_t>& drawRefs,
        std::uintptr_t scanBegin,
        std::uintptr_t scanEnd)
    {
        const std::uintptr_t textBegin =
            reinterpret_cast<std::uintptr_t>(layout.text.begin);
        const std::uintptr_t textEnd = textBegin + layout.text.size;
        scanBegin = std::max(scanBegin, textBegin);
        scanEnd = std::min(scanEnd, textEnd);

        std::vector<SubmitCandidate> candidates;
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
                    reinterpret_cast<const std::uint8_t*>(pushStart), call) < 4)
            {
                continue;
            }

            candidates.push_back({
                call,
                target,
                Distance(address, window.end),
                NearestRefDistance(drawRefs, address),
                address >= window.end,
            });
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const SubmitCandidate& a, const SubmitCandidate& b)
            {
                if (a.call != b.call)
                {
                    return a.call < b.call;
                }
                return a.target < b.target;
            });
        candidates.erase(
            std::unique(candidates.begin(), candidates.end(),
                [](const SubmitCandidate& a, const SubmitCandidate& b)
                {
                    return a.call == b.call;
                }),
            candidates.end());
        return candidates;
    }

    void LogCandidates(const std::vector<SubmitCandidate>& candidates)
    {
        for (std::size_t index = 0;
             index < std::min<std::size_t>(candidates.size(), 12);
             ++index)
        {
            const auto& candidate = candidates[index];
            Log("[MW2VR] axis submit[%u]=%08X -> %08X axisDist=0x%X drawDist=0x%X side=%s.\r\n",
                static_cast<unsigned>(index),
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(candidate.call)),
                static_cast<unsigned>(candidate.target),
                static_cast<unsigned>(candidate.axisDistance),
                candidate.drawDistance == std::numeric_limits<std::uintptr_t>::max()
                    ? 0xFFFFFFFFu
                    : static_cast<unsigned>(candidate.drawDistance),
                candidate.afterAxisCluster ? "after" : "before");
        }
    }

    std::uint8_t* ChooseCandidate(
        std::vector<SubmitCandidate> candidates,
        const char* stage)
    {
        if (candidates.empty())
        {
            return nullptr;
        }

        // CG_AddPlayerWeapon is expected after the cg_gun_x/y/z offset work.
        // Prefer post-cluster calls before considering any fallback candidate.
        std::vector<SubmitCandidate> after;
        for (const auto& candidate : candidates)
        {
            if (candidate.afterAxisCluster)
            {
                after.push_back(candidate);
            }
        }
        if (!after.empty())
        {
            candidates = std::move(after);
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const SubmitCandidate& a, const SubmitCandidate& b)
            {
                if (a.axisDistance != b.axisDistance)
                {
                    return a.axisDistance < b.axisDistance;
                }
                if (a.drawDistance != b.drawDistance)
                {
                    return a.drawDistance < b.drawDistance;
                }
                return a.call < b.call;
            });

        Log("[MW2VR] %s produced %u plausible five-argument submit call(s).\r\n",
            stage,
            static_cast<unsigned>(candidates.size()));
        LogCandidates(candidates);

        if (candidates.size() == 1)
        {
            return candidates.front().call;
        }

        // Accept the nearest post-axis submit only when it is very close to the
        // X/Y/Z work and clearly separated from the runner-up. This keeps the
        // hook fail-closed on ambiguous retail layouts.
        const auto& first = candidates[0];
        const auto& second = candidates[1];
        const bool firstIsClose = first.axisDistance <= 0x900u;
        const bool clearlyCloser =
            second.axisDistance > first.axisDistance + 0x180u;
        if (firstIsClose && clearlyCloser)
        {
            Log("[MW2VR] selecting nearest axis-cluster submit %08X (0x%X bytes from cluster; runner-up 0x%X).\r\n",
                static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(first.call)),
                static_cast<unsigned>(first.axisDistance),
                static_cast<unsigned>(second.axisDistance));
            return first.call;
        }

        Log("[MW2VR] axis-cluster submit candidates remain ambiguous; refusing hook.\r\n");
        return nullptr;
    }

    std::uint8_t* LocateAddPlayerWeaponCallsite_AxisCluster(
        const ModuleLayout& layout)
    {
        const std::uintptr_t drawGlobal = ResolveDvarGlobal(layout, "cg_drawGun");
        const std::uintptr_t gunXGlobal = ResolveDvarGlobal(layout, "cg_gun_x");
        const std::uintptr_t gunYGlobal = ResolveDvarGlobal(layout, "cg_gun_y");
        const std::uintptr_t gunZGlobal = ResolveDvarGlobal(layout, "cg_gun_z");
        if (!gunXGlobal || !gunYGlobal || !gunZGlobal)
        {
            Log("[MW2VR] could not resolve all cg_gun_x/y/z globals; refusing hook.\r\n");
            return nullptr;
        }

        Log("[MW2VR] dvar globals draw=%08X x=%08X y=%08X z=%08X.\r\n",
            static_cast<unsigned>(drawGlobal),
            static_cast<unsigned>(gunXGlobal),
            static_cast<unsigned>(gunYGlobal),
            static_cast<unsigned>(gunZGlobal));

        auto drawRefs = drawGlobal
            ? FindGlobalReads(layout, drawGlobal)
            : std::vector<std::uintptr_t>{};
        auto xRefs = FindGlobalReads(layout, gunXGlobal);
        auto yRefs = FindGlobalReads(layout, gunYGlobal);
        auto zRefs = FindGlobalReads(layout, gunZGlobal);

        std::sort(drawRefs.begin(), drawRefs.end());
        std::sort(xRefs.begin(), xRefs.end());
        std::sort(yRefs.begin(), yRefs.end());
        std::sort(zRefs.begin(), zRefs.end());

        Log("[MW2VR] dvar read counts draw=%u x=%u y=%u z=%u.\r\n",
            static_cast<unsigned>(drawRefs.size()),
            static_cast<unsigned>(xRefs.size()),
            static_cast<unsigned>(yRefs.size()),
            static_cast<unsigned>(zRefs.size()));

        if (xRefs.empty() || yRefs.empty() || zRefs.empty())
        {
            Log("[MW2VR] at least one cg_gun axis dvar has no code references; refusing hook.\r\n");
            return nullptr;
        }

        const AxisWindow window = FindSmallestAxisWindow(xRefs, yRefs, zRefs);
        if (!window.valid)
        {
            Log("[MW2VR] could not form a cg_gun_x/y/z code window.\r\n");
            return nullptr;
        }

        Log("[MW2VR] smallest cg_gun_x/y/z window %08X..%08X span=0x%X.\r\n",
            static_cast<unsigned>(window.begin),
            static_cast<unsigned>(window.end),
            static_cast<unsigned>(window.span));

        if (window.span > 0x1800u)
        {
            Log("[MW2VR] cg_gun_x/y/z window is too wide for one offset routine; refusing hook.\r\n");
            return nullptr;
        }

        const std::uintptr_t textBegin =
            reinterpret_cast<std::uintptr_t>(layout.text.begin);

        // First try only the code immediately after the last X/Y/Z read.
        auto candidates = FindSubmitCandidates(
            layout,
            window,
            drawRefs,
            window.end,
            window.end + 0x1800u);
        if (auto* chosen = ChooseCandidate(candidates, "post-axis scan"))
        {
            return chosen;
        }

        // If nothing survived, include a small amount of code before the axis
        // cluster. This is intentionally much narrower than the failed
        // all-dvar 0x13086 window from the user's executable.
        const std::uintptr_t fallbackBegin =
            window.begin > 0x700u ? window.begin - 0x700u : textBegin;
        candidates = FindSubmitCandidates(
            layout,
            window,
            drawRefs,
            fallbackBegin,
            window.end + 0x2400u);
        return ChooseCandidate(candidates, "axis-local fallback scan");
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

    std::uint8_t* callsite = LocateAddPlayerWeaponCallsite_AxisCluster(layout);
    if (!callsite)
    {
        Log("[MW2VR] live viewmodel hook NOT installed; axis-cluster executable validation failed safely.\r\n");
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

    Log("[MW2VR] LIVE HOOK INSTALLED: cg_gun_x/y/z axis-cluster locator accepted CG_AddPlayerWeapon and the viewmodel now passes through the VR transform.\r\n");
    return 1;
}

extern "C" __declspec(dllexport)
std::uintptr_t __cdecl MW2VR_GetOriginalAddPlayerWeaponAddress()
{
    return reinterpret_cast<std::uintptr_t>(
        gOriginalAddPlayerWeapon_MW2VRShared);
}
