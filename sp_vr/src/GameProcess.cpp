#include "GameProcess.hpp"

#include <tlhelp32.h>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace
{
    constexpr wchar_t kGameExe[] = L"iw4sp.exe";

    // CoD SCZ FoV Changer resolves these as module-base-relative pointer locations.
    // For the standard Steam-era iw4sp.exe base (0x400000), these resolve to
    // 0x85E968 (cg_fov) and 0x85E854 (cg_fovScale).
    constexpr uintptr_t kFovPointerOffset = 0x45E968;
    constexpr uintptr_t kFovScalePointerOffset = 0x45E854;
    constexpr uintptr_t kDvarCurrentValueOffset = 0x10;

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

    uintptr_t FindModuleBase(DWORD pid, const wchar_t* moduleName)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        uintptr_t base = 0;

        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, moduleName) == 0)
                {
                    base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return base;
    }
}

GameProcess::~GameProcess()
{
    if (handle_)
    {
        CloseHandle(handle_);
    }
}

bool GameProcess::StartGameIfNeeded()
{
    if (FindProcessId(kGameExe) != 0)
    {
        return true;
    }

    const auto exePath = std::filesystem::current_path() / kGameExe;
    if (!std::filesystem::exists(exePath))
    {
        std::wcerr << L"Could not find " << kGameExe
                   << L". Put MW2CampaignVR.exe beside iw4sp.exe, or start the campaign first.\n";
        return false;
    }

    std::wstring command = L"\"" + exePath.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        std::filesystem::current_path().c_str(), &si, &pi))
    {
        PrintWin32Error("CreateProcessW(iw4sp.exe)");
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::cout << "Started iw4sp.exe. Waiting for the campaign process...\n";
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

    base_ = FindModuleBase(pid_, kGameExe);
    if (!base_)
    {
        return false;
    }

    handle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ |
                              PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
                          FALSE, pid_);
    if (!handle_)
    {
        PrintWin32Error("OpenProcess(iw4sp.exe)");
        return false;
    }

    std::cout << "Attached to iw4sp.exe (PID " << pid_ << ", base 0x" << std::hex
              << base_ << std::dec << ").\n";
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

bool GameProcess::ReadU32(uintptr_t address, uint32_t& value) const
{
    SIZE_T bytes = 0;
    return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(address), &value,
                             sizeof(value), &bytes) &&
           bytes == sizeof(value);
}

bool GameProcess::WriteFloat(uintptr_t address, float value) const
{
    SIZE_T bytes = 0;
    return WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(address), &value,
                              sizeof(value), &bytes) &&
           bytes == sizeof(value);
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
