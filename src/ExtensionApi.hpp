// Extension service-table glue + log macros, mirroring the wxl-db2 recipe.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#pragma once

#include "wxl/PluginApi.h"
#include "wxl/Db2Api.h"
#include "wxl/FdidApi.h"

namespace wxl_housing
{
    /// Service table the core handed us in WXL_Load. Never null after a successful load.
    extern const WXL_Api* g_api;

    /// wxl-db2 interfaces; null until WXL_Load resolves them.
    extern const WXL_Db2Api*  g_db2;
    extern const WXL_FdidApi* g_fdid;
}

// Log tags; the core prefixes the extension id on its side (WXL_LOG_* level + origin).
#define WLOG_INFO(...)  wxl_housing::g_api->Log(WXL_LOG_INFO, "wxl-housing", __VA_ARGS__)
#define WLOG_WARN(...)  wxl_housing::g_api->Log(WXL_LOG_WARN, "wxl-housing", __VA_ARGS__)
#define WLOG_ERROR(...) wxl_housing::g_api->Log(WXL_LOG_ERROR, "wxl-housing", __VA_ARGS__)
