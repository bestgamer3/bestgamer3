#include "GameProcess.hpp"

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr wchar_t kGameExe[] = L"iw4sp.exe";
    constexpr wchar_t kSteamLaunchUri[] = L"steam://rungameid/10180";

    // CoD SCZ FoV Changer resolves these as module-base-relative pointer locations.
    constexpr uintptr_t kFovPointerOffset = 0x45E968;
    constexpr uintptr_t kFovScalePointerOffset = 0x45E854;
    constexpr uintptr_t kDvarCurrentValueOffset = 0x10;

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
    static_assert(offsetof(RefdefProbe, zNear) == 72);
    static_assert(offsetof(RefdefProbe, viewOffset) == 76);
    static_assert(offsetof(RefdefProbe, time) == 88);
    static_assert(sizeof(RefdefProbe) == 92);

    void PrintWin32Error(const char* what)
    {
        std::cerr << what << " failed. Win32 error " << GetLastError() << "\n";
    }

    DWORD FindProcessId(const wchar_t* name)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD pid = 0;
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, name) == 0)
                {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return pid;
    }

    bool FindModuleInfo(DWORD pid, const wchar_t* moduleName,
                        uintptr_t& base, std::size_t& size)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, moduleName) == 0)
                {
                    base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                    size = static_cast<std::size_t>(entry.modBaseSize);
                    found = true;
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return found;
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

    bool LooksLikeRefdef(const RefdefProbe& r, int clientWidth, int clientHeight,
                         int& score)
    {
        score = 0;
        if (r.width < 320 || r.width > 16384 ||
            r.height < 200 || r.height > 16384)
        {
            return false;
        }
        if (!std::isfinite(r.tanHalfFovX) || !std::isfinite(r.tanHalfFovY) ||
            r.tanHalfFovX < 0.1f || r.tanHalfFovX > 8.0f ||
            r.tanHalfFovY < 0.1f || r.tanHalfFovY > 8.0f)
        {
            return false;
        }
        if (!Finite3(r.org))
        {
            return false;
        }

        for (const auto& axis : r.axis)
        {
            if (!Finite3(axis))
            {
                return false;
            }
            const float length = Length3(axis);
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

        score += 10; // rigid view axis is the strongest structural signal
        if (std::abs(r.x) <= 16 && std::abs(r.y) <= 16)
        {
            score += 2;
        }

        const float renderAspect = static_cast<float>(r.width) /
                                   static_cast<float>(r.height);
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
            if (std::abs(r.width - clientWidth) <= 8 &&
                std::abs(r.height - clientHeight) <= 8)
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

    BOOL CALLBACK FindWindowCallback(HWND hwnd, LPARAM param)
    {
        auto* search = reinterpret_cast<WindowSearch*>(param);
        DWORD windowPid = 0;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid != search->pid || !IsWindowVisible(hwnd) ||
            GetWindow(hwnd, GW_OWNER) != nullptr)
        {
            return TRUE;
        }

        RECT client{};
        if (!GetClientRect(hwnd, &client) ||
            client.right - client.left < 320 ||
            client.bottom - client.top < 200)
        {
            return TRUE;
        }

        search->hwnd = hwnd;
        return FALSE;
    }
}

GameProcess::~GameProcess()
{
    ClearHeadTranslation();
    if (handle_)
    {
        CloseHandle(handle_);
    }
}

bool GameProcess::StartGameIfNeeded()
{
    if (FindProcessId(kGameExe) != 0)
    {
        std::cout << "MW2 campaign is already running. Attaching to iw4sp.exe...\n";
        return true;
    }

    const auto exePath = std::filesystem::current_path() / kGameExe;
    if (!std::filesystem::exists(exePath))
    {
        std::wcerr << L"Could not find " << kGameExe
                   << L". Put MW2CampaignVR.exe beside iw4sp.exe.\n";
        return false;
    }

    std::wcout << L"Launching MW2 (2009) Single Player through Steam...\n";
    const HINSTANCE launchResult = ShellExecuteW(
        nullptr, L"open", kSteamLaunchUri, nullptr,
        std::filesystem::current_path().c_str(), SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(launchResult) <= 32)
    {
        std::cerr << "Could not open the Steam launch URI. Make sure Steam is installed and registered to handle steam:// links.\n";
        return false;
    }

    std::cout << "Steam launch requested. Waiting for iw4sp.exe...\n";
    return true;
}

bool GameProcess::Attach()
{
    if (handle_)
    {
        return true;
    }

    pid_ = FindProcessId(kGameExe);
    if (!pid_)
    {
        return false;
    }

    if (!FindModuleInfo(pid_, kGameExe, base_, moduleSize_))
    {
        return false;
    }

    handle_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                              PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                          FALSE, pid_);
    if (!handle_)
    {
        PrintWin32Error("OpenProcess(iw4sp.exe)");
        return false;
    }

    std::cout << "Attached to iw4sp.exe (PID " << pid_ << ", base 0x" << std::hex
              << base_ << std::dec << ", image " << moduleSize_ << " bytes).\n";
    return true;
}

