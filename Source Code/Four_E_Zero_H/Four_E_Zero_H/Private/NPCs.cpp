#include "pch.h"
#include "../Public/NPCs.h"
#include "../Public/BotAI.h"
#include "../Public/Configuration.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include <set>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <cstdlib>

namespace NPCs
{
    static std::set<const void*> NPCObjects;
    static std::set<const void*> NPCControllers;
    static std::set<const void*> PlayerBotControllerClasses;
    static std::set<const void*> ClassifiedStates;
    static std::set<const void*> PatrolChecked;

    static bool bDisabled = false;
    static int  SehFaults = 0;
    static int  TicksWithHuman = 0;

    static int Stage = 0;
    static int StageCountdown = 0;
    static int PoiSpawnersDriven = 0;

    static void NpcLog(const char* Format, ...)
    {
        if (!FConfiguration::bNPCLog)
            return;

        char Buffer[1024];
        va_list Args;
        va_start(Args, Format);
        vsnprintf(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);

        printf("[NPC] %s\n", Buffer);
        fflush(stdout);
    }

    bool IsNPC(const void* Object)
    {
        if (!Object || NPCObjects.empty())
            return false;

        return NPCObjects.find(Object) != NPCObjects.end();
    }

    template <typename T>
    static T* Field(const UObject* Obj, const char* Name)
    {
        if (!Obj)
            return nullptr;

        const uint32 Offset = Obj->GetOffset(Name);

        if (Offset == (uint32)-1)
            return nullptr;

        return (T*)((uint8_t*)Obj + Offset);
    }

    static bool CallVoid(const UObject* Obj, const char* Name)
    {
        if (!Obj)
            return false;

        auto Function = Obj->GetFunction(Name);

        if (!Function)
            return false;

        Obj->Call<void>(Function);
        return true;
    }

    static bool ClassDeclares(const UStruct* Cls, const FName& FunctionName)
    {
        if (!Cls)
            return false;

        for (const UField* Child = Cls->GetChildren(); Child; Child = Child->GetNext())
        {
            if (Child->Class && (Child->Class->GetCastFlags() & 0x80000) && Child->GetName() == FunctionName)
                return true;
        }

        return false;
    }

    static std::string NameOf(const UObject* Obj)
    {
        if (!Obj)
            return std::string();

        auto Text = Obj->Name.ToString();
        return std::string(Text.c_str());
    }

    static std::string Lower(const char* In)
    {
        std::string Out;
        if (!In)
            return Out;

        for (const char* p = In; *p; p++)
            Out += (char)tolower((unsigned char)*p);

        return Out;
    }

    static void FindPoiSpawners(std::vector<AActor*>& Out)
    {
        auto World = UWorld::GetWorld();
        if (!World)
            return;

        static std::vector<const UClass*> Classes;
        static bool bScanned = false;

        if (!bScanned)
        {
            bScanned = true;

            static FName FnTrySpawn = FName(L"TrySpawnAI");
            static FName FnSpawnAndConfigure = FName(L"SpawnAndConfigureAI");

            const int Total = TUObjectArray::Num();

            for (int i = 0; i < Total; i++)
            {
                auto Obj = TUObjectArray::GetObjectByIndex(i);

                if (!Obj || !Obj->Class)
                    continue;

                if (!(Obj->Class->GetCastFlags() & 0x20))
                    continue;

                auto Cls = (const UStruct*)Obj;

                bool bMatch = ClassDeclares(Cls, FnTrySpawn) || ClassDeclares(Cls, FnSpawnAndConfigure);

                if (!bMatch)
                {
                    const std::string LowerName = Lower(NameOf(Obj).c_str());

                    bMatch = LowerName.find("mang_spawner") != std::string::npos
                          || (LowerName.find("spawner") != std::string::npos
                              && (LowerName.find("henchman") != std::string::npos
                                  || LowerName.find("henchmen") != std::string::npos
                                  || LowerName.find("guard") != std::string::npos));
                }

                if (bMatch)
                    Classes.push_back((const UClass*)Obj);
            }

            NpcLog("Class scan: %d candidate POI spawner classes.", (int)Classes.size());
        }

        for (auto Cls : Classes)
        {
            TArray<AActor*> Actors;
            UGameplayStatics::GetAllActorsOfClass(World, Cls, &Actors);

            if (Actors.Num() > 0)
                NpcLog("POI spawner class %s -- %d in the level.", NameOf((const UObject*)Cls).c_str(), Actors.Num());

            for (int i = 0; i < Actors.Num(); i++)
            {
                if (Actors[i])
                    Out.push_back(Actors[i]);
            }
        }
    }

