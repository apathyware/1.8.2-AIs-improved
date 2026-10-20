#include "pch.h"
#include "../Public/FortGameMode.h"
#include "../../Engine/Public/AbilitySystemComponent.h"
#include "../../Engine/Public/CurveTable.h"
#include "../../Engine/Public/DataTableFunctionLibrary.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Four_E_Zero_H/Public/Configuration.h"
#include "../../Four_E_Zero_H/Public/Events.h"
#include "../../Four_E_Zero_H/Public/Finders.h"
#include "../../Four_E_Zero_H/Public/GUI.h"
#include "../../Four_E_Zero_H/Public/LateGame.h"
#include "../../Four_E_Zero_H/Public/Misc.h"
#include "../../Four_E_Zero_H/Public/BotAI.h"
#include "../../Four_E_Zero_H/Plugins/CrashReporter/Public/CrashReporter.h"
#include "../../Four_E_Zero_H/Public/ServerConsole.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/BuildingFoundation.h"
#include "../Public/BuildingItemCollectorActor.h"
#include "../Public/FortAthenaCreativePortal.h"
#include "../Public/FortKismetLibrary.h"
#include "../Public/FortLootPackage.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortSafeZoneIndicator.h"
#include "../Public/LevelStreamingDynamic.h"
#include "../Public/FortAthenaSpawningPolicyManager.h"
#include <random>
#include <vector>

#include <cstdarg>
#include <cstdio>

void ShowFoundation(const ABuildingFoundation* Foundation)
{
    if (!Foundation)
        return;

    Foundation->SetDynamicFoundationEnabled(true);
}

bool bIsLargeTeamGame = false;

uint64_t StartStreamingAdditionalPlaylistLevel_ = 0;

void StreamAdditionalPlaylistLevels(AFortGameStateAthena* _this)
{
    auto Playlist = _this->HasCurrentPlaylistData() ? _this->CurrentPlaylistData : (_this->HasCurrentPlaylistInfo() ? (_this->CurrentPlaylistInfo.OverridePlaylist ? _this->CurrentPlaylistInfo.OverridePlaylist : _this->CurrentPlaylistInfo.BasePlaylist) : nullptr);

    auto& StartStreamingAdditionalPlaylistLevel = (void (*&)(AFortGameStateAthena*, FName, bool))StartStreamingAdditionalPlaylistLevel_;

    if (!Playlist || !Playlist->HasAdditionalLevels())
        return;

    auto GetLongPackageName = [](FName& Name)
    {
        auto NameStr = Name.ToString();
        return FName(NameStr.substr(NameStr.rfind('.')));
    };

    auto GetLongPackageNameForPath = [&](FSoftObjectPath& Path)
    {
        if (VersionInfo.FortniteVersion >= 23)
        {
            auto& PackageName = *(FName*)&Path;

            return PackageName;
        }

        return GetLongPackageName(Path.AssetPathName);
    };

    auto AdditionalPlaylistLevelsStreamed__Off = _this->GetOffset("AdditionalPlaylistLevelsStreamed");
    auto AdditionalLevelStruct = FAdditionalLevelStreamed::StaticStruct();

    auto StreamLevel = [&](TSoftObjectPtr<UWorld>& World, bool bServerOnly)
    {
        if (StartStreamingAdditionalPlaylistLevel)
        {
            auto& ObjectID = *(FSoftObjectPath*)(__int64(&World) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x8));

            StartStreamingAdditionalPlaylistLevel(_this, GetLongPackageNameForPath(ObjectID), bServerOnly);
        }
        else
        {
            bool Success = true;
            ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(UWorld::GetWorld(), World, FVector(), FRotator(), &Success, FString(), nullptr);

            if (AdditionalLevelStruct)
            {
                auto level = (FAdditionalLevelStreamed*)malloc(FAdditionalLevelStreamed::Size());
                memset((PBYTE)level, 0, FAdditionalLevelStreamed::Size());
                level->bIsServerOnly = bServerOnly;
                level->LevelName = World.ObjectID.AssetPathName;
                if (Success)
                    _this->AdditionalPlaylistLevelsStreamed.Add(*level, FAdditionalLevelStreamed::Size());
                free(level);
            }
            else
                GetFromOffset<TArray<FName>>(_this, AdditionalPlaylistLevelsStreamed__Off).Add(World.ObjectID.AssetPathName);
        }
    };

    for (auto& AdditionalLevel : Playlist->AdditionalLevels)
        StreamLevel(AdditionalLevel, false);

    if (Playlist->HasAdditionalLevelsServerOnly())
        for (auto& AdditionalLevel : Playlist->AdditionalLevelsServerOnly)
            StreamLevel(AdditionalLevel, true);

    if (Playlist->HasSharedAssetGroup() && Playlist->SharedAssetGroup)
        for (auto& SharedAsset : Playlist->SharedAssetGroup->SharedAssetsToLoad)
            for (auto& AdditionalLevel : SharedAsset->SharedAdditionalLevels)
                StreamLevel(AdditionalLevel, false);
}

void SetupPlaylist(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
{
    auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

    if (!Playlist)
        Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

    if (Playlist)
    {
        if (FConfiguration::bForceRespawns)
        {
            if (Playlist->HasbRespawnInAir())
                Playlist->bRespawnInAir = true;
            if (Playlist->HasRespawnHeight())
            {
                Playlist->RespawnHeight.Curve.CurveTable = nullptr;
                Playlist->RespawnHeight.Curve.RowName = FName();
                Playlist->RespawnHeight.Value = 20000;
            }
            if (Playlist->HasRespawnTime())
            {
                Playlist->RespawnTime.Curve.CurveTable = nullptr;
                Playlist->RespawnTime.Curve.RowName = FName();
                Playlist->RespawnTime.Value = 3;
            }
            Playlist->RespawnType = 1;
        }
        if (FConfiguration::bForceRespawns || FConfiguration::bJoinInProgress)
        {
            if (Playlist->HasbAllowJoinInProgress())
                Playlist->bAllowJoinInProgress = true;
            if (Playlist->HasJoinInProgressMatchType())
                Playlist->JoinInProgressMatchType = UKismetTextLibrary::Conv_StringToText(FString(L"Creative"));
        }

        {
            if (Playlist->HasGarbageCollectionFrequency())
                Playlist->GarbageCollectionFrequency = 9999999999999999.f;
            if (GameMode->HasPlaylistHotfixOriginalGCFrequency())
                GameMode->PlaylistHotfixOriginalGCFrequency = 9999999999999999.f;
            if (GameMode->HasbDisableGCOnServerDuringMatch())
                GameMode->bDisableGCOnServerDuringMatch = true;
            if (GameMode->HasbPlaylistHotfixChangedGCDisabling())
                GameMode->bPlaylistHotfixChangedGCDisabling = true;
        }
        if (GameState->HasCurrentPlaylistInfo())
        {
            GameState->CurrentPlaylistInfo.BasePlaylist = Playlist;
            GameState->CurrentPlaylistInfo.PlaylistReplicationKey++;
            GameState->CurrentPlaylistInfo.MarkArrayDirty();
            GameState->OnRep_CurrentPlaylistInfo();
        }
        else if (GameState->HasCurrentPlaylistData())
        {
            GameState->CurrentPlaylistData = Playlist;
            GameState->OnRep_CurrentPlaylistData();
        }

        GameMode->CurrentPlaylistId = Playlist->PlaylistId;
        if (GameState->HasCurrentPlaylistId())
            GameState->CurrentPlaylistId = Playlist->PlaylistId;
        if (GameMode->HasCurrentPlaylistName())
            GameMode->CurrentPlaylistName = Playlist->PlaylistName;

        if (GameMode->GameSession->HasMaxPlayers())
            GameMode->GameSession->MaxPlayers = Playlist->MaxPlayers;

        if (GameState->HasAirCraftBehavior() && Playlist->HasAirCraftBehavior())
            GameState->AirCraftBehavior = Playlist->AirCraftBehavior;
        if (GameState->HasCachedSafeZoneStartUp() && Playlist->HasSafeZoneStartUp())
            GameState->CachedSafeZoneStartUp = Playlist->SafeZoneStartUp;

        if (GameMode->HasbEnableDBNO())
            GameMode->bEnableDBNO = Playlist->MaxSquadSize > 1;

        bIsLargeTeamGame = Playlist->bIsLargeTeamGame;

        if (Playlist)
            StreamAdditionalPlaylistLevels(GameState);
    }
    else
    {
        GameState->CurrentPlaylistId = GameMode->CurrentPlaylistId = 0;

        if (GameMode->GameSession->HasMaxPlayers())
            GameMode->GameSession->MaxPlayers = 100;
    }
}

void (*VendWobble__FinishedFuncOG)(UObject* Context, FFrame& Stack);
void VendWobble__FinishedFunc(UObject* Context, FFrame& Stack)
{
    auto CollectorActor = (ABuildingItemCollectorActor*)Context;
    auto PlayerController = CollectorActor->ControllingPlayer;

    if (!PlayerController)
        return VendWobble__FinishedFuncOG(Context, Stack);

    auto Collection = CollectorActor->ItemCollections.Search([&](FCollectorUnitInfo& Coll) { return Coll.InputItem == CollectorActor->ClientPausedActiveInputItem; }, FCollectorUnitInfo::Size());

    if (!Collection)
        return VendWobble__FinishedFuncOG(Context, Stack);

    CollectorActor->ClientPausedActiveInputItem = nullptr;

    float Cost = Collection->InputCount.Evaluate();

    auto VMLoc = CollectorActor->K2_GetActorLocation();
    auto& SpawnLocation = CollectorActor->LootSpawnLocation;
    auto Loc = VMLoc + (CollectorActor->GetActorForwardVector() * SpawnLocation.X) + (CollectorActor->GetActorRightVector() * SpawnLocation.Y) + (CollectorActor->GetActorUpVector() * SpawnLocation.Z);

    for (int i = 0; i < Collection->OutputItemEntry.Num(); i++)
    {
        auto& Item = Collection->OutputItemEntry.Get(i, FFortItemEntry::Size());

        AFortInventory::SpawnPickup(Loc, Item);
        if (CollectorActor->HasPickupSpawned())
            CollectorActor->PickupSpawned.Process();
    }

    return VendWobble__FinishedFuncOG(Context, Stack);
}

std::unordered_map<int, float> WeightMap;
float Sum = 0;
float Weight;
float TotalWeight;

class AFortAthenaLivingWorldStaticPointProvider : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaLivingWorldStaticPointProvider);

    DEFINE_PROP(FiltersTags, FGameplayTagContainer);
    DEFINE_PROP(SpawnPoints, TArray<FTransform>);
    DEFINE_PROP(bStartEnabled, bool);
    DEFINE_PROP(bRandomizeStartPoint, bool);
    DEFINE_PROP(bRandomizePointRotation, bool);
};

class UFortVehicleItemDefinition : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortVehicleItemDefinition);

    DEFINE_PROP(VehicleMinSpawnPercent, FScalableFloat);
    DEFINE_PROP(VehicleMaxSpawnPercent, FScalableFloat);
};

class AFortAthenaVehicleSpawner : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaVehicleSpawner);

    DEFINE_PROP(CachedFortVehicleItemDef, UFortVehicleItemDefinition*);
    DEFINE_PROP(bForceSpawnAlways, bool);

    DEFINE_FUNC(GetVehicleClass, UClass*);
};

