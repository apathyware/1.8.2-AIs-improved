#pragma once
#include "../../pch.h"

namespace PlaceDump
{
    void RecordTick();

    void Flush();

    void EnsureLoaded();

    int GetNodeCount();
    int GetConnectedNodeCount();

    bool IsUsable();

    bool GetNextWaypoint(void* Identity, const FVector& From, const FVector& To, FVector& OutWaypoint);

    void ForgetBot(void* Identity);

    bool GetRandomNearbyPlace(const FVector& Near, float WithinDistance, FVector& OutPlace);

    int GetSpreadLandingSpots(int Count, FVector* OutSpots);

    bool GetLandmark(const char* Name, FVector& Out);

    int GetPlacesNear(const FVector& Near, float WithinDistance, int Count, FVector* OutSpots);

    void ReportOnce();

    void AnnounceHotkey();
}
