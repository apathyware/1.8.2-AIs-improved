#pragma once
#include "../../pch.h"
#include "../../../Four_E_Zero_H/Engine/Public/CurveTable.h"

struct FUIExtension final
{
public:
    uint8 Slot;
    uint8 Pad_1[0x7];
    TSoftClassPtr<class UClass> WidgetClass;
};

struct EPlaylistUIExtensionSlot
{
public:
    UENUM_COMMON_MEMBERS(EPlaylistUIExtensionSlot);

    DEFINE_ENUM_PROP(Primary);
};

struct EUIExtensionSlot
{
public:
    UENUM_COMMON_MEMBERS(EUIExtensionSlot);

    DEFINE_ENUM_PROP(Primary);
};

class UFortPlaylistAthena : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortPlaylistAthena);

    DEFINE_PROP(bRespawnInAir, bool);
    DEFINE_PROP(RespawnHeight, FScalableFloat);
    DEFINE_PROP(RespawnTime, FScalableFloat);
    DEFINE_PROP(RespawnType, uint8);
    DEFINE_PROP(bAllowJoinInProgress, bool);
    DEFINE_PROP(bForceCameraFadeOnRespawn, bool);
    DEFINE_PROP(UIExtensions, TArray<FUIExtension>);
    DEFINE_PROP(bForceRespawnLocationInsideOfVolume, bool);
};
