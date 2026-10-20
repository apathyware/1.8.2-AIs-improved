#pragma once
#include "../../pch.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../FortniteGame/Public/FortGameMode.h"

class AFortAthenaAIBotController : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaAIBotController);

    DEFINE_PROP(Inventory, AFortInventory*);
};

class AAIController : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AAIController);

    DEFINE_PROP(BrainComponent, UObject*);
    DEFINE_FUNC(Possess, void);

    DEFINE_FUNC(RunBehaviorTree, bool);

    DEFINE_FUNC(MoveToLocation, uint8_t);

    DEFINE_FUNC(SetControlRotation, void);

    DEFINE_FUNC(LineOfSightTo, bool);

    DEFINE_FUNC(GetControlRotation, FRotator);

    DEFINE_FUNC(StopMovement, void);

    DEFINE_FUNC(K2_SetFocus, void);
    DEFINE_FUNC(K2_SetFocalPoint, void);
    DEFINE_FUNC(K2_ClearFocus, void);
};

class UFortAthenaAISpawnerDataComponent_Behavior : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortAthenaAISpawnerDataComponent_Behavior);

    DEFINE_PROP(BehaviorTree, UObject*);
};

class UAthenaAISpawner : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UAthenaAISpawner);

    DEFINE_FUNC(RequestSpawn, int32);
};

class UAthenaAISystem : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UAthenaAISystem);

    DEFINE_PROP(AISpawner, UAthenaAISpawner*);
};

class UFortAthenaAISpawnerDataComponentList : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortAthenaAISpawnerDataComponentList);
};

class UFortAthenaAISpawnerData : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortAthenaAISpawnerData);

    DEFINE_STATIC_FUNC(CreateComponentListFromClass, UFortAthenaAISpawnerDataComponentList*);
};

class AFortPoiVolume : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPoiVolume);

    DEFINE_BITFIELD_PROP(bIsLargeGameVolume);

    DEFINE_PROP(LocationTags, FGameplayTagContainer);
};

class UAITask_MoveTo : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UAITask_MoveTo);

    DEFINE_STATIC_FUNC(AIMoveTo, UAITask_MoveTo*);
};

namespace BotAI
{
    int SpawnBots(const FVector& Origin, int Count, float ScatterRadius = 3000.f, const std::wstring& SpawnerDataAssetPath = L"");

    bool MoveSomeBotsToIsland(int MaxThisTick);

    void RegisterBotsAsAlive();

    void GameThreadTick();

    void RenameUnnamedBots();

    void DressUndressedBots();

    void CheckForVictory();

    bool ShouldGrantVictoryTo(AFortPlayerStateAthena* CandidateState, AFortPlayerStateAthena* DyingVictimState);

    bool MatchHasStarted();

    void SetBotSpawningPaused(bool bPaused);

    bool AreBotSpawnsPaused();

    bool BusMayLaunch();

    void ClearBusForLaunch(const char* Why);

    bool BusIsClearedForLaunch();

    void HoldTheWarmupClock(AFortGameStateAthena* GameState, AFortGameMode* GameMode, float Now);

    bool IsServerOwner(AFortPlayerControllerAthena* Controller);

    void EnsureWeaponHotfixes();

    bool SettleEliminationFromGameReport(AFortPlayerStateAthena* Victim, AFortPlayerStateAthena* Killer, AFortPlayerPawnAthena* VictimPawn,
                                         AFortPlayerPawnAthena* KillerPawn, AActor* DamageCauser);

    void LogFromClient(AFortPlayerControllerAthena* Controller, const char* Text);
}
