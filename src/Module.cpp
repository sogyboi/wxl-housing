// wxl-housing entry point: WXL_Query / WXL_Load, interface resolution, event-script boot.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "ExtensionApi.hpp"

#include "wxl/EventScript.hpp"

#include "Catalog.hpp"
#include "FreeBuildCamera.hpp"
#include "Placement.hpp"
#include "ImGuiHostExt.hpp"
#include "TerrainDeform.hpp"

#include <cstring>

namespace wxl_housing
{
    const WXL_Api* g_api = nullptr;
    const WXL_Db2Api*  g_db2  = nullptr;
    const WXL_FdidApi* g_fdid = nullptr;

    namespace
    {
        // One file-scope subscriber. The ctor binds the events we care about; it must be
        // constructed after EventScript::Bind(api) (done in WXL_Load below).
        class HousingScript : public wxl::ext::EventScript
        {
        public:
            HousingScript()
            {
                on<&HousingScript::OnEndScene>(wxl::events::Event::OnEndScene);
                on<&HousingScript::OnDoodadSpawn>(wxl::events::Event::OnDoodadSpawn);
                on<&HousingScript::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
                on<&HousingScript::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
            }

            void OnEndScene(const wxl::events::EndSceneArgs& a)
            {
                ImGuiHostExt::Instance().OnEndScene(a.device);
            }

            void OnDoodadSpawn(const wxl::events::DoodadSpawnArgs& a)
            {
                Placement::Instance().OnDoodadSpawn(a);
            }

            void OnDeviceLost(const wxl::events::DeviceResetArgs& a)
            {
                ImGuiHostExt::Instance().OnDeviceLost(a);
            }

            void OnDeviceReset(const wxl::events::DeviceResetArgs& a)
            {
                ImGuiHostExt::Instance().OnDeviceReset(a);
            }
        };

        // Lazy singleton: constructed on first use, i.e. from WXL_Load (post Bind).
        HousingScript& Script()
        {
            static HousingScript s;
            return s;
        }
    }
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo s_info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-housing",
        706,
        WXL_CLIENT_BUILD,
    };
    return &s_info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION)
        return 0;

    using namespace wxl_housing;
    g_api = api;

    // EventScript must be bound before any subscriber is constructed.
    wxl::ext::EventScript::Bind(api);

    g_db2 = static_cast<const WXL_Db2Api*>(api->GetInterface("wxl.db2", WXL_DB2_API_VERSION));
    g_fdid = static_cast<const WXL_FdidApi*>(api->GetInterface("wxl.fdid", WXL_FDID_API_VERSION));
    if (!g_db2 || !g_fdid)
    {
        api->Log(WXL_LOG_ERROR, "wxl-housing",
                 "wxl.db2 / wxl.fdid interfaces unavailable - install wxl-db2 (catalog disabled)");
        return 0;
    }

    // Bring up the subscriber graph (catalog, placement, ImGui host).
    (void)Script();
    ImGuiHostExt::Instance();
    if (!Catalog::Instance().EnsureLoaded())
    {
        api->Log(WXL_LOG_ERROR, "wxl-housing",
                 "HouseDecor catalog failed to load - housing editor not started");
        return 0;
    }
    Placement::Instance();
    const bool freeCameraReady = FreeBuildCamera::Instance().Initialize();
    if (!freeCameraReady)
        api->Log(WXL_LOG_ERROR, "wxl-housing",
                 "free camera initialization failed; housing remains available with native camera");
    (void)TerrainDeform::Instance().Initialize();

    api->Log(WXL_LOG_INFO, "wxl-housing",
              "wxl-housing 0.8.0: HouseDecor catalog + placement ready; "
              "free-camera=%s (Insert toggles the editor)",
              freeCameraReady ? "ready" : "disabled");
    return 1;
}
