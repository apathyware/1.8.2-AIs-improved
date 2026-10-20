#pragma once
#include "../../pch.h"

namespace NPCs
{
    void GameThreadTick();

    bool IsNPC(const void* Object);

    int SpawnNow();

    void ReportDiscovery();
}
