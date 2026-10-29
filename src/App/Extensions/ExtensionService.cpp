#include "ExtensionService.hpp"
#include "App/Extensions/Animation/Extension.hpp"
#include "App/Extensions/Attachment/Extension.hpp"
#include "App/Extensions/Customization/Extension.hpp"
#include "App/Extensions/FactoryIndex/Extension.hpp"
#include "App/Extensions/Garment/Extension.hpp"
#include "App/Extensions/InkSpawner/Extension.hpp"
#include "App/Extensions/Journal/Extension.hpp"
#include "App/Extensions/Localization/Extension.hpp"
#include "App/Extensions/Mesh/Extension.hpp"
#include "App/Extensions/PuppetState/Extension.hpp"
#include "App/Extensions/QuestPhase/Extension.hpp"
#include "App/Extensions/ResourceLink/Extension.hpp"
#include "App/Extensions/ResourceMeta/Extension.hpp"
#include "App/Extensions/ResourcePatch/Extension.hpp"
#include "App/Extensions/Transmog/Extension.hpp"
#include "App/Extensions/WorldStreaming/Extension.hpp"
#include "Core/Facades/Log.hpp"
#include "Red/ExtensionService.hpp"
#include "Red/GameApplication.hpp"
#include "Red/GameEngine.hpp"
#include "Red/ResourceLoader.hpp"
#include "Red/TweakDB.hpp"

App::ExtensionService::ExtensionService(std::filesystem::path aBundleDir)
    : m_bundleDir(std::move(aBundleDir))
{
}

