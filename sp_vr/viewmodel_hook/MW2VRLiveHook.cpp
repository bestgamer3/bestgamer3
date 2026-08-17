#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

extern "C" bool __cdecl MW2VR_ApplyWeaponPlacement(
    int weaponIndex,
    const float cameraOrigin[3],
    const float cameraAxis[3][3],
    float weaponOrigin[3],
    float weaponAxis[3][3]);

extern "C" void __cdecl MW2VR_ResetWeaponBaselines();

namespace
{
    struct GfxPlacement
    {
        float quat[4];
        float origin[3];
    };

    struct GfxScaledPlacement
    {
        GfxPlacement base;
        float scale;
    };

    static_assert(sizeof(GfxScaledPlacement) == 0x20);

    struct RefdefProbe
    {
        std::int32_t x;
        std::int32_t y;
        std::int32_t width;
        std::int32_t height;
        float tanHalfFovX;
        float tanHalfFovY;
        float org[3];
        float axis[3][3];
        float zNear;
        float viewOffset[3];
        std::int32_t time;
    };

    static_assert(offsetof(RefdefProbe, org) == 24);
    static_assert(offsetof(RefdefProbe, axis) == 36);
    static_assert(sizeof(RefdefProbe) == 92);

    struct SectionRange
    {
        std::uint8_t* begin = nullptr;
        std::size_t size = 0;
        DWORD characteristics = 0;
    };

    struct ModuleLayout
    {
        std::uint8_t* base = nullptr;
        std::size_t size = 0;
        SectionRange text{};
        std::vector<SectionRange> sections;
    };

    using CG_AddPlayerWeapon_t = void(__cdecl*)(
        int localClientNum,
        const GfxScaledPlacement* placement,
        const void* playerState,
        void* centity,
        int drawGun);

    CG_AddPlayerWeapon_t gOriginalAddPlayerWeapon = nullptr;
    std::uint8_t* gPatchedCallsite = nullptr;
    std::array<std::uint8_t, 5> gOriginalCallBytes{};
    std::atomic<std::uintptr_t> gRefdefAddress{0u};
    std::atomic<bool> gHookInstalled{false};
    std::atomic<bool> gLoggedFirstTransform{false};
    std::mutex gInstallMutex;

    void Log(const char* format, ...)
    {
        char text[1536]{};
        va_list args;
        va_start(args, format);
        _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
        va_end(args);

        OutputDebugStringA(text);

        HANDLE file = CreateFileW(
            L"MW2VRViewmodelHook.log",
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
            CloseHandle(file);
        }
    }

    bool BuildModuleLayout(ModuleLayout& layout)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        if (!module)
        {
            return false;
        }