void AFortGameMode::ReadyToStartMatch_(UObject* Context, FFrame& Stack, bool* Ret)
{
    Stack.IncrementCode();

    static auto FrontendMode = FindClass("FortGameModeFrontend");
    if (Context->IsA(FrontendMode))
    {
        *Ret = callOGWithRet(((AFortGameMode*)Context), Stack.GetCurrentNativeFunction(), ReadyToStartMatch);
        return;
    }
    auto GameMode = Context->Cast<AFortGameMode>();

    auto GameState = GameMode->GameState;

    static bool setup = false;
    if (GameMode->HasWarmupRequiredPlayerCount() ? GameMode->WarmupRequiredPlayerCount != 1 : !setup)
    {
        setup = true;

        {
            auto World = UWorld::GetWorld();
            auto Engine = UEngine::GetEngine();
            auto NetDriverName = FName(L"GameNetDriver");

            if (GameMode->HasbEnableReplicationGraph())
                GameMode->bEnableReplicationGraph = true;

            UNetDriver* NetDriver = nullptr;
            if (VersionInfo.FortniteVersion >= 16.00)
            {
                void* WorldCtx = ((void* (*)(UEngine*, UWorld*))FindGetWorldContext())(Engine, World);
                World->NetDriver = NetDriver = ((UNetDriver * (*)(UEngine*, void*, FName, int)) FindCreateNetDriverWorldContext())(Engine, WorldCtx, NetDriverName, 0);
            }
            else
                World->NetDriver = NetDriver = ((UNetDriver * (*)(UEngine*, UWorld*, FName)) FindCreateNetDriver())(Engine, World, NetDriverName);
            if (VersionInfo.FortniteVersion >= 20)
                NetDriver->NetServerMaxTickRate = 30;

            NetDriver->NetDriverName = NetDriverName;
            NetDriver->World = World;

            if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
            {
                *(bool*)(__int64(&NetDriver->ReplicationDriver) + 0x11) = true;
            }

            NetDriver->NetDriverName = NetDriverName;
            NetDriver->World = World;

            for (int i = 0; i < World->LevelCollections.Num(); i++)
            {
                auto& LevelCollection = World->LevelCollections.Get(i, FLevelCollection::Size());

                LevelCollection.NetDriver = NetDriver;
            }

            auto URL = (FURL*)malloc(FURL::Size());
            memset((PBYTE)URL, 0, FURL::Size());
            URL->Port = FConfiguration::Port;

            auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&))FindInitListen();
            auto SetWorld = (void (*)(UNetDriver*, UWorld*))FindSetWorld();

            SetWorld(NetDriver, World);
            FString Err;
            if (InitListen(NetDriver, World, URL, false, Err))
                SetWorld(NetDriver, World);
            else
                printf("Failed to listen!");

            free(URL);
        }

        if (GameMode->HasWarmupRequiredPlayerCount())
            GameMode->WarmupRequiredPlayerCount = 1;

        if (VersionInfo.FortniteVersion > 4.0 )
            SetupPlaylist(GameMode, GameState);

        auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

        if (!Playlist)
            Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

        if (VersionInfo.FortniteVersion >= 6 && VersionInfo.FortniteVersion < 7)
        {
            if (VersionInfo.FortniteVersion > 6.10)
                ShowFoundation(VersionInfo.FortniteVersion <= 6.21 ? FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Lake1")
                                                                   : FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Lake2"));
            else
                ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest12"));

            ShowFoundation(VersionInfo.FortniteVersion <= 6.10 ? FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest13")
                                                               : FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_FloatingIsland"));

            auto IslandScripting = TUObjectArray::FindFirstObject("BP_IslandScripting_C");
            if (IslandScripting)
            {
                auto UpdateMapOffset = IslandScripting->GetOffset("UpdateMap");
                if (UpdateMapOffset != -1)
                {
                    *(bool*)(__int64(IslandScripting) + UpdateMapOffset) = true;
                    IslandScripting->ProcessEvent(IslandScripting->GetFunction("OnRep_UpdateMap"), nullptr);
                }
            }
        }
        else if (VersionInfo.FortniteVersion >= 7 && VersionInfo.FortniteVersion < 8)
        {
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_25x36"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.ShopsNew"));
        }
        else if (VersionInfo.FortniteVersion >= 8 && VersionInfo.FortniteVersion < 10)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_50x53_Volcano"));
        else if (VersionInfo.FortniteVersion >= 10.20 && VersionInfo.FortniteVersion < 11)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_50x53_Volcano"));

        if (VersionInfo.FortniteVersion >= 7 && VersionInfo.FortniteVersion <= 10)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.SLAB_2"));
        else if (VersionInfo.EngineVersion == 4.23)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.SLAB_4"));

        bool bEvent = false;
        if (Playlist && Playlist->HasGameplayTagContainer())
        {
            for (int i = 0; i < Playlist->GameplayTagContainer.GameplayTags.Num(); i++)
            {
                auto& PlaylistTag = Playlist->GameplayTagContainer.GameplayTags.Get(i, FGameplayTag::Size());

                if (PlaylistTag.TagName.ToString() == "Athena.Playlist.SpecialEvent" || PlaylistTag.TagName.ToString() == "Athena.Playlist.Concert")
                {
                    bEvent = true;
                    if (VersionInfo.FortniteVersion == 7.30)
                        ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.PleasentParkFestivus"));

                    break;
                }
            }
        }

        if (VersionInfo.FortniteVersion == 12.41)
        {
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.LF_Athena_POI_19x19_2"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head6_18"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head5_14"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head3_8"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head_2"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head4_11"));
        }

        if (VersionInfo.FortniteVersion == 7.30 && !bEvent)
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.PleasentParkDefault"));

        if (VersionInfo.EngineVersion >= 4.27 && std::floor(VersionInfo.FortniteVersion) != 20)
        {
            auto MeshNetworkSubsystem = TUObjectArray::FindFirstObject("MeshNetworkSubsystem");

            if (MeshNetworkSubsystem)
                *(uint8_t*)(__int64(MeshNetworkSubsystem) + MeshNetworkSubsystem->GetOffset("NodeType")) = 2;
        }

        auto AIDirectorClass = GameMode->HasWarmupRequiredPlayerCount() ? FindClass("AthenaAIDirector") : FindObject<UClass>("/Game/AIDirector/AIDirector_Fortnite.AIDirector_Fortnite_C");
        if (!AIDirectorClass)
            AIDirectorClass = FindClass("FortAIDirector");

        if (!GameMode->AIDirector)
        {
            GameMode->AIDirector = UWorld::SpawnActor(AIDirectorClass, FVector{}, GameMode);
            if (GameMode->AIDirector)
                GameMode->AIDirector->Call(GameMode->AIDirector->GetFunction("Activate"));
        }

        if (GameMode->HasServerBotManager())
        {
            if (auto BotManager = (UFortServerBotManagerAthena*)UGameplayStatics::SpawnObject(UFortServerBotManagerAthena::StaticClass(), GameMode))
            {
                GameMode->ServerBotManager = BotManager;
                BotManager->CachedGameState = GameState;
                BotManager->CachedGameMode = GameMode;
            }
            else
            {
                printf("BotManager is nullptr!\n");
            }
        }

        if (!GameMode->AIGoalManager)
        {
            auto GoalManagerClass = GameMode->HasWarmupRequiredPlayerCount() ? FindClass("FortAIGoalManager") : FindObject<UClass>("/Game/AI/GoalSelection/AIGoalManager.AIGoalManager_C");

            GameMode->AIGoalManager = UWorld::SpawnActor(GoalManagerClass, FVector{}, GameMode);
        }

        if (GameMode->HasSpawningPolicyManager() && !GameMode->SpawningPolicyManager)
        {
            GameMode->SpawningPolicyManager = UWorld::SpawnActor<AFortAthenaSpawningPolicyManager>(AFortAthenaSpawningPolicyManager::StaticClass(), {});
            GameMode->SpawningPolicyManager->GameStateAthena = GameState;
            GameMode->SpawningPolicyManager->GameModeAthena = GameMode;
        }

        auto MissionManagerClass = GameMode->HasWarmupRequiredPlayerCount() ? nullptr : FindObject<UClass>("/Game/Blueprints/MissionManager.MissionManager_C");

        if (MissionManagerClass)
        {
            GameState->MissionManager = UWorld::SpawnActor(MissionManagerClass, FVector{}, GameState);
            GameState->OnRep_MissionManager();

            auto MissionInfo = FindObject<UFortMissionInfo>(L"/Game/Missions/Primary/EvacuateTheSurvivors/EvacuteTheSurvivors.EvacuteTheSurvivors");

            if (!MissionInfo)
                MissionInfo = FindObject<UFortMissionInfo>(L"/SaveTheWorld/Missions/Primary/EvacuateTheSurvivors/EvacuteTheSurvivors.EvacuteTheSurvivors");

            if (MissionInfo)
            {
                MissionInfo->bStartPlayingOnLoad = true;

                UFortMissionLibrary::LoadMission(UWorld::GetWorld(), MissionInfo);
            }
        }

        *Ret = false;
        return;
    }

    if (!GameMode->bWorldIsReady)
    {
        static auto WarmupStartClass = FindClass("PlayerStart");
        TArray<AActor*> Starts;
        Utils::GetAll(WarmupStartClass, Starts);
        auto StartsNum = Starts.Num();
        Starts.Free();

        if (StartsNum == 0 || !Misc::bHookedAll)
        {
            *Ret = false;
            return;
        }

        TArray<AFortAthenaMapInfo*> AllMapInfos;
        Utils::GetAll<AFortAthenaMapInfo>(AllMapInfos);

        if (AllMapInfos.Num() > 0 && !GameState->MapInfo)
        {
            *Ret = false;
            return;
        }
        AllMapInfos.Free();

        if ((VersionInfo.FortniteVersion >= 3.5 && VersionInfo.FortniteVersion <= 4.0))
            SetupPlaylist(GameMode, GameState);

        if (VersionInfo.FortniteVersion >= 25.20 && GameState->HasMapInfo() && GameState->MapInfo)
        {
            auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(GameState);

            if (GamePhaseLogic)
            {
                auto InitializeFlightPath = (void (*)(AFortAthenaMapInfo*, AFortGameStateAthena*, UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, bool, double, float, float))FindInitializeFlightPath();
                if (InitializeFlightPath)
                    InitializeFlightPath(GameState->MapInfo, GameState, GamePhaseLogic, false, 0.f, 0.f, 360.f);

                GamePhaseLogic->InitializeSafeZoneLocations();
            }
        }

        auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount()
                            ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData)
                            : nullptr;

        if (Playlist && Playlist->HasbSkipWarmup())
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bSkipWarmup = Playlist->bSkipWarmup;
        if (Playlist && Playlist->HasbSkipAircraft())
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bSkipAircraft = Playlist->bSkipAircraft;

        if (Playlist && Playlist->HasGameplayTagContainer())
        {
            for (int i = 0; i < Playlist->GameplayTagContainer.GameplayTags.Num(); i++)
            {
                auto& PlaylistTag = Playlist->GameplayTagContainer.GameplayTags.Get(i, FGameplayTag::Size());

                if (PlaylistTag.TagName.ToString() == "Athena.Playlist.SpecialEvent")
                {
                    for (auto& Event : Events::EventsArray)
                    {
                        if (Event.EventVersion != VersionInfo.FortniteVersion)
                            continue;

                        UObject* LoaderObject = nullptr;
                        if (Event.LoaderClass)
                            if (const UClass* LoaderClass = FindObject<UClass>(Event.LoaderClass))
                            {
                                TArray<AActor*> AllLoaders;
                                Utils::GetAll(LoaderClass, AllLoaders);
                                LoaderObject = AllLoaders.Num() > 0 ? AllLoaders[0] : nullptr;
                                AllLoaders.Free();
                            }

                        if (Event.LoaderFuncPath != nullptr && LoaderObject)
                            if (const UFunction* LoaderFunction = FindObject<UFunction>(Event.LoaderFuncPath))
                            {
                                int Param = 1;
                                LoaderObject->ProcessEvent(const_cast<UFunction*>(LoaderFunction), &Param);
                                printf("[Events] Loaded event level!\n");
                            }
                            else
                                printf("[Events] Failed to load event level!\n");

                        if (GameMode->HasSafeZoneLocations())
                            GameMode->SafeZoneLocations.Free();
                        else
                            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bEnableZones = false;
                        break;
                    }

                    break;
                }
            }
        }

        auto AbilitySet = VersionInfo.FortniteVersion > 8.30 ? FindObject<UFortAbilitySet>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer")
                                                             : FindObject<UFortAbilitySet>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_DefaultPlayer.GAS_DefaultPlayer");
        AbilitySet->AddToRoot();
        AbilitySets.Add(AbilitySet);

        if (VersionInfo.FortniteVersion >= 20)
        {
            auto TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprintGame/Gameplay/AS_TacticalSprint.AS_TacticalSprint");

            if (!TacticalSprintAbility)
                TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprint/Gameplay/AS_TacticalSprint.AS_TacticalSprint");
            TacticalSprintAbility->AddToRoot();
            AbilitySets.Add(TacticalSprintAbility);

            auto AscenderAbility = FindObject<UFortAbilitySet>(L"/Ascender/Gameplay/Ascender/AS_Ascender.AS_Ascender");
            AscenderAbility->AddToRoot();
            AbilitySets.Add(AscenderAbility);

            auto DoorBashAbility = FindObject<UFortAbilitySet>(L"/DoorBashContent/Gameplay/AS_DoorBash.AS_DoorBash");
            DoorBashAbility->AddToRoot();
            AbilitySets.Add(DoorBashAbility);

            auto HillScrambleAbility = FindObject<UFortAbilitySet>(L"/HillScramble/Gameplay/AS_HillScramble.AS_HillScramble");
            HillScrambleAbility->AddToRoot();
            AbilitySets.Add(HillScrambleAbility);

            auto SlideImpulseAbility = FindObject<UFortAbilitySet>(L"/SlideImpulse/Gameplay/AS_SlideImpulse.AS_SlideImpulse");
            SlideImpulseAbility->AddToRoot();
            AbilitySets.Add(SlideImpulseAbility);

            if (std::floor(VersionInfo.FortniteVersion) == 21)
            {
                auto RealitySaplingAbility = FindObject<UFortAbilitySet>(L"/RealitySeedGameplay/Environment/Foliage/GAS_Athena_RealitySapling.GAS_Athena_RealitySapling");
                AbilitySets.Add(RealitySaplingAbility);
            }
        }

        for (auto& Set : AbilitySets)
            if (Set)
                Set->AddToRoot();

        if (Playlist && Playlist->HasModifierList())
            for (int i = 0; i < Playlist->ModifierList.Num(); i++)
            {
                auto Modifier = Playlist->ModifierList.Get(i, FSoftObjectPtr::Size()).Get();

                if (!Modifier)
                    continue;

                for (int j = 0; j < Modifier->PersistentAbilitySets.Num(); j++)
                {
                    auto& DeliveryInfo = Modifier->PersistentAbilitySets.Get(j, FFortAbilitySetDeliveryInfo::Size());

                    if (!DeliveryInfo.DeliveryRequirements.bApplyToPlayerPawns)
                        continue;

                    for (int k = 0; k < DeliveryInfo.AbilitySets.Num(); k++)
                    {
                        auto AbilitySet = DeliveryInfo.AbilitySets.Get(k, FSoftObjectPtr::Size()).Get();

                        AbilitySets.Add(AbilitySet);
                    }
                }
            }

        if (VersionInfo.EngineVersion >= 4.27)
        {
            if (GameState->HasDefaultParachuteDeployTraceForGroundDistance())
                GameState->DefaultParachuteDeployTraceForGroundDistance = 10000;
        }

        if (VersionInfo.FortniteVersion >= 27)
        {
            auto GameData = FindObject<UCurveTable>("/GrindRail/DataTables/GrindRailGameData.GrindRailGameData");

            if (GameData)
            {
                static FName UseGrindingMME = FName(L"Default.GrindRails.UseGrindingMME");

                for (const auto& [RowName, RowPtr] : GameData->RowMap)
                {
                    if (RowName != UseGrindingMME)
                        continue;

                    FSimpleCurve* Row = (FSimpleCurve*)RowPtr;

                    if (!Row)
                        continue;

                    for (auto& Key : Row->Keys)
                        Key.Value = 0.f;
                }
            }
        }
        if (GameState->HasMapInfo() && GameState->MapInfo)
        {
            if (VersionInfo.FortniteVersion >= 3.4)
            {
                GameData = Playlist ? Playlist->GameData : nullptr;
                if (!GameData)
                    GameData = FindObject<UCurveTable>(L"/Game/Athena/Balance/DataTables/AthenaGameData.AthenaGameData");

                for (int i = 0; i < 6; i++)
                {
                    float Weight;
                    UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->MapInfo->VendingMachineRarityCount.Curve.CurveTable, GameState->MapInfo->VendingMachineRarityCount.Curve.RowName, (float)i, nullptr,
                                                                     &Weight, FString());

                    WeightMap[i] = Weight;
                    Sum += Weight;
                }

                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->MapInfo->VendingMachineRarityCount.Curve.CurveTable, GameState->MapInfo->VendingMachineRarityCount.Curve.RowName, 0.f, nullptr, &Weight,
                                                                 FString());

                TotalWeight = std::accumulate(WeightMap.begin(), WeightMap.end(), 0.0f, [&](float acc, const std::pair<int, float>& p) { return acc + p.second; });
            }

            if (VersionInfo.FortniteVersion >= 3.3 && VersionInfo.FortniteVersion < 17 && GameState->MapInfo->LlamaClass)
            {
                auto PickSupplyDropLocation = (FVector * (*)(AFortAthenaMapInfo*, FVector*, FVector*, float, bool, float, float)) FindPickSupplyDropLocation();

                if (PickSupplyDropLocation)
                {
                    FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;

                    auto LlamaMin = GameState->MapInfo->LlamaQuantityMin.Evaluate();
                    auto LlamaMax = GameState->MapInfo->LlamaQuantityMax.Evaluate();
                    auto LlamaCount = UKismetMathLibrary::RandomIntegerInRange((int)LlamaMin, (int)LlamaMax);
                    auto Radius = GameState->MapInfo->HasSafeZoneDefinition() ? SafeZoneDefinition.Radius.Evaluate(0) : 0;

                    if (Radius == 0)
                        Radius = 120000;
                    auto Center = GameState->MapInfo->GetMapCenter();
                    Center.Z = 10000;

                    for (int i = 0; i < LlamaCount; i++)
                    {
                        FVector Loc(0, 0, 0);
                        PickSupplyDropLocation(GameState->MapInfo, &Loc, &Center, Radius, 0, -1, -1);

                        if (Loc.X != 0 || Loc.Y != 0 || Loc.Z != 0)
                        {
                            FRotator Rot{};
                            Rot.Yaw = (float)rand() * 0.010986663f;

                            auto NewLlama = UWorld::SpawnActorUnfinished(GameState->MapInfo->LlamaClass, Loc, Rot);

                            static auto FindGroundLocationAt = NewLlama->GetFunction("FindGroundLocationAt");
                            auto GroundLoc = NewLlama->Call<FVector>(FindGroundLocationAt, Loc);

                            UWorld::FinishSpawnActor(NewLlama, GroundLoc, Rot);
                        }
                    }
                }
            }
        }

        GameMode->DefaultPawnClass = FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

        if (VersionInfo.EngineVersion == 4.16 && VersionInfo.FortniteVersion < 1.9)
        {
            auto sRef = Memcury::Scanner::FindStringRef(L"CollectGarbageInternal() is flushing async loading").Get();
            uint64_t CollectGarbage = 0;

            if (sRef)
            {
                for (int i = 0; i < 1000; i++)
                {
                    auto Ptr = (uint8_t*)(sRef - i);

                    if (*Ptr == 0x48 && *(Ptr + 1) == 0x89 && *(Ptr + 2) == 0x5C)
                    {
                        CollectGarbage = uint64_t(Ptr);
                        break;
                    }
                    else if (*Ptr == 0x40 && *(Ptr + 1) == 0x55)
                    {
                        CollectGarbage = uint64_t(Ptr);
                        break;
                    }
                    else if (*Ptr == 0x48 && *(Ptr + 1) == 0x8B && *(Ptr + 2) == 0xC4)
                    {
                        CollectGarbage = uint64_t(Ptr);
                        break;
                    }
                }

                Hooking::Patch<uint8_t>(CollectGarbage, 0xC3);
            }
        }
        else if (VersionInfo.EngineVersion <= 4.20)
        {
            auto pattern = VersionInfo.FortniteVersion > 3.2 ? Memcury::Scanner::FindPattern("E8 ? ? ? ? EB 26 40 38 3D ? ? ? ?") : Memcury::Scanner::FindPattern("E8 ? ? ? ? F0 FF 0D ? ? ? ? 0F B6 C3");

            if (pattern.IsValid())
                Hooking::Patch<uint8_t>(pattern.RelativeOffset(1).Get(), 0xC3);
        }

        if (GameState->HasAllPlayerBuildableClassesIndexLookup())
            for (auto& [Class, Handle] : GameState->AllPlayerBuildableClassesIndexLookup)
                AFortGameStateAthena::BuildingClassMap[Handle] = Class;

        if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
        {
            auto curl = curl_easy_init();

            curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
            curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            char version[6];

            sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

            auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Server is joinable!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" +
                           (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " +
                           "\"7237230\", \"footer\": {\"text\":\"4e0h Gameserver\", "
                           "\"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/"
                           "L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" +
                           iso8601() + "\"}] }";

            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

            curl_easy_perform(curl);

            curl_easy_cleanup(curl);
        }

        if (!Playlist && VersionInfo.FortniteVersion <= 4)
            if (GameMode->GameSession->HasMaxPlayers())
                GameMode->GameSession->MaxPlayers = 100;

        GUI::gsStatus = Joinable;
        sprintf_s(GUI::windowTitle, "1.8.2 AIs - 4e0h improved by apathy");
        SetConsoleTitleA(GUI::windowTitle);
        GameMode->bWorldIsReady = true;
    }

    if (VersionInfo.EngineVersion >= 4.24 && GameMode->IsA<AFortGameModeAthena>())
    {
        int ReadyPlayers = 0;
        TArray<AFortPlayerControllerAthena*> PlayerList;
        Utils::GetAll<AFortPlayerControllerAthena>(PlayerList);

        for (auto& PlayerController : PlayerList)
        {
            auto PlayerState = PlayerController->PlayerState;

            if (!PlayerState->bIsSpectator && PlayerController->bReadyToStartMatch)
                ReadyPlayers++;
        }

        PlayerList.Free();

        auto VolumeManager = GameState->HasVolumeManager() ? GameState->VolumeManager : nullptr;

        bool bAllLevelsFinishedStreaming = true;
        if (GameState->HasAdditionalPlaylistLevelsStreamed())
        {
            TArray<FPlaylistStreamedLevelData>& AdditionalPlaylistLevels = *(TArray<FPlaylistStreamedLevelData>*)(__int64(GameState) + GameState->GetOffset("AdditionalPlaylistLevelsStreamed") - 0x10);
            for (int i = 0; i < AdditionalPlaylistLevels.Num(); i++)
            {
                auto& AdditionalPlaylistLevel = AdditionalPlaylistLevels.Get(i, FPlaylistStreamedLevelData::Size());

                if (!AdditionalPlaylistLevel.bIsFinishedStreaming || !AdditionalPlaylistLevel.StreamingLevel || !AdditionalPlaylistLevel.StreamingLevel->LoadedLevel->bIsVisible)
                {
                    bAllLevelsFinishedStreaming = false;
                    break;
                }
            }
        }

        static auto WaitingToStart = FName(L"WaitingToStart");
        *Ret = GameMode->bWorldIsReady && (GameState->HasbPlaylistDataIsLoaded() ? GameState->bPlaylistDataIsLoaded : true) && GameMode->MatchState == WaitingToStart && bAllLevelsFinishedStreaming &&
               (!VolumeManager || !(VolumeManager->HasbInSpawningStartup() ? VolumeManager->bInSpawningStartup : GameState->bInSpawningStartup)) &&
               ReadyPlayers >= (GameMode->HasWarmupRequiredPlayerCount() ? GameMode->WarmupRequiredPlayerCount : 1);
    }
    else
        *Ret = callOGWithRet(GameMode, Stack.GetCurrentNativeFunction(), ReadyToStartMatch);

    if (VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 25.20 && !*Ret)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

        auto WarmupDuration = 60.f;
        if (FConfiguration::AutoFillTargetPlayerCount > 0 && FConfiguration::AutoFillBatchSize > 0)
        {
            float needed = ((float)FConfiguration::AutoFillTargetPlayerCount / (float)FConfiguration::AutoFillBatchSize)
                           * (float)FConfiguration::AutoFillIntervalSeconds;
            WarmupDuration = needed + 20.f;
            if (WarmupDuration < 60.f)
                WarmupDuration = 60.f;
        }

        if (GameState->HasWarmupCountdownEndTime())
        {
            GameState->WarmupCountdownStartTime = Time;
            GameState->WarmupCountdownEndTime = Time + WarmupDuration;
            GameMode->WarmupCountdownDuration = WarmupDuration;
            GameMode->WarmupEarlyCountdownDuration = WarmupDuration;
        }
    }
    return;
}

