#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    constexpr wchar_t kGameExe[] = L"iw4sp.exe";
    constexpr wchar_t kHookDll[] = L"MW2VRViewmodelHook.dll";

    DWORD FindProcessId()
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
                if (_wcsicmp(entry.szExeFile, kGameExe) == 0)
                {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return pid;
    }

    std::uintptr_t FindRemoteModule(DWORD pid, const wchar_t* moduleName)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        std::uintptr_t result = 0;
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, moduleName) == 0)
                {
                    result = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    bool WaitThread(HANDLE thread, DWORD& exitCode)
    {
        if (WaitForSingleObject(thread, 15000) != WAIT_OBJECT_0)
        {
            return false;
        }
        return GetExitCodeThread(thread, &exitCode) != FALSE;
    }

    std::uintptr_t RemoteKernelFunction(
        DWORD pid,
        const char* functionName)
    {
        HMODULE localKernel = GetModuleHandleW(L"kernel32.dll");
        if (!localKernel)
        {
            return 0;
        }
        FARPROC localFunction = GetProcAddress(localKernel, functionName);
        if (!localFunction)
        {
            return 0;
        }

        const std::uintptr_t remoteKernel =
            FindRemoteModule(pid, L"kernel32.dll");
        if (!remoteKernel)
        {
            return 0;
        }

        const std::uintptr_t rva =
            reinterpret_cast<std::uintptr_t>(localFunction) -
            reinterpret_cast<std::uintptr_t>(localKernel);
        return remoteKernel + rva;
    }

    std::uintptr_t LoadDllRemote(
        HANDLE process,
        DWORD pid,
        const std::filesystem::path& dllPath)
    {
        const std::wstring path = dllPath.wstring();
        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);

        void* remotePath = VirtualAllocEx(
            process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath)
        {
            return 0;
        }

        SIZE_T written = 0;
        if (!WriteProcessMemory(
                process, remotePath, path.c_str(), bytes, &written) ||
            written != bytes)
        {
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return 0;
        }

        const std::uintptr_t remoteLoadLibrary =
            RemoteKernelFunction(pid, "LoadLibraryW");
        if (!remoteLoadLibrary)
        {
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return 0;
        }

        HANDLE thread = CreateRemoteThread(
            process,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoadLibrary),
            remotePath,
            0,
            nullptr);
        if (!thread)
        {
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return 0;
        }

        DWORD exitCode = 0;
        const bool completed = WaitThread(thread, exitCode);
        CloseHandle(thread);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        if (!completed || exitCode == 0)
        {
            return 0;
        }
        return static_cast<std::uintptr_t>(exitCode);
    }

    std::uintptr_t ExportRva(
        const std::filesystem::path& dllPath,
        const char* exportName)
    {
        HMODULE localModule = LoadLibraryExW(
            dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!localModule)
        {
            return 0;
        }

        FARPROC function = GetProcAddress(localModule, exportName);
        if (!function)
        {
            FreeLibrary(localModule);
            return 0;
        }

        const std::uintptr_t rva =
            reinterpret_cast<std::uintptr_t>(function) -
            reinterpret_cast<std::uintptr_t>(localModule);
        FreeLibrary(localModule);
        return rva;
    }

    bool RunRemoteInstaller(
        HANDLE process,
        std::uintptr_t remoteDll,
        const std::filesystem::path& dllPath)
    {
        const std::uintptr_t installerRva =
            ExportRva(dllPath, "MW2VR_InstallHook");
        if (!installerRva)
        {
            std::cerr << "Could not resolve MW2VR_InstallHook export.\n";
            return false;
        }

        HANDLE thread = CreateRemoteThread(
            process,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteDll + installerRva),
            nullptr,
            0,
            nullptr);
        if (!thread)
        {
            std::cerr << "CreateRemoteThread(MW2VR_InstallHook) failed: "
                      << GetLastError() << "\n";
            return false;
        }

        DWORD exitCode = 0;
        const bool completed = WaitThread(thread, exitCode);
        CloseHandle(thread);
        if (!completed)
        {
            std::cerr << "MW2VR_InstallHook did not finish normally.\n";
            return false;
        }
        if (exitCode == 0)
        {
            std::cerr
                << "The viewmodel hook refused to install because its runtime "
                   "IW4 SP validation did not match. Check MW2VRViewmodelHook.log.\n";
            return false;
        }
        return true;
    }
}

int wmain()
{
    std::wcout
        << L"MW2 VR - Win32 Single-Player Viewmodel Hook Loader\n"
           L"Waiting for iw4sp.exe...\n";

    DWORD pid = 0;
    for (int attempt = 0; attempt < 1200 && !pid; ++attempt)
    {
        pid = FindProcessId();
        if (!pid)
        {
            Sleep(100);
        }
    }
    if (!pid)
    {
        std::cerr << "iw4sp.exe was not found.\n";
        return 1;
    }

    std::filesystem::path dllPath =
        std::filesystem::absolute(std::filesystem::current_path() / kHookDll);
    if (!std::filesystem::exists(dllPath))
    {
        std::wcerr << L"Missing " << dllPath << L"\n";
        return 2;
    }

    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION |
            PROCESS_CREATE_THREAD |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
        FALSE,
        pid);
    if (!process)
    {
        std::cerr << "OpenProcess(iw4sp.exe) failed: " << GetLastError() << "\n";
        return 3;
    }

    std::uintptr_t remoteDll = FindRemoteModule(pid, kHookDll);
    if (!remoteDll)
    {
        remoteDll = LoadDllRemote(process, pid, dllPath);
    }
    if (!remoteDll)
    {
        std::cerr << "Could not load MW2VRViewmodelHook.dll into iw4sp.exe.\n";
        CloseHandle(process);
        return 4;
    }

    const bool installed = RunRemoteInstaller(process, remoteDll, dllPath);
    CloseHandle(process);
    if (!installed)
    {
        return 5;
    }

    std::cout
        << "MW2 VR live weapon-viewmodel hook installed successfully.\n"
           "The actual first-person viewmodel is now routed through the Touch "
           "controller transform when a valid VR pose and IW4 camera are available.\n";
    return 0;
}
