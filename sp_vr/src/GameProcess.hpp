#pragma once

#include <windows.h>

#include <array>
#include <cstdint>

class GameProcess
{
public:
    ~GameProcess();

    bool Attach();
    bool IsAlive() const;
    bool ApplyFov(float fov);

    DWORD Pid() const { return pid_; }
    HWND MainWindow() const;

    // Locates the active IW4 refdef using structural validation only. No
    // translation is written unless a high-confidence candidate is found.
    bool EnsureRefdefLocated();

    // HMD-local translation: +right, +up, +forward in meters.
    // unitsPerMeter controls world scale and defaults to an experimental IW
    // scale selected by the caller.
    bool ApplyHeadTranslation(float rightMeters, float upMeters,
                              float forwardMeters, float unitsPerMeter);
    void ClearHeadTranslation();

    static bool StartGameIfNeeded();

private:
    bool ReadU32(uintptr_t address, uint32_t& value) const;
    bool ReadMemory(uintptr_t address, void* data, std::size_t size) const;
    bool WriteMemory(uintptr_t address, const void* data, std::size_t size) const;
    bool WriteFloat(uintptr_t address, float value) const;
    bool WriteDvarFloat(uintptr_t pointerOffset, float value);
    bool LocateRefdef();

    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
    uintptr_t base_ = 0;
    std::size_t moduleSize_ = 0;

    uintptr_t refdefAddress_ = 0;
    bool lastTranslationValid_ = false;
    std::array<float, 3> lastWrittenOrigin_{};
    std::array<float, 3> lastAppliedOffset_{};
};