auto SpawnDefaultPawnForIdx = 0;
uint64_t ApplyCharacterCustomization;

void AFortGameMode::SpawnDefaultPawnFor(UObject* Context, FFrame& Stack, AActor** Ret)
{
    AFortPlayerControllerAthena* NewPlayer;
    AActor* StartSpot;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.StepCompiledIn(&StartSpot);
    Stack.IncrementCode();
    auto GameMode = (AFortGameMode*)Context;

    if (!NewPlayer || !StartSpot)
        return;

    auto GameState = GameMode->GameState;
    AFortPlayerPawnAthena* Pawn = nullptr;

    Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, 3);

    while (!Pawn)
    {
        auto PlayerStart = GameMode->ChoosePlayerStart(NewPlayer);
        if (PlayerStart)
            Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), PlayerStart->GetTransform(), NewPlayer, 3);
    }

    *Ret = Pawn;

    auto Num = NewPlayer->WorldInventory ? NewPlayer->WorldInventory->Inventory.ReplicatedEntries.Num() : 0;
    if (Num == 0)
    {
        if (VersionInfo.FortniteVersion <= 1.91 && VersionInfo.FortniteVersion != 1.1 && VersionInfo.FortniteVersion != 1.11 && NewPlayer->HasStrongMyHero())
        {
            static auto HeroCharPartsOffset = NewPlayer->StrongMyHero->GetOffset("CharacterParts");
            auto& HeroCharParts = GetFromOffset<TArray<UObject*>>(NewPlayer->StrongMyHero, HeroCharPartsOffset);
            static auto CharacterPartsOffset = NewPlayer->PlayerState->GetOffset("CharacterParts");
            auto& CharacterParts = GetFromOffset<const UObject* [0x6]>(NewPlayer->PlayerState, CharacterPartsOffset);

            if (HeroCharParts.Num() > 0)
            {
                for (auto& Part : HeroCharParts)
                {
                    static auto PartTypeOffset = Part->GetOffset("CharacterPartType");
                    CharacterParts[GetFromOffset<uint8>(Part, PartTypeOffset)] = Part;
                }
            }
            else
            {
                static auto Head = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
                static auto Body = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
                static auto Backpack = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");

                CharacterParts[0] = Head;
                CharacterParts[1] = Body;
                CharacterParts[3] = Backpack;
            }
        }

        if (NewPlayer->HasXPComponent())
        {
            if (NewPlayer->XPComponent->HasbRegisteredWithQuestManager())
            {
                NewPlayer->XPComponent->bRegisteredWithQuestManager = true;
                NewPlayer->XPComponent->OnRep_bRegisteredWithQuestManager();
            }

            if (NewPlayer->PlayerState->HasSeasonLevelUIDisplay())
            {
                NewPlayer->PlayerState->SeasonLevelUIDisplay = NewPlayer->XPComponent->CurrentLevel;
                NewPlayer->PlayerState->OnRep_SeasonLevelUIDisplay();
            }
        }

        const UObject* BattleBusDef = nullptr;
        const UClass* SupplyDropClass = nullptr;
        if (VersionInfo.FortniteVersion == 18.40)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_HeadbandBus.BBID_HeadbandBus");
        else if (VersionInfo.FortniteVersion == 1.11 || VersionInfo.FortniteVersion == 7.30 || VersionInfo.FortniteVersion == 11.31 || VersionInfo.FortniteVersion == 15.10 || VersionInfo.FortniteVersion == 19.01 ||
                 VersionInfo.FortniteVersion == 28.01)
        {
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_WinterBus.BBID_WinterBus");

            if (VersionInfo.FortniteVersion == 1.11)
                SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/B_AthenaSupplyDrop_Gift.B_AthenaSupplyDrop_Gift_C");
            else
                SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_Holiday.AthenaSupplyDrop_Holiday_C");
        }
        else if (VersionInfo.FortniteVersion == 23.10)
        {
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BattleBus_Booster_Winter.BBID_BattleBus_Booster_Winter");
            SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_Holiday.AthenaSupplyDrop_Holiday_C");
        }
        else if (VersionInfo.FortniteVersion == 5.10 || VersionInfo.FortniteVersion == 9.41 || VersionInfo.FortniteVersion == 14.20 || VersionInfo.FortniteVersion == 18.00 || VersionInfo.FortniteVersion == 22.00 ||
                 VersionInfo.FortniteVersion == 26.20)
        {
            if (VersionInfo.FortniteVersion == 5.10)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus.BBID_BirthdayBus");
            else if (VersionInfo.FortniteVersion == 9.41)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus2nd.BBID_BirthdayBus2nd");
            else if (VersionInfo.FortniteVersion == 14.20)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus3rd.BBID_BirthdayBus3rd");
            else if (VersionInfo.FortniteVersion == 18.00)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus4th.BBID_BirthdayBus4th");
            else if (VersionInfo.FortniteVersion == 22.00)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus5th.BBID_BirthdayBus5th");
            else if (VersionInfo.FortniteVersion == 26.20)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus6th.BBID_BirthdayBus6th");

            SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_BDay.AthenaSupplyDrop_BDay_C");
        }
        else if (VersionInfo.FortniteVersion == 6.20 || VersionInfo.FortniteVersion == 6.21 || VersionInfo.FortniteVersion == 11.10 || VersionInfo.FortniteVersion == 14.40 || VersionInfo.FortniteVersion == 18.21)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_HalloweenBus.BBID_HalloweenBus");
        else if (VersionInfo.FortniteVersion == 26.30)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_HalloweenBus_Booster.BBID_HalloweenBus_Booster");
        else if (VersionInfo.FortniteVersion == 14.30)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BusUpgrade1.BBID_BusUpgrade1");
        else if (VersionInfo.FortniteVersion == 14.50)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BusUpgrade2.BBID_BusUpgrade2");
        else if (VersionInfo.FortniteVersion == 14.60)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BusUpgrade3.BBID_BusUpgrade3");
        else if (VersionInfo.FortniteVersion >= 12.30 && VersionInfo.FortniteVersion <= 12.61)
        {
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_DonutBus.BBID_DonutBus");
            SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_Donut.AthenaSupplyDrop_Donut_C");
        }
        else if (VersionInfo.FortniteVersion == 9.30)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_WorldCupBus.BBID_WorldCupBus");
        else if (VersionInfo.FortniteVersion == 21.00)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_CelebrationBus.BBID_CelebrationBus");
        else if (std::floor(VersionInfo.FortniteVersion) == 27)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_DefaultBus.BBID_DefaultBus");

        if (BattleBusDef)
        {
            if (GameState->HasDefaultBattleBus())
                GameState->DefaultBattleBus = BattleBusDef;

            TArray<AFortAthenaAircraft*> Aircrafts;
            Utils::GetAll<AFortAthenaAircraft>(Aircrafts);
            for (auto& Aircraft : Aircrafts)
            {
                Aircraft->DefaultBusSkin = BattleBusDef;

                if (Aircraft->SpawnedCosmeticActor)
                {
                    static auto Offset = Aircraft->SpawnedCosmeticActor->GetOffset("ActiveSkin");

                    GetFromOffset<const UObject*>(Aircraft->SpawnedCosmeticActor, Offset) = BattleBusDef;
                }
            }
            Aircrafts.Free();
        }

        if (GameState->HasMapInfo() && GameState->MapInfo)
        {
            if (SupplyDropClass)
            {
                if (GameState->MapInfo->HasSupplyDropInfoList())
                    for (auto& Info : GameState->MapInfo->SupplyDropInfoList)
                        Info->SupplyDropClass = SupplyDropClass;
                else
                    GameState->MapInfo->SupplyDropClass = SupplyDropClass;
            }
        }
    }
}