void App::ExtensionService::OnBootstrap()
{
    m_loader = Core::MakeUnique<ExtensionLoader>(m_bundleDir, L".xl");

    m_loader->Add<ResourceMetaExtension>();
    m_loader->Add<ResourceLinkExtension>();
    m_loader->Add<ResourcePatchExtension>();
    m_loader->Add<MeshExtension>();
    m_loader->Add<FactoryIndexExtension>();
    m_loader->Add<LocalizationExtension>();
    m_loader->Add<JournalExtension>();
    m_loader->Add<AnimationExtension>();
    m_loader->Add<TransmogExtension>();
    m_loader->Add<AttachmentExtension>();
    m_loader->Add<CustomizationExtension>();
    m_loader->Add<GarmentExtension>();
    m_loader->Add<PuppetStateExtension>();
    m_loader->Add<QuestPhaseExtension>();
    m_loader->Add<WorldStreamingExtension>();
    m_loader->Add<InkSpawnerExtension>();

    HookOnceAfter<Raw::GameApplication::InitResourceDepot>([&]() {
        m_loader->Configure();
        m_loader->Load();
    }).OrThrow();

    HookOnceAfter<Raw::CBaseEngine::LoadGatheredResources>([&]() {
        m_loader->OnDepotReady();
    }).OrThrow();

    HookAfter<Raw::LoadTweakDB>([&]() {
        m_loader->OnTweakDBReady();
    }).OrThrow();

    // A/B/C experiment toggle (see DEBUGGING_NOTES.md, section 0.8.4):
    //   AXL_SUPPRESS_BUILTIN_EXTENSIONS:
    //     1 = completely suppress built-in ExtensionService (Test B -> items invisible)
    //     0 = let built-in ExtensionService bootstrap (Test A / Test C)
    //   AXL_BISECT_DEFAULT:
    //     "GARMENT", "MESH", "GARMENT_AND_MESH", "ALL_EXCEPT_GARMENT", "ALL_EXCEPT_MESH", "ALL", "NONE"
    //     Can also be overridden at runtime via Windows environment variable: set AXL_BISECT=...
#ifndef AXL_SUPPRESS_BUILTIN_EXTENSIONS
#define AXL_SUPPRESS_BUILTIN_EXTENSIONS 0
#endif

#ifndef AXL_BISECT_DEFAULT
#define AXL_BISECT_DEFAULT "NONE"
#endif

#if AXL_SUPPRESS_BUILTIN_EXTENSIONS
    Core::Log::Info("[TruthLogger] AXL_SUPPRESS_BUILTIN_EXTENSIONS=1 (built-in ExtensionService suppressed)");
    Hook<Raw::ExtensionService::Bootstrap>(+[](void*) {
        Core::Log::Info("[TruthLogger] Intercepted built-in Raw::ExtensionService::Bootstrap (no-op)");
    }).OrThrow();
    Hook<Raw::ExtensionService::Shutdown>(+[](void*) {
        Core::Log::Info("[TruthLogger] Intercepted built-in Raw::ExtensionService::Shutdown (no-op)");
    }).OrThrow();
    Hook<Raw::ExtensionLoader::Configure>(+[](void*) {
        Core::Log::Info("[TruthLogger] Intercepted built-in Raw::ExtensionLoader::Configure (no-op)");
    }).OrThrow();
#else
    Core::Log::Info("[TruthLogger] AXL_SUPPRESS_BUILTIN_EXTENSIONS=0 (built-in ExtensionService active, Test C Bisection enabled)");

    struct BisectSlot
    {
        std::string_view name;
        uintptr_t instanceRva;
        uintptr_t ctrlBlockRva;
    };

    static constexpr BisectSlot s_bisectSlots[] = {
        {"Garment",     0x3963858, 0x3963860}, // 0x142300880, Bridge voffs: E0, D8, E8, 100, 108, 110, 118, 120, 130
        {"Mesh",        0x3963898, 0x39638A0}, // 0x142300970, Bridge voffs: 68, 70, 78, A0
        {"Slot_830",    0x39638A8, 0x39638B0}, // 0x142300830, Bridge voffs: 40, 48, 50, 58, 60, 88
        {"Slot_9C0",    0x3963878, 0x3963880}, // 0x1423009C0, Bridge voffs: 90, 98, B0, B8, C0, C8
        {"Slot_8D0",    0x39638C8, 0x39638D0}, // 0x1423008D0, Bridge voffs: 28, 30
        {"Slot_920",    0x39638E8, 0x39638F0}, // 0x142300920, Bridge voffs: 140, 148, 150, 158, 160, 168
        {"Slot_A10",    0x3963888, 0x3963890}, // 0x142300A10, Bridge voff: 80
        {"Slot_A60",    0x39638D8, 0x39638E0}, // 0x142300A60, Bridge voffs: 18, 20
    };

    HookAfter<Raw::ExtensionService::Bootstrap>([](void*) {
        const auto base = Core::RawBase::GetImageBase();

        char envBuf[64] = {};
        DWORD envLen = GetEnvironmentVariableA("AXL_BISECT", envBuf, sizeof(envBuf));
        std::string mode = (envLen > 0) ? std::string(envBuf) : std::string(AXL_BISECT_DEFAULT);
        for (auto& c : mode) c = static_cast<char>(toupper(c));

        Core::Log::Info("[TruthLogger] Test C Bisection Active. Mode: '{}' (ImageBase: {:p})",
                        mode, reinterpret_cast<void*>(base));

        Core::Log::Info("[TruthLogger] Post-Bootstrap Slot Dump:");
        for (const auto& slot : s_bisectSlots)
        {
            auto* inst = reinterpret_cast<uintptr_t*>(base + slot.instanceRva);
            auto* ctrl = reinterpret_cast<uintptr_t*>(base + slot.ctrlBlockRva);
            Core::Log::Info("[TruthLogger]   Slot {:<10} @ inst RVA +{:#x} = {:p}, ctrl RVA +{:#x} = {:p}",
                            slot.name, slot.instanceRva, reinterpret_cast<void*>(*inst),
                            slot.ctrlBlockRva, reinterpret_cast<void*>(*ctrl));
        }

        auto Nullify = [&](const BisectSlot& slot) {
            auto* inst = reinterpret_cast<uintptr_t*>(base + slot.instanceRva);
            auto* ctrl = reinterpret_cast<uintptr_t*>(base + slot.ctrlBlockRva);
            Core::Log::Info("[TruthLogger]   --> Nullifying slot '{}' (inst {:p} -> 0, ctrl {:p} -> 0)",
                            slot.name, reinterpret_cast<void*>(*inst), reinterpret_cast<void*>(*ctrl));
            *inst = 0;
            *ctrl = 0;
        };

        if (mode == "GARMENT")
        {
            Nullify(s_bisectSlots[0]);
        }
        else if (mode == "MESH")
        {
            Nullify(s_bisectSlots[1]);
        }
        else if (mode == "GARMENT_AND_MESH")
        {
            Nullify(s_bisectSlots[0]);
            Nullify(s_bisectSlots[1]);
        }
        else if (mode == "ALL_EXCEPT_GARMENT")
        {
            for (size_t i = 1; i < std::size(s_bisectSlots); ++i)
                Nullify(s_bisectSlots[i]);
        }
        else if (mode == "ALL_EXCEPT_MESH")
        {
            for (size_t i = 0; i < std::size(s_bisectSlots); ++i)
                if (i != 1) Nullify(s_bisectSlots[i]);
        }
        else if (mode == "ALL")
        {
            for (const auto& slot : s_bisectSlots)
                Nullify(slot);
        }
        else if (mode == "NONE")
        {
            Core::Log::Info("[TruthLogger]   --> Mode NONE: Keeping all slots intact (Test A baseline).");
        }
        else
        {
            Core::Log::Warning("[TruthLogger]   --> Unknown mode '{}', keeping all slots intact.");
        }
    }).OrThrow();
#endif

    Raw::InkSpawner::ControllerInjectionEnabled::Ref() = false;
}

void App::ExtensionService::OnShutdown()
{
    m_loader->Unload();
    m_loader = nullptr;
}

void App::ExtensionService::Configure()
{
    std::unique_lock _(m_reloadMutex);

    if (!IsHooked<Raw::ResourceLoader::OnUpdate>())
    {
        HookAfter<Raw::ResourceLoader::OnUpdate>([&]() {
            std::unique_lock _(m_reloadMutex);

            m_loader->Configure();
            m_loader->Reload();

            Unhook<Raw::ResourceLoader::OnUpdate>();
        });
    }
}