    static UObject* EnsureServerBotManager()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->AuthorityGameMode)
            return nullptr;

        auto GameMode = (AActor*)World->AuthorityGameMode;

        auto Slot = Field<UObject*>(GameMode, "ServerBotManager");
        if (!Slot)
            return nullptr;

        if (!*Slot)
        {
            auto Cls = FindClass("FortServerBotManagerAthena");
            if (!Cls)
                return nullptr;

            auto Manager = (UObject*)UGameplayStatics::SpawnObject(Cls, GameMode);
            if (!Manager)
            {
                NpcLog("Could not create a server bot manager -- POI spawners may refuse to spawn.");
                return nullptr;
            }

            *Slot = Manager;
            NpcLog("Created the server bot manager the POI spawners need.");
        }

        auto Manager = *Slot;

        if (auto GameModeSlot = Field<AActor*>(Manager, "CachedGameMode"))
            if (!*GameModeSlot)
                *GameModeSlot = GameMode;

        if (auto GameStateSlot = Field<AActor*>(Manager, "CachedGameState"))
            if (!*GameStateSlot)
                *GameStateSlot = (AActor*)World->GameState;

        if (auto MutatorSlot = Field<AActor*>(Manager, "CachedBotMutator"))
        {
            if (!*MutatorSlot)
            {
                auto MutatorClass = FindClass("FortAthenaMutator_Bots");

                if (MutatorClass)
                {
                    TArray<AActor*> Mutators;
                    UGameplayStatics::GetAllActorsOfClass(World, MutatorClass, &Mutators);

                    if (Mutators.Num() > 0 && Mutators[0])
                    {
                        *MutatorSlot = Mutators[0];
                        NpcLog("Attached the bot mutator to the server bot manager.");
                    }
                }
            }
        }

        return Manager;
    }

    static int DrivePoiSpawners(bool bForce)
    {
        std::vector<AActor*> Spawners;
        FindPoiSpawners(Spawners);

        if (Spawners.empty())
        {
            NpcLog("No POI spawners in the level%s.", bForce ? "" : " -- the POI sublevels may not have streamed in yet");
            return 0;
        }

        EnsureServerBotManager();

        auto World = UWorld::GetWorld();
        auto GameMode = World ? (AActor*)World->AuthorityGameMode : nullptr;
        auto ManagerSlot = GameMode ? Field<UObject*>(GameMode, "ServerBotManager") : nullptr;
        UObject* Manager = ManagerSlot ? *ManagerSlot : nullptr;

        int Driven = 0;

        for (auto Spawner : Spawners)
        {
            if (!Spawner)
                continue;

            if (auto ManagerSlot = Field<UObject*>(Spawner, "ServerBotManager"))
                if (!*ManagerSlot && Manager)
                    *ManagerSlot = Manager;

            if (bForce)
            {
                if (auto Allowed = Field<int32>(Spawner, "NumberOfAIWeAreAllowedToSpawn"))
                    if (*Allowed <= 0)
                        *Allowed = FConfiguration::HenchmenPerPOI;

                if (auto AllDead = Field<bool>(Spawner, "AllHenchmenDead"))
                    *AllDead = false;

                if (FConfiguration::bForceBossAtEveryPOI)
                    if (auto Boss = Field<bool>(Spawner, "SpawnBossAtThisPOI"))
                        *Boss = true;
            }

            CallVoid(Spawner, "AssignDataToVariables");
            CallVoid(Spawner, "SetSpawnGroupBucketsIntoPatrolsAndBotData");

            bool bCalled = false;

            static const char* Normal[] = { "TrySpawnAI", "SpawnAndConfigureAI", "WipeAndRespawnHenchmen", "RespawnHenchmen" };
            static const char* Forced[] = { "SpawnAndConfigureAI", "TrySpawnAI", "WipeAndRespawnHenchmen", "RespawnHenchmen" };

            const char** Order = bForce ? Forced : Normal;

            for (int Try = 0; Try < 4 && !bCalled; Try++)
                bCalled = CallVoid(Spawner, Order[Try]);

            CallVoid(Spawner, "SetupAIDeathEvents");

            if (bCalled)
            {
                Driven++;
                NpcLog("%s %s", bForce ? "Forced" : "Started", NameOf((const UObject*)Spawner).c_str());
            }
        }

        NpcLog("%s %d POI spawner%s.", bForce ? "Forced" : "Nudged", Driven, Driven == 1 ? "" : "s");
        return Driven;
    }

    static bool HasSpawnParams(const UClass* Cls)
    {
        if (!Cls)
            return false;

        auto CDO = Cls->GetDefaultObj();
        if (!CDO)
            return false;

        auto Slot = Field<UObject*>(CDO, "SpawnParamsComponent");
        return Slot && *Slot;
    }

    struct FSpawnerDataEntry
    {
        const UClass* Class = nullptr;
        std::string   Name;
        bool          bUsable = false;
    };

    static void CollectSpawnerData(std::vector<FSpawnerDataEntry>& Out)
    {
        auto BaseClass = FindClass("FortAthenaAISpawnerData");
        auto BotBaseClass = FindClass("FortAthenaAIBotSpawnerData");

        if (!BaseClass && !BotBaseClass)
            return;

        const int Total = TUObjectArray::Num();

        for (int i = 0; i < Total; i++)
        {
            auto Obj = TUObjectArray::GetObjectByIndex(i);

            if (!Obj || !Obj->Class)
                continue;

            if (!(Obj->Class->GetCastFlags() & 0x20))
                continue;

            const std::string RawName = NameOf(Obj);
            const std::string LowerName = Lower(RawName.c_str());

            if (LowerName.find("spawnerdata") == std::string::npos)
                continue;

            if (LowerName.find("phoebe") != std::string::npos)
                continue;

            if ((const void*)Obj == (const void*)BaseClass || (const void*)Obj == (const void*)BotBaseClass)
                continue;

            bool bDerives = false;
            for (auto Super = (const UStruct*)Obj; Super; Super = Super->GetSuper())
            {
                if ((const void*)Super == (const void*)BaseClass || (const void*)Super == (const void*)BotBaseClass)
                {
                    bDerives = true;
                    break;
                }
            }

            if (!bDerives)
                continue;

            FSpawnerDataEntry Entry;
            Entry.Class = (const UClass*)Obj;
            Entry.Name = RawName;
            Entry.bUsable = HasSpawnParams(Entry.Class);
            Out.push_back(Entry);
        }
    }

    static void CollectPatrolPaths(std::vector<AActor*>& Out)
    {
        auto World = UWorld::GetWorld();
        if (!World)
            return;

        auto PathClass = FindClass("FortAthenaPatrolPath");
        if (!PathClass)
            return;

        TArray<AActor*> Actors;
        UGameplayStatics::GetAllActorsOfClass(World, PathClass, &Actors);

        for (int i = 0; i < Actors.Num(); i++)
            if (Actors[i])
                Out.push_back(Actors[i]);
    }

    static bool FirstPatrolPoint(AActor* Path, FVector& Out)
    {
        auto Points = Field<TArray<AActor*>>(Path, "PatrolPoints");

        if (!Points || Points->Num() <= 0)
            return false;

        auto Point = (*Points)[0];
        if (!Point)
            return false;

        Out = Point->K2_GetActorLocation();
        return true;
    }

    static int SpawnFromSpawnerData()
    {
        auto World = UWorld::GetWorld();
        if (!World)
            return 0;

        auto AISystem = (UAthenaAISystem*)World->AISystem;
        if (!AISystem || !AISystem->AISpawner)
            return 0;

        std::vector<FSpawnerDataEntry> Entries;
        CollectSpawnerData(Entries);

        int Usable = 0;
        for (auto& Entry : Entries)
        {
            NpcLog("Spawner data %s -- %s", Entry.Name.c_str(),
                   Entry.bUsable ? "usable" : "REJECTED, it has no SpawnParams component");
            if (Entry.bUsable)
                Usable++;
        }

        if (Usable == 0)
        {
            NpcLog("No usable AI spawner data on this build -- expected, 14.60 does not ship any.");
            return 0;
        }

        std::vector<AActor*> Paths;
        CollectPatrolPaths(Paths);

        if (Paths.empty())
        {
            NpcLog("No patrol routes to place them on.");
            return 0;
        }

        const int MaxTotal = FConfiguration::MaxNPCs > 0 ? FConfiguration::MaxNPCs : 40;
        int Spawned = 0;
        size_t Next = 0;

        for (auto Path : Paths)
        {
            if (Spawned >= MaxTotal)
                break;

            FVector SpawnLoc;
            if (!FirstPatrolPoint(Path, SpawnLoc))
                continue;

            const FSpawnerDataEntry* Entry = nullptr;
            for (size_t Tried = 0; Tried < Entries.size() && !Entry; Tried++)
            {
                auto& Candidate = Entries[(Next + Tried) % Entries.size()];
                if (Candidate.bUsable)
                    Entry = &Candidate;
            }
            Next++;

            if (!Entry)
                break;

            FTransform Transform{};
            Transform.Translation = SpawnLoc;
            FVector Scale = FVector(1, 1, 1);
            Transform.Scale3D = Scale;

            auto ComponentList = UFortAthenaAISpawnerData::CreateComponentListFromClass(Entry->Class, World);

            if (!ComponentList)
                continue;

            if (AISystem->AISpawner->RequestSpawn(ComponentList, Transform) >= 0)
                Spawned++;
        }

        NpcLog("Spawner-data fallback requested %d.", Spawned);
        return Spawned;
    }

    static void EnsurePatrolling(AActor* Controller)
    {
        if (!Controller || PatrolChecked.find((const void*)Controller) != PatrolChecked.end())
            return;

        auto PawnSlot = Field<AActor*>(Controller, "Pawn");
        if (!PawnSlot || !*PawnSlot)
            return;

        PatrolChecked.insert((const void*)Controller);

        auto CompSlot = Field<UObject*>(Controller, "CachedPatrollingComponent");
        if (!CompSlot || !*CompSlot)
            return;

        auto Component = *CompSlot;

        auto PathSlot = Field<AActor*>(Component, "PatrolPath");
        if (PathSlot && *PathSlot)
            return;

        std::vector<AActor*> Paths;
        CollectPatrolPaths(Paths);

        if (Paths.empty())
            return;

        const FVector Here = (*PawnSlot)->K2_GetActorLocation();

        AActor* Best = nullptr;
        double BestDist = 0.0;

        for (auto Path : Paths)
        {
            FVector Point;
            if (!FirstPatrolPoint(Path, Point))
                continue;

            const double dx = Point.X - Here.X;
            const double dy = Point.Y - Here.Y;
            const double dz = Point.Z - Here.Z;
            const double Dist = dx * dx + dy * dy + dz * dz;

            if (!Best || Dist < BestDist)
            {
                Best = Path;
                BestDist = Dist;
            }
        }

        if (!Best)
            return;

        if (auto Function = Component->GetFunction("SetPatrolPath"))
            Component->Call<void>(Function, Best);
        else if (PathSlot)
            *PathSlot = Best;

        NpcLog("Put %s on a patrol route.", NameOf((const UObject*)Controller).c_str());
    }

    static void LearnPlayerBotClasses(AFortGameStateAthena* GameState)
    {
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot)
                continue;

            ClassifiedStates.insert((const void*)PS);

            auto Ctrl = (AActor*)PS->Owner;
            if (Ctrl && Ctrl->Class)
                PlayerBotControllerClasses.insert((const void*)Ctrl->Class);
        }
    }

    static void ClassifyNewBots(AFortGameStateAthena* GameState)
    {
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot)
                continue;

            auto Ctrl = (AActor*)PS->Owner;

            if (ClassifiedStates.find((const void*)PS) != ClassifiedStates.end())
            {
                if (Ctrl && NPCObjects.find((const void*)Ctrl) != NPCObjects.end())
                {
                    if (auto PawnSlot = Field<AActor*>(Ctrl, "Pawn"))
                        if (*PawnSlot)
                            NPCObjects.insert((const void*)*PawnSlot);

                    EnsurePatrolling(Ctrl);
                }
                continue;
            }

            if (!Ctrl || !Ctrl->Class)
                continue;

            ClassifiedStates.insert((const void*)PS);

            if (PlayerBotControllerClasses.find((const void*)Ctrl->Class) != PlayerBotControllerClasses.end())
                continue;

            const std::string ClassName = Lower(NameOf((const UObject*)Ctrl->Class).c_str());

            if (ClassName.find("phoebe") != std::string::npos)
            {
                PlayerBotControllerClasses.insert((const void*)Ctrl->Class);
                continue;
            }

            NPCControllers.insert((const void*)Ctrl);
            NPCObjects.insert((const void*)PS);
            NPCObjects.insert((const void*)Ctrl);

            if (auto PawnSlot = Field<AActor*>(Ctrl, "Pawn"))
                if (*PawnSlot)
                    NPCObjects.insert((const void*)*PawnSlot);

            NpcLog("Boss/henchman is in: %s (%d live).", ClassName.c_str(), (int)NPCControllers.size());
        }
    }

    static bool MatchIsLive(AFortGameStateAthena* GameState)
    {
        if (GameState->HasGamePhase())
            return GameState->GamePhase >= 3;

        int Humans = 0;
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (PS && (!PS->HasbIsABot() || !PS->bIsABot))
                Humans++;
        }

        if (Humans > 0)
            TicksWithHuman++;

        return TicksWithHuman > 600;
    }

    static void Tick_Inner()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;

        if (Stage == 0)
        {
            LearnPlayerBotClasses(GameState);

            if (!MatchIsLive(GameState))
                return;

            NpcLog("Match is live -- starting the POI spawners.");
            PoiSpawnersDriven = DrivePoiSpawners(false);

            Stage = 1;
            StageCountdown = 300;
            return;
        }

        ClassifyNewBots(GameState);

        if (Stage >= 4)
            return;

        if (StageCountdown-- > 0)
            return;

        if (!NPCControllers.empty())
        {
            NpcLog("%d bosses/henchmen are up and on patrol.", (int)NPCControllers.size());
            Stage = 4;
            return;
        }

        if (Stage == 1)
        {
            NpcLog("Nothing spawned yet -- overriding the spawners' own limits and forcing them.");
            DrivePoiSpawners(true);
            Stage = 2;
            StageCountdown = 300;
            return;
        }

        if (Stage == 2)
        {
            NpcLog("Still nothing -- trying the spawner-data route as a last resort.");
            SpawnFromSpawnerData();
            Stage = 3;
            StageCountdown = 300;
            return;
        }

        NpcLog("No bosses or henchmen could be spawned. Type \"npcinfo\" for the full picture.");
        Stage = 4;
    }

    void GameThreadTick()
    {
        if (!FConfiguration::bSpawnBossesAndHenchmen || bDisabled)
            return;

        __try
        {
            Tick_Inner();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SehFaults++;

            if (SehFaults >= 3)
            {
                bDisabled = true;
                printf("[NPC] Faulted %d times -- boss/henchman spawning is off for this session.\n", SehFaults);
                fflush(stdout);
            }
        }
    }

    int SpawnNow()
    {
        auto World = UWorld::GetWorld();
        if (World && World->GameState)
            LearnPlayerBotClasses((AFortGameStateAthena*)World->GameState);

        const int Before = (int)NPCControllers.size();

        DrivePoiSpawners(true);

        Stage = 1;
        StageCountdown = 300;

        return (int)NPCControllers.size() - Before;
    }

    void ReportDiscovery()
    {
        std::vector<AActor*> Spawners;
        FindPoiSpawners(Spawners);

        printf("[NPC] ---- POI spawners in the level (%d) ----\n", (int)Spawners.size());
        for (auto Spawner : Spawners)
        {
            const int32* Allowed = Field<int32>(Spawner, "NumberOfAIWeAreAllowedToSpawn");
            const bool* Boss = Field<bool>(Spawner, "SpawnBossAtThisPOI");
            printf("[NPC]   %s   allowed:%s  boss:%s\n",
                   NameOf((const UObject*)Spawner).c_str(),
                   Allowed ? std::to_string(*Allowed).c_str() : "n/a",
                   Boss ? (*Boss ? "yes" : "no") : "n/a");
        }

        std::vector<FSpawnerDataEntry> Entries;
        CollectSpawnerData(Entries);

        printf("[NPC] ---- AI spawner data classes (%d) ----\n", (int)Entries.size());
        for (auto& Entry : Entries)
            printf("[NPC]   %s   %s\n", Entry.Name.c_str(), Entry.bUsable ? "usable" : "no SpawnParams -- unusable");

        std::vector<AActor*> Paths;
        CollectPatrolPaths(Paths);

        printf("[NPC] ---- patrol routes (%d) ----\n", (int)Paths.size());
        for (size_t i = 0; i < Paths.size() && i < 40; i++)
        {
            auto Points = Field<TArray<AActor*>>(Paths[i], "PatrolPoints");
            printf("[NPC]   %s   points:%d\n", NameOf((const UObject*)Paths[i]).c_str(), Points ? Points->Num() : -1);
        }

        auto World = UWorld::GetWorld();
        auto GameMode = World ? (AActor*)World->AuthorityGameMode : nullptr;
        auto ManagerSlot = GameMode ? Field<UObject*>(GameMode, "ServerBotManager") : nullptr;

        printf("[NPC] ---- server bot manager: %s ----\n",
               !ManagerSlot ? "no such property on this build" : (*ManagerSlot ? "present" : "MISSING"));
        printf("[NPC] ---- live NPCs: %d ----\n", (int)NPCControllers.size());
        fflush(stdout);
    }
}
