#pragma once

#include <windows.h>
#include <cstdint>

class GameProcess
{
public:
    ~GameProcess();

    bool Attach();
    bool IsAlive() const;
    bool ApplyFov(float fov);

    static bool StartGameIfNeeded();

private:
    bool ReadU32(uintptr_t address, uint32_t& value) const;
    bool WriteFloat(uintptr_t address, float value) const;
    bool WriteDvarFloat(uintptr_t pointerOffset, float value);

    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
    uintptr_t base_ = 0;
};