        auto* base = reinterpret_cast<std::uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return false;
        }

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            base + static_cast<std::size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            return false;
        }

        layout = {};
        layout.base = base;
        layout.size = nt->OptionalHeader.SizeOfImage;

        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index)
        {
            SectionRange range{};
            range.begin = base + section[index].VirtualAddress;
            range.size = std::max<std::size_t>(
                section[index].Misc.VirtualSize,
                section[index].SizeOfRawData);
            range.characteristics = section[index].Characteristics;
            layout.sections.push_back(range);

            char name[9]{};
            std::memcpy(name, section[index].Name, 8);
            if (std::strcmp(name, ".text") == 0)
            {
                layout.text = range;
            }
        }

        return layout.text.begin != nullptr && layout.text.size > 0;
    }

    bool AddressInside(const SectionRange& range, std::uintptr_t address)
    {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.begin);
        return range.begin && address >= begin && address < begin + range.size;
    }

    bool AddressInsideModule(const ModuleLayout& layout, std::uintptr_t address)
    {
        const auto begin = reinterpret_cast<std::uintptr_t>(layout.base);
        return address >= begin && address < begin + layout.size;
    }

    bool AddressInWritableSection(const ModuleLayout& layout, std::uintptr_t address)
    {
        for (const SectionRange& section : layout.sections)
        {
            if ((section.characteristics & IMAGE_SCN_MEM_WRITE) != 0 &&
                AddressInside(section, address))
            {
                return true;
            }
        }
        return false;
    }

    std::uint32_t ReadU32(const std::uint8_t* address)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }

    std::vector<std::uintptr_t> FindAscii(
        const ModuleLayout& layout,
        const char* text)
    {
        std::vector<std::uintptr_t> matches;
        const std::size_t length = std::strlen(text) + 1;
        for (const SectionRange& section : layout.sections)
        {
            if ((section.characteristics & IMAGE_SCN_MEM_READ) == 0 ||
                section.size < length)
            {
                continue;
            }

            for (std::size_t offset = 0; offset + length <= section.size; ++offset)
            {
                if (std::memcmp(section.begin + offset, text, length) == 0)
                {
                    matches.push_back(
                        reinterpret_cast<std::uintptr_t>(section.begin + offset));
                }
            }
        }
        return matches;
    }

    bool IsAbsoluteGlobalRead(
        const std::uint8_t* code,
        std::size_t remaining,
        std::uint32_t globalAddress,
        std::size_t& instructionLength)
    {
        instructionLength = 0;
        if (remaining >= 5 && code[0] == 0xA1 && ReadU32(code + 1) == globalAddress)
        {
            instructionLength = 5;
            return true;
        }

        if (remaining >= 6 && code[0] == 0x8B &&
            (code[1] & 0xC7u) == 0x05u && ReadU32(code + 2) == globalAddress)
        {
            instructionLength = 6;
            return true;
        }

        if (remaining >= 6 &&
            (code[0] == 0xD8 || code[0] == 0xD9 || code[0] == 0xDA ||
             code[0] == 0xDB || code[0] == 0xDC || code[0] == 0xDD) &&
            (code[1] & 0xC7u) == 0x05u && ReadU32(code + 2) == globalAddress)
        {
            instructionLength = 6;
            return true;
        }

        if (remaining >= 6 && code[0] == 0xFF && code[1] == 0x35 &&
            ReadU32(code + 2) == globalAddress)
        {
            instructionLength = 6;
            return true;
        }

        if (remaining >= 7 &&
            (code[0] == 0x80 || code[0] == 0x81 || code[0] == 0x83) &&
            code[1] == 0x3D && ReadU32(code + 2) == globalAddress)
        {
            instructionLength = code[0] == 0x81 ? 10 : 7;
            return true;
        }

        return false;
    }

    std::vector<std::uintptr_t> FindGlobalReads(
        const ModuleLayout& layout,
        std::uintptr_t globalAddress)
    {
        std::vector<std::uintptr_t> refs;
        const auto global32 = static_cast<std::uint32_t>(globalAddress);
        for (std::size_t offset = 0; offset < layout.text.size; ++offset)
        {
            std::size_t length = 0;
            if (IsAbsoluteGlobalRead(
                    layout.text.begin + offset,
                    layout.text.size - offset,
                    global32,
                    length))
            {
                refs.push_back(
                    reinterpret_cast<std::uintptr_t>(layout.text.begin + offset));
                if (length > 0)
                {
                    offset += length - 1;
                }
            }
        }
        return refs;
    }

    std::uintptr_t ResolveDvarGlobal(
        const ModuleLayout& layout,
        const char* dvarName)
    {
        const auto strings = FindAscii(layout, dvarName);
        if (strings.size() != 1)
        {
            Log("[MW2VR] dvar string '%s' expected once, found %u.\r\n",
                dvarName,
                static_cast<unsigned>(strings.size()));
            return 0;
        }

        const std::uint32_t string32 = static_cast<std::uint32_t>(strings.front());
        std::vector<std::uintptr_t> candidates;

        for (std::size_t offset = 0; offset + 5 <= layout.text.size; ++offset)
        {
            const std::uint8_t* instruction = layout.text.begin + offset;
            if (instruction[0] != 0x68 || ReadU32(instruction + 1) != string32)
            {
                continue;
            }

            bool sawCall = false;
            const std::size_t limit = std::min(layout.text.size, offset + 180u);
            for (std::size_t cursor = offset + 5; cursor + 6 <= limit; ++cursor)
            {
                const std::uint8_t* code = layout.text.begin + cursor;
                if (code[0] == 0xE8)
                {
                    sawCall = true;
                    continue;
                }
                if (!sawCall)
                {
                    continue;
                }

                std::uintptr_t destination = 0;
                if (code[0] == 0xA3)
                {
                    destination = ReadU32(code + 1);
                }
                else if (code[0] == 0x89 && code[1] == 0x05)
                {
                    destination = ReadU32(code + 2);
                }

                if (destination && AddressInWritableSection(layout, destination))
                {
                    const auto reads = FindGlobalReads(layout, destination);
                    if (!reads.empty())
                    {
                        candidates.push_back(destination);
                    }
                    break;
                }
            }
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        if (candidates.size() != 1)
        {
            Log("[MW2VR] dvar global '%s' expected one candidate, found %u.\r\n",
                dvarName,
                static_cast<unsigned>(candidates.size()));
            return 0;
        }
        return candidates.front();
    }

    std::uintptr_t FirstRefAfter(
        const std::vector<std::uintptr_t>& refs,
        std::uintptr_t start,
        std::uintptr_t end)
    {
        for (const std::uintptr_t ref : refs)
        {
            if (ref >= start && ref <= end)
            {
                return ref;
            }
        }
        return 0;
    }

    bool HasCallerCleanup20(const std::uint8_t* call, const SectionRange& text)
    {
        const auto textEnd = reinterpret_cast<std::uintptr_t>(text.begin) + text.size;
        const auto afterAddress = reinterpret_cast<std::uintptr_t>(call + 5);
        if (afterAddress + 7 > textEnd)
        {
            return false;
        }
        const std::uint8_t* after = call + 5;
        return (after[0] == 0x83 && after[1] == 0xC4 && after[2] == 0x14) ||
               (after[0] == 0x81 && after[1] == 0xC4 &&
                after[2] == 0x14 && after[3] == 0x00 &&
                after[4] == 0x00 && after[5] == 0x00);
    }

    int CountLikelyPushes(const std::uint8_t* begin, const std::uint8_t* end)
    {
        int count = 0;
        for (const std::uint8_t* p = begin; p < end; ++p)
        {
            if ((*p >= 0x50 && *p <= 0x57) || *p == 0x68 || *p == 0x6A)
            {
                ++count;
            }
            else if (*p == 0xFF && p + 1 < end && ((p[1] >> 3) & 7) == 6)
            {
                ++count;
            }
        }
        return count;
    }

    std::uint8_t* LocateAddPlayerWeaponCallsite(const ModuleLayout& layout)
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

        struct Cluster
        {
            std::uintptr_t draw = 0;
            std::uintptr_t x = 0;
            std::uintptr_t y = 0;
            std::uintptr_t z = 0;
            std::uintptr_t span = std::numeric_limits<std::uintptr_t>::max();
        };

        std::vector<Cluster> clusters;
        for (const std::uintptr_t drawRef : drawRefs)
        {
            const std::uintptr_t end = drawRef + 0x1800u;
            const std::uintptr_t x = FirstRefAfter(xRefs, drawRef, end);
            const std::uintptr_t y = FirstRefAfter(yRefs, drawRef, end);
            const std::uintptr_t z = FirstRefAfter(zRefs, drawRef, end);
            if (!x || !y || !z)
            {
                continue;
            }
            const std::uintptr_t maximum = std::max({drawRef, x, y, z});
            const std::uintptr_t minimum = std::min({drawRef, x, y, z});
            if (maximum - minimum <= 0x1200u)
            {
                clusters.push_back({drawRef, x, y, z, maximum - minimum});
            }
        }

        if (clusters.empty())
        {
            Log("[MW2VR] no unique cg_drawGun/cg_gun_x/y/z read cluster found.\r\n");
            return nullptr;
        }

        std::sort(clusters.begin(), clusters.end(),
            [](const Cluster& a, const Cluster& b) { return a.span < b.span; });
        if (clusters.size() > 1 && clusters[0].span == clusters[1].span &&
            clusters[0].draw != clusters[1].draw)
        {
            Log("[MW2VR] ambiguous CG_AddViewWeapon clusters; refusing hook.\r\n");
            return nullptr;
        }

        const Cluster& cluster = clusters.front();
        const std::uintptr_t lastGunRef = std::max({cluster.x, cluster.y, cluster.z});
        const std::uintptr_t textBegin = reinterpret_cast<std::uintptr_t>(layout.text.begin);
        const std::uintptr_t textEnd = textBegin + layout.text.size;
        const std::uintptr_t scanEnd = std::min(textEnd, lastGunRef + 0x900u);

        std::vector<std::uint8_t*> calls;
        for (std::uintptr_t address = lastGunRef; address + 8 < scanEnd; ++address)
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

            const std::uintptr_t pushStartAddress =
                address > textBegin + 96 ? address - 96 : textBegin;
            const int pushes = CountLikelyPushes(
                reinterpret_cast<const std::uint8_t*>(pushStartAddress), call);
            if (pushes >= 4)
            {
                calls.push_back(call);
            }
        }

        if (calls.size() != 1)
        {
            Log("[MW2VR] expected one five-argument viewmodel submit call, found %u; refusing hook.\r\n",
                static_cast<unsigned>(calls.size()));
            return nullptr;
        }

        Log("[MW2VR] verified CG_AddPlayerWeapon callsite at %08X.\r\n",
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(calls.front())));
        return calls.front();
    }

    float Dot3(const float a[3], const float b[3])
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    float Length3(const float v[3])
    {
        return std::sqrt(Dot3(v, v));
    }

    bool Finite3(const float v[3])
    {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) &&
               std::abs(v[0]) < 100000000.0f &&
               std::abs(v[1]) < 100000000.0f &&
               std::abs(v[2]) < 100000000.0f;
    }

    bool LooksLikeRefdef(const RefdefProbe& r, int clientWidth, int clientHeight, int& score)
    {
        score = 0;
        if (r.width < 320 || r.width > 16384 || r.height < 200 || r.height > 16384)
        {
            return false;
        }
        if (!std::isfinite(r.tanHalfFovX) || !std::isfinite(r.tanHalfFovY) ||
            r.tanHalfFovX < 0.1f || r.tanHalfFovX > 8.0f ||
            r.tanHalfFovY < 0.1f || r.tanHalfFovY > 8.0f || !Finite3(r.org))
        {
            return false;
        }
        for (const auto& row : r.axis)
        {
            if (!Finite3(row))
            {
                return false;
            }
            const float length = Length3(row);
            if (length < 0.85f || length > 1.15f)
            {
                return false;
            }
        }
        if (std::abs(Dot3(r.axis[0], r.axis[1])) > 0.12f ||
            std::abs(Dot3(r.axis[0], r.axis[2])) > 0.12f ||
            std::abs(Dot3(r.axis[1], r.axis[2])) > 0.12f)
        {
            return false;
        }
        if (!std::isfinite(r.zNear) || r.zNear <= 0.0001f || r.zNear > 100.0f)
        {
            return false;
        }

        score += 10;
        if (std::abs(r.x) <= 16 && std::abs(r.y) <= 16)
        {
            score += 2;
        }
        const float renderAspect = static_cast<float>(r.width) / static_cast<float>(r.height);
        const float fovAspect = r.tanHalfFovX / r.tanHalfFovY;
        const float aspectError = std::abs(renderAspect - fovAspect) /
                                  std::max(renderAspect, 0.001f);
        if (aspectError < 0.12f)
        {
            score += 4;
        }
        else if (aspectError > 0.45f)
        {
            return false;
        }

        if (clientWidth > 0 && clientHeight > 0)
        {
            if (std::abs(r.width - clientWidth) <= 8 && std::abs(r.height - clientHeight) <= 8)
            {
                score += 8;
            }
            else
            {
                const float clientAspect = static_cast<float>(clientWidth) /
                                           static_cast<float>(clientHeight);
                if (std::abs(clientAspect - renderAspect) < 0.03f)
                {
                    score += 3;
                }
            }
        }
        if (r.time >= 0)
        {
            score += 1;
        }
        return true;
    }

    struct WindowSearch
    {
        DWORD pid = 0;
        HWND hwnd = nullptr;
    };

    BOOL CALLBACK FindWindowCallback(HWND hwnd, LPARAM value)
    {
        auto* search = reinterpret_cast<WindowSearch*>(value);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        {
            return TRUE;
        }
        RECT client{};
        if (!GetClientRect(hwnd, &client) || client.right - client.left < 320 ||
            client.bottom - client.top < 200)
        {
            return TRUE;
        }
        search->hwnd = hwnd;
        return FALSE;
    }

    std::uintptr_t LocateRefdef(const ModuleLayout& layout)
    {
        int clientWidth = 0;
        int clientHeight = 0;
        WindowSearch search{GetCurrentProcessId(), nullptr};
        EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
        if (search.hwnd)
        {
            RECT client{};
            if (GetClientRect(search.hwnd, &client))
            {
                clientWidth = client.right - client.left;
                clientHeight = client.bottom - client.top;
            }
        }

        struct Candidate
        {
            std::uintptr_t address = 0;
            RefdefProbe probe{};
            int score = 0;
        };
        std::vector<Candidate> candidates;

        const std::uintptr_t imageBegin = reinterpret_cast<std::uintptr_t>(layout.base);
        const std::uintptr_t imageEnd = imageBegin + layout.size;
        std::uintptr_t cursor = imageBegin;
        HANDLE process = GetCurrentProcess();

        while (cursor < imageEnd)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)))
            {
                break;
            }
            const std::uintptr_t regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uintptr_t regionEnd = std::min(
                imageEnd, regionBegin + static_cast<std::uintptr_t>(mbi.RegionSize));
            const DWORD writableMask = PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            const bool usable = mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
                (mbi.Protect & writableMask) != 0;

            if (usable && regionEnd > regionBegin &&
                regionEnd - regionBegin >= sizeof(RefdefProbe))
            {
                const std::size_t regionSize = static_cast<std::size_t>(regionEnd - regionBegin);
                std::vector<std::uint8_t> bytes(regionSize);
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(regionBegin),
                        bytes.data(), bytes.size(), &bytesRead) &&
                    bytesRead >= sizeof(RefdefProbe))
                {
                    const std::size_t limit = static_cast<std::size_t>(bytesRead) - sizeof(RefdefProbe);
                    for (std::size_t offset = 0; offset <= limit; offset += 4)
                    {
                        RefdefProbe probe{};
                        std::memcpy(&probe, bytes.data() + offset, sizeof(probe));
                        int score = 0;
                        if (LooksLikeRefdef(probe, clientWidth, clientHeight, score))
                        {
                            candidates.push_back({regionBegin + offset, probe, score});
                        }
                    }
                }
            }

            if (regionEnd <= cursor)
            {
                break;
            }
            cursor = regionEnd;
        }

        if (candidates.empty())
        {
            return 0;
        }

        Sleep(70);
        for (Candidate& candidate : candidates)
        {
            RefdefProbe second{};
            SIZE_T bytesRead = 0;
            int structuralScore = 0;
            if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(candidate.address),
                    &second, sizeof(second), &bytesRead) || bytesRead != sizeof(second) ||
                !LooksLikeRefdef(second, clientWidth, clientHeight, structuralScore))
            {
                candidate.score = std::numeric_limits<int>::min();
                continue;
            }
            candidate.score = std::max(candidate.score, structuralScore);
            if (second.time > candidate.probe.time)
            {
                candidate.score += 12;
            }
            else if (second.time == candidate.probe.time)
            {
                candidate.score += 1;
            }
            else
            {
                candidate.score -= 8;
            }
            candidate.probe = second;
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        if (candidates.front().score < 22)
        {
            return 0;
        }
        if (candidates.size() > 1 &&
            candidates[1].score >= candidates.front().score - 1 &&
            candidates[1].address != candidates.front().address)
        {
            return 0;
        }
        return candidates.front().address;
    }

    bool ReadCurrentCamera(float origin[3], float axis[3][3])
    {
        const std::uintptr_t address = gRefdefAddress.load(std::memory_order_acquire);
        if (!address)
        {
            return false;
        }
        RefdefProbe probe{};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
                &probe, sizeof(probe), &bytesRead) || bytesRead != sizeof(probe))
        {
            gRefdefAddress.store(0u, std::memory_order_release);
            return false;
        }
        int score = 0;
        if (!LooksLikeRefdef(probe, 0, 0, score))
        {
            gRefdefAddress.store(0u, std::memory_order_release);
            return false;
        }
        std::memcpy(origin, probe.org, sizeof(probe.org));
        std::memcpy(axis, probe.axis, sizeof(probe.axis));
        return true;
    }

    void QuatToAxis(const float q[4], float axis[3][3])
    {
        float x = q[0];
        float y = q[1];
        float z = q[2];
        float w = q[3];
        const float length = std::sqrt(x * x + y * y + z * z + w * w);
        if (length > 0.000001f)
        {
            x /= length;
            y /= length;
            z /= length;
            w /= length;
        }
        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float xw = x * w;
        const float yw = y * w;
        const float zw = z * w;

        axis[0][0] = 1.0f - 2.0f * (yy + zz);
        axis[0][1] = 2.0f * (xy + zw);
        axis[0][2] = 2.0f * (xz - yw);
        axis[1][0] = 2.0f * (xy - zw);
        axis[1][1] = 1.0f - 2.0f * (xx + zz);
        axis[1][2] = 2.0f * (yz + xw);
        axis[2][0] = 2.0f * (xz + yw);
        axis[2][1] = 2.0f * (yz - xw);
        axis[2][2] = 1.0f - 2.0f * (xx + yy);
    }

    void AxisToQuat(const float m[3][3], float q[4])
    {
        const float trace = m[0][0] + m[1][1] + m[2][2];
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q[3] = 0.25f * s;
            q[0] = (m[1][2] - m[2][1]) / s;
            q[1] = (m[2][0] - m[0][2]) / s;
            q[2] = (m[0][1] - m[1][0]) / s;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
            q[3] = (m[1][2] - m[2][1]) / s;
            q[0] = 0.25f * s;
            q[1] = (m[0][1] + m[1][0]) / s;
            q[2] = (m[0][2] + m[2][0]) / s;
        }
        else if (m[1][1] > m[2][2])
        {
            const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
            q[3] = (m[2][0] - m[0][2]) / s;
            q[0] = (m[0][1] + m[1][0]) / s;
            q[1] = 0.25f * s;
            q[2] = (m[1][2] + m[2][1]) / s;
        }
        else
        {
            const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
            q[3] = (m[0][1] - m[1][0]) / s;
            q[0] = (m[0][2] + m[2][0]) / s;
            q[1] = (m[1][2] + m[2][1]) / s;
            q[2] = 0.25f * s;
        }
        const float length = std::sqrt(
            q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (length > 0.000001f)
        {
            q[0] /= length;
            q[1] /= length;
            q[2] /= length;
            q[3] /= length;
        }
    }

    void __cdecl HookedAddPlayerWeapon(
        int localClientNum,
        const GfxScaledPlacement* placement,
        const void* playerState,
        void* centity,
        int drawGun)
    {
        if (!gOriginalAddPlayerWeapon)
        {
            return;
        }

        const GfxScaledPlacement* submittedPlacement = placement;
        GfxScaledPlacement vrPlacement{};

        if (placement && drawGun)
        {
            float cameraOrigin[3]{};
            float cameraAxis[3][3]{};
            if (ReadCurrentCamera(cameraOrigin, cameraAxis))
            {
                vrPlacement = *placement;
                float weaponAxis[3][3]{};
                QuatToAxis(vrPlacement.base.quat, weaponAxis);

                // Until the stock SP weapon-index getter is independently
                // verified, use one safe root baseline. IW4 viewmodels share
                // the hand/root placement convention, so this is enough to
                // prove real controller-driven viewmodel motion without
                // dereferencing an unverified playerState layout.
                constexpr int kGenericViewmodelBaseline = 1;
                if (MW2VR_ApplyWeaponPlacement(
                        kGenericViewmodelBaseline,
                        cameraOrigin,
                        cameraAxis,
                        vrPlacement.base.origin,
                        weaponAxis))
                {
                    AxisToQuat(weaponAxis, vrPlacement.base.quat);
                    submittedPlacement = &vrPlacement;
                    if (!gLoggedFirstTransform.exchange(true))
                    {
                        Log("[MW2VR] first live controller transform applied to the IW4 first-person viewmodel.\r\n");
                    }
                }
            }
        }

        gOriginalAddPlayerWeapon(
            localClientNum,
            submittedPlacement,
            playerState,
            centity,
            drawGun);
    }

    DWORD WINAPI CameraResolverThread(LPVOID)
    {
        ModuleLayout layout{};
        if (!BuildModuleLayout(layout))
        {
            return 0;
        }
        while (gHookInstalled.load(std::memory_order_acquire))
        {
            if (!gRefdefAddress.load(std::memory_order_acquire))
            {
                const std::uintptr_t address = LocateRefdef(layout);
                if (address)
                {
                    gRefdefAddress.store(address, std::memory_order_release);
                    MW2VR_ResetWeaponBaselines();
                    Log("[MW2VR] high-confidence IW4 refdef camera found at %08X.\r\n",
                        static_cast<unsigned>(address));
                }
            }
            Sleep(500);
        }
        return 0;
    }

    bool PatchCallsite(std::uint8_t* callsite)
    {
        if (!callsite || callsite[0] != 0xE8)
        {
            return false;
        }

        std::int32_t originalRelative = 0;
        std::memcpy(&originalRelative, callsite + 1, sizeof(originalRelative));
        const std::uintptr_t originalTarget =
            reinterpret_cast<std::uintptr_t>(callsite + 5) + originalRelative;
        gOriginalAddPlayerWeapon =
            reinterpret_cast<CG_AddPlayerWeapon_t>(originalTarget);

        const std::intptr_t newRelativeWide =
            reinterpret_cast<std::intptr_t>(&HookedAddPlayerWeapon) -
            reinterpret_cast<std::intptr_t>(callsite + 5);
        if (newRelativeWide < std::numeric_limits<std::int32_t>::min() ||
            newRelativeWide > std::numeric_limits<std::int32_t>::max())
        {
            gOriginalAddPlayerWeapon = nullptr;
            return false;
        }
        const std::int32_t newRelative = static_cast<std::int32_t>(newRelativeWide);

        std::memcpy(gOriginalCallBytes.data(), callsite, gOriginalCallBytes.size());
        DWORD oldProtect = 0;
        if (!VirtualProtect(callsite, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            gOriginalAddPlayerWeapon = nullptr;
            return false;
        }
        std::memcpy(callsite + 1, &newRelative, sizeof(newRelative));
        FlushInstructionCache(GetCurrentProcess(), callsite, 5);
        DWORD ignored = 0;
        VirtualProtect(callsite, 5, oldProtect, &ignored);
        gPatchedCallsite = callsite;
        return true;
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

    std::uint8_t* callsite = LocateAddPlayerWeaponCallsite(layout);
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

    HANDLE resolver = CreateThread(nullptr, 0, CameraResolverThread, nullptr, 0, nullptr);
    if (resolver)
    {
        CloseHandle(resolver);
    }

    Log("[MW2VR] LIVE HOOK INSTALLED: CG_AddPlayerWeapon submission now passes through the VR transform.\r\n");
    return 1;
}