bool GameProcess::IsAlive() const
{
    if (!handle_)
    {
        return false;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(handle_, &exitCode) && exitCode == STILL_ACTIVE;
}

HWND GameProcess::MainWindow() const
{
    if (!pid_)
    {
        return nullptr;
    }
    WindowSearch search{pid_, nullptr};
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

bool GameProcess::ReadMemory(uintptr_t address, void* data, std::size_t size) const
{
    SIZE_T bytes = 0;
    return handle_ && ReadProcessMemory(
        handle_, reinterpret_cast<LPCVOID>(address), data, size, &bytes) &&
        bytes == size;
}

bool GameProcess::WriteMemory(uintptr_t address, const void* data,
                              std::size_t size) const
{
    SIZE_T bytes = 0;
    return handle_ && WriteProcessMemory(
        handle_, reinterpret_cast<LPVOID>(address), data, size, &bytes) &&
        bytes == size;
}

bool GameProcess::ReadU32(uintptr_t address, uint32_t& value) const
{
    return ReadMemory(address, &value, sizeof(value));
}

bool GameProcess::WriteFloat(uintptr_t address, float value) const
{
    return WriteMemory(address, &value, sizeof(value));
}

bool GameProcess::WriteDvarFloat(uintptr_t pointerOffset, float value)
{
    uint32_t dvar32 = 0;
    if (!ReadU32(base_ + pointerOffset, dvar32) || dvar32 == 0)
    {
        return false;
    }
    return WriteFloat(static_cast<uintptr_t>(dvar32) + kDvarCurrentValueOffset, value);
}

bool GameProcess::ApplyFov(float fov)
{
    return WriteDvarFloat(kFovPointerOffset, fov) &&
           WriteDvarFloat(kFovScalePointerOffset, 1.0f);
}

bool GameProcess::EnsureRefdefLocated()
{
    if (refdefAddress_)
    {
        RefdefProbe probe{};
        int score = 0;
        if (ReadMemory(refdefAddress_, &probe, sizeof(probe)) &&
            LooksLikeRefdef(probe, 0, 0, score))
        {
            return true;
        }
        refdefAddress_ = 0;
        lastTranslationValid_ = false;
    }
    return LocateRefdef();
}

bool GameProcess::LocateRefdef()
{
    if (!handle_ || !base_ || moduleSize_ < sizeof(RefdefProbe))
    {
        return false;
    }

    int clientWidth = 0;
    int clientHeight = 0;
    if (const HWND hwnd = MainWindow())
    {
        RECT client{};
        if (GetClientRect(hwnd, &client))
        {
            clientWidth = client.right - client.left;
            clientHeight = client.bottom - client.top;
        }
    }

    struct Candidate
    {
        uintptr_t address = 0;
        RefdefProbe probe{};
        int score = 0;
    };
    std::vector<Candidate> candidates;

    const uintptr_t imageEnd = base_ + moduleSize_;
    uintptr_t cursor = base_;
    while (cursor < imageEnd)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(cursor),
                            &mbi, sizeof(mbi)))
        {
            break;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = std::min(
            imageEnd, regionBase + static_cast<uintptr_t>(mbi.RegionSize));

        const DWORD writableMask = PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const bool readableWritable =
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            (mbi.Protect & writableMask) != 0;

        if (readableWritable && regionEnd > regionBase &&
            regionEnd - regionBase >= sizeof(RefdefProbe))
        {
            const std::size_t regionSize =
                static_cast<std::size_t>(regionEnd - regionBase);
            std::vector<std::uint8_t> bytes(regionSize);
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(regionBase),
                                  bytes.data(), bytes.size(), &bytesRead) &&
                bytesRead >= sizeof(RefdefProbe))
            {
                const std::size_t limit =
                    static_cast<std::size_t>(bytesRead) - sizeof(RefdefProbe);
                for (std::size_t offset = 0; offset <= limit; offset += 4)
                {
                    RefdefProbe probe{};
                    std::memcpy(&probe, bytes.data() + offset, sizeof(probe));
                    int score = 0;
                    if (LooksLikeRefdef(probe, clientWidth, clientHeight, score))
                    {
                        candidates.push_back(
                            {regionBase + offset, probe, score});
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
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    for (auto& candidate : candidates)
    {
        RefdefProbe second{};
        int structuralScore = 0;
        if (!ReadMemory(candidate.address, &second, sizeof(second)) ||
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
              [](const Candidate& a, const Candidate& b)
              {
                  return a.score > b.score;
              });

    if (candidates.front().score < 22)
    {
        return false;
    }

    // Avoid writing if two unrelated structures look almost equally likely.
    if (candidates.size() > 1 && candidates[1].score >= candidates.front().score - 1 &&
        candidates[1].address != candidates.front().address)
    {
        return false;
    }

    refdefAddress_ = candidates.front().address;
    lastTranslationValid_ = false;
    std::cout << "6DoF: located high-confidence IW4 refdef at 0x" << std::hex
              << refdefAddress_ << std::dec << " (score "
              << candidates.front().score << ", "
              << candidates.front().probe.width << "x"
              << candidates.front().probe.height << ").\n";
    return true;
}

bool GameProcess::ApplyHeadTranslation(float rightMeters, float upMeters,
                                       float forwardMeters, float unitsPerMeter)
{
    if (!EnsureRefdefLocated() || !std::isfinite(unitsPerMeter) ||
        unitsPerMeter <= 0.0f)
    {
        return false;
    }

    RefdefProbe refdef{};
    int score = 0;
    if (!ReadMemory(refdefAddress_, &refdef, sizeof(refdef)) ||
        !LooksLikeRefdef(refdef, 0, 0, score))
    {
        refdefAddress_ = 0;
        lastTranslationValid_ = false;
        return false;
    }

    std::array<float, 3> baseOrigin = {
        refdef.org[0], refdef.org[1], refdef.org[2]};

    if (lastTranslationValid_)
    {
        const float dx = refdef.org[0] - lastWrittenOrigin_[0];
        const float dy = refdef.org[1] - lastWrittenOrigin_[1];
        const float dz = refdef.org[2] - lastWrittenOrigin_[2];
        const float distanceFromLastWrite = std::sqrt(dx * dx + dy * dy + dz * dz);

        // If IW4 has not regenerated refdef since our last write, first undo
        // our previous offset so positional tracking never accumulates.
        if (distanceFromLastWrite < 0.25f)
        {
            for (std::size_t i = 0; i < 3; ++i)
            {
                baseOrigin[i] -= lastAppliedOffset_[i];
            }
        }
    }

    const float right = rightMeters * unitsPerMeter;
    const float up = upMeters * unitsPerMeter;
    const float forward = forwardMeters * unitsPerMeter;

    std::array<float, 3> worldOffset{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        // IW refdef axis: [0]=forward, [1]=right, [2]=up.
        worldOffset[i] = refdef.axis[1][i] * right +
                         refdef.axis[2][i] * up +
                         refdef.axis[0][i] * forward;
        lastWrittenOrigin_[i] = baseOrigin[i] + worldOffset[i];
    }

    if (!WriteMemory(refdefAddress_ + offsetof(RefdefProbe, org),
                     lastWrittenOrigin_.data(), sizeof(float) * 3))
    {
        return false;
    }

    lastAppliedOffset_ = worldOffset;
    lastTranslationValid_ = true;
    return true;
}

void GameProcess::ClearHeadTranslation()
{
    if (!handle_ || !refdefAddress_ || !lastTranslationValid_)
    {
        lastTranslationValid_ = false;
        return;
    }

    RefdefProbe refdef{};
    if (ReadMemory(refdefAddress_, &refdef, sizeof(refdef)))
    {
        const float dx = refdef.org[0] - lastWrittenOrigin_[0];
        const float dy = refdef.org[1] - lastWrittenOrigin_[1];
        const float dz = refdef.org[2] - lastWrittenOrigin_[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < 0.25f)
        {
            std::array<float, 3> restored{};
            for (std::size_t i = 0; i < 3; ++i)
            {
                restored[i] = refdef.org[i] - lastAppliedOffset_[i];
            }
            WriteMemory(refdefAddress_ + offsetof(RefdefProbe, org),
                        restored.data(), sizeof(float) * 3);
        }
    }
    lastTranslationValid_ = false;
}