bool GStormMapDataWasRepaired = false;

bool GStormCurvesAreFlat = false;
bool GStormCurvesChecked = false;

const char* GStormStep = "idle";

void StormLog(const char* Fmt, ...)
{
    if (!FConfiguration::bStormLog)
        return;

    char Buffer[1024];

    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);

    printf("[Storm] %s\n", Buffer);
    fflush(stdout);
}

#define STORM_F(Obj, Name) (Obj->Has##Name() ? (float)Obj->Name : -1.f)

void StormDumpIndicator(const char* When, AFortGameMode* GameMode, float TimeSeconds)
{
    if (!FConfiguration::bStormLog)
        return;

    if (!GameMode || !GameMode->SafeZoneIndicator)
    {
        StormLog("%s: there is no safe zone indicator.", When);
        return;
    }

    auto Indicator = GameMode->SafeZoneIndicator;

    const float Start = STORM_F(Indicator, SafeZoneStartShrinkTime);
    const float Finish = STORM_F(Indicator, SafeZoneFinishShrinkTime);

    const int PhaseShown = Indicator->HasCurrentPhase()
                               ? (int)Indicator->CurrentPhase
                               : (GameMode->HasSafeZonePhase() ? (int)GameMode->SafeZonePhase : -1);

    StormLog("%s: phase %d of %d | now %.2f | starts closing %.2f (in %.2fs) | closed %.2f (in %.2fs)",
             When, PhaseShown,
             Indicator->HasPhaseCount() ? (int)Indicator->PhaseCount : -1,
             TimeSeconds, Start, Start - TimeSeconds, Finish, Finish - TimeSeconds);

    StormLog("%s: radius previous %.0f last %.0f next %.0f nextnext %.0f",
             When,
             STORM_F(Indicator, PreviousRadius), STORM_F(Indicator, LastRadius),
             STORM_F(Indicator, NextRadius), STORM_F(Indicator, NextNextRadius));

    if (Indicator->HasNextCenter() && Indicator->HasPreviousCenter())
    {
        auto& Next = Indicator->NextCenter;
        auto& Previous = Indicator->PreviousCenter;

        StormLog("%s: centre previous (%.0f, %.0f, %.0f) -> next (%.0f, %.0f, %.0f)",
                 When, Previous.X, Previous.Y, Previous.Z, Next.X, Next.Y, Next.Z);
    }

    if (Indicator->HasSafeZonePhases())
        StormLog("%s: the engine's phase list has %d entries.", When, Indicator->SafeZonePhases.Num());
    else
        StormLog("%s: this build's indicator has no phase list at all (normal for 14.60).", When);

    StormLog("%s: future replicator %s | damage info %s",
             When,
             (Indicator->HasFutureReplicator() && Indicator->FutureReplicator) ? "present" : "MISSING",
             Indicator->HasCurrentDamageInfo() ? "present" : "MISSING");
}

#undef STORM_F

void StormDumpMapCurves(AFortGameStateAthena* GameState)
{
    if (!FConfiguration::bStormLog || !GameState || !GameState->HasMapInfo() || !GameState->MapInfo)
        return;

    if (!GameState->MapInfo->HasSafeZoneDefinition())
    {
        StormLog("This build's map info has no safe zone definition at all.");
        return;
    }

    static bool bDone = false;
    if (bDone)
        return;
    bDone = true;

    auto& Definition = GameState->MapInfo->SafeZoneDefinition;

    if (FFortSafeZoneDefinition::HasCount())
        StormLog("The map says this playlist has %.0f phases.", Definition.Count.Evaluate(0.f));

    for (int Phase = 0; Phase < 12; Phase++)
    {
        const float Wait = FFortSafeZoneDefinition::HasWaitTime() ? Definition.WaitTime.Evaluate((float)Phase) : -1.f;
        const float Shrink = FFortSafeZoneDefinition::HasShrinkTime() ? Definition.ShrinkTime.Evaluate((float)Phase) : -1.f;
        const float Radius = FFortSafeZoneDefinition::HasRadius() ? Definition.Radius.Evaluate((float)Phase) : -1.f;

        StormLog("  map phase %-2d  hold %7.2fs  close %7.2fs  radius %9.0f", Phase, Wait, Shrink, Radius);
    }
}

namespace StormPacing
{
    struct FPhaseTiming
    {
        float Wait;
        float Shrink;
        float Damage;
    };

    static const FPhaseTiming Table[] = {
         { 210.f, 120.f, 1.f },
         { 180.f, 120.f, 1.f },
         { 150.f, 110.f, 1.f },
         { 120.f, 90.f, 1.f },
         { 90.f, 80.f, 1.f },
         { 75.f, 70.f, 2.f },
         { 60.f, 60.f, 2.f },
         { 45.f, 45.f, 5.f },
         { 40.f, 40.f, 5.f },
         { 30.f, 30.f, 10.f },
         { 25.f, 25.f, 10.f },
    };

    static const FPhaseTiming& For(int Phase)
    {
        const int Count = (int)(sizeof(Table) / sizeof(Table[0]));

        if (Phase < 0)
            Phase = 0;
        if (Phase >= Count)
            Phase = Count - 1;

        return Table[Phase];
    }

    static const float MinimumWait = 30.f;
    static const float MinimumShrink = 10.f;

    static const float EarlyPhaseWait = 20.0f;
    static const float EarlyPhaseShrink = 15.0f;

    static bool IsSane(float Value, float Min, float Max)
    {
        return (Value == Value) && Value >= Min && Value <= Max;
    }

    static float Wait(int Phase, float FromCurve)
    {
        return IsSane(FromCurve, MinimumWait, 3600.f) ? FromCurve : For(Phase).Wait;
    }

    static float Shrink(int Phase, float FromCurve)
    {
        return IsSane(FromCurve, MinimumShrink, 3600.f) ? FromCurve : For(Phase).Shrink;
    }

    static float Damage(int Phase, float FromCurve)
    {
        return IsSane(FromCurve, 0.01f, 100.f) ? FromCurve : For(Phase).Damage;
    }

    static void Schedule(int Phase, float CurveWait, float CurveShrink, float& OutWait, float& OutShrink)
    {
        const int OpenOn = FConfiguration::StormStartPhase;
        const float Scale = (FConfiguration::StormPhaseTimeScale > 0.f) ? FConfiguration::StormPhaseTimeScale : 1.f;

        if (GStormMapDataWasRepaired || GStormCurvesAreFlat)
        {
            CurveWait = 0.f;
            CurveShrink = 0.f;
        }

        if (Phase < OpenOn)
        {
            OutWait = EarlyPhaseWait * Scale;
            OutShrink = EarlyPhaseShrink * Scale;
            return;
        }

        if (FConfiguration::bUseMapStormTimes && IsSane(CurveWait, 0.f, 3600.f) && IsSane(CurveShrink, 1.f, 3600.f))
        {
            OutWait = CurveWait * Scale;
            OutShrink = CurveShrink * Scale;
        }
        else
        {
            OutWait = Wait(Phase, CurveWait) * Scale;
            OutShrink = Shrink(Phase, CurveShrink) * Scale;
        }

        if (!(OutWait >= MinimumWait * Scale))
            OutWait = MinimumWait * Scale;
        if (!(OutShrink >= MinimumShrink * Scale))
            OutShrink = MinimumShrink * Scale;
    }
}

static void ApplySafeZoneSchedule(AFortGameMode* GameMode, float TimeSeconds, float Wait, float Shrink)
{
    if (!GameMode || !GameMode->SafeZoneIndicator)
        return;

    GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + Wait;
    GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + Shrink;
}

