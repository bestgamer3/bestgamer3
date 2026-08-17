#include "VRViewmodelProtocol.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using mw2vr::viewmodel::SharedState;

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

    // IW4 expands the DObj bone bitsets from COD4's four words to six words.
    // These offsets are independently validated against every returned object
    // before any hide bits are changed.
    struct Iw4DObj
    {
        void* tree;                        // 0x00
        std::uint16_t duplicateParts;      // 0x04
        std::uint16_t entnum;              // 0x06
        std::uint8_t duplicatePartsSize;   // 0x08
        std::uint8_t numModels;            // 0x09
        std::uint8_t numBones;             // 0x0A
        std::uint8_t flags;                // 0x0B
        std::uint32_t ignoreCollision;      // 0x0C
        volatile std::int32_t locked;       // 0x10
        std::uint8_t skel[0x68];            // 0x14
        float radius;                       // 0x7C
        std::uint32_t hidePartBits[6];      // 0x80
        void** models;                      // 0x98
    };

    static_assert(offsetof(Iw4DObj, hidePartBits) == 0x80);
    static_assert(offsetof(Iw4DObj, models) == 0x98);
    static_assert(sizeof(Iw4DObj) == 0x9C);

    struct XModelPrefix
    {
        const char* name;
        std::uint8_t numBones;
        std::uint8_t numRootBones;
        std::uint8_t numSurfs;
        std::uint8_t lodRampType;
    };

    static_assert(sizeof(XModelPrefix) == 8);

    using GetClientDObj_t = void*(__cdecl*)(int handle, int localClientNum);

    GetClientDObj_t gOriginalGetClientDObj = nullptr;
    std::uint8_t* gPatchedDObjCallsite = nullptr;
    std::array<std::uint8_t, 5> gOriginalDObjCallBytes{};
    std::atomic<bool> gHandsFilterInstalled{false};
    std::atomic<bool> gLoggedFirstFilteredDObj{false};
    std::mutex gInstallMutex;
    std::mutex gFilterMutex;

    struct FilterRecord
    {
        Iw4DObj* object = nullptr;
        void** modelsIdentity = nullptr;
        std::array<std::uint32_t, 6> originalBits{};
        bool valid = false;
    };

    std::array<FilterRecord, 32> gFilterRecords{};
    std::size_t gNextFilterRecord = 0;

    HANDLE gMapping = nullptr;
    const SharedState* gSharedState = nullptr;

    void Log(const char* format, ...)
    {
        char text[2048]{};
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

    bool IsReadableRange(const void* pointer, std::size_t size)
    {
        if (!pointer || size == 0)
        {
            return false;
        }

        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        if (start > std::numeric_limits<std::uintptr_t>::max() - size)
        {
            return false;
        }
        const auto end = start + size;
        std::uintptr_t cursor = start;

        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)))
            {
                return false;
            }
            if (mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            {
                return false;
            }

            const DWORD baseProtect = mbi.Protect & 0xFFu;
            const bool readable =
                baseProtect == PAGE_READONLY ||
                baseProtect == PAGE_READWRITE ||
                baseProtect == PAGE_WRITECOPY ||
                baseProtect == PAGE_EXECUTE_READ ||
                baseProtect == PAGE_EXECUTE_READWRITE ||
                baseProtect == PAGE_EXECUTE_WRITECOPY;
            if (!readable)
            {
                return false;
            }

            const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) +
                static_cast<std::uintptr_t>(mbi.RegionSize);
            if (regionEnd <= cursor)
            {
                return false;
            }
            cursor = std::min(end, regionEnd);
        }
        return true;
    }

    bool IsWritableRange(void* pointer, std::size_t size)
    {
        if (!pointer || size == 0)
        {
            return false;
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(pointer, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const DWORD baseProtect = mbi.Protect & 0xFFu;
        const bool writable =
            baseProtect == PAGE_READWRITE ||
            baseProtect == PAGE_WRITECOPY ||
            baseProtect == PAGE_EXECUTE_READWRITE ||
            baseProtect == PAGE_EXECUTE_WRITECOPY;
        if (!writable)
        {
            return false;
        }

        const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto end = begin + static_cast<std::uintptr_t>(mbi.RegionSize);
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        return address >= begin && address + size <= end;
    }

    bool ReadBoundedAscii(const char* pointer, std::string& value)
    {
        value.clear();
        if (!pointer)
        {
            return false;
        }

        for (std::size_t index = 0; index < 128; ++index)
        {
            const char* current = pointer + index;
            if (!IsReadableRange(current, 1))
            {
                return false;
            }
            const unsigned char ch = static_cast<unsigned char>(*current);
            if (ch == 0)
            {
                return !value.empty();
            }
            if (ch < 0x20u || ch > 0x7Eu)
            {
                return false;
            }
            value.push_back(static_cast<char>(ch));
        }
        return false;
    }

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    bool IsHandOrArmModelName(const std::string& original)
    {
        const std::string name = Lower(original);
        static constexpr const char* tokens[] = {
            "_arms", "arms_", "viewarms", "view_arms",
            "_hands", "hands_", "viewhands", "view_hands",
            "_hand", "hand_", "_glove", "glove_", "viewglove",
        };
        for (const char* token : tokens)
        {
            if (name.find(token) != std::string::npos)
            {
                return true;
            }
        }
        return name == "arms" || name == "hands" || name == "hand" ||
            name == "glove" || name.ends_with("_arms") ||
            name.ends_with("_hands") || name.ends_with("_gloves");
    }

    bool EnsureSharedMapping()
    {
        if (gSharedState)
        {
            return true;
        }

        HANDLE mapping = OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            mw2vr::viewmodel::kMappingName);
        if (!mapping)
        {
            return false;
        }

        const void* mapped = MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0,
            0,
            sizeof(SharedState));
        if (!mapped)
        {
            CloseHandle(mapping);
            return false;
        }

        gMapping = mapping;
        gSharedState = static_cast<const SharedState*>(mapped);
        return true;
    }

    bool ReadSharedSnapshot(SharedState& snapshot)
    {
        if (!EnsureSharedMapping())
        {
            return false;
        }

        for (int attempt = 0; attempt < 5; ++attempt)
        {
            const volatile LONG* sequence =
                reinterpret_cast<const volatile LONG*>(&gSharedState->sequence);
            const LONG before = *sequence;
            if ((before & 1) != 0)
            {
                YieldProcessor();
                continue;
            }

            MemoryBarrier();
            std::memcpy(&snapshot, gSharedState, sizeof(snapshot));
            MemoryBarrier();
            const LONG after = *sequence;

            if (before == after && (after & 1) == 0 &&
                snapshot.magic == mw2vr::viewmodel::kMagic &&
                snapshot.version == mw2vr::viewmodel::kVersion &&
                snapshot.structSize == sizeof(SharedState))
            {
                return true;
            }
        }
        return false;
    }

    bool VrWeaponPoseEnabled()
    {
        SharedState snapshot{};
        if (!ReadSharedSnapshot(snapshot))
        {
            return false;
        }
        const std::uint32_t required =
            mw2vr::viewmodel::FlagEnabled |
            mw2vr::viewmodel::FlagPoseValid;
        return (snapshot.flags & required) == required;
    }

    FilterRecord* FindRecord(Iw4DObj* object, void** modelsIdentity)
    {
        for (FilterRecord& record : gFilterRecords)
        {
            if (record.valid && record.object == object &&
                record.modelsIdentity == modelsIdentity)
            {
                return &record;
            }
        }
        return nullptr;
    }

    FilterRecord& CreateRecord(Iw4DObj* object, void** modelsIdentity)
    {
        for (FilterRecord& record : gFilterRecords)
        {
            if (!record.valid)
            {
                record = {};
                record.object = object;
                record.modelsIdentity = modelsIdentity;
                std::copy(std::begin(object->hidePartBits),
                    std::end(object->hidePartBits), record.originalBits.begin());
                record.valid = true;
                return record;
            }
        }

        FilterRecord& record = gFilterRecords[gNextFilterRecord++ % gFilterRecords.size()];
        record = {};
        record.object = object;
        record.modelsIdentity = modelsIdentity;
        std::copy(std::begin(object->hidePartBits),
            std::end(object->hidePartBits), record.originalBits.begin());
        record.valid = true;
        return record;
    }

    bool ValidateAndDescribeDObj(
        void* pointer,
        Iw4DObj*& object,
        std::vector<std::string>& modelNames,
        std::vector<std::uint8_t>& modelBoneCounts,
        std::vector<bool>& handModels)
    {
        object = nullptr;
        modelNames.clear();
        modelBoneCounts.clear();
        handModels.clear();

        if (!IsReadableRange(pointer, sizeof(Iw4DObj)))
        {
            return false;
        }

        auto* candidate = static_cast<Iw4DObj*>(pointer);
        if (candidate->numModels < 2 || candidate->numModels > 8 ||
            candidate->numBones < 2 || candidate->numBones > 192 ||
            !candidate->models ||
            !IsReadableRange(candidate->models,
                static_cast<std::size_t>(candidate->numModels) * sizeof(void*)))
        {
            return false;
        }

        int totalBones = 0;
        int handCount = 0;
        int nonHandCount = 0;
        for (int index = 0; index < candidate->numModels; ++index)
        {
            void* modelPointer = candidate->models[index];
            if (!IsReadableRange(modelPointer, sizeof(XModelPrefix)))
            {
                return false;
            }

            const auto* model = static_cast<const XModelPrefix*>(modelPointer);
            if (model->numBones == 0 || model->numBones > 160 ||
                model->numRootBones > model->numBones)
            {
                return false;
            }

            std::string modelName;
            if (!ReadBoundedAscii(model->name, modelName))
            {
                return false;
            }

            const bool isHand = IsHandOrArmModelName(modelName);
            handCount += isHand ? 1 : 0;
            nonHandCount += isHand ? 0 : 1;
            totalBones += model->numBones;
            if (totalBones > 192)
            {
                return false;
            }

            modelNames.push_back(modelName);
            modelBoneCounts.push_back(model->numBones);
            handModels.push_back(isHand);
        }

        if (totalBones != candidate->numBones || handCount == 0 || nonHandCount == 0 ||
            !IsWritableRange(candidate->hidePartBits, sizeof(candidate->hidePartBits)))
        {
            return false;
        }

        object = candidate;
        return true;
    }

    void RestoreIfTracked(Iw4DObj* object)
    {
        if (!object || !IsReadableRange(object, sizeof(Iw4DObj)) ||
            !IsWritableRange(object->hidePartBits, sizeof(object->hidePartBits)))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(gFilterMutex);
        FilterRecord* record = FindRecord(object, object->models);
        if (!record)
        {
            return;
        }
        std::copy(record->originalBits.begin(), record->originalBits.end(),
            std::begin(object->hidePartBits));
    }

    bool FilterHandsFromDObj(void* pointer)
    {
        Iw4DObj* object = nullptr;
        std::vector<std::string> names;
        std::vector<std::uint8_t> bones;
        std::vector<bool> handModels;
        if (!ValidateAndDescribeDObj(pointer, object, names, bones, handModels))
        {
            return false;
        }

        if (!VrWeaponPoseEnabled())
        {
            RestoreIfTracked(object);
            return false;
        }

        std::lock_guard<std::mutex> lock(gFilterMutex);
        FilterRecord* record = FindRecord(object, object->models);
        if (!record)
        {
            record = &CreateRecord(object, object->models);
        }

        // Rebuild from the game's original hide mask every time. This preserves
        // stock attachment/animation visibility while making our hand mask
        // deterministic even if another frame temporarily altered the DObj.
        std::array<std::uint32_t, 6> filtered = record->originalBits;
        int globalBone = 0;
        bool hiddenAnything = false;

        for (std::size_t modelIndex = 0; modelIndex < names.size(); ++modelIndex)
        {
            const int startBone = globalBone;
            const int endBone = startBone + bones[modelIndex];
            if (handModels[modelIndex])
            {
                for (int bone = startBone; bone < endBone; ++bone)
                {
                    filtered[static_cast<std::size_t>(bone >> 5)] |=
                        0x80000000u >> (bone & 31);
                }
                hiddenAnything = true;
            }
            globalBone = endBone;
        }

        if (!hiddenAnything)
        {
            return false;
        }

        std::copy(filtered.begin(), filtered.end(), std::begin(object->hidePartBits));

        if (!gLoggedFirstFilteredDObj.exchange(true))
        {
            Log("[MW2VR][HANDS] weapon-only VR filter active: DObj=%p models=%u bones=%u.\r\n",
                object,
                static_cast<unsigned>(object->numModels),
                static_cast<unsigned>(object->numBones));
            int startBone = 0;
            for (std::size_t index = 0; index < names.size(); ++index)
            {
                const int endBone = startBone + bones[index];
                Log("[MW2VR][HANDS] model[%u] '%s' bones %d..%d %s.\r\n",
                    static_cast<unsigned>(index),
                    names[index].c_str(),
                    startBone,
                    endBone - 1,
                    handModels[index] ? "HIDDEN (stock FPS hand/arm)" : "kept");
                startBone = endBone;
            }
        }
        return true;
    }

    void* __cdecl HookedGetClientDObj(int handle, int localClientNum)
    {
        if (!gOriginalGetClientDObj)
        {
            return nullptr;
        }
        void* object = gOriginalGetClientDObj(handle, localClientNum);
        if (object)
        {
            FilterHandsFromDObj(object);
        }
        return object;
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
        return layout.text.begin && layout.text.size > 0;
    }

    bool AddressInside(const SectionRange& range, std::uintptr_t address)
    {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.begin);
        return range.begin && address >= begin && address < begin + range.size;
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

    std::vector<std::uintptr_t> FindAscii(const ModuleLayout& layout, const char* text)
    {
        std::vector<std::uintptr_t> matches;
        const std::size_t length = std::strlen(text) + 1;
        for (const SectionRange& section : layout.sections)
        {
            if ((section.characteristics & IMAGE_SCN_MEM_READ) == 0 || section.size < length)
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

    std::uintptr_t ResolveDvarGlobal(const ModuleLayout& layout, const char* dvarName)
    {
        const auto strings = FindAscii(layout, dvarName);
        if (strings.size() != 1)
        {
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
                    if (!FindGlobalReads(layout, destination).empty())
                    {
                        candidates.push_back(destination);
                    }
                    break;
                }
            }
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        return candidates.size() == 1 ? candidates.front() : 0;
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

    bool HasCallerCleanup(const std::uint8_t* call, const SectionRange& text, int bytes)
    {
        const auto textEnd = reinterpret_cast<std::uintptr_t>(text.begin) + text.size;
        const auto afterAddress = reinterpret_cast<std::uintptr_t>(call + 5);
        if (afterAddress + 7 > textEnd)
        {
            return false;
        }
        const std::uint8_t* after = call + 5;
        return (bytes <= 0x7F && after[0] == 0x83 && after[1] == 0xC4 &&
                after[2] == static_cast<std::uint8_t>(bytes)) ||
               (after[0] == 0x81 && after[1] == 0xC4 &&
                ReadU32(after + 2) == static_cast<std::uint32_t>(bytes));
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
            return nullptr;
        }

        std::sort(clusters.begin(), clusters.end(),
            [](const Cluster& a, const Cluster& b) { return a.span < b.span; });
        if (clusters.size() > 1 && clusters[0].span == clusters[1].span &&
            clusters[0].draw != clusters[1].draw)
        {
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
            if (call[0] != 0xE8 || !HasCallerCleanup(call, layout.text, 20))
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
            const std::uintptr_t pushStart = address > textBegin + 96 ? address - 96 : textBegin;
            if (CountLikelyPushes(
                    reinterpret_cast<const std::uint8_t*>(pushStart), call) >= 4)
            {
                calls.push_back(call);
            }
        }
        return calls.size() == 1 ? calls.front() : nullptr;
    }

    int ScoreDObjGetterCall(
        const ModuleLayout& layout,
        std::uint8_t* call,
        std::uintptr_t target)
    {
        int score = 0;
        const auto textBegin = reinterpret_cast<std::uintptr_t>(layout.text.begin);
        const auto textEnd = textBegin + layout.text.size;
        const auto address = reinterpret_cast<std::uintptr_t>(call);

        const std::uintptr_t pushBegin = address > textBegin + 32 ? address - 32 : textBegin;
        const int pushes = CountLikelyPushes(
            reinterpret_cast<const std::uint8_t*>(pushBegin), call);
        if (pushes >= 2)
        {
            score += 3;
        }

        const std::uint8_t* after = call + 5;
        if (after + 12 < layout.text.begin + layout.text.size)
        {
            if ((after[3] == 0x85 && after[4] == 0xC0) ||
                (after[3] == 0x84 && after[4] == 0xC0))
            {
                score += 4;
            }
            for (int index = 3; index < 16; ++index)
            {
                if (after[index] >= 0x40 && after[index] <= 0x5F)
                {
                    continue;
                }
                if (after[index] == 0x8B && index + 1 < 16 &&
                    (after[index + 1] & 0x07u) == 0)
                {
                    score += 2;
                    break;
                }
            }
        }

        const auto* body = reinterpret_cast<const std::uint8_t*>(target);
        const std::size_t bodySize = static_cast<std::size_t>(
            std::min<std::uintptr_t>(0x220u, textEnd - target));
        if (bodySize >= 3 &&
            ((body[0] == 0x55 && body[1] == 0x8B) ||
             (body[0] == 0x8B && body[1] == 0x44)))
        {
            ++score;
        }

        bool sawReturn = false;
        bool sawStackArg = false;
        for (std::size_t index = 0; index + 4 < bodySize; ++index)
        {
            if (body[index] == 0xC3 || body[index] == 0xC2)
            {
                sawReturn = true;
            }
            if (body[index] == 0x24 &&
                (body[index + 1] == 0x04 || body[index + 1] == 0x08 ||
                 body[index + 1] == 0x0C))
            {
                sawStackArg = true;
            }
        }
        score += sawReturn ? 1 : 0;
        score += sawStackArg ? 1 : 0;
        return score;
    }

    std::uint8_t* LocateGetClientDObjCallsite(
        const ModuleLayout& layout,
        std::uint8_t* addPlayerWeaponCallsite)
    {
        if (!addPlayerWeaponCallsite || addPlayerWeaponCallsite[0] != 0xE8)
        {
            return nullptr;
        }

        std::int32_t relative = 0;
        std::memcpy(&relative, addPlayerWeaponCallsite + 1, sizeof(relative));
        const std::uintptr_t addPlayerWeapon =
            reinterpret_cast<std::uintptr_t>(addPlayerWeaponCallsite + 5) + relative;
        if (!AddressInside(layout.text, addPlayerWeapon))
        {
            return nullptr;
        }

        const auto textEnd = reinterpret_cast<std::uintptr_t>(layout.text.begin) + layout.text.size;
        const auto scanEnd = std::min(textEnd, addPlayerWeapon + 0x1800u);

        struct Candidate
        {
            std::uint8_t* call = nullptr;
            std::uintptr_t target = 0;
            int score = 0;
        };
        std::vector<Candidate> candidates;

        for (std::uintptr_t address = addPlayerWeapon; address + 12 < scanEnd; ++address)
        {
            auto* call = reinterpret_cast<std::uint8_t*>(address);
            if (call[0] != 0xE8 || !HasCallerCleanup(call, layout.text, 8))
            {
                continue;
            }
            std::int32_t callRelative = 0;
            std::memcpy(&callRelative, call + 1, sizeof(callRelative));
            const std::uintptr_t target = address + 5 + callRelative;
            if (!AddressInside(layout.text, target))
            {
                continue;
            }
            const int score = ScoreDObjGetterCall(layout, call, target);
            if (score >= 4)
            {
                candidates.push_back({call, target, score});
            }
        }

        if (candidates.empty())
        {
            Log("[MW2VR][HANDS] no two-argument DObj-getter candidate found inside CG_AddPlayerWeapon.\r\n");
            return nullptr;
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

        Log("[MW2VR][HANDS] DObj getter candidates=%u topScore=%d at %08X -> %08X.\r\n",
            static_cast<unsigned>(candidates.size()),
            candidates.front().score,
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(candidates.front().call)),
            static_cast<unsigned>(candidates.front().target));

        if (candidates.front().score < 6)
        {
            Log("[MW2VR][HANDS] top DObj getter score too low; refusing patch.\r\n");
            return nullptr;
        }
        if (candidates.size() > 1 &&
            candidates[1].score >= candidates.front().score - 1)
        {
            Log("[MW2VR][HANDS] DObj getter locator is ambiguous (%d vs %d); refusing patch.\r\n",
                candidates.front().score, candidates[1].score);
            return nullptr;
        }
        return candidates.front().call;
    }

    bool PatchDObjGetterCallsite(std::uint8_t* callsite)
    {
        if (!callsite || callsite[0] != 0xE8)
        {
            return false;
        }

        std::int32_t originalRelative = 0;
        std::memcpy(&originalRelative, callsite + 1, sizeof(originalRelative));
        const std::uintptr_t originalTarget =
            reinterpret_cast<std::uintptr_t>(callsite + 5) + originalRelative;
        gOriginalGetClientDObj = reinterpret_cast<GetClientDObj_t>(originalTarget);

        const std::intptr_t newRelativeWide =
            reinterpret_cast<std::intptr_t>(&HookedGetClientDObj) -
            reinterpret_cast<std::intptr_t>(callsite + 5);
        if (newRelativeWide < std::numeric_limits<std::int32_t>::min() ||
            newRelativeWide > std::numeric_limits<std::int32_t>::max())
        {
            gOriginalGetClientDObj = nullptr;
            return false;
        }
        const std::int32_t newRelative = static_cast<std::int32_t>(newRelativeWide);

        std::memcpy(gOriginalDObjCallBytes.data(), callsite, gOriginalDObjCallBytes.size());
        DWORD oldProtect = 0;
        if (!VirtualProtect(callsite, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            gOriginalGetClientDObj = nullptr;
            return false;
        }
        std::memcpy(callsite + 1, &newRelative, sizeof(newRelative));
        FlushInstructionCache(GetCurrentProcess(), callsite, 5);
        DWORD ignored = 0;
        VirtualProtect(callsite, 5, oldProtect, &ignored);
        gPatchedDObjCallsite = callsite;
        return true;
    }
}

extern "C" __declspec(dllexport)
DWORD WINAPI MW2VR_InstallHandsFilter(LPVOID)
{
    std::lock_guard<std::mutex> lock(gInstallMutex);
    if (gHandsFilterInstalled.load(std::memory_order_acquire))
    {
        return 1;
    }

    ModuleLayout layout{};
    if (!BuildModuleLayout(layout))
    {
        Log("[MW2VR][HANDS] failed to parse iw4sp.exe PE image.\r\n");
        return 0;
    }

    std::uint8_t* outerCall = LocateAddPlayerWeaponCallsite(layout);
    if (!outerCall)
    {
        Log("[MW2VR][HANDS] could not validate the original CG_AddPlayerWeapon callsite; no hand filter installed.\r\n");
        return 0;
    }

    std::uint8_t* dObjCall = LocateGetClientDObjCallsite(layout, outerCall);
    if (!dObjCall)
    {
        Log("[MW2VR][HANDS] could not uniquely locate the first-person DObj getter; no hand filter installed.\r\n");
        return 0;
    }

    if (!PatchDObjGetterCallsite(dObjCall))
    {
        Log("[MW2VR][HANDS] DObj getter call patch failed; game code left unmodified.\r\n");
        return 0;
    }

    gHandsFilterInstalled.store(true, std::memory_order_release);
    Log("[MW2VR][HANDS] WEAPON-ONLY FILTER INSTALLED: stock first-person hand/arm submodels will be hidden when VR weapon tracking is active.\r\n");
    return 1;
}
