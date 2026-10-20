#pragma once
#include "../../pch.h"
#include "FortCheatManager.h"

class ABuildingPlayerPrimitivePreview : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(ABuildingPlayerPrimitivePreview);

    DEFINE_BITFIELD_PROP(bHidden);
};

class AFortPlayerControllerAthena : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPlayerControllerAthena);

    DEFINE_BITFIELD_PROP(bBuildFree);
    DEFINE_BITFIELD_PROP(bInfiniteAmmo);
    DEFINE_PROP(CheatManager, UFortCheatManager*);
    DEFINE_PROP(CheatClass, TSubclassOf<UObject>);
    DEFINE_PROP(TargetedBuilding, AActor*);

    DEFINE_PROP(BuildPreviewMarker, ABuildingPlayerPrimitivePreview*);
    DEFINE_BITFIELD_PROP(bBuildPreviewMirrored);
};