static void CallSafeZoneOriginal(AFortGameMode* GameMode, int NewSafeZonePhase_Inp)
{
    if (!AFortGameMode::HandlePostSafeZonePhaseChangedOG)
    {
        static bool bWarned = false;
        if (!bWarned)
        {
            bWarned = true;
            printf("[SafeZone] The game's own phase handler was never found -- running the storm without it.\n");
            fflush(stdout);
        }
        return;
    }

    __try
    {
        AFortGameMode::HandlePostSafeZonePhaseChangedOG(GameMode, NewSafeZonePhase_Inp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[SafeZone] The game's own phase handler faulted. Storm continues; match is not lost.\n");
        fflush(stdout);
    }
}

void StartNewSafeZonePhase(AFortGameMode* GameMode, int NewSafeZonePhase, bool bInitial);

static void HandleSafeZonePhaseChanged_Impl(AFortGameMode* GameMode, int NewSafeZonePhase_Inp)
{
    GStormStep = "entering the phase handler";

    if (!GameMode || !GameMode->SafeZoneIndicator)
    {
        StormLog("Phase change arrived with no game mode or no indicator -- nothing to do.");
        GStormStep = "idle";
        return;
    }

    StormLog("---- phase change, incoming value %d ----", NewSafeZonePhase_Inp);

    if (FConfiguration::bDisableStorm)
    {
        GameMode->SafeZoneIndicator->PreviousRadius = 1000000.f;
        GameMode->SafeZoneIndicator->LastRadius = 1000000.f;
        GameMode->SafeZoneIndicator->NextRadius = 1000000.f;
        GameMode->SafeZoneIndicator->NextNextRadius = 1000000.f;
        GameMode->SafeZoneIndicator->CurrentPhase = 0;

        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = 3600.f * 24.f;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = 3600.f * 25.f;

        if (GameMode->HasbSafeZonePaused())
            GameMode->bSafeZonePaused = true;
        if (GameMode->HasbSafeZoneActive())
            GameMode->bSafeZoneActive = false;

        GStormStep = "idle";
        return;
    }

    GStormStep = "working out which phase this is";
    auto NewSafeZonePhase = NewSafeZonePhase_Inp >= 0 ? NewSafeZonePhase_Inp : ((GameMode->HasSafeZonePhase() ? GameMode->SafeZonePhase : GameMode->SafeZoneIndicator->CurrentPhase) + 1);
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    GStormStep = "reading the world clock";
    float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(GameState);

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        GStormStep = "handing straight to the game's own handler (21.10+)";
        CallSafeZoneOriginal(GameMode, NewSafeZonePhase_Inp);
        GStormStep = "idle";
        return;
    }

    GStormStep = "checking the map info";
    if (!GameState || !GameState->HasMapInfo() || !GameState->MapInfo || !GameState->MapInfo->HasSafeZoneDefinition())
    {
        StormLog("No map info (or no safe zone definition) on the game state -- the storm has no data to read.");
        GStormStep = "idle";
        return;
    }

    const int PhaseNow = GameMode->SafeZoneIndicator->HasCurrentPhase()
                             ? (int)GameMode->SafeZoneIndicator->CurrentPhase
                             : NewSafeZonePhase;

    GStormStep = "reading the map's phase curves";
    StormDumpMapCurves(GameState);

    if (!GStormCurvesChecked)
    {
        GStormCurvesChecked = true;

        auto& CheckDefinition = GameState->MapInfo->SafeZoneDefinition;

        const bool bHasWait = FFortSafeZoneDefinition::HasWaitTime();
        const bool bHasShrink = FFortSafeZoneDefinition::HasShrinkTime();

        const float EarlyWaitValue = bHasWait ? CheckDefinition.WaitTime.Evaluate(1.f) : 0.f;
        const float LateWaitValue = bHasWait ? CheckDefinition.WaitTime.Evaluate(9.f) : 0.f;
        const float EarlyShrinkValue = bHasShrink ? CheckDefinition.ShrinkTime.Evaluate(1.f) : 0.f;
        const float LateShrinkValue = bHasShrink ? CheckDefinition.ShrinkTime.Evaluate(9.f) : 0.f;

        GStormCurvesAreFlat = (EarlyWaitValue == LateWaitValue) && (EarlyShrinkValue == LateShrinkValue);

        if (GStormCurvesAreFlat)
            StormLog("The map gives every phase the same timing (%.0fs hold, %.0fs close) -- that is a missing curve table, not real pacing. Using the built-in pacing instead, so late circles are fast and early ones are not.",
                     EarlyWaitValue, EarlyShrinkValue);
        else
            StormLog("The map has real per-phase curves (phase 1 holds %.0fs, phase 9 holds %.0fs) -- using them.",
                     EarlyWaitValue, LateWaitValue);
    }

    StormDumpIndicator("before anything", GameMode, TimeSeconds);

    {
        GStormStep = "writing the schedule before the game's handler runs";
        auto& EarlyDefinition = GameState->MapInfo->SafeZoneDefinition;
        float EarlyWaitCurve = 0.f;
        float EarlyShrinkCurve = 0.f;

        if (FFortSafeZoneDefinition::HasWaitTime())
            EarlyWaitCurve = EarlyDefinition.WaitTime.Evaluate((float)PhaseNow);
        if (FFortSafeZoneDefinition::HasShrinkTime())
            EarlyShrinkCurve = EarlyDefinition.ShrinkTime.Evaluate((float)PhaseNow);

        float EarlyWait = 0.f;
        float EarlyShrink = 0.f;
        StormPacing::Schedule(PhaseNow, EarlyWaitCurve, EarlyShrinkCurve, EarlyWait, EarlyShrink);

        ApplySafeZoneSchedule(GameMode, TimeSeconds, EarlyWait, EarlyShrink);

        StormLog("Phase %d: map curve says hold %.2fs / close %.2fs; using hold %.2fs / close %.2fs%s.",
                 PhaseNow, EarlyWaitCurve, EarlyShrinkCurve, EarlyWait, EarlyShrink,
                 PhaseNow < FConfiguration::StormStartPhase ? " (stepping through to the opening phase)" : "");

        static int LastReportedPhase = -1;
        if (PhaseNow != LastReportedPhase)
        {
            LastReportedPhase = PhaseNow;
            printf("[SafeZone] Phase %d -- holding %.0fs, then closing over %.0fs.\n", PhaseNow, EarlyWait, EarlyShrink);
            fflush(stdout);
        }

        StormDumpIndicator("schedule written", GameMode, TimeSeconds);
    }

    constexpr static std::array<float, 8> LateGameDurations{
        0.f, 120.f, 90.f, 60.f, 50.f, 35.f, 30.f, 40.f,
    };

    constexpr static std::array<float, 8> LateGameHoldDurations{
        0.f, 90.f, 75.f, 60.f, 45.f, 30.f, 0.f, 0.f,
    };

    auto SafeZoneDefinition = &GameState->MapInfo->SafeZoneDefinition;

    const bool bUseScalableFloats =
        VersionInfo.FortniteVersion >= 13.00 && VersionInfo.FortniteVersion < 15.20;

    auto GetHoldDurationFor = [&](int Phase) -> float
    {
        if (!FFortSafeZoneDefinition::HasWaitTime())
            return 0.f;
        return SafeZoneDefinition->WaitTime.Evaluate((float)Phase);
    };
    auto GetShrinkDurationFor = [&](int Phase) -> float
    {
        if (!FFortSafeZoneDefinition::HasShrinkTime())
            return 0.f;
        return SafeZoneDefinition->ShrinkTime.Evaluate((float)Phase);
    };

    static auto DurationsOffset = 0;
    if (DurationsOffset == 0)
    {
        DurationsOffset = 0x258;

        if (VersionInfo.FortniteVersion >= 18)
            DurationsOffset = 0x248;
        else if (VersionInfo.FortniteVersion < 15.20)
            DurationsOffset = 0x1f8;
    }

    TArray<float>* DurationsPtr = bUseScalableFloats ? nullptr : (TArray<float>*)(SafeZoneDefinition + DurationsOffset);
    TArray<float>* HoldDurationsPtr = bUseScalableFloats ? nullptr : (TArray<float>*)(SafeZoneDefinition + DurationsOffset - 0x10);

    if (VersionInfo.FortniteVersion >= 13.00 && !bUseScalableFloats)
    {
        static bool bSetDurations = false;
        if (!bSetDurations)
        {
            bSetDurations = true;

            auto GameData = GameMode->HasAthenaGameDataTable() ? GameMode->AthenaGameDataTable : GameState->AthenaGameDataTable;

            auto ShrinkTime = FName(L"Default.SafeZone.ShrinkTime");
            auto HoldTime = FName(L"Default.SafeZone.WaitTime");

            if (DurationsPtr)
            {
                for (int i = 0; i < DurationsPtr->Num(); i++)
                    UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData, ShrinkTime, (float)i, nullptr, &(*DurationsPtr)[i], FString());
            }
            if (HoldDurationsPtr)
            {
                for (int i = 0; i < HoldDurationsPtr->Num(); i++)
                    UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData, HoldTime, (float)i, nullptr, &(*HoldDurationsPtr)[i], FString());
            }
        }
    }

    GStormStep = "inside the game's own phase handler";
    StormLog("Calling the game's own phase handler now.");

    CallSafeZoneOriginal(GameMode, NewSafeZonePhase_Inp);

    GStormStep = "back from the game's own phase handler";
    StormLog("Back from the game's own phase handler in one piece.");
    StormDumpIndicator("after the game's handler", GameMode, TimeSeconds);

    if (VersionInfo.FortniteVersion >= 13.00 && (!FConfiguration::bLateGame || GameMode->SafeZonePhase > FConfiguration::LateGameZone))
    {
        float Duration = 0.f;
        float HoldDuration = 0.f;

        if (bUseScalableFloats)
        {
            Duration = GetShrinkDurationFor(PhaseNow);
            HoldDuration = GetHoldDurationFor(PhaseNow);
        }
        else
        {
            Duration = (DurationsPtr && PhaseNow < DurationsPtr->Num()) ? (*DurationsPtr)[PhaseNow] : 0.f;
            HoldDuration = (HoldDurationsPtr && PhaseNow < HoldDurationsPtr->Num()) ? (*HoldDurationsPtr)[PhaseNow] : 0.f;
        }

        GStormStep = "putting the schedule back on after the game's handler";

        float FinalHold = 0.f;
        float FinalShrink = 0.f;
        StormPacing::Schedule(PhaseNow, HoldDuration, Duration, FinalHold, FinalShrink);

        ApplySafeZoneSchedule(GameMode, TimeSeconds, FinalHold, FinalShrink);

        StormLog("Schedule re-applied for phase %d: hold %.2fs, close %.2fs.", PhaseNow, FinalHold, FinalShrink);
    }

    if (FConfiguration::bLateGame && GameMode->SafeZonePhase < FConfiguration::LateGameZone)
    {
        ApplySafeZoneSchedule(GameMode, TimeSeconds, StormPacing::EarlyPhaseWait, StormPacing::EarlyPhaseShrink);
        return;
    }
    else if (FConfiguration::bLateGame && GameMode->SafeZonePhase == FConfiguration::LateGameZone)
    {
        if (FConfiguration::bLateGameLongZone)
            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = 676767.f;
        else if (VersionInfo.FortniteVersion >= 13)
            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + (bUseScalableFloats
                ? GetHoldDurationFor(FConfiguration::LateGameZone)
                : ((HoldDurationsPtr && FConfiguration::LateGameZone < HoldDurationsPtr->Num()) ? (*HoldDurationsPtr)[FConfiguration::LateGameZone] : 90.f));
        if (VersionInfo.FortniteVersion >= 13)
            GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + (bUseScalableFloats
                ? GetShrinkDurationFor(FConfiguration::LateGameZone)
                : ((DurationsPtr && FConfiguration::LateGameZone < DurationsPtr->Num()) ? (*DurationsPtr)[FConfiguration::LateGameZone] : 60.f));
    }

    if (FConfiguration::bLateGame && (AFortGameMode::SafeZoneLoc.X != 0 || AFortGameMode::SafeZoneLoc.Y != 0 || AFortGameMode::SafeZoneLoc.Z != 0))
    {
        GameMode->SafeZoneIndicator->NextCenter = AFortGameMode::SafeZoneLoc;
        GameMode->SafeZoneIndicator->LastCenter = AFortGameMode::SafeZoneLoc;
    }

    GStormStep = "telling players the storm moved (quest stats)";

    if (NewSafeZonePhase > (FConfiguration::bLateGame ? FConfiguration::LateGameZone : 1))
    {
        for (auto& UncastedPlayer : GameMode->AlivePlayers)
        {
            auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

            if (!PlayerController || !PlayerController->PlayerState || PlayerController->PlayerState->bIsABot)
                continue;

            auto Quests = PlayerController->GetQuestManager(1);
            if (Quests)
                Quests->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
        }
    }

    GStormStep = "deciding whether to jump straight to the opening phase";

    static bool bOpeningJumpHandled = false;

    if (!bOpeningJumpHandled && !FConfiguration::bLateGame)
    {
        bOpeningJumpHandled = true;

        const int OpenOn = FConfiguration::StormStartPhase;

        if (OpenOn <= PhaseNow)
        {
            StormLog("Already on phase %d, which is at or past the opening phase %d -- letting it run.", PhaseNow, OpenOn);
        }
        else if (!GameMode->SafeZoneIndicator->HasSafeZonePhases())
        {
            StormLog("This build's indicator keeps no phase list, so the storm cannot be moved straight to phase %d. "
                     "Phases below %d will run in %.0fs each instead, which gets there within about %.0fs.",
                     OpenOn, OpenOn, StormPacing::EarlyPhaseWait + StormPacing::EarlyPhaseShrink,
                     (StormPacing::EarlyPhaseWait + StormPacing::EarlyPhaseShrink) * (float)(OpenOn - PhaseNow));
        }
        else
        {
            auto& PhaseList = GameMode->SafeZoneIndicator->SafeZonePhases;

            if (PhaseList.IsValidIndex(OpenOn))
            {
                printf("[SafeZone] Opening on phase %d instead of %d.\n", OpenOn, PhaseNow);
                fflush(stdout);

                GStormStep = "jumping the storm to the opening phase";
                StartNewSafeZonePhase(GameMode, OpenOn, true);
            }
            else
            {
                printf("[SafeZone] Wanted to open on phase %d but the phase list only has %d entries -- staying on %d.\n",
                       OpenOn, PhaseList.Num(), PhaseNow);
                fflush(stdout);
            }
        }
    }

    GStormStep = "finished the phase change";
    StormLog("Phase change handled cleanly.");
    GStormStep = "idle";
}

