#pragma once

static constexpr bool kQuietLogsForLodDiag = true;
static constexpr bool kEnableLodLogs = false;

inline bool QuietLogsForLodDiag()
{
    return kQuietLogsForLodDiag;
}

inline bool LodLogsEnabled()
{
    return kEnableLodLogs;
}