void AFortGameMode::HandlePostSafeZonePhaseChanged(AFortGameMode* GameMode, int NewSafeZonePhase_Inp)
{
    FCrashReporter::EnterGuardedSection();

    __try
    {
        HandleSafeZonePhaseChanged_Impl(GameMode, NewSafeZonePhase_Inp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[SafeZone] Storm phase change faulted while %s. Contained -- the match continues.\n", GStormStep);
        fflush(stdout);
        GStormStep = "idle";
    }

    FCrashReporter::LeaveGuardedSection();
}

uint64_t NotifyGameMemberAdded_ = 0;
int16_t WorldPlayerId = 0;
void AFortGameMode::HandleStartingNewPlayer_(UObject* Context, FFrame& Stack)
{
    AFortPlayerControllerAthena* NewPlayer;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.IncrementCode();
    auto GameMode = (AFortGameMode*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    AFortPlayerStateAthena* PlayerState = (AFortPlayerStateAthena*)NewPlayer->PlayerState;

    if (VersionInfo.FortniteVersion <= 2.5)
    {
        NewPlayer->QuickBars = UWorld::SpawnActor<AFortQuickBars>(FVector{});
        NewPlayer->QuickBars->SetOwner(NewPlayer);
    }

    if (PlayerState->HasSquadId())
    {
        PlayerState->SquadId = PlayerState->TeamIndex - 3;
        PlayerState->OnRep_SquadId();
    }

    if (GameState->HasGameMemberInfoArray())
    {
        auto Member = (FGameMemberInfo*)malloc(FGameMemberInfo::Size());
        memset((PBYTE)Member, 0, FGameMemberInfo::Size());

        Member->MostRecentArrayReplicationKey = -1;
        Member->ReplicationID = -1;
        Member->ReplicationKey = -1;
        Member->TeamIndex = PlayerState->TeamIndex;
        Member->SquadId = PlayerState->SquadId;
        Member->MemberUniqueId = PlayerState->HasUniqueID() ? PlayerState->UniqueID : PlayerState->UniqueId;

        auto& NewMember = GameState->GameMemberInfoArray.Members.Add(*Member, FGameMemberInfo::Size());
        GameState->GameMemberInfoArray.MarkItemDirty(NewMember);

        auto NotifyGameMemberAdded = (void (*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*))NotifyGameMemberAdded_;
        if (NotifyGameMemberAdded)
            NotifyGameMemberAdded(GameState, Member->SquadId, Member->TeamIndex, &Member->MemberUniqueId);

        free(Member);
    }

    if (!NewPlayer->WorldInventory)
    {
        NewPlayer->WorldInventory = UWorld::SpawnActor<AFortInventory>(NewPlayer->WorldInventoryClass, FVector{}, FRotator{}, NewPlayer);
        NewPlayer->WorldInventory->InventoryType = 0;
    }

    if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2"))
        AFortAthenaCreativePortal::Create(NewPlayer);

    PlayerState->WorldPlayerId = WorldPlayerId;

    return callOG(GameMode, Stack.GetCurrentNativeFunction(), HandleStartingNewPlayer, NewPlayer);
}

uint8_t AFortGameMode::PickTeam(AFortGameMode* GameMode, uint8_t PreferredTeam, AFortPlayerControllerAthena* Controller)
{
    if (!GameMode->HasWarmupRequiredPlayerCount())
        return 0;

    uint8_t ret = CurrentTeam;
    auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount()
                        ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData)
                        : nullptr;

    if (wcscmp(FConfiguration::Playlist, L"/DurianPlaylist/Playlist/Playlist_Durian.Playlist_Durian") == 0)
    {
        CurrentTeam++;
        return ret;
    }
    printf("Picked team %d %d\n", ret, Playlist ? Playlist->MaxSquadSize : 1);
    if (bIsLargeTeamGame)
    {
        if (CurrentTeam == 4)
            CurrentTeam = 3;
        else
            CurrentTeam = 4;
    }
    else
    {
        if (++PlayersOnCurTeam >= (Playlist ? Playlist->MaxSquadSize : 1))
        {
            CurrentTeam++;
            PlayersOnCurTeam = 0;
        }
    }

    return ret;
}

bool AFortGameMode::StartAircraftPhase(AFortGameMode* GameMode, char a2)
{

    if (!BotAI::BusIsClearedForLaunch())
    {
        static int Refused = 0;

        if (++Refused <= 5)
            printf("[BUS] Something tried to start the bus before it was cleared to go -- held on the ground.\n");

        if (Refused == 5)
            printf("[BUS] (further attempts will not be printed)\n");

        fflush(stdout);
        return false;
    }

    auto Ret = StartAircraftPhaseOG(GameMode, a2);

    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount()
                        ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData)
                        : nullptr;
    if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
    {
        auto curl = curl_easy_init();

        curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
        curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        char version[6];

        sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

        auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has started!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" +
                       (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"},{\"name\":\"Players\",\"value\":\"" + std::to_string(GameMode->AlivePlayers.Num()).c_str() +
                       "\"}], \"color\": " +
                       "\"7237230\", \"footer\": {\"text\":\"4e0h Gameserver\", "
                       "\"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/"
                       "L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" +
                       iso8601() + "\"}] }";

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

        curl_easy_perform(curl);

        curl_easy_cleanup(curl);
    }
    GUI::gsStatus = StartedMatch;
    sprintf_s(GUI::windowTitle, "1.8.2 AIs - 4e0h improved by apathy");
    SetConsoleTitleA(GUI::windowTitle);

    if (FConfiguration::bJoinInProgress || (Playlist && (Playlist->HasbAllowJoinInProgress() ? Playlist->bAllowJoinInProgress : false)))
        *(bool*)(uint64_t(&GameMode->WarmupRequiredPlayerCount) - 4) = false;

    if (FConfiguration::bLateGame && VersionInfo.FortniteVersion < 25.20)
    {
        auto Aircraft = GameState->HasAircrafts() ? (GameState->Aircrafts.Num() > 0 ? GameState->Aircrafts[0] : nullptr) : GameState->Aircraft;

        if (!Aircraft)
            return Ret;

        FVector Loc;
        bool bScuffed = false;
        if (GameMode->SafeZoneLocations.Num() < 4)
        {
            bScuffed = true;

            TArray<ABuildingFoundation*> Foundations;
            Utils::GetAll<ABuildingFoundation>(Foundations);
            auto Foundation = Foundations[rand() % Foundations.Num()];

            Foundations.Free();

            SafeZoneLoc = Loc = Foundation->K2_GetActorLocation();
        }
        else
        {
            Loc = GameMode->SafeZoneLocations.Get(FConfiguration::LateGameZone + (VersionInfo.FortniteVersion >= 24 ? 3 : 0) - 1, FVector::Size());
        }

        Loc.Z = 17500.f;

        if (GameState->HasDefaultParachuteDeployTraceForGroundDistance())
        {
            GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;
        }

        if (Aircraft->HasFlightInfo())
        {
            Aircraft->FlightInfo.FlightSpeed = 0.f;

            Aircraft->FlightInfo.FlightStartLocation = Loc;

            Aircraft->FlightInfo.TimeTillFlightEnd = 7.f;
            Aircraft->FlightInfo.TimeTillDropEnd = 7.f;
            Aircraft->FlightInfo.TimeTillDropStart = 0.f;
        }
        else
        {
            Aircraft->FlightSpeed = 0.f;

            Aircraft->FlightStartLocation = Loc;

            if (Aircraft->HasTimeTillFlightEnd())
            {
                Aircraft->TimeTillFlightEnd = 7.f;
                Aircraft->TimeTillDropEnd = 7.f;
                Aircraft->TimeTillDropStart = 0.f;
            }
        }
        Aircraft->DropStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        Aircraft->DropEndTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 7.f;
        Aircraft->FlightStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        Aircraft->FlightEndTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 7.f;
    }

    return Ret;
}

void AFortGameMode::OnAircraftExitedDropZone_(UObject* Context, FFrame& Stack)
{
    AFortAthenaAircraft* Aircraft;
    Stack.StepCompiledIn(&Aircraft);
    Stack.IncrementCode();

    auto GameMode = (AFortGameMode*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    if (FConfiguration::bLateGame)
    {
        static auto CompClass = FindClass("FortControllerComponent_Aircraft");

        if (CompClass)
        {
            for (auto& Player : GameMode->AlivePlayers)
            {
                if (((AFortPlayerControllerAthena*)Player)->IsInAircraft())
                {
                    ((AFortPlayerControllerAthena*)Player)->GetAircraftComponent()->ServerAttemptAircraftJump(FRotator{});
                }
            }
        }
        else
        {
            for (auto& Player : GameMode->AlivePlayers)
            {
                if (((AFortPlayerControllerAthena*)Player)->IsInAircraft())
                {
                    ((AFortPlayerControllerAthena*)Player)->ServerAttemptAircraftJump(FRotator{});
                }
            }
        }
    }

    if (FConfiguration::bLateGame)
    {
        GameState->GamePhase = 4;
        GameState->GamePhaseStep = 7;
        GameState->OnRep_GamePhase(3);
    }

    callOG(GameMode, Stack.GetCurrentNativeFunction(), OnAircraftExitedDropZone, Aircraft);
}

TArray<FFortSafeZonePhaseInfo> Phases;

AFortSafeZoneIndicator* SetupSafeZoneIndicator(AFortGameMode* GameMode)
{
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    if (!GameMode->SafeZoneIndicator)
    {
        AFortSafeZoneIndicator* SafeZoneIndicator = UWorld::SpawnActor<AFortSafeZoneIndicator>(GameMode->SafeZoneIndicatorClass, FVector{});

        if (SafeZoneIndicator)
        {
            FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;

            float RawSafeZoneCount = SafeZoneDefinition.Count.Evaluate();
            float SafeZoneCount = RawSafeZoneCount;

            const bool bCountIsFinite = (RawSafeZoneCount == RawSafeZoneCount) &&
                                        (RawSafeZoneCount < 1e30f) && (RawSafeZoneCount > -1e30f);

            if (!bCountIsFinite || RawSafeZoneCount < 1.f || RawSafeZoneCount > 64.f)
            {
                printf("[SafeZone] Count.Evaluate() returned %f -- OUT OF SANE RANGE (1..64). "
                       "Clamping to 10 to prevent the unbounded phase-allocation crash.\n",
                       RawSafeZoneCount);
                SafeZoneCount = 10.f;
            }

            if (SafeZoneCount > 64.f)
                SafeZoneCount = 64.f;

            auto& Array = SafeZoneIndicator->HasSafeZonePhases() ? SafeZoneIndicator->SafeZonePhases : Phases;

            if (Array.IsValid())
                Array.Free();

            const float Time = (float)UGameplayStatics::GetTimeSeconds(GameState);

            for (float i = 0; i < SafeZoneCount; i++)
            {
                auto PhaseInfo = (FFortSafeZonePhaseInfo*)malloc(FFortSafeZonePhaseInfo::Size());
                memset((PBYTE)PhaseInfo, 0, FFortSafeZonePhaseInfo::Size());

                const int PhaseIndex = (int)i;

                PhaseInfo->Radius = SafeZoneDefinition.Radius.Evaluate(i);

                float WaitTime = StormPacing::Wait(PhaseIndex, SafeZoneDefinition.WaitTime.Evaluate(i));
                float ShrinkTime = StormPacing::Shrink(PhaseIndex, SafeZoneDefinition.ShrinkTime.Evaluate(i));

                PhaseInfo->WaitTime = WaitTime;
                PhaseInfo->ShrinkTime = ShrinkTime;
                PhaseInfo->PlayerCap = (int)SafeZoneDefinition.PlayerCapSolo.Evaluate(i);

                float CurveDamage = 0.f;
                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->AthenaGameDataTable, FName(L"Default.SafeZone.Damage"), i, nullptr, &CurveDamage, FString());

                float PhaseDamage = StormPacing::Damage(PhaseIndex, CurveDamage);
                if (PhaseIndex == 0)
                    PhaseDamage = 0.01f;

                PhaseInfo->DamageInfo.Damage = PhaseDamage;
                PhaseInfo->DamageInfo.bPercentageBasedDamage = true;
                PhaseInfo->TimeBetweenStormCapDamage = GameMode->TimeBetweenStormCapDamage.Evaluate(i);
                PhaseInfo->StormCapDamagePerTick = GameMode->StormCapDamagePerTick.Evaluate(i);
                PhaseInfo->StormCampingIncrementTimeAfterDelay = GameMode->StormCampingIncrementTimeAfterDelay.Evaluate(i);
                PhaseInfo->StormCampingInitialDelayTime = GameMode->StormCampingInitialDelayTime.Evaluate(i);
                PhaseInfo->MegaStormGridCellThickness = (int)SafeZoneDefinition.MegaStormGridCellThickness.Evaluate(i);

                if (FFortSafeZonePhaseInfo::HasUsePOIStormCenter())
                    PhaseInfo->UsePOIStormCenter = false;

                if (GameMode->SafeZoneLocations.GetData() && GameMode->SafeZoneLocations.Num() > i)
                    PhaseInfo->Center = GameMode->SafeZoneLocations.Get((int)i, FVector::Size());

                Array.Add(*PhaseInfo, FFortSafeZonePhaseInfo::Size());
                free(PhaseInfo);

                if (SafeZoneIndicator->HasPhaseCount())
                    SafeZoneIndicator->PhaseCount++;
            }

            SafeZoneIndicator->OnRep_PhaseCount();

            float InitWait = Array[0].WaitTime;
            float InitShrink = Array[0].ShrinkTime;
            if (!(InitWait > 5.f) || !(InitWait < 3600.f))
                InitWait = 90.f;
            if (!(InitShrink > 0.f) || !(InitShrink < 3600.f))
                InitShrink = 60.f;

            SafeZoneIndicator->SafeZoneStartShrinkTime = Time + InitWait;
            SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + InitShrink;

            SafeZoneIndicator->CurrentPhase = 0;
            SafeZoneIndicator->OnRep_CurrentPhase();
        }

        GameMode->SafeZoneIndicator = SafeZoneIndicator;
        GameState->SafeZoneIndicator = SafeZoneIndicator;
        GameState->OnRep_SafeZoneIndicator();
    }

    return GameMode->SafeZoneIndicator;
}

void StartNewSafeZonePhase(AFortGameMode* GameMode, int NewSafeZonePhase, bool bInitial = false)
{
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(GameState);
    auto& Array = GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;

    if (Array.IsValidIndex(NewSafeZonePhase))
    {
        if (Array.IsValidIndex(NewSafeZonePhase - 1))
        {
            auto& PreviousPhaseInfo = Array.Get(NewSafeZonePhase - 1, FFortSafeZonePhaseInfo::Size());

            GameMode->SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
            GameMode->SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
        }

        auto& PhaseInfo = Array.Get(NewSafeZonePhase, FFortSafeZonePhaseInfo::Size());

        GameMode->SafeZoneIndicator->NextCenter = PhaseInfo.Center;
        GameMode->SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
        GameMode->SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

        if (Array.IsValidIndex(NewSafeZonePhase + 1))
        {
            auto& NextPhaseInfo = Array.Get(NewSafeZonePhase + 1, FFortSafeZonePhaseInfo::Size());

            if (GameMode->SafeZoneIndicator->HasFutureReplicator() && GameMode->SafeZoneIndicator->FutureReplicator)
            {
                GameMode->SafeZoneIndicator->FutureReplicator->NextNextCenter = NextPhaseInfo.Center;
                GameMode->SafeZoneIndicator->FutureReplicator->NextNextRadius = NextPhaseInfo.Radius;
            }

            GameMode->SafeZoneIndicator->NextNextCenter = NextPhaseInfo.Center;
            GameMode->SafeZoneIndicator->NextNextRadius = NextPhaseInfo.Radius;
            GameMode->SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhaseInfo.MegaStormGridCellThickness;
        }

        float PhaseWait = 0.f;
        float PhaseShrink = 0.f;
        StormPacing::Schedule(NewSafeZonePhase, PhaseInfo.WaitTime, PhaseInfo.ShrinkTime, PhaseWait, PhaseShrink);

        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = FConfiguration::bLateGame && FConfiguration::bLateGameLongZone ? 676767.f : TimeSeconds + PhaseWait;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + PhaseShrink;

        GameMode->SafeZoneIndicator->CurrentDamageInfo = PhaseInfo.DamageInfo;
        GameMode->SafeZoneIndicator->OnRep_CurrentDamageInfo();

        GameMode->SafeZoneIndicator->CurrentPhase = NewSafeZonePhase;
        GameMode->SafeZoneIndicator->OnRep_CurrentPhase();

        GameMode->SafeZoneIndicator->OnSafeZonePhaseChanged.Process();

        if (GameMode->SafeZoneIndicator->HasFutureReplicator())
        {
            auto& SafeZoneState = *(uint8_t*)(__int64(&GameMode->SafeZoneIndicator->FutureReplicator) - 0x4);
            SafeZoneState = 2;
        }

        GameMode->SafeZoneIndicator->OnSafeZoneStateChange(2, false);
        if (GameMode->SafeZoneIndicator->HasSafezoneStateChangedDelegate())
            GameMode->SafeZoneIndicator->SafezoneStateChangedDelegate.Process(GameMode->SafeZoneIndicator, 2);

        if (!bInitial)
            for (auto& UncastedPlayer : GameMode->AlivePlayers)
            {
                auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

                if (!PlayerController->PlayerState || PlayerController->PlayerState->bIsABot)
                    continue;

                PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
            }
    }
}

void (*SpawnInitialSafeZoneOG)(AFortGameMode* GameMode);
void SpawnInitialSafeZone(AFortGameMode* GameMode)
{
    GameMode->bSafeZoneActive = true;
    auto SafeZoneIndicator = SetupSafeZoneIndicator(GameMode);

    if (!SafeZoneIndicator)
    {
        printf("[SafeZone] No safe zone indicator -- storm cannot start.\n");
        return;
    }

    SafeZoneIndicator->OnSafeZonePhaseChanged.Bind(GameMode, FName(L"HandlePostSafeZonePhaseChanged"));
    GameMode->OnSafeZoneIndicatorSpawned.Process(SafeZoneIndicator);

    int StartPhase = FConfiguration::bLateGame
                         ? (FConfiguration::LateGameZone + (VersionInfo.FortniteVersion >= 24 ? 3 : 0))
                         : FConfiguration::StormStartPhase;

    {
        auto& PhaseArray = SafeZoneIndicator->HasSafeZonePhases() ? SafeZoneIndicator->SafeZonePhases : Phases;
        const int LastPhase = PhaseArray.Num() - 1;

        if (StartPhase < 0)
            StartPhase = 0;
        if (LastPhase >= 0 && StartPhase > LastPhase)
            StartPhase = LastPhase;

        printf("[SafeZone] Storm opening on phase %d of %d.\n", StartPhase, LastPhase);
    }

    StartNewSafeZonePhase(GameMode, StartPhase, true);
}

static bool GStormDamageParked = false;
static std::vector<float> GParkedStormDamage;

static void ParkStormDamage(AFortGameMode* GameMode)
{
    if (GStormDamageParked || !GameMode || !GameMode->SafeZoneIndicator)
        return;

    auto& Array = GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;

    if (!Array.IsValid())
        return;

    GParkedStormDamage.clear();

    float Nothing = 0.f;

    for (int i = 0; i < Array.Num(); i++)
    {
        auto& PhaseInfo = Array.Get(i, FFortSafeZonePhaseInfo::Size());

        GParkedStormDamage.push_back((float)PhaseInfo.DamageInfo.Damage);
        PhaseInfo.DamageInfo.Damage = Nothing;
    }

    GStormDamageParked = true;

    printf("[ZONE] Storm fully paused -- it will not move and it will not hurt anyone until \"cheat resume zone\".\n");
    fflush(stdout);
}

static void UnparkStormDamage(AFortGameMode* GameMode)
{
    if (!GStormDamageParked)
        return;

    GStormDamageParked = false;

    if (!GameMode || !GameMode->SafeZoneIndicator)
    {
        GParkedStormDamage.clear();
        return;
    }

    auto& Array = GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;

    if (Array.IsValid())
    {
        for (int i = 0; i < Array.Num() && i < (int)GParkedStormDamage.size(); i++)
        {
            auto& PhaseInfo = Array.Get(i, FFortSafeZonePhaseInfo::Size());

            float Restored = GParkedStormDamage[(size_t)i];
            PhaseInfo.DamageInfo.Damage = Restored;
        }
    }

    GParkedStormDamage.clear();

    printf("[ZONE] Storm running again.\n");
    fflush(stdout);
}

void (*UpdateSafeZonesPhaseOG)(AFortGameMode* GameMode);
void UpdateSafeZonesPhase(AFortGameMode* GameMode)
{
    if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
    {

        if (GameMode && GameMode->SafeZoneIndicator)
        {
            const float Now = (float)UGameplayStatics::GetTimeSeconds(GameMode);
            float HeldOpen = Now + 600.f;

            if (GameMode->SafeZoneIndicator->HasSafeZoneStartShrinkTime() && (float)GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime < HeldOpen)
                GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = HeldOpen;

            if (GameMode->SafeZoneIndicator->HasSafeZoneFinishShrinkTime() && (float)GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime < HeldOpen)
                GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = HeldOpen;

            if (GameMode->HasbSafeZonePaused())
                GameMode->bSafeZonePaused = true;

            ParkStormDamage(GameMode);
        }

        return;
    }

    UnparkStormDamage(GameMode);

    auto& Array = GameMode->SafeZoneIndicator && GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;
    if (GameMode->bSafeZoneActive && UGameplayStatics::GetTimeSeconds(GameMode) >= GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime && !GameMode->bSafeZonePaused &&
        Array.IsValidIndex(GameMode->SafeZoneIndicator->CurrentPhase + 1))
        StartNewSafeZonePhase(GameMode, GameMode->SafeZoneIndicator->CurrentPhase + 1);

    return UpdateSafeZonesPhaseOG(GameMode);
}

void GetPhaseInfo(UObject* Context, FFrame& Stack, bool* Ret)
{
    auto& OutSafeZonePhase = Stack.StepCompiledInRef<FFortSafeZonePhaseInfo>();
    int32 InPhaseToGet;
    Stack.StepCompiledIn(&InPhaseToGet);
    Stack.IncrementCode();
    auto SafeZoneIndicator = (AFortSafeZoneIndicator*)Context;
    auto& Array = SafeZoneIndicator->HasSafeZonePhases() ? SafeZoneIndicator->SafeZonePhases : Phases;

    if (Array.IsValidIndex(InPhaseToGet))
    {
        OutSafeZonePhase = Array[InPhaseToGet];

        *Ret = true;
        return;
    }
    *Ret = false;
}

class AFortNavMesh : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortNavMesh);

    DEFINE_PROP(HotSpotManager, const UObject*);
};
void (*OnWorldInitDoneOG)(UNavigationSystem* NavSys, char Mode);
void OnWorldInitDone(UNavigationSystem* NavSys, char Mode)
{
    printf("OnWorldInitDone\n");
}

void AFortGameMode::FinishWorldInitialization(AFortGameMode* _this, AActor* WorldManager)
{
    auto GameMode = (AFortGameModeAthena*)_this;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    if (VersionInfo.EngineVersion >= 4.22 && VersionInfo.EngineVersion < 4.26)
        GameState->OnRep_CurrentPlaylistInfo();

    printf("[GameMode] FinishWorldInitialization\n");
    FinishWorldInitializationOG(_this, WorldManager);

    auto AddToTierData = [&](const UDataTable* Table, TArray<FFortLootTierData*>& TempArr)
    {
        if (!Table)
            return;

        Table->AddToRoot();
        if (VersionInfo.FortniteVersion >= 20)
        {
            if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                for (auto& ParentTable : CompositeTable->ParentTables)
                    if (ParentTable)
                        for (auto& [Key, Val] : *(TMap<int32, FFortLootTierData*>*)(__int64(ParentTable) + 0x30))
                            TempArr.Add(Val);

            for (auto& [Key, Val] : *(TMap<int32, FFortLootTierData*>*)(__int64(Table) + 0x30))
            {
                bool bFound = false;

                for (auto& TierData : TempArr)
                    if (TierData->TierGroup == Val->TierGroup && TierData->LootPackage == Val->LootPackage)
                    {
                        TierData = Val;
                        bFound = true;
                        break;
                    }

                if (!bFound)
                    TempArr.Add(Val);
            }
        }
        else
        {
            if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                for (auto& ParentTable : CompositeTable->ParentTables)
                    if (ParentTable)
                        for (auto& [Key, Val] : (TMap<FName, FFortLootTierData*>)ParentTable->RowMap)
                            TempArr.Add(Val);

            for (auto& [Key, Val] : (TMap<FName, FFortLootTierData*>)Table->RowMap)
            {
                bool bFound = false;

                for (auto& TierData : TempArr)
                    if (TierData->TierGroup == Val->TierGroup && TierData->LootPackage == Val->LootPackage)
                    {
                        TierData = Val;
                        bFound = true;
                        break;
                    }

                if (!bFound)
                    TempArr.Add(Val);
            }
        }
    };

    auto AddToPackages = [&](const UDataTable* Table, std::unordered_map<int32, FFortLootPackageData*>& TempArr)
    {
        if (!Table)
            return;

        Table->AddToRoot();
        if (VersionInfo.FortniteVersion >= 20)
        {
            if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                for (auto& ParentTable : CompositeTable->ParentTables)
                    if (ParentTable)
                        for (auto& [Key, Val] : *(TMap<int32, FFortLootPackageData*>*)(__int64(ParentTable) + 0x30))
                            TempArr[Key] = Val;

            for (auto& [Key, Val] : *(TMap<int32, FFortLootPackageData*>*)(__int64(Table) + 0x30))
                TempArr[Key] = Val;
        }
        else
        {
            if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                for (auto& ParentTable : CompositeTable->ParentTables)
                    if (ParentTable)
                        for (auto& [Key, Val] : (TMap<FName, FFortLootPackageData*>)ParentTable->RowMap)
                            TempArr[Key.ComparisonIndex] = Val;

            for (auto& [Key, Val] : (TMap<FName, FFortLootPackageData*>)Table->RowMap)
            {
                TempArr[Key.ComparisonIndex] = Val;
            }
        }
    };

    auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

    if (!Playlist)
        Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

    TArray<FFortLootTierData*> LootTierDataTempArr;
    auto LootTierData = Playlist ? Playlist->LootTierData.Get() : nullptr;
    if (!LootTierData)
        LootTierData = FindObject<UDataTable>(GameMode->HasWarmupRequiredPlayerCount() ? L"/Game/Items/Datatables/AthenaLootTierData_Client.AthenaLootTierData_Client"
                                                                                       : L"/Game/Items/Datatables/LootTierData_Client.LootTierData_Client");
    if (LootTierData)
        AddToTierData(LootTierData, LootTierDataTempArr);

    for (auto& Val : LootTierDataTempArr)
        TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);

    std::unordered_map<int32, FFortLootPackageData*> LootPackageTempArr;
    auto LootPackages = Playlist ? Playlist->LootPackages.Get() : nullptr;
    if (!LootPackages)
        LootPackages = FindObject<UDataTable>(GameMode->HasWarmupRequiredPlayerCount() ? L"/Game/Items/Datatables/AthenaLootPackages_Client.AthenaLootPackages_Client"
                                                                                       : L"/Game/Items/Datatables/LootPackages_Client.LootPackages_Client");
    if (LootPackages)
        AddToPackages(LootPackages, LootPackageTempArr);

    for (auto& [_, Val] : LootPackageTempArr)
        LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);

    auto GameFeatureDataClass = FindClass("FortGameFeatureData");
    if (GameFeatureDataClass)
        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            auto Object = TUObjectArray::GetObjectByIndex(i);

            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (Object->IsA(GameFeatureDataClass))
            {
                static auto DefaultLootTableDataOffset = Object->GetOffset("DefaultLootTableData");
                static auto PlaylistOverrideLootTableDataOffset = Object->GetOffset("PlaylistOverrideLootTableData");

                auto& LootTableData = GetFromOffset<FFortGameFeatureLootTableData>(Object, DefaultLootTableDataOffset);
                auto& LootTableDataUE53 = GetFromOffset<FFortGameFeatureLootTableData_UE53>(Object, DefaultLootTableDataOffset);
                auto& PlaylistOverrideLootTableData = GetFromOffset<TMap<FGameplayTag, FFortGameFeatureLootTableData>>(Object, PlaylistOverrideLootTableDataOffset);
                auto& PlaylistOverrideLootTableDataLWC = GetFromOffset<TMap<int32, FFortGameFeatureLootTableData>>(Object, PlaylistOverrideLootTableDataOffset);
                auto& PlaylistOverrideLootTableDataUE53 = GetFromOffset<TMap<int32, FFortGameFeatureLootTableData_UE53>>(Object, PlaylistOverrideLootTableDataOffset);
                auto LTDFeatureData = VersionInfo.EngineVersion >= 5.3 ? LootTableDataUE53.LootTierData.Get() : LootTableData.LootTierData.Get();
                auto LootPackageData = VersionInfo.EngineVersion >= 5.3 ? LootTableDataUE53.LootPackageData.Get() : LootTableData.LootPackageData.Get();

                if (LTDFeatureData)
                {
                    TArray<FFortLootTierData*> LTDTempData;

                    AddToTierData(LTDFeatureData, LTDTempData);

                    if (Playlist)
                    {
                        if (VersionInfo.EngineVersion >= 5.3)
                        {
                        }
                        else if (VersionInfo.FortniteVersion < 20.00)
                        {
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableData)
                                    if (Tag.TagName == Override.First.TagName)
                                        AddToTierData(Override.Second.LootTierData.Get(), LTDTempData);
                        }
                        else
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableDataLWC)
                                    if (Tag.TagName.ComparisonIndex == Override.First)
                                        AddToTierData(Override.Second.LootTierData.Get(), LTDTempData);
                    }

                    for (auto& Val : LTDTempData)
                        TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);
                }

                if (LootPackageData)
                {
                    std::unordered_map<int32, FFortLootPackageData*> LPTempData;

                    AddToPackages(LootPackageData, LPTempData);

                    if (Playlist)
                    {
                        if (VersionInfo.EngineVersion >= 5.3)
                        {
                        }
                        else if (VersionInfo.FortniteVersion < 20.00)
                        {
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableData)
                                    if (Tag.TagName == Override.First.TagName)
                                        AddToPackages(Override.Second.LootPackageData.Get(), LPTempData);
                        }
                        else
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableDataLWC)
                                    if (Tag.TagName.ComparisonIndex == Override.First)
                                        AddToPackages(Override.Second.LootPackageData.Get(), LPTempData);
                    }

                    for (auto& [_, Val] : LPTempData)
                        LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);
                }
            }
        }

    if (_this->HasOnPlaylistLootTablesAppliedDelegate())
    {
        *(bool*)(__int64(&_this->OnPlaylistLootTablesAppliedDelegate) + 0x10) = true;
        _this->OnPlaylistLootTablesAppliedDelegate.Process();
    }

    UFortLootPackage::SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_Warmup.Tiered_Athena_FloorLoot_Warmup_C"));
    UFortLootPackage::SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_01.Tiered_Athena_FloorLoot_01_C"));

    TArray<ABGAConsumableSpawner*> ConsumableSpawners{};
    Utils::GetAll<ABGAConsumableSpawner>(ConsumableSpawners);

    for (auto& Spawner : ConsumableSpawners)
        UFortLootPackage::SpawnConsumableActor(Spawner);

    ConsumableSpawners.Free();

    if (AFortAthenaLivingWorldStaticPointProvider::StaticClass())
    {
        TArray<AFortAthenaLivingWorldStaticPointProvider*> Spawners;
        Utils::GetAll<AFortAthenaLivingWorldStaticPointProvider>(Spawners);
        UEAllocatedMap<FName, const UClass*> VehicleSpawnerMap = {
            { FName(L"Athena.Vehicle.SpawnLocation.Motorcycle.Dirtbike"),       FindObject<UClass>(L"/Dirtbike/Vehicle/Motorcycle_DirtBike_Vehicle.Motorcycle_DirtBike_Vehicle_C")                   },
            { FName(L"Athena.Vehicle.SpawnLocation.Motorcycle.Sportbike"),      FindObject<UClass>(L"/Sportbike/Vehicle/Motorcycle_Sport_Vehicle.Motorcycle_Sport_Vehicle_C")                        },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Taxi"),       FindObject<UClass>(L"/Valet/TaxiCab/Valet_TaxiCab_Vehicle.Valet_TaxiCab_Vehicle_C")                                  },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Modded"),     FindObject<UClass>(L"/ModdedBasicCar/Vehicle/Valet_BasicCar_Vehicle_SuperSedan.Valet_BasicCar_Vehicle_SuperSedan_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicTruck.Upgraded"), FindObject<UClass>(L"/Valet/BasicTruck/Valet_BasicTruck_Vehicle_Upgrade.Valet_BasicTruck_Vehicle_Upgrade_C")         },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BigRig.Upgraded"),     FindObject<UClass>(L"/Valet/BigRig/Valet_BigRig_Vehicle_Upgrade.Valet_BigRig_Vehicle_Upgrade_C")                     },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.SportsCar.Upgraded"),  FindObject<UClass>(L"/Valet/SportsCar/Valet_SportsCar_Vehicle_Upgrade.Valet_SportsCar_Vehicle_Upgrade_C")            },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Upgraded"),   FindObject<UClass>(L"/Valet/BasicCar/Valet_BasicCar_Vehicle_Upgrade.Valet_BasicCar_Vehicle_Upgrade_C")               }
        };

        for (auto& Spawner : Spawners)
        {
            const UClass* VehicleClass = nullptr;
            for (int i = 0; i < Spawner->FiltersTags.GameplayTags.Num(); i++)
            {
                auto& Tag = Spawner->FiltersTags.GameplayTags.Get(i, FGameplayTag::Size());

                if (VehicleSpawnerMap.contains(Tag.TagName))
                {
                    VehicleClass = VehicleSpawnerMap[Tag.TagName];
                    break;
                }
            }

            if (VehicleClass)
            {
                auto Vehicle = UWorld::SpawnActor<AFortAthenaVehicle>(VehicleClass, Spawner->K2_GetActorLocation(), Spawner->K2_GetActorRotation());

                if (auto Car = Vehicle->Cast<AFortDagwoodVehicle>())
                    Car->SetFuel(100.f);
            }
            else
            {
                for (auto& Tag : Spawner->FiltersTags.GameplayTags)
                    printf("Fix: Tag: %s\n", Tag.TagName.ToString().c_str());
            }
        }
        Spawners.Free();
    }

    if (VersionInfo.EngineVersion >= 4.23 && std::floor(VersionInfo.FortniteVersion) != 20 && std::floor(VersionInfo.FortniteVersion) != 21 &&
        std::floor(VersionInfo.FortniteVersion) != 22)
    {
        TArray<AFortAthenaVehicleSpawner*> Spawners{};
        Utils::GetAll<AFortAthenaVehicleSpawner>(Spawners);

        for (auto& Spawner : Spawners)
        {
            auto VehicleClass = Spawner->GetVehicleClass();

            if (Spawner->HasCachedFortVehicleItemDef() && (!Spawner->HasbForceSpawnAlways() || !Spawner->bForceSpawnAlways))
            {
                auto VehicleDef = Spawner->CachedFortVehicleItemDef;
                if (!VehicleDef)
                    continue;

                double Min = std::clamp(VehicleDef->VehicleMinSpawnPercent.Evaluate() * 0.01f, 0.0f, 1.0f);
                double Max = std::clamp(VehicleDef->VehicleMaxSpawnPercent.Evaluate() * 0.01f, 0.0f, 1.0f);

                auto SpawnPercent = Min + (Max - Min) * (rand() / (float)RAND_MAX);
                auto bShouldSpawn = (rand() / (float)RAND_MAX) <= SpawnPercent;

                if (!bShouldSpawn)
                    continue;
            }

            auto Vehicle = UWorld::SpawnActor<AFortAthenaVehicle>(Spawner->GetVehicleClass(), Spawner->K2_GetActorLocation(), Spawner->K2_GetActorRotation());

            if (auto Car = Vehicle->Cast<AFortDagwoodVehicle>())
                Car->SetFuel(100.f);
        }

        Spawners.Free();
    }

    if (VersionInfo.FortniteVersion > 3.4)
    {
        TArray<ABuildingItemCollectorActor*> Collectors{};
        Utils::GetAll<ABuildingItemCollectorActor>(Collectors);
        for (auto& CollectorActor : Collectors)
        {
            if (Sum > Weight)
            {
            PickNum:
                auto RandomNum = (float)rand() / (RAND_MAX / TotalWeight);

                int Rarity = 0;
                bool found = false;

                for (auto& Element : WeightMap)
                {
                    float Weight = Element.second;

                    if (Weight == 0)
                        continue;

                    if (RandomNum <= Weight)
                    {
                        Rarity = Element.first;

                        found = true;
                        break;
                    }

                    RandomNum -= Weight;
                }

                if (!found)
                    goto PickNum;

                if (Rarity == 0)
                {
                    CollectorActor->K2_DestroyActor();
                    continue;
                }

                int AttemptsToGetItem = 0;
                for (int i = 0; i < CollectorActor->ItemCollections.Num(); i++)
                {
                    if (AttemptsToGetItem > 10)
                    {
                        AttemptsToGetItem = 0;
                        goto PickNum;
                    }

                    auto& Collection = CollectorActor->ItemCollections.Get(i, FCollectorUnitInfo::Size());

                    if (Collection.bUseDefinedOutputItem)
                        continue;

                    TArray<FFortItemEntry*> LootDrops{};

                    UFortLootPackage::ChooseLootForContainer(LootDrops, CollectorActor->DefaultItemLootTierGroupName, Rarity);

                    if (Collection.OutputItemEntry.Num() > 0)
                    {
                        Collection.OutputItemEntry.ResetNum();
                        Collection.OutputItem = nullptr;
                    }

                    for (auto& LootDrop : LootDrops)
                    {
                        if (!Collection.OutputItem && AFortInventory::IsPrimaryQuickbar(LootDrop->ItemDefinition))
                            Collection.OutputItem = LootDrop->ItemDefinition;

                        Collection.OutputItemEntry.Add(*LootDrop, FFortItemEntry::Size());
                        free(LootDrop);
                    }

                    if (!Collection.OutputItem)
                    {
                        i--;
                        AttemptsToGetItem++;

                        continue;
                    }

                    AttemptsToGetItem = 0;
                }

                CollectorActor->StartingGoalLevel = Rarity;
            }
            else
                CollectorActor->K2_DestroyActor();
        }
        Collectors.Free();

        Hooking::ExecHook((UFunction*)FindObject<UFunction>(L"/Game/Athena/Items/Gameplay/VendingMachine/B_Athena_VendingMachine.B_Athena_VendingMachine_C:VendWobble__FinishedFunc"), VendWobble__FinishedFunc,
                          VendWobble__FinishedFuncOG);
    }
}

void PlayerCanRestart(UObject* Context, FFrame& Stack, bool* Ret)
{
    AFortPlayerControllerAthena* Controller;

    Stack.StepCompiledIn(&Controller);
    Stack.IncrementCode();

    *Ret = true;
}

void AFortGameMode::Hook()
{
    Hooking::ExecHook(GetDefaultObj()->GetFunction("ReadyToStartMatch"), ReadyToStartMatch_, ReadyToStartMatch_OG);
    Hooking::Hook(FindFinishWorldInitialization(), FinishWorldInitialization, FinishWorldInitializationOG);
}

void AFortGameMode::PostLoadHook()
{
    ApplyCharacterCustomization = FindApplyCharacterCustomization();
    NotifyGameMemberAdded_ = FindNotifyGameMemberAdded();
    StartStreamingAdditionalPlaylistLevel_ = FindStartStreamingAdditionalPlaylistLevel();

    auto spdf = GetDefaultObj()->GetFunction("SpawnDefaultPawnFor");
    SpawnDefaultPawnForIdx = spdf->GetVTableIndex();

    Hooking::ExecHook(spdf, SpawnDefaultPawnFor);
    Hooking::ExecHook(GetDefaultObj()->GetFunction("HandleStartingNewPlayer"), HandleStartingNewPlayer_, HandleStartingNewPlayer_OG);
    Hooking::Hook(FindPickTeam(), PickTeam, PickTeamOG);
    if (VersionInfo.FortniteVersion < 25.20)
    {
        Hooking::Hook(FindStartAircraftPhase(), StartAircraftPhase, StartAircraftPhaseOG);

        const uint64 StormHandlerAddress = FindHandlePostSafeZonePhaseChanged();
        const uint64 ModuleBase = (uint64)GetModuleHandleW(nullptr);

        if (StormHandlerAddress)
            printf("[Storm] The game's own phase handler is at +0x%llX (absolute 0x%llX). Hooking it now.\n",
                   StormHandlerAddress > ModuleBase ? StormHandlerAddress - ModuleBase : StormHandlerAddress, StormHandlerAddress);
        else
            printf("[Storm] The game's own phase handler could NOT be found -- the storm will run on this server's own timing only.\n");
        fflush(stdout);

        Hooking::Hook(StormHandlerAddress, HandlePostSafeZonePhaseChanged, HandlePostSafeZonePhaseChangedOG);

        printf("[Storm] Hook installed. Calls back into the game will go through +0x%llX.\n",
               (uint64)HandlePostSafeZonePhaseChangedOG > ModuleBase ? (uint64)HandlePostSafeZonePhaseChangedOG - ModuleBase : (uint64)HandlePostSafeZonePhaseChangedOG);
        fflush(stdout);
    }
    Hooking::ExecHook(AFortGameModeAthena::GetDefaultObj()->GetFunction("OnAircraftExitedDropZone"), OnAircraftExitedDropZone_, OnAircraftExitedDropZone_OG);

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        if (VersionInfo.FortniteVersion < 25.20)
        {
            Hooking::Hook(FindSpawnInitialSafeZone(), SpawnInitialSafeZone, SpawnInitialSafeZoneOG);
            Hooking::Hook(FindUpdateSafeZonesPhase(), UpdateSafeZonesPhase, UpdateSafeZonesPhaseOG);
        }
        Hooking::ExecHook((UFunction*)FindObject<UFunction>(L"/Script/FortniteGame.FortSafeZoneIndicator.GetPhaseInfo"), GetPhaseInfo);
    }
}
