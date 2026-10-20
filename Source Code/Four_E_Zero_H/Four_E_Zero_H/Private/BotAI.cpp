#include "pch.h"
#include "../Public/BotAI.h"
#include "../Public/Configuration.h"

#include "../Public/NPCs.h"

#include "../Public/PlaceDump.h"
#include "../Public/ServerConsole.h"
#include "../Public/NoKillerMessages.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"

#include "../../FortniteGame/Public/FortWeapon.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"

#include "../../FortniteGame/Public/FortLootPackage.h"

#include "../Plugins/CrashReporter/Public/CrashReporter.h"
#include <chrono>
#include <thread>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>


#define printf(...) ((void)0)
extern bool GStormMapDataWasRepaired;

extern uint64_t ApplyCharacterCustomization;

uint8 ToDeathCause(AFortPlayerPawnAthena* Pawn, FGameplayTagContainer& DeathTags, bool bDBNO);

uint64 FindEnterAircraft();

// Only log lines that start with one of these tags are printed / written to BotAI_Log.txt.
// Everything else stays silent. Add or remove tags to change what you see.
static const char* kAllowedLogTags[] = {
    "[SWING-FX]",
    "[BUILDFIGHT]",
    // "[HITSOUND]",
    // "[BREAK]",
    // "[CRACKS]",
};

static bool LogTagAllowed(const char* Fmt)
{
    if (!Fmt)
        return false;

    for (auto Tag : kAllowedLogTags)
        if (strncmp(Fmt, Tag, strlen(Tag)) == 0)
            return true;

    return false;
}

// Writes a finished line to the console AND BotAI_Log.txt.
static void WriteLogText(const char* Text)
{
    fprintf(stdout, "%s\n", Text);
    fflush(stdout);

    // "w" wipes the old file once, the first time a line is written this run
    static FILE* File = nullptr;
    static bool bTried = false;

    if (!bTried)
    {
        bTried = true;
        fopen_s(&File, "BotAI_Log.txt", "w");
    }

    if (File)
    {
        fprintf(File, "%s\n", Text);
        fflush(File);
    }
}

// Every log line goes through this. Only whitelisted tags print.
static void LogLine(const char* Fmt, ...)
{
    if (!LogTagAllowed(Fmt))
        return;

    static char Buffer[16384];

    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);

    WriteLogText(Buffer);
}

// Hit sound logger. Always prints, regardless of the whitelist.
static void HitLog(const char* Fmt, ...)
{
    if (!Fmt)
        return;

    static char Buffer[4096];

    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);

    std::string Line = std::string("[HITSOUND] ") + Buffer;
    WriteLogText(Line.c_str());
}

namespace BotAI
{
    static std::set<void*> ThankedBots;

    static std::set<void*> MovedToIsland;

    static std::set<void*> BTRetried;

    static int MoveResult_Failed = 0;
    static int MoveResult_AlreadyAtGoal = 0;
    static int MoveResult_Success = 0;
    static int MoveResult_Other = 0;

    static float BusCountdownStart = -1.f;
    static int LastBusSecondAnnounced = -1;

    static std::map<void*, int> BotCombatTick;

    static AFortInventory* EnsureBotInventory(AActor* BotControllerActor);
    static bool BotIsHealing(AFortPlayerPawnAthena* Pawn);

    static const UFortItemDefinition* FindItemDefByName(const wchar_t* name);
    static int BotBestGunScore(AFortPlayerControllerAthena* Controller);

    static bool BotGroundBelow(AFortPlayerPawnAthena* Pawn, double X, double Y, double FromZ, double ToZ, double& OutZ);

    static int GiveBotLootpoolKit(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState);
    static void NoteFixedBotKit(AFortPlayerStateAthena* PlayerState, const char* Why);
    static std::string GiveBotSpareRounds(AFortInventory* Inventory, const UFortItemDefinition* Gun);

    static std::set<void*> LoadoutGiven;
    static constexpr bool kBotsSpawnWithGuns = false;

    static std::map<void*, const UFortItemDefinition*> BotWeapons;

    static std::map<void*, AFortInventory*> BotInventories;

    static std::map<void*, int> BotWeaponClipSize;

    static std::set<void*> BotTeamsAssigned;

    static std::set<void*> PickaxeGiven;

    static std::map<void*, int> BotZoneMoveTick;

    static std::map<void*, float> BotLastZoneDistance;

    static std::map<void*, float> BotFacingYaw;

    struct FBotFireState
    {
        int ReactionTicksLeft = -1;
        int BurstTicksLeft = 0;
        int CooldownTicksLeft = 0;
        bool bFiring = false;

        int RoundsSinceReload = 0;
    };

    static std::map<void*, FBotFireState> BotFire;

    struct FBotReload
    {
        bool bActive = false;
        bool bGameReload = false;
        void* Weapon = nullptr;
        int RoundsBefore = -1;
        int Magazine = 0;
        float StartedAt = 0.f;
        float Duration = 0.f;
        const char* Why = "";
        float NoFightSince = -1.f;
        float ReloadFlagSince = -1.f;
        float EquipFlagSince = -1.f;
        void* MagazineOf = nullptr;
        int MagazineSize = 0;
    };

    static std::map<void*, FBotReload> BotReloads;

    struct FBotBody
    {
        float Clock = 0.f;
        bool bHasGoal = false;
        FVector Goal{};
        float AcceptRadius = 100.f;
        bool bScripted = false;
        bool bChecked = false;
        float WatchSince = -1.f;
        float WatchZ = 0.f;
        void* Owner = nullptr;
        bool bGroundKnown = false;
        double GroundZ = 0.0;
        FVector GroundFor{};
        bool bOnGround = false;

        // pathing
        int SteerCheckTick = -1000;
        bool bSteerValid = false;
        double SteerX = 0.0, SteerY = 0.0;
        double SteerGoalX = 0.0, SteerGoalY = 0.0;
        int SteerSide = 1;
        int StuckTick = -1;
        FVector StuckPos{};
        int StuckLevel = 0;
        int OverrideUntil = -1;
        double OverrideX = 0.0, OverrideY = 0.0;
        int BoxedUntil = -1;
        FVector LastGoodPos{};
        bool bHaveGoodPos = false;

        // progress watchdog
        int ProgressTick = -1;
        double BestGoalDist = 0.0;
        double ProgressGoalX = 0.0, ProgressGoalY = 0.0;
        int DetourCount = 0;
        int DetourSide = 1;
        bool bDirectMove = false;
        // ledge guard
        int EdgeCheckTick = -1000;
        bool bEdgeAhead = false;
    };

    static std::map<void*, FBotBody> BotBodies;
    struct FBotAimState
    {
        bool bInit = false;
        void* Target = nullptr;
        double Yaw = 0.0;
        double Pitch = 0.0;
        double OffYaw = 0.0;         // the offset the aim currently has (eases towards the wanted one)
        double OffPitch = 0.0;
        double WantOffYaw = 0.0;     // the offset the bot is drifting towards
        double WantOffPitch = 0.0;
        int NextOffsetTick = 0;
        int ReactUntilTick = 0;      // the bot has not "noticed" the target before this tick
        double SeenX = 0.0, SeenY = 0.0, SeenZ = 0.0;   // where the bot THINKS the target is (lags behind)
        uint32 Seed = 0;
    };

    static std::map<void*, FBotAimState> BotAim;

    static bool IsPlayerControllerBot(AActor* Controller)
    {
        static auto PlayerControllerClass = AFortPlayerControllerAthena::StaticClass();
        return Controller && PlayerControllerClass && Controller->IsA(PlayerControllerClass);
    }

    static bool SetBoolByName(UObject* Object, const char* Name, bool bValue)
    {
        auto Prop = Object ? Object->GetProperty(Name, 0x20000) : nullptr;
        if (!Prop)
            return false;

        const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
        const uint8 Mask = Prop->GetFieldMask();

        if (!Mask || Offset >= 0x10000)
            return false;

        uint8& Byte = GetFromOffset<uint8>(Object, Offset);

        if (Mask == 0xFF)
            Byte = bValue ? 1 : 0;
        else if (bValue)
            Byte |= Mask;
        else
            Byte &= (uint8)~Mask;

        return true;
    }

    static int ReadBoolByName(const UObject* Object, const char* Name)
    {
        auto Prop = Object ? Object->GetProperty(Name, 0x20000) : nullptr;
        if (!Prop)
            return -1;

        const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
        const uint8 Mask = Prop->GetFieldMask();

        if (!Mask || Offset >= 0x10000)
            return -1;

        return (GetFromOffset<uint8>(Object, Offset) & Mask) ? 1 : 0;
    }

    static void MarkBotReadyLikeAPlayer(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState, bool bHasPawn)
    {
        if (!Controller)
            return;

        auto PC = (UObject*)Controller;

        SetBoolByName(PC, "bHasClientFinishedLoading", true);
        SetBoolByName(PC, "bHasServerFinishedLoading", true);
        SetBoolByName(PC, "bHasFinishedLoading", true);
        SetBoolByName(PC, "bReadyToStartMatch", true);
        SetBoolByName(PC, "bAssignedStartSpawn", true);

        if (bHasPawn)
        {
            SetBoolByName(PC, "bHasInitiallySpawned", true);
            SetBoolByName(PC, "bClientPawnIsLoaded", true);
        }

        SetBoolByName(PC, "bMarkedAlive", true);

        if (PlayerState)
        {
            auto PS = (UObject*)PlayerState;

            SetBoolByName(PS, "bIsSpectator", false);
            SetBoolByName(PS, "bHasFinishedLoading", true);
            SetBoolByName(PS, "bHasStartedPlaying", true);
        }
    }

    struct FBusExitLatch
    {
        bool bResolved = false;
        bool bSupported = false;
        uint32 ExitedOffset = 0;
        uint32 ReadyOffset = 0;
        uint8 ReadyMask = 0;
        int Written = 0;
        int Applied = 0;
        int Refused = 0;
        int StillAboard = 0;
        int Logged = 0;
    };

    static FBusExitLatch GBusExitLatch;

    struct FCmpByteZero
    {
        uint8 BaseRegister = 0;
        uint32 Displacement = 0;
        size_t Size = 0;
    };

    static bool DecodeCmpByteZero(const uint8* Code, size_t Remaining, FCmpByteZero& Out)
    {
        if (!Code || Remaining < 7)
            return false;

        size_t Cursor = 0;
        uint8 Rex = 0;

        if ((Code[Cursor] & 0xF0) == 0x40)
        {
            Rex = Code[Cursor++];

            if (Remaining - Cursor < 7)
                return false;
        }

        if (Code[Cursor++] != 0x80)
            return false;

        const uint8 ModRM = Code[Cursor++];

        if ((ModRM & 0xF8) != 0xB8)
            return false;

        uint8 BaseRegister = ModRM & 7;

        if (BaseRegister == 4)
        {
            if (Remaining <= Cursor)
                return false;

            const uint8 Sib = Code[Cursor++];
            const uint8 IndexRegister = (uint8)(((Sib >> 3) & 7) + ((Rex & 0x02) ? 8 : 0));

            if (IndexRegister != 4)
                return false;

            BaseRegister = (uint8)((Sib & 7) + ((Rex & 0x01) ? 8 : 0));
        }
        else
        {
            BaseRegister = (uint8)(BaseRegister + ((Rex & 0x01) ? 8 : 0));
        }

        if (Remaining - Cursor < sizeof(uint32) + 1)
            return false;

        uint32 Displacement = 0;
        memcpy(&Displacement, Code + Cursor, sizeof(Displacement));
        Cursor += sizeof(Displacement);

        if (Code[Cursor++] != 0)
            return false;

        Out.BaseRegister = BaseRegister;
        Out.Displacement = Displacement;
        Out.Size = Cursor;
        return true;
    }

    static bool FunctionSavesThisInRegister(const uint8* Code, size_t End, uint8 ExpectedRegister)
    {
        if (!Code || ExpectedRegister > 15)
            return false;

        for (size_t At = 0; At + 2 < End; At++)
        {
            size_t Cursor = At;
            uint8 Rex = 0;

            if ((Code[Cursor] & 0xF0) == 0x40)
                Rex = Code[Cursor++];

            if (Cursor + 1 >= End)
                break;

            const uint8 Opcode = Code[Cursor++];

            if (Opcode != 0x8B && Opcode != 0x89)
                continue;

            const uint8 ModRM = Code[Cursor];

            if ((ModRM & 0xC0) != 0xC0)
                continue;

            const uint8 Reg = (uint8)(((ModRM >> 3) & 7) + ((Rex & 0x04) ? 8 : 0));
            const uint8 Rm = (uint8)((ModRM & 7) + ((Rex & 0x01) ? 8 : 0));
            const uint8 Destination = Opcode == 0x8B ? Reg : Rm;
            const uint8 Source = Opcode == 0x8B ? Rm : Reg;

            if (Source == 1 && Destination == ExpectedRegister)
                return true;
        }

        return false;
    }

    static bool MemoryIsReadable(const uint8* Start, size_t Size)
    {
        if (!Start || !Size)
            return false;

        for (size_t At = 0; At < Size; At += 0x1000)
            if (IsBadReadPtr((void*)(Start + At)))
                return false;

        return !IsBadReadPtr((void*)(Start + Size - 1));
    }

    static bool ResolveBusExitLatch(AFortPlayerControllerAthena* Controller)
    {
        auto& Latch = GBusExitLatch;

        if (Latch.bResolved)
            return Latch.bSupported;

        if (!Controller)
            return false;

        Latch.bResolved = true;

        auto ReadyProp = ((UObject*)Controller)->GetProperty("bReadyToStartMatch", 0x20000);

        if (!ReadyProp)
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT looked for: bReadyToStartMatch is not on this "
                "controller class -- bots get their pawn by hand, and the bus may still throw them out at the end of its drop zone.");
            return false;
        }

        const uint32 ReadyOffset = GetFromOffset<uint32>(ReadyProp, Offsets::Offset_Internal);
        const uint8 ReadyMask = ReadyProp->GetFieldMask();

        if (!ReadyMask || ReadyOffset >= 0x20000)
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT looked for: bReadyToStartMatch reads as offset 0x%X, "
                "mask 0x%X.", ReadyOffset, (unsigned)ReadyMask);
            return false;
        }

        const uint64 EnterAircraft = FindEnterAircraft();
        const size_t ScanSize = 0x600;
        const uint64 WarningRef = Memcury::Scanner::FindStringRef(
            L"EnterAircraft: [%s] is attempting to enter aircraft after having already exited.", false, 0,
            VersionInfo.FortniteVersion >= 19).Get();

        if (!EnterAircraft || WarningRef < EnterAircraft || WarningRef >= EnterAircraft + ScanSize ||
            !MemoryIsReadable((const uint8*)EnterAircraft, ScanSize))
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT found: the game's EnterAircraft is %s, its "
                "\"already exited\" warning %s.",
                EnterAircraft ? "found" : "NOT found",
                !WarningRef ? "is NOT found" : ((WarningRef >= EnterAircraft && WarningRef < EnterAircraft + ScanSize)
                    ? "is inside it" : "is NOT inside it"));
            return false;
        }

        const uint8* Code = (const uint8*)EnterAircraft;
        const size_t WarningAt = (size_t)(WarningRef - EnterAircraft);
        uint32 ResolvedOffset = 0;
        int MatchCount = 0;

        for (size_t FirstAt = 0; FirstAt + 7 <= ScanSize; FirstAt++)
        {
            FCmpByteZero First{};

            if (!DecodeCmpByteZero(Code + FirstAt, ScanSize - FirstAt, First))
                continue;

            if (First.Displacement == 0 || First.Displacement >= ReadyOffset || ReadyOffset - First.Displacement > 0x40 ||
                FirstAt >= WarningAt || WarningAt - FirstAt > 0x100 ||
                !FunctionSavesThisInRegister(Code, FirstAt, First.BaseRegister))
                continue;

            const size_t PairEnd = (FirstAt + 0x300 < ScanSize) ? FirstAt + 0x300 : ScanSize;
            bool bFoundPair = false;

            for (size_t SecondAt = FirstAt + First.Size; SecondAt + 7 <= PairEnd; SecondAt++)
            {
                FCmpByteZero Second{};

                if (!DecodeCmpByteZero(Code + SecondAt, ScanSize - SecondAt, Second))
                    continue;

                if (Second.BaseRegister == First.BaseRegister && Second.Displacement + 1 == First.Displacement)
                {
                    bFoundPair = true;
                    break;
                }
            }

            if (!bFoundPair)
                continue;

            ResolvedOffset = First.Displacement;
            MatchCount++;
        }

        if (MatchCount != 1 || ResolvedOffset == 0)
        {
            LogLine("[BUS] Where the game marks a player as off the bus was NOT found: %d matching pair(s) of checks in EnterAircraft "
                "(exactly 1 is needed) -- nothing is written; bots get their pawn by hand, and if the bus throws them out "
                "anyway they are put back on their own pawn.", MatchCount);
            return false;
        }

        Latch.ExitedOffset = ResolvedOffset;
        Latch.ReadyOffset = ReadyOffset;
        Latch.ReadyMask = ReadyMask;
        Latch.bSupported = true;

        LogLine("[BUS] Found where the game marks a player as off the bus (the way Magnesium finds it): \"in the aircraft\" at "
            "+0x%X and \"has exited the aircraft\" at +0x%X on the controller, %u byte(s) before bReadyToStartMatch.",
            ResolvedOffset - 1, ResolvedOffset, ReadyOffset - ResolvedOffset);
        return true;
    }

    static bool MarkBotOffTheBus(AFortPlayerControllerAthena* Controller, const char** Why)
    {
        const char* Ignored = nullptr;
        const char*& Reason = Why ? *Why : Ignored;

        if (!Controller)
        {
            Reason = "no controller";
            return false;
        }

        if (!Controller->IsInAircraft())
        {
            Reason = "the game already had it off the bus";
            return true;
        }

        if (!ResolveBusExitLatch(Controller) || !GBusExitLatch.bSupported)
        {
            Reason = "where the game marks it was not found on this build";
            return false;
        }

        auto& Latch = GBusExitLatch;
        uint8* Base = (uint8*)Controller;
        const uint8* ReadyByte = Base + Latch.ReadyOffset;
        uint8* State = Base + Latch.ExitedOffset - 1;

        if ((*ReadyByte & Latch.ReadyMask) == 0)
        {
            Reason = "it is not marked ready to start the match";
            return false;
        }

        const uint8 InAircraft = State[0];
        const uint8 HasExited = State[1];

        if (InAircraft > 1 || HasExited > 1)
        {
            Latch.Refused++;
            Reason = "the two bytes are not a pair of bools on this controller -- left alone";
            return false;
        }

        if (InAircraft != 0 || HasExited != 1)
        {
            State[0] = 0;
            State[1] = 1;
            Latch.Written++;
        }

        const bool bStillAboard = Controller->IsInAircraft();

        if (Latch.Logged < 3)
        {
            Latch.Logged++;
            LogLine("[BUS] Marked off the bus (check %d of 3): \"in the aircraft\" %u -> %u, \"has exited\" %u -> %u | the game's "
                "IsInAircraft: on board before, %s after.",
                Latch.Logged, (unsigned)InAircraft, (unsigned)State[0], (unsigned)HasExited, (unsigned)State[1],
                bStillAboard ? "STILL ON BOARD" : "off the bus");
        }

        if (bStillAboard)
        {
            Latch.StillAboard++;
            Reason = "the two bytes are set, but the game's IsInAircraft still says on board";
            return false;
        }

        Latch.Applied++;
        Reason = "marked off the bus, and the game agrees";
        return true;
    }

    static int GCombatEngagements = 0;
    static int GCombatBurstsFired = 0;
    static int GCombatBotOnBotHits = 0;

    static int GCombatNoWeapon = 0;

    static int GMagazinesRefilled = 0;

    static int GReloadsStarted = 0;

    static int GBotsMadeKillableAgain = 0;

    static int GWeaponsReissued = 0;

    static int GLocksMade = 0;
    static int GLocksEnded = 0;
    static int GBurstsCutOutOfSight = 0;
    static int GTicksHeldFireOutOfSight = 0;
    static int GAimDeliberateMisses = 0;
    static int GChaseHops = 0;
    static std::map<void*, std::string> BotLockedOnName;

    static std::map<void*, AFortPlayerPawnAthena*> BotCombatTarget;

    static std::map<void*, FVector> BotLastSeenSpot;

    static std::set<void*> BotStoppedForFight;

    static std::set<void*> BotHasFocus;

    static std::map<void*, std::pair<void*, int>> BotAmmoSeen;
    static std::map<void*, int> BotTriggerUpTick;
    static int GShotTicksOurs = 0;
    static int GShotTicksStray = 0;

    struct FBotAimRecord
    {
        double Yaw = 0.0;
        double Pitch = 0.0;
    };
    static std::map<void*, FBotAimRecord> BotIntendedAim;
    static int GAimChecks = 0;
    static int GAimHeld = 0;
    static double GAimOffSum = 0.0;
    static double GAimOffMax = 0.0;

    static int GHitsOnLockedTarget = 0;
    static int GHitsOnSomebodyElse = 0;

    static int GTerrainBlocks = 0;

    static const uint8 GamePhase_Aircraft = 2;
    static const uint8 GamePhase_SafeZones = 3;

    static bool PlayersAreProtected(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasGamePhase())
            return false;

        return GameState->GamePhase < GamePhase_Aircraft;
    }

    static std::set<void*> LandedBots;

    static bool AircraftPhaseReached()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return false;

        auto GS = (AFortGameStateAthena*)World->GameState;

        if (!GS->HasGamePhase())
            return true;

        return GS->GamePhase >= GamePhase_Aircraft;
    }

    static bool HasLanded(void* ControllerKey, AFortPlayerPawnAthena* Pawn)
    {
        if (!ControllerKey)
            return false;

        if (!AircraftPhaseReached())
            return false;

        if (LandedBots.find(ControllerKey) != LandedBots.end())
            return true;

        if (!Pawn)
            return false;

        bool bDropped = false;

        if (MovedToIsland.find((void*)Pawn) != MovedToIsland.end())
            bDropped = true;

        if (!bDropped && Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
            bDropped = true;

        if (!bDropped && Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus)
            bDropped = true;

        if (!bDropped && Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen)
            bDropped = true;

        if (!bDropped)
        {
            FVector Loc = Pawn->K2_GetActorLocation();
            if (Loc.Z > 8000.0)
                bDropped = true;
        }

        if (bDropped)
        {
            LandedBots.insert(ControllerKey);
            return true;
        }

        return false;
    }

    static void KeepTheClockHonest()
    {
        if (!FConfiguration::bLockServerSpeed)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->HasPersistentLevel() || !World->PersistentLevel)
            return;

        auto Level = (UObject*)World->PersistentLevel;

        static int32 SettingsOffset = -2;
        if (SettingsOffset == -2)
            SettingsOffset = (int32)Level->GetOffset("WorldSettings");

        if (SettingsOffset < 0)
            return;

        auto Settings = GetFromOffset<UObject*>(Level, (uint32)SettingsOffset);
        if (!Settings)
            return;

        static const char* Fields[] = { "TimeDilation", "MatineeTimeDilation", "DemoPlayTimeDilation" };
        static int32 Offsets[3] = { -2, -2, -2 };

        for (int i = 0; i < 3; i++)
        {
            if (Offsets[i] == -2)
                Offsets[i] = (int32)Settings->GetOffset(Fields[i]);

            if (Offsets[i] < 0)
                continue;

            float& Value = GetFromOffset<float>(Settings, (uint32)Offsets[i]);

            if (Value != 1.f)
            {
                Value = 1.f;

                static int Reported = 0;
                if (++Reported <= 3)
                    LogLine("[SPEED] Something changed the server's clock (%s). Put back to normal speed.", Fields[i]);
            }
        }
    }

    static bool LooksLikeAName(const std::string& Name)
    {
        if (Name.size() < 3 || Name.size() > 32)
            return false;

        int Letters = 0;
        int HexDigits = 0;

        for (size_t i = 0; i < Name.size(); i++)
        {
            const unsigned char Ch = (unsigned char)Name[i];

            if (Ch < 0x20 || Ch > 0x7E)
                return false;

            if ((Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z'))
                Letters++;

            if ((Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f') || (Ch >= 'A' && Ch <= 'F'))
                HexDigits++;
        }

        if (Letters == 0)
            return false;

        if (Name.size() >= 16 && HexDigits == (int)Name.size())
            return false;

        return true;
    }

    static std::string LongestAnsiRun(const uint8* Bytes, int Count)
    {
        std::string Best;
        std::string Current;

        for (int i = 0; i < Count; i++)
        {
            const unsigned char Ch = (unsigned char)Bytes[i];

            if (Ch >= 0x20 && Ch <= 0x7E)
            {
                Current += (char)Ch;
                continue;
            }

            if (Current.size() > Best.size())
                Best = Current;

            Current.clear();
        }

        if (Current.size() > Best.size())
            Best = Current;

        return Best;
    }

    static std::string LongestWideRun(const uint8* Bytes, int Count)
    {
        std::string Best;

        for (int Start = 0; Start < 2; Start++)
        {
            std::string Current;

            for (int i = Start; i + 1 < Count; i += 2)
            {
                const unsigned char Low = (unsigned char)Bytes[i];
                const unsigned char High = (unsigned char)Bytes[i + 1];

                if (High == 0x00 && Low >= 0x20 && Low <= 0x7E)
                {
                    Current += (char)Low;
                    continue;
                }

                if (Current.size() > Best.size())
                    Best = Current;

                Current.clear();
            }

            if (Current.size() > Best.size())
                Best = Current;
        }

        return Best;
    }

    static std::string NameFromUniqueId(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return std::string();

        const uint8* Bytes = nullptr;
        int Count = 0;

        if (!FUniqueNetIdRepl::HasReplicationBytes())
            return std::string();

        if (PlayerState->HasUniqueId())
        {
            auto& Replicated = PlayerState->UniqueId.ReplicationBytes;

            if (Replicated.IsValid() && Replicated.Num() > 0)
            {
                Bytes = (const uint8*)Replicated.GetData();
                Count = Replicated.Num();
            }
        }

        if ((!Bytes || Count <= 0) && PlayerState->HasUniqueID())
        {
            auto& Replicated = PlayerState->UniqueID.ReplicationBytes;

            if (Replicated.IsValid() && Replicated.Num() > 0)
            {
                Bytes = (const uint8*)Replicated.GetData();
                Count = Replicated.Num();
            }
        }

        if (!Bytes || Count <= 0 || Count > 4096)
            return std::string();

        std::string Narrow = LongestAnsiRun(Bytes, Count);
        std::string Wide = LongestWideRun(Bytes, Count);

        std::string Picked = Wide.size() > Narrow.size() ? Wide : Narrow;

        const size_t Colon = Picked.find_last_of(':');

        if (Colon != std::string::npos && (Picked.size() - Colon - 1) >= 3)
            Picked = Picked.substr(Colon + 1);

        while (!Picked.empty() && Picked.front() == ' ')
            Picked.erase(Picked.begin());

        while (!Picked.empty() && Picked.back() == ' ')
            Picked.pop_back();

        return LooksLikeAName(Picked) ? Picked : std::string();
    }

    static std::string NameAsStored(AFortPlayerStateAthena* PlayerState);

    static std::string SafeNameOf(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return std::string();

        const bool bIsBot = PlayerState->HasbIsABot() && PlayerState->bIsABot;

        if (!bIsBot)
        {
            const std::string FromAccount = NameFromUniqueId(PlayerState);

            if (!FromAccount.empty())
                return FromAccount;
        }

        return NameAsStored(PlayerState);
    }

    static const FString* PlayerNameField(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return nullptr;

        if (PlayerState->HasPlayerNamePrivate())
            return &PlayerState->GetPlayerNamePrivate();

        static int32 Offset = -2;

        if (Offset == -2)
        {
            const uint32 Off = ((UObject*)PlayerState)->GetOffset("PlayerName");
            Offset = (Off == (uint32)-1) ? -1 : (int32)Off;

            LogLine("[NAMES] This build keeps player names in %s.",
                Offset >= 0 ? "APlayerState::PlayerName (there is no PlayerNamePrivate here)"
                : "NEITHER PlayerNamePrivate NOR PlayerName -- names will come out blank");
        }

        if (Offset < 0)
            return nullptr;

        return (const FString*)((const uint8*)PlayerState + Offset);
    }

    static std::string NameAsStored(AFortPlayerStateAthena* PlayerState)
    {
        const FString* Field = PlayerNameField(PlayerState);

        if (!Field)
            return std::string();

        const auto& Stored = *Field;

        const wchar_t* Characters = Stored.CStr();
        int32 Length = Stored.Num();

        if (!Characters || Length <= 0 || Length > 512)
            return std::string();

        std::wstring Wide;
        Wide.reserve((size_t)Length);

        for (int32 i = 0; i < Length; i++)
        {
            const wchar_t Ch = Characters[i];

            if (Ch == L'\0')
                break;

            Wide += Ch;
        }

        if (Wide.empty())
            return std::string();

        std::string Narrow;

        const int Needed = WideCharToMultiByte(CP_UTF8, 0, Wide.c_str(), (int)Wide.size(), nullptr, 0, nullptr, nullptr);

        if (Needed > 0)
        {
            Narrow.resize((size_t)Needed);
            WideCharToMultiByte(CP_UTF8, 0, Wide.c_str(), (int)Wide.size(), &Narrow[0], Needed, nullptr, nullptr);
        }
        else
        {
            for (size_t i = 0; i < Wide.size(); i++)
            {
                const wchar_t Ch = Wide[i];

                if (Ch > 0 && Ch < 128)
                    Narrow += (char)Ch;
            }
        }

        std::string Clean;
        Clean.reserve(Narrow.size());

        for (size_t i = 0; i < Narrow.size(); i++)
        {
            const unsigned char Ch = (unsigned char)Narrow[i];

            if (Ch >= 0x20 && Ch != 0x7F)
                Clean += (char)Ch;
        }

        while (!Clean.empty() && Clean.front() == ' ')
            Clean.erase(Clean.begin());

        while (!Clean.empty() && Clean.back() == ' ')
            Clean.pop_back();

        return Clean;
    }

    static void HoldTheStorm(AFortGameMode* GameMode, float Now)
    {
        if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
            return;

        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;

        float HeldOpen = Now + 600.f;

        if (Indicator->HasSafeZoneStartShrinkTime() && (float)Indicator->SafeZoneStartShrinkTime < HeldOpen)
            Indicator->SafeZoneStartShrinkTime = HeldOpen;

        if (Indicator->HasSafeZoneFinishShrinkTime() && (float)Indicator->SafeZoneFinishShrinkTime < HeldOpen)
            Indicator->SafeZoneFinishShrinkTime = HeldOpen;

        auto World = UWorld::GetWorld();

        if (World && World->GameState)
        {
            auto GameState = (AFortGameStateAthena*)World->GameState;

            if (GameState->HasSafeZonesStartTime() && (float)GameState->SafeZonesStartTime < HeldOpen)
                GameState->SafeZonesStartTime = HeldOpen;
        }

        if (GameMode->HasbSafeZonePaused())
            GameMode->bSafeZonePaused = true;

        static bool bSaid = false;
        if (!bSaid)
        {
            bSaid = true;
            LogLine("[ZONE] Storm held where it is. Nothing shrinks and the clock stops until \"cheat resume zone\".");
        }
    }

    static void ReleaseTheStorm(AFortGameMode* GameMode, float Now)
    {
        static bool bWasPaused = false;

        if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
        {
            bWasPaused = true;
            return;
        }

        if (!bWasPaused)
            return;

        bWasPaused = false;

        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;

        float StartsIn = Now + 30.f;
        float FinishesIn = Now + 60.f;

        if (Indicator->HasSafeZoneStartShrinkTime())
            Indicator->SafeZoneStartShrinkTime = StartsIn;

        if (Indicator->HasSafeZoneFinishShrinkTime())
            Indicator->SafeZoneFinishShrinkTime = FinishesIn;

        auto World = UWorld::GetWorld();

        if (World && World->GameState)
        {
            auto GameState = (AFortGameStateAthena*)World->GameState;

            if (GameState->HasSafeZonesStartTime() && (float)GameState->SafeZonesStartTime > Now)
                GameState->SafeZonesStartTime = Now;
        }

        if (GameMode->HasbSafeZonePaused())
            GameMode->bSafeZonePaused = false;

        LogLine("[ZONE] Storm running again -- it starts closing in 30 seconds.");
    }

    static std::set<const UFortItemDefinition*> GrapplerDefinitions;

    static void ApplyWeaponHotfixes()
    {
        static bool bDone = false;

        if (bDone)
            return;

        static const wchar_t* Grapplers[] = {
            nullptr
        };

        const int Wanted = FConfiguration::GrapplerClipSize;

        if (Wanted <= 0)
        {
            bDone = true;
            return;
        }

        int Patched = 0;
        int Found = 0;

        for (auto Name : Grapplers)
        {
            if (!Name)
                break;

            auto Definition = FindObject<UFortWeaponItemDefinition>(Name);

            if (!Definition)
            {
                std::wstring Wide = Name;
                std::string Narrow(Wide.begin(), Wide.end());
                Definition = TUObjectArray::FindObject<UFortWeaponItemDefinition>(Narrow.c_str());
            }

            if (!Definition)
                continue;

            Found++;
            GrapplerDefinitions.insert((const UFortItemDefinition*)Definition);

            auto Stats = AFortInventory::GetStats(Definition);

            if (!Stats || !FFortRangedWeaponStats::HasClipSize())
                continue;

            if (Stats->ClipSize == Wanted)
            {
                Patched++;
                continue;
            }

            int32 Clip = (int32)Wanted;
            Stats->ClipSize = Clip;

            if (FFortRangedWeaponStats::HasInitialClips())
            {
                int32 Clips = 1;
                Stats->InitialClips = Clips;
            }

            Patched++;
        }

        if (Found == 0)
            return;

        bDone = true;

        LogLine("[HOTFIX] Grappler clip size set to %d on %d of %d grappler definition(s) found -- the same thing the "
            "backend RowUpdate does, applied here instead.", Wanted, Patched, Found);
    }

    static int32* WeaponAmmoCount(AActor* Weapon);

    static void TopUpGrapplers(AFortGameStateAthena* GameState, int TickNow)
    {
        const int Wanted = FConfiguration::GrapplerClipSize;

        if (Wanted <= 0 || GrapplerDefinitions.empty() || !GameState || !GameState->HasPlayerArray())
            return;

        static int NextCheck = 0;

        if (TickNow < NextCheck)
            return;

        NextCheck = TickNow + 15;

        if (!FFortItemEntry::HasItemDefinition() || !FFortItemEntry::HasLoadedAmmo())
            return;

        int ToppedUp = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            if (PS->HasbIsABot() && PS->bIsABot)
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            if (!Controller || !Controller->IsA<AFortPlayerControllerAthena>())
                continue;

            auto Inventory = Controller->WorldInventory;
            if (!Inventory || !Inventory->HasInventory())
                continue;

            for (int e = 0; e < Inventory->Inventory.ItemInstances.Num(); e++)
            {
                auto Item = Inventory->Inventory.ItemInstances[e];
                if (!Item || !Item->HasItemEntry())
                    continue;

                auto& Entry = Item->ItemEntry;

                if (!Entry.ItemDefinition)
                    continue;

                if (GrapplerDefinitions.find(Entry.ItemDefinition) == GrapplerDefinitions.end())
                    continue;

                if (Entry.LoadedAmmo >= Wanted)
                    continue;

                int32 Full = (int32)Wanted;
                Entry.LoadedAmmo = Full;
                Inventory->Update(&Entry);
                ToppedUp++;
            }

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
            {
                auto Held = (AFortWeapon*)Pawn->CurrentWeapon;

                if (Held->HasWeaponData() && Held->WeaponData &&
                    GrapplerDefinitions.find((const UFortItemDefinition*)Held->WeaponData) != GrapplerDefinitions.end())
                {
                    if (auto Ammo = WeaponAmmoCount((AActor*)Held))
                        if (*Ammo < Wanted)
                            *Ammo = Wanted;
                }
            }
        }

        if (ToppedUp > 0)
        {
            static int Reported = 0;

            if (++Reported <= 5)
                LogLine("[HOTFIX] Grappler put back to %d round(s) on %d item(s).", Wanted, ToppedUp);
        }
    }

    static std::map<void*, void*> HumanPawnToppedUp;
    static std::map<void*, int> HumanTopUpUntilTick;

    static void* GOwnerController = nullptr;

    bool IsServerOwner(AFortPlayerControllerAthena* Controller)
    {
        if (!Controller)
            return false;

        if (!GOwnerController)
            return true;

        return GOwnerController == (void*)Controller;
    }

    static void ClaimServerOwner(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        void* First = nullptr;
        bool bOwnerStillHere = false;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            if ((PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS))
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            if (!First)
                First = (void*)Owner;

            if (GOwnerController == (void*)Owner)
                bOwnerStillHere = true;
        }

        if (GOwnerController && bOwnerStillHere)
            return;

        if (GOwnerController != First)
        {
            GOwnerController = First;

            if (GOwnerController)
                LogLine("[OWNER] The server owner is the first player who joined.");
        }
    }

    static bool bBotSpawningPaused = false;

    static bool bBusClearedForLaunch = false;

    void ClearBusForLaunch(const char* Why)
    {
        if (bBusClearedForLaunch)
            return;

        bBusClearedForLaunch = true;

        LogLine("[BUS] Cleared for launch (%s).", Why ? Why : "no reason given");
    }

    bool BusIsClearedForLaunch()
    {
        return bBusClearedForLaunch;
    }

    void EnsureWeaponHotfixes()
    {
        ApplyWeaponHotfixes();
    }

    void SetBotSpawningPaused(bool bPaused)
    {
        if (bBotSpawningPaused == bPaused)
            return;

        bBotSpawningPaused = bPaused;

        if (bPaused)
        {

            bBusClearedForLaunch = false;
            BusCountdownStart = -1.f;
            LastBusSecondAnnounced = -1;
        }

        LogLine("[BotAI] Bot spawning %s.", bPaused ? "paused -- the bus is held on the ground too" : "resumed");
    }

    bool AreBotSpawnsPaused()
    {
        return bBotSpawningPaused;
    }

    void HoldTheWarmupClock(AFortGameStateAthena* GameState, AFortGameMode* GameMode, float Now)
    {
        float HeldOpen = Now + 600.f;

        if (GameState)
        {
            if (GameState->HasWarmupCountdownStartTime())
                GameState->WarmupCountdownStartTime = Now;
            if (GameState->HasWarmupCountdownEndTime())
                GameState->WarmupCountdownEndTime = HeldOpen;
        }

        if (GameMode && GameMode->HasWarmupEarlyCountdownDuration())
            GameMode->WarmupEarlyCountdownDuration = HeldOpen;

        static int Reported = 0;
        if (++Reported <= 3)
            LogLine("[BUS] Warmup clock held open -- the bus waits for %d players.",
                FConfiguration::AutoFillTargetPlayerCount);
    }

    bool BusMayLaunch()
    {
        if (bBotSpawningPaused)
            return false;

        const int Target = FConfiguration::AutoFillTargetPlayerCount;

        if (Target <= 0)
            return true;

        auto World = UWorld::GetWorld();

        if (!World || !World->AuthorityGameMode)
            return false;

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;

        if (!GameMode->HasAlivePlayers())
            return true;

        return GameMode->AlivePlayers.Num() >= Target;
    }

    bool MatchHasStarted()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return true;

        return !PlayersAreProtected((AFortGameStateAthena*)World->GameState);
    }

    static void WipeTheSlateAtBusTime(AFortGameStateAthena* GameState);

    static std::map<void*, std::pair<void*, int>> ShotAtBy;

    static std::map<void*, std::pair<void*, int>> AggroedByHuman;

    // how long a bot holds a grudge against whoever shot it (refreshed every tick it can see them)
    static constexpr int kGrudgeTicks = 30 * 90;

    static std::map<void*, int> BotUnderFireSince;
    static std::set<void*> BotAlreadyBuiltWall;

    static int GElimsFromLedger = 0;

    static std::map<void*, FVector> MovementProbe;

    static int LandingLogCount = 0;

    static std::map<void*, std::string> AssignedBotNames;

    static std::set<void*> EliminatedBots;

    static std::set<void*> SeenAliveBots;

    static std::map<void*, float> BotBeingFixedUntil;

    static bool BotIsBeingFixed(void* Controller)
    {
        if (!Controller || BotBeingFixedUntil.empty())
            return false;

        auto It = BotBeingFixedUntil.find(Controller);
        if (It == BotBeingFixedUntil.end())
            return false;

        auto World = UWorld::GetWorld();
        if (World && (float)UGameplayStatics::GetTimeSeconds(World) <= It->second)
            return true;

        BotBeingFixedUntil.erase(It);
        return false;
    }

    static void HandleBotDeath(AFortPlayerControllerAthena* DeadBotController);

    static bool bVictoryDeclared = false;

    static int PeakAlivePlayers = 0;
    static int PeakAliveBots = 0;

    static int VictoryConfirmTicks = 0;
    static constexpr int VictoryConfirmTicksRequired = 3;

    static int VictoryResendTicksLeft = 0;
    static uint8 VictoryDeathCause = 0;
    static constexpr int VictoryResendDurationTicks = 450;
    static constexpr int VictoryResendEveryTicks = 30;

    struct FAliveSnapshot
    {
        int Total = 0;
        int Bots = 0;
        int Humans = 0;
    };

    static bool PlayerIsStillIn(AFortGameMode* GameMode, AFortPlayerStateAthena* PS, bool* OutOnTheBus = nullptr)
    {
        if (OutOnTheBus)
            *OutOnTheBus = false;

        if (!PS)
            return false;

        auto OwnerActor = (AActor*)PS->Owner;
        if (!OwnerActor)
            return false;

        auto Pw = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)OwnerActor)->Pawn;
        if (Pw)
            return Pw->GetHealth() > 0.f || BotIsBeingFixed((void*)OwnerActor);

        if (EliminatedBots.find((void*)OwnerActor) != EliminatedBots.end())
            return false;

        if (!GameMode || !GameMode->HasAlivePlayers())
            return false;

        for (int i = 0; i < GameMode->AlivePlayers.Num(); i++)
        {
            if ((void*)GameMode->AlivePlayers[i] == (void*)OwnerActor)
            {
                if (OutOnTheBus)
                    *OutOnTheBus = true;

                return true;
            }
        }

        return false;
    }

    static AFortGameMode* CurrentGameMode()
    {
        auto World = UWorld::GetWorld();
        return World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
    }

    static FAliveSnapshot SnapshotAlive(AFortGameStateAthena* GameState,
        std::vector<AFortPlayerStateAthena*>* OutAlive,
        AFortPlayerStateAthena* Ignore)
    {
        FAliveSnapshot Snap{};

        if (OutAlive)
            OutAlive->clear();

        if (!GameState || !GameState->HasPlayerArray())
            return Snap;

        auto GameMode = CurrentGameMode();

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || PS == Ignore || NPCs::IsNPC(PS))
                continue;

            if (!PlayerIsStillIn(GameMode, PS))
                continue;

            Snap.Total++;
            if (PS->HasbIsABot() && PS->bIsABot)
                Snap.Bots++;
            else
                Snap.Humans++;

            if (OutAlive)
                OutAlive->push_back(PS);
        }

        return Snap;
    }

    static void InvokeNoArgs(const UObject* Object, const char* FunctionName)
    {
        if (!Object || !FunctionName)
            return;

        auto Fn = Object->GetFunction(FunctionName);
        if (!Fn)
            return;

        const int32 Size = Fn->GetPropertiesSize();

        if (Size <= 0 || Size > 0x1000)
        {
            Object->ProcessEvent(Fn, nullptr);
            return;
        }

        std::vector<uint8> Buffer((size_t)Size, 0);
        Object->ProcessEvent(Fn, Buffer.data());
    }

    static std::set<void*> WinAnnouncedTo;

    static void SendWinTo(AFortPlayerStateAthena* WinnerState, uint8 DeathCause, bool bAnnounceToClient)
    {
        if (!WinnerState)
            return;

        if (WinnerState->HasPlace())
        {
            WinnerState->Place = 1;
            InvokeNoArgs(WinnerState, "OnRep_Place");
        }

        auto OwnerActor = (AActor*)WinnerState->Owner;
        auto Controller = OwnerActor ? OwnerActor->Cast<AFortPlayerControllerAthena>() : nullptr;
        if (!Controller)
            return;

        if (bAnnounceToClient && WinAnnouncedTo.find((void*)WinnerState) == WinAnnouncedTo.end())
        {
            WinAnnouncedTo.insert((void*)WinnerState);

            auto WinnerPawn = Controller->Pawn;
            UObject* NoWeapon = nullptr;

            if (Controller->GetFunction("PlayWinEffects"))
                Controller->PlayWinEffects(WinnerPawn, NoWeapon, DeathCause, false);

            if (Controller->GetFunction("ClientNotifyWon"))
                Controller->ClientNotifyWon(WinnerPawn, NoWeapon, DeathCause);

            if (Controller->GetFunction("ClientNotifyTeamWon"))
                Controller->ClientNotifyTeamWon(WinnerPawn, NoWeapon, DeathCause);
        }

        InvokeNoArgs(Controller, "ForceNetUpdate");
    }

    static void DeclareVictory(const std::vector<AFortPlayerStateAthena*>& Winners, uint8 DeathCause,
        const char* Reason)
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || Winners.empty())
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;

        bVictoryDeclared = true;
        VictoryDeathCause = DeathCause;

        AFortPlayerStateAthena* Headline = Winners[0];
        for (auto PS : Winners)
        {
            if (PS && !(PS->HasbIsABot() && PS->bIsABot))
            {
                Headline = PS;
                break;
            }
        }

        if (GameState->HasWinningTeam() && Headline->HasTeamIndex())
        {
            GameState->WinningTeam = (int32)Headline->TeamIndex;
            InvokeNoArgs(GameState, "OnRep_WinningTeam");
        }

        if (GameState->HasWinningPlayerState())
        {
            GameState->WinningPlayerState = Headline;
            InvokeNoArgs(GameState, "OnRep_WinningPlayerState");
        }

        for (auto PS : Winners)
            SendWinTo(PS, DeathCause, true);

        if (GameState->HasPlayersLeft())
        {
            GameState->PlayersLeft = (int32)Winners.size();
            InvokeNoArgs(GameState, "OnRep_PlayersLeft");
        }
        if (GameState->HasTeamsLeft())
            GameState->TeamsLeft = 1;

        if (GameState->HasGamePhase() && (uint8)GameState->GamePhase < 5)
        {
            GameState->GamePhase = (uint8)5;
            InvokeNoArgs(GameState, "OnRep_GamePhase");
        }

        VictoryResendTicksLeft = VictoryResendDurationTicks;

        LogLine("[WIN] ================ VICTORY ROYALE ================");
        LogLine("[WIN] %d winner(s) -- reason: %s", (int)Winners.size(), Reason);
        for (auto PS : Winners)
        {
            if (!PS)
                continue;
            const std::string n = SafeNameOf(PS);
            LogLine("[WIN]   '%s' (team %d)%s", n.c_str(),
                PS->HasTeamIndex() ? (int)PS->TeamIndex : -1,
                (PS->HasbIsABot() && PS->bIsABot) ? "  [bot -- no client to notify]" : "");
        }
        LogLine("[WIN] Notification sent once. The won state is re-asserted for the next 15s in case a client is still catching up.");
    }

    static void TickVictoryResend()
    {
        if (VictoryResendTicksLeft <= 0)
            return;

        VictoryResendTicksLeft--;
        if ((VictoryResendTicksLeft % VictoryResendEveryTicks) != 0)
            return;

        auto World = UWorld::GetWorld();
        auto GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasPlace() || PS->Place != 1)
                continue;

            SendWinTo(PS, VictoryDeathCause, false);
        }
    }

    bool ShouldGrantVictoryTo(AFortPlayerStateAthena* CandidateState, AFortPlayerStateAthena* DyingVictimState)
    {
        if (bVictoryDeclared || !CandidateState || CandidateState == DyingVictimState)
            return false;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return false;

        auto GameState = (AFortGameStateAthena*)World->GameState;

        if (PlayersAreProtected(GameState))
            return false;

        if (PeakAlivePlayers < 2)
            return false;

        std::vector<AFortPlayerStateAthena*> Alive;
        auto Snap = SnapshotAlive(GameState, &Alive, DyingVictimState);

        const bool bLastStanding = (Snap.Total <= 1);
        const bool bAllBotsDead = (PeakAliveBots > 0 && Snap.Bots == 0);

        if (!bLastStanding && !bAllBotsDead)
            return false;

        if (std::find(Alive.begin(), Alive.end(), CandidateState) == Alive.end())
            Alive.push_back(CandidateState);

        DeclareVictory(Alive, 0, bLastStanding ? "final elimination -- one player left"
            : "final elimination -- all AI players dead");
        return true;
    }

    void CheckForVictory()
    {
        if (bVictoryDeclared)
        {
            TickVictoryResend();
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;

        if (PlayersAreProtected(GameState))
        {
            VictoryConfirmTicks = 0;
            return;
        }

        static auto InProgressName = FName(L"InProgress");
        if (GameMode->HasMatchState() && !(GameMode->MatchState == InProgressName))
        {
            VictoryConfirmTicks = 0;
            return;
        }

        std::vector<AFortPlayerStateAthena*> Alive;
        auto Snap = SnapshotAlive(GameState, &Alive, nullptr);

        if (Snap.Total > PeakAlivePlayers)
            PeakAlivePlayers = Snap.Total;
        if (Snap.Bots > PeakAliveBots)
            PeakAliveBots = Snap.Bots;

        if (PeakAlivePlayers < 2 || Snap.Total == 0)
        {
            VictoryConfirmTicks = 0;
            return;
        }

        const char* Reason = nullptr;

        if (Snap.Total == 1)
        {
            Reason = "one player left standing";
        }
        else
        {
            bool bSameTeam = Alive[0] && Alive[0]->HasTeamIndex();
            if (bSameTeam)
            {
                const uint8 Team = (uint8)Alive[0]->TeamIndex;
                for (auto PS : Alive)
                {
                    if (!PS || !PS->HasTeamIndex() || (uint8)PS->TeamIndex != Team)
                    {
                        bSameTeam = false;
                        break;
                    }
                }
            }

            if (bSameTeam)
                Reason = "only one team left alive";
        }

        if (!Reason)
        {
            VictoryConfirmTicks = 0;
            return;
        }

        if (VictoryConfirmTicks == 0)
            LogLine("[WIN] Win condition met (%s) -- confirming over %d ticks before awarding...",
                Reason, VictoryConfirmTicksRequired);

        if (++VictoryConfirmTicks < VictoryConfirmTicksRequired)
            return;

        DeclareVictory(Alive, 0, Reason);
    }

    static float LastThankTime = -1000.f;

    static std::map<void*, float> PendingParachuteOpen;

    static std::map<void*, AFortPickupAthena*> BotLootTargets;

    static std::map<void*, float> GlidingBots;
    static std::map<void*, int> GlidingSettledFor;

    class BFortPlayerPawn : public UObject
    {
    public:
        UCLASS_COMMON_MEMBERS(BFortPlayerPawn);

        DEFINE_FUNC(ForceOpenParachuteAndOverrideGlider, void);
        DEFINE_FUNC(OnRep_IsParachuteForcedOpen, void);
        DEFINE_BITFIELD_PROP(bIsParachuteForcedOpen);
    };

    static const wchar_t* BotGliderIds[] = {
        L"Umbrella_Season_11",
        L"Umbrella_BuildABrella",
        L"Umbrella_Silver",
        L"Glider_ID_015_Brite",
        L"Glider_ID_031_Metal",
        L"Glider_ID_080_PrairiePusher",
    };

    static std::vector<const UObject*> BotGliderChoices;
    static std::map<void*, const UObject*> BotGliderOf;

    static const UObject* PickBotGlider()
    {
        static bool bResolved = false;

        if (!bResolved)
        {
            bResolved = true;

            static auto GliderItemClass = FindClass("AthenaGliderItemDefinition");

            const int Listed = (int)(sizeof(BotGliderIds) / sizeof(BotGliderIds[0]));
            std::string Missing;

            for (int i = 0; i < Listed; i++)
            {
                std::wstring Wide(BotGliderIds[i]);
                std::wstring Path = L"/Game/Athena/Items/Cosmetics/Gliders/" + Wide + L"." + Wide;
                std::string Narrow(Wide.begin(), Wide.end());

                const UObject* Glider = FindObject(Path.c_str(), UObject::StaticClass());

                if (!Glider)
                    Glider = TUObjectArray::FindObject<UObject>(Narrow.c_str());

                if (Glider && GliderItemClass && !Glider->IsA(GliderItemClass))
                    Glider = nullptr;

                if (Glider)
                {
                    BotGliderChoices.push_back(Glider);
                }
                else
                {
                    if (!Missing.empty())
                        Missing += ", ";

                    Missing += Narrow;
                }
            }

            LogLine("[GLIDE] %d of the %d bot gliders are on this build%s%s.", (int)BotGliderChoices.size(), Listed,
                Missing.empty() ? "" : " -- not found: ", Missing.c_str());

            if (GliderItemClass)
            {
                const size_t Named = BotGliderChoices.size();
                std::string Extra;

                for (int i = 0; i < TUObjectArray::Num(); i++)
                {
                    const UObject* Object = TUObjectArray::GetObjectByIndex(i);

                    if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA(GliderItemClass))
                        continue;

                    if (std::find(BotGliderChoices.begin(), BotGliderChoices.end(), Object) != BotGliderChoices.end())
                        continue;

                    BotGliderChoices.push_back(Object);

                    if (BotGliderChoices.size() - Named <= 12)
                    {
                        if (!Extra.empty())
                            Extra += ", ";

                        Extra += Object->Name.ToString().c_str();
                    }
                }

                LogLine("[GLIDE] %zu more glider(s) loaded on this server%s%s%s -- %zu to pick from.", BotGliderChoices.size() - Named,
                    Extra.empty() ? "" : ": ", Extra.c_str(), (BotGliderChoices.size() - Named) > 12 ? ", ..." : "", BotGliderChoices.size());
            }
        }

        if (BotGliderChoices.empty())
            return nullptr;

        return BotGliderChoices[(size_t)rand() % BotGliderChoices.size()];
    }

    struct FBotGlide
    {
        float JumpTime = 0.f;
        float JumpZ = 0.f;
        float OpenTime = -1.f;
        float OpenZ = 0.f;
        float LastDescentZ = 0.f;
        float LastDescentTime = 0.f;
        float NextReopenTime = 0.f;
        int Reopens = 0;
        bool bOpenLastTick = false;
    };

    static std::map<void*, FBotGlide> BotGlides;

    static int GGlideOpened = 0;
    static int GGlideOpenedByGame = 0;
    static int GGlideOpenedByFlag = 0;
    static int GGlideOpenFailed = 0;
    static int GGlideReopened = 0;
    static int GGlideLanded = 0;
    static int GGlideLandedGliding = 0;
    static bool bGliderForceOpenInFlight = false;
    static bool bGliderForceOpenBroken = false;

    static std::string BotPawnName(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasPlayerState() || !Pawn->PlayerState)
            return "a bot";

        const std::string Name = SafeNameOf((AFortPlayerStateAthena*)Pawn->PlayerState);
        return Name.empty() ? std::string("a bot") : Name;
    }

    static const char* OpenBotGlider(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasbIsParachuteOpen())
            return "";

        if (bGliderForceOpenInFlight)
        {
            bGliderForceOpenInFlight = false;
            bGliderForceOpenBroken = true;
            LogLine("[GLIDE] The game's force-open crashed on a bot -- gliders are opened with bIsParachuteOpen from now on.");
        }

        auto GliderIt = BotGliderOf.find((void*)Pawn);
        const UObject* Glider = (GliderIt != BotGliderOf.end()) ? GliderIt->second : nullptr;

        if (!bGliderForceOpenBroken && Glider && ((UObject*)Pawn)->GetFunction("ForceOpenParachuteAndOverrideGlider"))
        {
            const UObject* GliderArgument = Glider;
            bool bResetOverrideOnLanding = true;
            bool bIntoGliderRedeploy = false;

            bGliderForceOpenInFlight = true;
            ((BFortPlayerPawn*)Pawn)->ForceOpenParachuteAndOverrideGlider(GliderArgument, bResetOverrideOnLanding, bIntoGliderRedeploy);
            bGliderForceOpenInFlight = false;

            if (Pawn->bIsParachuteOpen)
                return "the game's force-open";
        }

        Pawn->bIsParachuteOpen = true;
        Pawn->OnRep_IsParachuteOpen(false);

        return Pawn->bIsParachuteOpen ? "bIsParachuteOpen" : "";
    }

    static void ForceClearSkydive(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn)
            return;

        if (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen)
        {
            Pawn->bIsParachuteOpen = false;
            Pawn->OnRep_IsParachuteOpen(true);
        }

        auto Chute = (BFortPlayerPawn*)Pawn;
        if (Chute->HasbIsParachuteForcedOpen() && Chute->bIsParachuteForcedOpen)
        {
            Chute->bIsParachuteForcedOpen = false;
            Chute->OnRep_IsParachuteForcedOpen();
        }

        if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
        {
            Pawn->bIsSkydiving = false;

            bool bWasSkydiving = true;
            Pawn->OnRep_IsSkydiving(bWasSkydiving);
        }
        if (Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus)
            Pawn->bIsSkydivingFromBus = false;

        if (AFortPlayerPawnAthena::EndSkydivingOG)
            AFortPlayerPawnAthena::EndSkydiving(Pawn);
    }

    static int32 GLoadoutGliderOffset = -2;
    static int32 GPawnLoadoutOffset = -2;
    static int32 GControllerLoadoutOffset = -2;
    static int GGlidersPutInLoadout = 0;
    static int GGlidersAlreadyInLoadout = 0;

    static bool ResolveLoadoutGlider(AFortPlayerPawnAthena* Pawn)
    {
        if (GLoadoutGliderOffset == -2 && Pawn)
        {
            auto Struct = FindStruct("FortAthenaLoadout");
            const uint32 Off = Struct ? Struct->GetOffset("Glider") : (uint32)-1;
            GLoadoutGliderOffset = (Off == (uint32)-1 || Off > 0x200) ? -1 : (int32)Off;

            const uint32 PawnOff = ((UObject*)Pawn)->GetOffset("CustomizationLoadout");
            GPawnLoadoutOffset = (PawnOff == (uint32)-1) ? -1 : (int32)PawnOff;

            LogLine("[GLIDE] The cosmetic loadout's glider: %s | the pawn's loadout: %s.",
                GLoadoutGliderOffset >= 0 ? "found" : "NOT on this build", GPawnLoadoutOffset >= 0 ? "found" : "NOT on this build");
        }

        return GLoadoutGliderOffset >= 0 && GPawnLoadoutOffset >= 0;
    }

    static const UObject* GliderInBotLoadout(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !ResolveLoadoutGlider(Pawn))
            return nullptr;

        return *(const UObject**)((uint8*)Pawn + GPawnLoadoutOffset + GLoadoutGliderOffset);
    }

    static const char* PutGliderInBotLoadout(AFortPlayerPawnAthena* Pawn, const UObject* Glider)
    {
        if (!Pawn || !Glider)
            return "no glider to put in";

        if (!ResolveLoadoutGlider(Pawn))
            return "this build's loadout has no glider";

        auto& PawnGlider = *(const UObject**)((uint8*)Pawn + GPawnLoadoutOffset + GLoadoutGliderOffset);
        const char* What = "already had one";

        if (!PawnGlider)
        {
            PawnGlider = Glider;
            GGlidersPutInLoadout++;
            What = "put in its loadout";
        }
        else
        {
            GGlidersAlreadyInLoadout++;
        }

        if (auto Controller = (UObject*)Pawn->Controller)
        {
            if (GControllerLoadoutOffset == -2)
            {
                const uint32 Off = Controller->GetOffset("CustomizationLoadout");
                GControllerLoadoutOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
            }

            if (GControllerLoadoutOffset >= 0)
            {
                auto& ControllerGlider = *(const UObject**)((uint8*)Controller + GControllerLoadoutOffset + GLoadoutGliderOffset);

                if (!ControllerGlider)
                    ControllerGlider = PawnGlider;
            }
        }

        return What;
    }

    static bool GGlideSpeedRead = false;
    static float GGlideTerminal = 700.f;
    static int GGlideHeldTicks = 0;
    static double GGlideRateSum = 0.0;
    static int GGlideRateCount = 0;

    static float ReadAirTerminalVelocity(UObject* Move, const char* ParamsName)
    {
        static int32 TerminalOffset = -2;

        if (TerminalOffset == -2)
        {
            auto Struct = FindStruct("AirControlParams");
            const uint32 Off = Struct ? Struct->GetOffset("TerminalVelocity") : (uint32)-1;
            TerminalOffset = (Off == (uint32)-1 || Off > 0x200) ? -1 : (int32)Off;
        }

        const uint32 ParamsOffset = Move ? Move->GetOffset(ParamsName) : (uint32)-1;

        if (TerminalOffset < 0 || ParamsOffset == (uint32)-1 || ParamsOffset > 0x4000)
            return -1.f;

        auto Speed = (FScalableFloat*)((uint8*)Move + ParamsOffset + TerminalOffset);
        return Speed->Evaluate();
    }

    static float BotGlideTerminal(AFortPlayerPawnAthena* Pawn)
    {
        if (GGlideSpeedRead || !Pawn)
            return GGlideTerminal;

        GGlideSpeedRead = true;

        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? (UObject*)Pawn->CharacterMovement : nullptr;

        const float Skydive = ReadAirTerminalVelocity(Move, "SkydivingControlParamsPassive");
        const float Diving = ReadAirTerminalVelocity(Move, "SkydivingControlParamsActive");
        const float Glider = ReadAirTerminalVelocity(Move, "ParachuteControlParamsSkydive");
        const float GliderFromJump = ReadAirTerminalVelocity(Move, "ParachuteControlParamsJump");

        const bool bUsable = Glider >= 150.f && Glider <= 1500.f;

        if (bUsable)
            GGlideTerminal = Glider;

        LogLine("[GLIDE] This build's own fall speeds, units a second: skydiving %.0f (diving %.0f), under the glider %.0f (opened from a "
            "jump %.0f) -- bots come down under their glider at no more than %.0f a second (%s).",
            Skydive, Diving, Glider, GliderFromJump, GGlideTerminal,
            bUsable ? "the game's own glider speed" : "the game's own could not be read, so the scripted glide's speed");

        return GGlideTerminal;
    }

    static void HoldBotGlideDescent(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasbIsParachuteOpen() || !Pawn->bIsParachuteOpen)
            return;

        auto GlideIt = BotGlides.find((void*)Pawn);
        if (GlideIt == BotGlides.end() || GlideIt->second.OpenTime < 0.f)
            return;

        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? Pawn->CharacterMovement : nullptr;
        if (!Move || !Move->HasVelocity())
            return;

        const double Limit = -(double)BotGlideTerminal(Pawn);
        FVector Velocity = Move->Velocity;

        if ((double)Velocity.Z < Limit)
        {
            Velocity.Z = Limit;
            Move->Velocity = Velocity;
            GGlideHeldTicks++;
        }
    }

    void TickLandings(UWorld* World)
    {

        if (World && World->GameState)
        {
            auto GS = (AFortGameStateAthena*)World->GameState;
            if (GS->HasPlayerArray())
            {
                for (int i = 0; i < GS->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
                    if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                        continue;
                    auto Ctrl = (AFortPlayerControllerAthena*)PS->Owner;
                    if (!Ctrl)
                        continue;
                    auto Pawn = (AFortPlayerPawnAthena*)Ctrl->Pawn;
                    if (!Pawn || Pawn->GetHealth() <= 0.f)
                        continue;

                    if (GlidingBots.find((void*)Pawn) != GlidingBots.end())
                        continue;

                    bool bWalking = false;
                    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement &&
                        Pawn->CharacterMovement->HasMovementMode())
                    {
                        const uint8 Mode = Pawn->CharacterMovement->MovementMode;

                        if (Mode == 1 || Mode == 2)
                            bWalking = true;
                    }

                    const bool bSkydiveStuck =
                        (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ||
                        (Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus) ||
                        (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen);

                    if (bSkydiveStuck && bWalking)
                    {
                        ForceClearSkydive(Pawn);
                        GlidingBots.erase((void*)Pawn);
                        GlidingSettledFor.erase((void*)Pawn);
                        MovedToIsland.insert((void*)Pawn);
                    }
                }
            }
        }

        static std::map<void*, std::pair<float, float>> HumanGlideStart;
        static std::set<void*> HumanGlideMeasured;
        static int HumanGlideLines = 0;

        if (World && World->GameState && HumanGlideLines < 4)
        {
            auto GS = (AFortGameStateAthena*)World->GameState;
            const float NowHuman = (float)UGameplayStatics::GetTimeSeconds(World);

            for (int i = 0; GS->HasPlayerArray() && i < GS->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
                if (!PS || (PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS) || !PS->Owner)
                    continue;

                auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)PS->Owner)->Pawn;
                if (!Pawn || HumanGlideMeasured.find((void*)Pawn) != HumanGlideMeasured.end())
                    continue;

                const bool bOpen = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;
                auto Start = HumanGlideStart.find((void*)Pawn);

                if (Start == HumanGlideStart.end())
                {
                    if (bOpen)
                        HumanGlideStart[(void*)Pawn] = { (float)Pawn->K2_GetActorLocation().Z, NowHuman };

                    continue;
                }

                const float Flown = NowHuman - Start->second.second;

                if (bOpen && Flown < 6.f)
                    continue;

                HumanGlideMeasured.insert((void*)Pawn);

                if (Flown >= 1.f)
                {
                    const float Z = (float)Pawn->K2_GetActorLocation().Z;
                    HumanGlideLines++;

                    LogLine("[GLIDE] '%s' (a real player) came down %.0f a second under their glider (%.0f units in %.1fs from Z=%.0f) -- "
                        "this build's own glider, to hold the bots' glide up against.",
                        SafeNameOf(PS).c_str(), (Start->second.first - Z) / Flown, Start->second.first - Z, Flown, Start->second.first);
                }
            }
        }

        if (GlidingBots.empty())
            return;

        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        for (auto it = GlidingBots.begin(); it != GlidingBots.end();)
        {
            auto Pawn = (AFortPlayerPawnAthena*)it->first;

            if (!Pawn)
            {
                GlidingSettledFor.erase(it->first);
                BotGlides.erase(it->first);
                it = GlidingBots.erase(it);
                continue;
            }

            const float Height = (float)Pawn->K2_GetActorLocation().Z;
            FBotGlide& Glide = BotGlides[it->first];

            if (Glide.LastDescentTime <= 0.f)
            {
                Glide.LastDescentZ = Height;
                Glide.LastDescentTime = Now;

                if (Glide.JumpTime <= 0.f)
                {
                    Glide.JumpTime = Now;
                    Glide.JumpZ = Height;
                }
            }

            const bool bGliderOpen = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;

            bool bDown = false;
            const char* Why = "";

            if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement &&
                Pawn->CharacterMovement->HasMovementMode())
            {
                const uint8 Mode = Pawn->CharacterMovement->MovementMode;

                if (Mode == 1 || Mode == 2)
                {
                    bDown = true;
                    Why = "on the ground";
                }
                else if (Mode == 4)
                {
                    bDown = true;
                    Why = "in the water";
                }
            }

            if (Height < Glide.LastDescentZ - 20.f)
            {
                Glide.LastDescentZ = Height;
                Glide.LastDescentTime = Now;
            }
            else if (!bDown && (Now - Glide.LastDescentTime) > 1.5f)
            {
                bDown = true;
                Why = "stopped going down";
            }

            if (!bDown && (Now - Glide.JumpTime) > 240.f)
            {
                bDown = true;
                Why = "four minutes without landing";
            }

            it->second = Height;

            if (!bDown)
            {

                if (Glide.OpenTime >= 0.f && !bGliderOpen && Now >= Glide.NextReopenTime)
                {
                    Glide.NextReopenTime = Now + 0.5f;

                    Pawn->bIsParachuteOpen = true;
                    Pawn->OnRep_IsParachuteOpen(false);

                    Glide.Reopens++;
                    GGlideReopened++;

                    if (GGlideReopened <= 6)
                        LogLine("[GLIDE] %s's glider was closed in the air at Z=%.0f -- opened it again (now %s).",
                            BotPawnName(Pawn).c_str(), Height, Pawn->bIsParachuteOpen ? "open" : "STILL CLOSED");
                }

                Glide.bOpenLastTick = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;

                ++it;
                continue;
            }

            const bool bWasGliding = bGliderOpen || Glide.bOpenLastTick;

            GGlideLanded++;

            if (bWasGliding)
                GGlideLandedGliding++;

            const float Flown = (Glide.OpenTime >= 0.f) ? Now - Glide.OpenTime : 0.f;
            const float GlideRate = (Flown > 0.5f) ? (Glide.OpenZ - Height) / Flown : -1.f;

            if (GlideRate > 0.f)
            {
                GGlideRateSum += (double)GlideRate;
                GGlideRateCount++;
            }

            if (GGlideLanded <= 8)
            {
                char Glided[160] = "";

                if (Glide.OpenTime >= 0.f)
                    snprintf(Glided, sizeof(Glided), ", glider opened at Z=%.0f and flown for %.1fs, coming down %.0f a second under it",
                        Glide.OpenZ, Flown, GlideRate);

                LogLine("[GLIDE] '%s' touched down (%s) at Z=%.0f, %.1fs after leaving the bus -- %s%s%s.", BotPawnName(Pawn).c_str(), Why,
                    Height, Now - Glide.JumpTime, bWasGliding ? "still under its glider" : "NOT under a glider", Glided,
                    Glide.Reopens > 0 ? " (it had to be re-opened in the air)" : "");
            }
            else if ((GGlideLanded % 20) == 0)
            {
                LogLine("[GLIDE] %d bot(s) down from the bus: %d still under the glider when they touched down, %d not.", GGlideLanded,
                    GGlideLandedGliding, GGlideLanded - GGlideLandedGliding);
            }

            ForceClearSkydive(Pawn);
            MovedToIsland.insert((void*)Pawn);

            GlidingSettledFor.erase(it->first);
            BotGlides.erase(it->first);
            BotGliderOf.erase(it->first);
            it = GlidingBots.erase(it);
        }

        static bool bSaidEveryoneDown = false;

        if (GlidingBots.empty() && GGlideLanded > 0 && !bSaidEveryoneDown)
        {
            bSaidEveryoneDown = true;
            LogLine("[GLIDE] Every bot is down from the bus: %d of %d were still under the glider when they touched down | gliders opened: "
                "%d (%d with the game's force-open and the bot's own glider, %d with bIsParachuteOpen), %d could not be opened, "
                "%d re-opened after closing in the air.",
                GGlideLandedGliding, GGlideLanded, GGlideOpened, GGlideOpenedByGame, GGlideOpenedByFlag, GGlideOpenFailed, GGlideReopened);

            LogLine("[GLIDE] Under their gliders they came down %.0f a second on average (%d bot(s) measured; the game's own glider: %.0f) | "
                "ticks a bot was coming down faster than a glider and was held to it: %d | gliders put in a bot's cosmetic loadout: %d "
                "(it had one already: %d).",
                GGlideRateCount > 0 ? GGlideRateSum / (double)GGlideRateCount : -1.0, GGlideRateCount, GGlideTerminal, GGlideHeldTicks,
                GGlidersPutInLoadout, GGlidersAlreadyInLoadout);
        }
    }

    void TickParachutes(UWorld* World)
    {
        (void)World;

        // Bots never open a glider by hand. They skydive on the game's own movement.
        PendingParachuteOpen.clear();
    }

    static bool bBusDeparted = false;

    static uint32 BotPersonalSeed(void* Pawn)
    {
        uint64 Value = (uint64)Pawn;

        Value ^= Value >> 33;
        Value *= 0xFF51AFD7ED558CCDull;
        Value ^= Value >> 33;

        return (uint32)Value;
    }

    static bool BotIsLookingAt(AFortPlayerPawnAthena* Me, const FVector& MyLoc, const FVector& ThemLoc)
    {
        const double dx = ThemLoc.X - MyLoc.X;
        const double dy = ThemLoc.Y - MyLoc.Y;
        const double Flat = sqrt(dx * dx + dy * dy);

        if (Flat <= (double)FConfiguration::BotHearingRange)
            return true;

        if (Flat < 1.0)
            return true;

        FVector Facing = Me->GetActorForwardVector();

        const double FacingLength = sqrt(Facing.X * Facing.X + Facing.Y * Facing.Y);

        if (FacingLength < 0.001)
            return true;

        const double Dot = ((Facing.X / FacingLength) * (dx / Flat)) + ((Facing.Y / FacingLength) * (dy / Flat));

        double HalfAngle = (double)FConfiguration::BotFieldOfView * 0.5;

        if (!(HalfAngle > 5.0) || HalfAngle > 180.0)
            HalfAngle = 60.0;

        return Dot >= cos(HalfAngle * 3.14159265358979323846 / 180.0);
    }

    static bool BotTraceAvailable();
    static bool BotStraightLineSees(AFortPlayerPawnAthena* Me, AActor* Them, std::string* WhatIsInTheWay);

    static int GSightByLineClear = 0;
    static int GSightByLineBlocked = 0;
    static std::string GSightFirstBlocker;

    static bool BotHasLineOfSight(AAIController* AIController, AActor* Them)
    {
        static int Available = -1;

        if (Available == -1)
        {
            Available = (AIController && AIController->GetFunction("LineOfSightTo")) ? 1 : 0;

            LogLine("[EYES] The game's own line-of-sight check %s -- bots %s see through walls.",
                Available ? "is available" : "is NOT on this build",
                Available ? "cannot" : "can still");
        }

        if (AIController && Them && IsPlayerControllerBot((AActor*)AIController))
        {
            auto MyPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)AIController)->Pawn;

            if (MyPawn && BotTraceAvailable())
            {
                std::string Blocker;
                const bool bSees = BotStraightLineSees(MyPawn, Them, &Blocker);

                if (bSees)
                {
                    GSightByLineClear++;
                }
                else
                {
                    GSightByLineBlocked++;

                    if (GSightFirstBlocker.empty())
                        GSightFirstBlocker = Blocker;
                }

                static int Compared = 0;

                if (Available == 1 && Compared < 6)
                {
                    Compared++;

                    FVector ViewPoint = MyPawn->K2_GetActorLocation();
                    ViewPoint.Z = ViewPoint.Z + 60.0;

                    const bool bGameSays = AIController->LineOfSightTo(Them, ViewPoint, false);
                    auto MyState = (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)AIController)->PlayerState;

                    LogLine("[EYES] Sample %d: the straight line from '%s's eyes to '%s' %s%s -- the game's own check says \"%s\" "
                        "(the straight line decides).",
                        Compared, MyState ? SafeNameOf(MyState).c_str() : "a bot", BotPawnName((AFortPlayerPawnAthena*)Them).c_str(),
                        bSees ? "is clear" : "runs into ", bSees ? "" : Blocker.c_str(),
                        bGameSays ? "can see" : "cannot see");
                }

                return bSees;
            }
        }

        if (Available != 1 || !AIController || !Them)
            return true;

        FVector ViewPoint{};

        if (IsPlayerControllerBot((AActor*)AIController))
        {
            auto MyPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)AIController)->Pawn;

            if (MyPawn)
            {
                ViewPoint = MyPawn->K2_GetActorLocation();
                ViewPoint.Z = ViewPoint.Z + 60.0;
            }
        }

        return AIController->LineOfSightTo(Them, ViewPoint, false);
    }

    static bool BotGroundBelow(AFortPlayerPawnAthena* Pawn, double X, double Y, double FromZ, double ToZ, double& OutZ);
    static std::map<void*, std::pair<void*, int>> BotLastSaw;

    static bool BotCanSee(AAIController* AIController, AFortPlayerPawnAthena* Me, const FVector& MyLoc,
        AFortPlayerPawnAthena* Them, AActor* ThemActor, int TickNow, bool bStrict = false)
    {
        if (!Me || !Them)
            return false;

        const FVector ThemLoc = Them->K2_GetActorLocation();

        const bool bInFront = BotIsLookingAt(Me, MyLoc, ThemLoc);
        const bool bClearLine = bInFront && BotHasLineOfSight(AIController, ThemActor);

        if (bInFront && bClearLine)
        {
            BotLastSaw[(void*)Me] = { (void*)Them, TickNow };
            return true;
        }

        if (bStrict)
            return false;

        auto Remembered = BotLastSaw.find((void*)Me);

        if (Remembered != BotLastSaw.end() && Remembered->second.first == (void*)Them)
        {
            int Memory = FConfiguration::BotMemoryTicks;

            if (Memory < 0)
                Memory = 0;

            if ((TickNow - Remembered->second.second) <= Memory)
                return true;

            BotLastSaw.erase(Remembered);
        }

        return false;
    }

    static bool BotHasLanded(void* Pawn)
    {
        if (!Pawn)
            return false;
        if (!bBusDeparted)
            return false;
        if (MovedToIsland.find(Pawn) == MovedToIsland.end())
            return false;
        if (GlidingBots.find(Pawn) != GlidingBots.end())
            return false;

        return true;
    }

    static bool BotCombatIsOpen()
    {
        if (!bBusDeparted)
            return false;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return false;

        auto GS = (AFortGameStateAthena*)World->GameState;
        if (PlayersAreProtected(GS))
            return false;

        if (GS->HasGamePhase() && GS->GamePhase < GamePhase_SafeZones)
            return false;

        return true;
    }

    int SpawnBots(const FVector& Origin, int Count, float ScatterRadius, const std::wstring& SpawnerDataAssetPath)
    {
        (void)SpawnerDataAssetPath;
        if (Count > 100)
        {
            LogLine("[BotAI] Requested %d bots, capping at 100.\n", Count);
            Count = 100;
        }

        auto World = UWorld::GetWorld();
        if (!World)
        {
            LogLine("[BotAI] No world yet, can't spawn bots.\n");
            return 0;
        }

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        auto GameState = World->GameState ? (AFortGameStateAthena*)World->GameState : nullptr;
        if (!GameMode || !GameState)
        {
            LogLine("[BotAI] GameMode/GameState missing -- can't spawn bots.\n");
            return 0;
        }

        if (!GameMode->DefaultPawnClass)
        {
            LogLine("[BotAI] DefaultPawnClass is null -- too early.\n");
            return 0;
        }

        static auto PCClass = FindObject<UClass>(L"/Game/Athena/Athena_PlayerController.Athena_PlayerController_C");
        if (!PCClass)
            PCClass = FindClass("FortPlayerControllerAthena");
        if (!PCClass)
        {
            LogLine("[BotAI] Could not find Athena_PlayerController class.\n");
            return 0;
        }

        TArray<AActor*> PlayerStartsLocal;
        static auto WarmupStartClass = FindClass("FortPlayerStartWarmup");
        if (WarmupStartClass)
            UGameplayStatics::GetAllActorsOfClass(World, WarmupStartClass, &PlayerStartsLocal);

        static auto PlayerStartClass = FindClass("PlayerStart");
        if (PlayerStartsLocal.Num() == 0 && PlayerStartClass)
            UGameplayStatics::GetAllActorsOfClass(World, PlayerStartClass, &PlayerStartsLocal);

        int spawned = 0;
        for (int i = 0; i < Count; i++)
        {
            FVector SpawnLoc = Origin;
            if (PlayerStartsLocal.Num() > 0)
            {
                auto Start = PlayerStartsLocal[rand() % PlayerStartsLocal.Num()];
                if (Start)
                    SpawnLoc = Start->K2_GetActorLocation();
            }
            else if (ScatterRadius > 0.f)
            {
                SpawnLoc.X += ((float)(rand() % 2000) - 1000.f);
                SpawnLoc.Y += ((float)(rand() % 2000) - 1000.f);
            }

            FTransform Transform{};
            Transform.Translation = SpawnLoc;
            FVector OneScale(1.f, 1.f, 1.f);
            Transform.Scale3D = OneScale;

            auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GameMode->DefaultPawnClass, Transform);
            auto PC = (AFortPlayerControllerAthena*)UWorld::SpawnActor(PCClass, Transform);

            if (!PC || !Pawn)
            {
                if (Pawn)
                    Pawn->K2_DestroyActor();
                if (PC)
                    PC->K2_DestroyActor();
                LogLine("[BotAI] SpawnActor failed for bot %d (PC=%p Pawn=%p).\n", i, (void*)PC, (void*)Pawn);
                continue;
            }

            auto PlayerState = PC->PlayerState ? PC->PlayerState->Cast<AFortPlayerStateAthena>() : nullptr;
            if (!PlayerState)
            {
                PlayerState = (AFortPlayerStateAthena*)UWorld::SpawnActor(AFortPlayerStateAthena::StaticClass(), Transform);
                if (PlayerState)
                {
                    PlayerState->SetOwner(PC);
                    PC->PlayerState = PlayerState;
                    PC->OnRep_PlayerState();
                }
            }

            if (!PlayerState)
            {
                Pawn->K2_DestroyActor();
                PC->K2_DestroyActor();
                LogLine("[BotAI] No PlayerState for bot %d.\n", i);
                continue;
            }

            PC->Possess(Pawn);
            if (PC->HasMyFortPawn())
                PC->MyFortPawn = Pawn;

            if (Pawn->PlayerState != PlayerState)
            {
                Pawn->PlayerState = PlayerState;
                Pawn->OnRep_PlayerState();
            }

            if (PlayerState->HasbIsABot())
                PlayerState->bIsABot = true;

            MarkBotReadyLikeAPlayer(PC, PlayerState, true);

            Pawn->SetMaxHealth(100.f);
            Pawn->SetHealth(100.f);

            Pawn->SetShield(0.f);
            if (Pawn->HasbAlwaysRelevant())
                Pawn->bAlwaysRelevant = true;

            PutGliderInBotLoadout(Pawn, PickBotGlider());

            if (!PC->WorldInventory)
            {
                PC->WorldInventory = (AFortInventory*)UWorld::SpawnActor(AFortInventory::StaticClass(), Transform);
                if (PC->WorldInventory)
                {
                    PC->WorldInventory->SetOwner(PC);
                    if (PC->WorldInventory->HasInventoryType())
                        PC->WorldInventory->InventoryType = 0;
                }
            }
            if (PC->HasbHasInitializedWorldInventory())
                PC->bHasInitializedWorldInventory = true;



            const bool bLootpoolKit = kBotsSpawnWithGuns && PC->WorldInventory && GiveBotLootpoolKit(PC, PlayerState) > 0;

            static const wchar_t* KitPaths[] = {
                L"/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03",
                L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03",
                L"/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
                L"/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff",
                L"/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff",
                L"/Game/Athena/Items/Consumables/Grenade/Athena_Grenade.Athena_Grenade",
            };
            static const wchar_t* KitShort[] = {
                L"WID_Assault_AutoHigh_Athena_SR_Ore_T03",
                L"WID_Shotgun_Standard_Athena_C_Ore_T03",
                L"WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
                L"Athena_PurpleStuff",
                L"Athena_PurpleStuff",
                L"Athena_Grenade",
            };
            if (kBotsSpawnWithGuns && PC->WorldInventory && !bLootpoolKit)
            {
                for (int ki = 0; ki < 6; ki++)
                {
                    auto Def = FindObject<UFortItemDefinition>(KitPaths[ki]);
                    if (!Def)
                        Def = FindObject<UFortItemDefinition>(KitShort[ki]);
                    if (!Def)
                    {
                        std::wstring wn(KitShort[ki]);
                        std::string nn(wn.begin(), wn.end());
                        Def = TUObjectArray::FindObject<UFortItemDefinition>(nn.c_str());
                    }
                    if (!Def)
                    {
                        std::wstring wn(KitShort[ki]);
                        std::string nn(wn.begin(), wn.end());
                        std::string dn = nn + "." + nn;
                        Def = TUObjectArray::FindObject<UFortItemDefinition>(dn.c_str());
                    }
                    if (Def)
                    {
                        int clip = 1;
                        if (auto W = Def->Cast<UFortWeaponItemDefinition>())
                        {
                            if (auto Stats = AFortInventory::GetStats(W))
                                if (Stats->ClipSize > 0)
                                    clip = Stats->ClipSize;
                            PC->WorldInventory->GiveItem(Def, 1, clip);

                            GiveBotSpareRounds(PC->WorldInventory, Def);
                        }
                        else
                            PC->WorldInventory->GiveItem(Def, 1);
                    }
                }

                NoteFixedBotKit(PlayerState, "the loot tables gave no gun");
            }

            {
                GameState->PlayersLeft++;
                GameState->OnRep_PlayersLeft();
            }
            if (GameMode->HasAlivePlayers())
                GameMode->AlivePlayers.Add(PC);

            if (Pawn->HasbAlwaysRelevant())
                Pawn->bAlwaysRelevant = true;

            LoadoutGiven.insert((void*)PlayerState);
            spawned++;
        }

        LogLine("[BotAI] Magnesium-style spawn: %d/%d bot(s) created.\n", spawned, Count);
        return spawned;
    }

    static bool GameJumpRaw(AFortPlayerControllerAthena* Controller)
    {
        FRotator Facing{};
        Controller->ServerAttemptAircraftJump(Facing);
        return true;
    }

    static bool GameJumpGuarded(AFortPlayerControllerAthena* Controller)
    {
        __try
        {
            return GameJumpRaw(Controller);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GGameJumpBroken = false;
    static int GGameJumpTries = 0;
    static int GGameJumps = 0;
    static int GHandMadeBotPawns = 0;
    static int GThrownOutByBus = 0;
    static int GMarkedOffTheBus = 0;
    static int GNotMarkedOffTheBus = 0;
    static int GBrokenPawnsReplaced = 0;

    static float BusDropEndTime(AFortGameStateAthena* GameState)
    {
        if (!GameState)
            return -1.f;

        AFortAthenaAircraft* Aircraft = nullptr;

        if (GameState->HasAircrafts() && GameState->Aircrafts.Num() > 0)
            Aircraft = GameState->Aircrafts[0];
        if (!Aircraft && GameState->HasAircraft())
            Aircraft = GameState->Aircraft;

        if (!Aircraft || !Aircraft->HasDropEndTime())
            return -1.f;

        const float DropEnd = Aircraft->DropEndTime;
        return (DropEnd > 0.f) ? DropEnd : -1.f;
    }

    static float BusDropStartTime(AFortGameStateAthena* GameState)
    {
        if (!GameState)
            return -1.f;

        AFortAthenaAircraft* Aircraft = nullptr;

        if (GameState->HasAircrafts() && GameState->Aircrafts.Num() > 0)
            Aircraft = GameState->Aircrafts[0];
        if (!Aircraft && GameState->HasAircraft())
            Aircraft = GameState->Aircraft;

        if (!Aircraft || !Aircraft->HasDropStartTime())
            return -1.f;

        return Aircraft->DropStartTime;
    }

    static bool BusPosition(AFortGameStateAthena* GameState, FVector& Out)
    {
        if (!GameState)
            return false;

        AFortAthenaAircraft* Aircraft = nullptr;

        if (GameState->HasAircrafts() && GameState->Aircrafts.Num() > 0)
            Aircraft = GameState->Aircrafts[0];
        if (!Aircraft && GameState->HasAircraft())
            Aircraft = GameState->Aircraft;

        if (!Aircraft)
            return false;

        Out = ((AActor*)Aircraft)->K2_GetActorLocation();
        return (fabs(Out.X) + fabs(Out.Y)) > 1.0;
    }

    static std::map<void*, void*> BotPawnHealthChecked;
    static int GFreshPawnsHealed = 0;

    static void GiveFreshBotPawnHealth(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, const char* Where)
    {
        if (!Controller || !Pawn)
            return;

        auto& Checked = BotPawnHealthChecked[(void*)Controller];
        if (Checked == (void*)Pawn)
            return;

        Checked = (void*)Pawn;

        const float Before = Pawn->GetHealth();

        if (Before > 0.f || ReadBoolByName((UObject*)Pawn, "bIsDying") == 1)
            return;

        Pawn->SetMaxHealth(100.f);
        Pawn->SetHealth(100.f);

        GFreshPawnsHealed++;

        if (GFreshPawnsHealed <= 3 || (GFreshPawnsHealed % 25) == 0)
        {
            auto PlayerState = (AFortPlayerStateAthena*)Controller->PlayerState;

            LogLine("[BUS] '%s' got a pawn with %.0f health (%s) -- given 100, the health a bot spawns with. Health read back: %.0f "
                "(%d bot pawn(s) fixed so far).",
                PlayerState ? SafeNameOf(PlayerState).c_str() : "a bot", Before, Where, Pawn->GetHealth(), GFreshPawnsHealed);
        }
    }

    static void GiveFreshBotPawnsHealth(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (Pawn)
                GiveFreshBotPawnHealth(Controller, Pawn, "a pawn the game made");
            else
                BotPawnHealthChecked.erase((void*)Controller);
        }
    }

    struct FGoodBotPawn
    {
        AFortPlayerPawnAthena* Pawn = nullptr;
        FWeakObjectPtr Weak{};
        float LastHealth = 0.f;
    };

    static std::map<void*, FGoodBotPawn> GoodBotPawns;

    static void RememberGoodBotPawn(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        if (!Controller || !Pawn)
            return;

        FGoodBotPawn& Good = GoodBotPawns[(void*)Controller];
        Good.Pawn = Pawn;
        Good.Weak = FWeakObjectPtr((const UObject*)Pawn);
        Good.LastHealth = Pawn->GetHealth();
    }

    static bool BotPawnStillExists(const FGoodBotPawn& Good)
    {
        if (!Good.Pawn || Good.Weak.Get() != (const UObject*)Good.Pawn)
            return false;

        if (VersionInfo.EngineVersion < 5.0)
        {

            auto Item = TUObjectArray::GetItemByIndex(Good.Weak.ObjectIndex);

            if (!Item || (Item->Flags & (0x20000000 | 0x10000000)))
                return false;
        }

        return true;
    }

    static bool BotPawnIsUsable(const FGoodBotPawn& Good)
    {
        if (!BotPawnStillExists(Good))
            return false;

        if (ReadBoolByName((UObject*)Good.Pawn, "bIsDying") == 1)
            return false;

        return Good.Pawn->GetHealth() > 0.f;
    }

    static bool ActorIsLive(const UObject* Object);
    static std::string BotClassName(const UObject* Object);
    static bool BotLooksLikeDoor(AActor* Actor);

    static std::vector<AActor*> GChestCache;
    static float GChestCacheAt = -1000.f;
    static std::set<AActor*> GBadChests;

    // Every unopened chest in the level (refreshed every 20 s).
    static const std::vector<AActor*>& GetChests(UWorld* World)
    {
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        if (World && (Now - GChestCacheAt) > 20.f)
        {
            GChestCacheAt = Now;
            GChestCache.clear();

            static auto ContainerClass = FindClass("BuildingContainer");

            if (ContainerClass)
            {
                TArray<AActor*> All;
                UGameplayStatics::GetAllActorsOfClass(World, ContainerClass, &All);

                for (int i = 0; i < All.Num(); i++)
                {
                    AActor* Candidate = All[i];

                    if (!Candidate)
                        continue;

                    if (BotClassName((const UObject*)Candidate).find("Chest") == std::string::npos)
                        continue;

                    if (ReadBoolByName((const UObject*)Candidate, "bAlreadySearched") == 1)
                        continue;

                    GChestCache.push_back(Candidate);
                }
            }

            static int Reports = 0;
            if (++Reports <= 3)
                LogLine("[LOOT] Found %d unopened chest(s) in the level (%s).", (int)GChestCache.size(),
                    ContainerClass ? "BuildingContainer class found" : "BuildingContainer class NOT found");
        }

        return GChestCache;
    }

    static std::vector<FVector> LandingPlan;
    static size_t LandingPlanCursor = 0;
    static bool bPlanBuilt = false;

    static void BuildLandingPlan(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (bPlanBuilt || !World || !GameState)
            return;

        {
            bPlanBuilt = true;

            auto Normalize = [](std::string s) -> std::string
                {
                    std::string out;
                    for (char ch : s)
                    {
                        if (ch == ' ' || ch == '\'')
                            continue;
                        out += (char)tolower((unsigned char)ch);
                    }
                    return out;
                };

            TArray<AActor*> PoiVolumesRaw;
            static auto PoiVolumeClass = FindClass("FortPoiVolume");
            if (PoiVolumeClass)
                UGameplayStatics::GetAllActorsOfClass(World, PoiVolumeClass, &PoiVolumesRaw);

            LogLine("[BotAI] Found %d POI volume(s) in the level.\n", PoiVolumesRaw.Num());

            struct FoundPoi { FVector Loc; std::vector<std::string> Tags; bool bUsed; };
            std::vector<FoundPoi> FoundPois;
            for (int i = 0; i < PoiVolumesRaw.Num(); i++)
            {
                auto Poi = (AFortPoiVolume*)PoiVolumesRaw[i];
                if (!Poi)
                    continue;

                FoundPoi fp;
                fp.Loc = Poi->K2_GetActorLocation();
                fp.bUsed = false;

                if (Poi->HasLocationTags())
                {
                    for (int t = 0; t < Poi->LocationTags.GameplayTags.Num(); t++)
                    {
                        auto& Tag = Poi->LocationTags.GameplayTags.Get(t, FGameplayTag::Size());
                        auto WideStr = Tag.TagName.ToString();
                        std::string TagStr(WideStr.begin(), WideStr.end());
                        fp.Tags.push_back(Normalize(TagStr));
                    }
                }
                FoundPois.push_back(fp);
            }

            std::vector<FoundPoi*> RealPois;
            for (auto& fp : FoundPois)
            {
                for (auto& tag : fp.Tags)
                {
                    if (tag.find("location.poi.") != std::string::npos)
                    {
                        RealPois.push_back(&fp);

                        LogLine("[BotAI] Real POI: %-40s at X=%.0f Y=%.0f Z=%.0f\n",
                            tag.c_str(), (float)fp.Loc.X, (float)fp.Loc.Y, (float)fp.Loc.Z);
                        break;
                    }
                }
            }

            LogLine("[BotAI] %zu real POI volume(s) identified out of %zu total volumes.\n",
                RealPois.size(), FoundPois.size());

            std::vector<FVector> Plan;

            FVector BoundsOrigin{};
            FVector BoundsExtent{};
            bool bHaveBounds = false;

            if (GameState->HasMapInfo() && GameState->MapInfo && GameState->MapInfo->HasCachedPlayableBoundsForClients())
            {
                auto& B = GameState->MapInfo->CachedPlayableBoundsForClients;
                BoundsOrigin = B.Origin;
                BoundsExtent = B.BoxExtent;

                if (BoundsExtent.X > 1000.0 && BoundsExtent.Y > 1000.0)
                    bHaveBounds = true;
                LogLine("[BotAI] Playable bounds: origin X=%.0f Y=%.0f Z=%.0f extent X=%.0f Y=%.0f (usable=%s)\n",
                    (float)BoundsOrigin.X, (float)BoundsOrigin.Y, (float)BoundsOrigin.Z,
                    (float)BoundsExtent.X, (float)BoundsExtent.Y, bHaveBounds ? "yes" : "no");
            }
            else
            {
                LogLine("[BotAI] MapInfo/CachedPlayableBoundsForClients unavailable.\n");
            }

            if (!bHaveBounds)
            {
                BoundsOrigin = FVector(0, 0, 0);
                BoundsExtent = FVector(112000, 112000, 0);
                LogLine("[BotAI] Using fallback playable bounds +/-112000 (derived from level overlay extents).\n");
            }

            {
                int total = FConfiguration::AutoFillTargetPlayerCount;
                if (total < 1) total = 100;

                int cols = (int)ceil(sqrt((double)total));
                if (cols < 1) cols = 1;
                int rows = (total + cols - 1) / cols;

                double usableX = BoundsExtent.X * 1.55;
                double usableY = BoundsExtent.Y * 1.55;
                double startX = BoundsOrigin.X - usableX * 0.5;
                double startY = BoundsOrigin.Y - usableY * 0.5;
                double cellW = usableX / (double)cols;
                double cellH = usableY / (double)rows;

                for (int i = 0; i < total; i++)
                {
                    int cx = i % cols;
                    int cy = i / cols;

                    double jx = ((double)(rand() % 1000) / 1000.0) * cellW * 0.85 + cellW * 0.075;
                    double jy = ((double)(rand() % 1000) / 1000.0) * cellH * 0.85 + cellH * 0.075;

                    FVector Spot{};
                    Spot.X = startX + cx * cellW + jx;
                    Spot.Y = startY + cy * cellH + jy;
                    Spot.Z = 0.0;
                    Plan.push_back(Spot);
                }

                LogLine("[BotAI] Built %zu widely-spread landing spots on a %dx%d grid (usable %.0fx%.0f).\n",
                    Plan.size(), cols, rows, (float)usableX, (float)usableY);
            }

            {
                struct FDropArea { const char* Name; int Bots; float Radius; };

                // Names are matched against the POI tags in the log lines "[BotAI] Real POI: ...".
                // Lowercase, no spaces. Edit counts/names freely. The rest of the bots spread over the map.
                static const FDropArea Areas[] = {
                    { "pleasantpark", 10, 7000.f },
                    { "anarchyacres",  8, 7000.f },
                    { "lootlake",      8, 9000.f },
                    { "dustydepot",    8, 6000.f },
                    { "tomatotown",    8, 6000.f },
                    { "wailingwoods",  6, 9000.f },
                    { "lonelylodge",   5, 6000.f },
                    { "retailrow",     8, 6000.f },
                    { "saltysprings",  8, 6000.f },
                    { "fatalfields",   6, 7000.f },
                    { "moistymire",    6, 8000.f },
                    { "greasygrove",   6, 6000.f },
                    { "flushfactory",  5, 6000.f },
                };

                const size_t Reserve = FConfiguration::HotDropBots > 0 ? (size_t)FConfiguration::HotDropBots : 0;
                size_t FillFrom = Plan.size();

                for (const auto& Area : Areas)
                {
                    FVector Middle{};
                    bool bFound = false;
                    const std::string WantedTag = Normalize(Area.Name);

                    for (auto* Poi : RealPois)
                    {
                        for (auto& tag : Poi->Tags)
                        {
                            if (tag.find(WantedTag) == std::string::npos)
                                continue;

                            const double Away = sqrt((double)Poi->Loc.X * (double)Poi->Loc.X +
                                (double)Poi->Loc.Y * (double)Poi->Loc.Y);
                            if (Away < 1000.0)
                                continue;

                            Middle = Poi->Loc;
                            bFound = true;
                            break;
                        }

                        if (bFound)
                            break;
                    }

                    if (!bFound && PlaceDump::GetLandmark(Area.Name, Middle))
                        bFound = true;

                    if (!bFound)
                    {
                        LogLine("[DROP] Area \"%s\" was not found as a POI or a recorded place -- skipped.", Area.Name);
                        continue;
                    }

                    int Placed = 0;

                    while (Placed < Area.Bots && FillFrom > Reserve)
                    {
                        const double Angle = ((double)(rand() % 3600) / 10.0) * 3.14159265358979323846 / 180.0;
                        const double Away = (double)Area.Radius * ((double)(rand() % 1000) / 1000.0);

                        FVector Spot = Middle;
                        Spot.X += cos(Angle) * Away;
                        Spot.Y += sin(Angle) * Away;

                        Plan[--FillFrom] = Spot;
                        Placed++;
                    }

                    LogLine("[DROP] %d bot(s) will land at \"%s\" (X=%.0f Y=%.0f).", Placed, Area.Name, (float)Middle.X, (float)Middle.Y);
                }
            }

            {
                const char* Wanted = FConfiguration::HotDropPoi;
                int HowMany = FConfiguration::HotDropBots;

                if (HowMany > (int)Plan.size())
                    HowMany = (int)Plan.size();

                if (Wanted && *Wanted && HowMany > 0)
                {
                    FVector Middle{};
                    bool bFound = false;

                    const std::string WantedTag = Normalize(Wanted);

                    for (auto* Poi : RealPois)
                    {
                        for (auto& tag : Poi->Tags)
                        {
                            if (tag.find(WantedTag) == std::string::npos)
                                continue;

                            const double Away = sqrt((double)Poi->Loc.X * (double)Poi->Loc.X +
                                (double)Poi->Loc.Y * (double)Poi->Loc.Y);

                            if (Away < 1000.0)
                            {
                                LogLine("[BotAI] The level's \"%s\" volume says it is at the map origin, "
                                    "which cannot be right -- using the recording instead.", Wanted);
                                continue;
                            }

                            Middle = Poi->Loc;
                            bFound = true;
                            break;
                        }

                        if (bFound)
                            break;
                    }

                    if (bFound)
                        LogLine("[BotAI] Hot drop \"%s\" found as a POI volume at X=%.0f Y=%.0f.",
                            Wanted, (float)Middle.X, (float)Middle.Y);
                    else if (PlaceDump::GetLandmark(Wanted, Middle))
                    {
                        bFound = true;
                        LogLine("[BotAI] Hot drop \"%s\" taken from the recorded places at X=%.0f Y=%.0f.",
                            Wanted, (float)Middle.X, (float)Middle.Y);
                    }

                    if (!bFound)
                        LogLine("[BotAI] Hot drop \"%s\" is not a POI volume on this map and there is no "
                            "recording by that name -- everyone spreads out as usual.", Wanted);
                    else
                    {
                        std::vector<FVector> Spots((size_t)HowMany);

                        int Got = PlaceDump::GetPlacesNear(Middle, FConfiguration::HotDropRadius,
                            HowMany, Spots.data());

                        if (Got <= 0)
                        {
                            for (int i = 0; i < HowMany; i++)
                            {
                                const double Angle = ((double)(rand() % 3600) / 10.0) * 3.14159265358979323846 / 180.0;
                                const double Away = (double)FConfiguration::HotDropRadius * ((double)(rand() % 1000) / 1000.0);

                                Spots[(size_t)i] = Middle;
                                Spots[(size_t)i].X += cos(Angle) * Away;
                                Spots[(size_t)i].Y += sin(Angle) * Away;
                            }

                            Got = HowMany;
                        }

                        for (int i = 0; i < Got && i < (int)Plan.size(); i++)
                            Plan[(size_t)i] = Spots[(size_t)i];

                        LogLine("[BotAI] %d bot(s) are dropping on \"%s\"; the other %d spread out over the map.",
                            Got, Wanted, (int)Plan.size() - Got);
                    }
                }
            }

            // Spots in the wilderness (no chest within 60 m) are moved next to one of the 6 nearest chests.
            {
                const auto& Chests = GetChests(World);
                int Snapped = 0;

                if (!Chests.empty())
                {
                    for (auto& Spot : Plan)
                    {
                        std::vector<std::pair<double, AActor*>> Near;
                        bool bChestClose = false;

                        for (auto Chest : Chests)
                        {
                            if (!Chest)
                                continue;

                            const FVector At = Chest->K2_GetActorLocation();
                            const double Dx = At.X - Spot.X;
                            const double Dy = At.Y - Spot.Y;
                            const double D = Dx * Dx + Dy * Dy;

                            if (D < 6000.0 * 6000.0)
                            {
                                bChestClose = true;
                                break;
                            }

                            Near.push_back({ D, Chest });
                        }

                        if (bChestClose || Near.empty())
                            continue;

                        std::sort(Near.begin(), Near.end(), [](const std::pair<double, AActor*>& A, const std::pair<double, AActor*>& B)
                            { return A.first < B.first; });

                        const size_t Pick = (size_t)(rand() % (int)(Near.size() < 6 ? Near.size() : 6));
                        const FVector At = Near[Pick].second->K2_GetActorLocation();

                        Spot.X = At.X + (double)((rand() % 1600) - 800);
                        Spot.Y = At.Y + (double)((rand() % 1600) - 800);
                        Snapped++;
                    }
                }

                LogLine("[LOOT] %d landing spot(s) in the open were moved next to a chest (%zu chest(s) known).", Snapped, Chests.size());
            }

            // No two bots get the same landing spot: anything within 20 m of an earlier spot is moved a bit.
            {
                const double MinApart = 2000.0;
                int Nudged = 0;

                for (size_t i = 1; i < Plan.size(); i++)
                {
                    FVector Base = Plan[i];

                    for (int Try = 0; Try < 40; Try++)
                    {
                        bool bTooClose = false;

                        for (size_t j = 0; j < i; j++)
                        {
                            const double Ax = Plan[i].X - Plan[j].X;
                            const double Ay = Plan[i].Y - Plan[j].Y;

                            if (Ax * Ax + Ay * Ay < MinApart * MinApart)
                            {
                                bTooClose = true;
                                break;
                            }
                        }

                        if (!bTooClose)
                            break;

                        const double Angle = ((double)(rand() % 3600) / 10.0) * 3.14159265358979323846 / 180.0;
                        const double Away = MinApart + (double)(Try * 150) + (double)(rand() % 2000);

                        Plan[i] = Base;
                        Plan[i].X += cos(Angle) * Away;
                        Plan[i].Y += sin(Angle) * Away;

                        if (Try == 0)
                            Nudged++;
                    }
                }

                LogLine("[DROP] %d landing spot(s) were on top of another bot's and got moved apart.", Nudged);
            }

            for (size_t i = Plan.size(); i > 1; i--)
            {
                size_t j = (size_t)(rand() % (int)i);
                std::swap(Plan[i - 1], Plan[j]);
            }

            LandingPlan = Plan;
            LogLine("[BotAI] Landing plan built: %zu destinations.\n", LandingPlan.size());
        }
    }

    static std::vector<bool> PlanTaken;               // which landing spots are already given to a bot
    static std::map<void*, size_t> BotSpotIndex;      // which plan spot each bot owns

    static FVector NextLandingSpot(UWorld* World, AFortGameStateAthena* GameState, size_t* OutIndex = nullptr)
    {
        BuildLandingPlan(World, GameState);

        if (PlanTaken.size() != LandingPlan.size())
            PlanTaken.assign(LandingPlan.size(), false);

        FVector Dest{};

        if (!LandingPlan.empty())
        {
            const size_t Idx = LandingPlanCursor % LandingPlan.size();
            LandingPlanCursor++;

            PlanTaken[Idx] = true;
            Dest = LandingPlan[Idx];

            if (OutIndex)
                *OutIndex = Idx;
        }

        Dest.Z = 15500.0;
        return Dest;
    }

    static std::map<void*, FVector> BotLandingSpot;
    static std::map<void*, double> BotClosestToSpot;

    static FVector LandingSpotFor(UWorld* World, AFortGameStateAthena* GameState, void* Controller)
    {
        auto It = BotLandingSpot.find(Controller);

        if (It != BotLandingSpot.end())
            return It->second;

        size_t Idx = (size_t)-1;
        FVector Spot = NextLandingSpot(World, GameState, &Idx);

        if (Idx != (size_t)-1)
            BotSpotIndex[Controller] = Idx;

        BotLandingSpot.emplace(Controller, Spot);
        return Spot;
    }

    // A bot whose spot is out of reach swaps it for the free spot nearest to the bus.
    static bool ReassignToReachableSpot(void* Controller, const FVector& BusAt)
    {
        size_t Best = (size_t)-1;
        double BestDist = 1e18;

        for (size_t i = 0; i < LandingPlan.size() && i < PlanTaken.size(); i++)
        {
            if (PlanTaken[i])
                continue;

            const double Dx = LandingPlan[i].X - BusAt.X;
            const double Dy = LandingPlan[i].Y - BusAt.Y;
            const double D = Dx * Dx + Dy * Dy;

            if (D < BestDist)
            {
                BestDist = D;
                Best = i;
            }
        }

        if (Best == (size_t)-1)
            return false;

        auto Old = BotSpotIndex.find(Controller);
        if (Old != BotSpotIndex.end() && Old->second < PlanTaken.size())
            PlanTaken[Old->second] = false;

        PlanTaken[Best] = true;
        BotSpotIndex[Controller] = Best;

        FVector Spot = LandingPlan[Best];
        Spot.Z = 15500.0;
        BotLandingSpot[Controller] = Spot;
        return true;
    }

    static AFortPlayerPawnAthena* MakeBotPawnByHand(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState,
        const FVector& Where)
    {
        auto World = UWorld::GetWorld();
        auto GM = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
        if (!Controller || !GM || !GM->DefaultPawnClass)
            return nullptr;

        FTransform Tr{};
        FVector One(1.f, 1.f, 1.f);
        FVector SpawnAt(Where.X, Where.Y, Where.Z);
        Tr.Scale3D = One;
        Tr.Translation = SpawnAt;

        auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GM->DefaultPawnClass, Tr);
        if (!Pawn)
            return nullptr;

        Controller->Possess(Pawn);
        if (Controller->HasMyFortPawn())
            Controller->MyFortPawn = Pawn;
        Pawn->PlayerState = PlayerState;
        Pawn->OnRep_PlayerState();
        Pawn->SetMaxHealth(100.f);
        Pawn->SetHealth(100.f);
        Pawn->SetShield(0.f);

        GHandMadeBotPawns++;
        return Pawn;
    }

    static void SendBotToItsLandingSpot(UWorld* World, AFortPlayerPawnAthena* Pawn, FVector Dest)
    {
        if (!World || !Pawn)
            return;

        MovedToIsland.insert((void*)Pawn);

        FVector Before = Pawn->K2_GetActorLocation();

        bool bTeleported = Pawn->K2_TeleportTo(Dest, Pawn->K2_GetActorRotation());
        if (!bTeleported)
            Pawn->K2_SetActorLocation(Dest, false, (void*)nullptr, true);

        Pawn->BeginSkydiving(true);

        if (Pawn->HasbIsSkydivingFromBus() && !Pawn->bIsSkydivingFromBus)
            Pawn->bIsSkydivingFromBus = true;
        if (Pawn->HasbIsSkydiving() && !Pawn->bIsSkydiving)
        {
            Pawn->bIsSkydiving = true;
            bool bWasSkydiving = false;
            Pawn->OnRep_IsSkydiving(bWasSkydiving);
        }

        const UObject* Glider = GliderInBotLoadout(Pawn);
        const char* GliderLoadout = "in its loadout since it spawned";

        if (!Glider)
        {
            Glider = PickBotGlider();
            GliderLoadout = PutGliderInBotLoadout(Pawn, Glider);
        }

        if (Glider)
            BotGliderOf[(void*)Pawn] = Glider;

        FVector After = Pawn->K2_GetActorLocation();
        double movedDistSq = (After.X - Before.X) * (After.X - Before.X)
            + (After.Y - Before.Y) * (After.Y - Before.Y);

        if (LandingLogCount < 12)
        {
            LandingLogCount++;
            LogLine("[BotAI] Land#%d: target X=%.0f Y=%.0f Z=%.0f | teleport=%s | ended X=%.0f Y=%.0f Z=%.0f | moved %.0f units | skydiving=%s | glider=%s (%s) | health %.0f\n",
                LandingLogCount,
                (float)Dest.X, (float)Dest.Y, (float)Dest.Z,
                bTeleported ? "OK" : "REFUSED->forced",
                (float)After.X, (float)After.Y, (float)After.Z,
                (float)sqrt(movedDistSq),
                (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ? "yes" : "NO",
                Glider ? Glider->Name.ToString().c_str() : "default", GliderLoadout,
                Pawn->GetHealth());
        }

        const float DropTime = (float)UGameplayStatics::GetTimeSeconds(World);

        PendingParachuteOpen[(void*)Pawn] = DropTime;

        GlidingBots[(void*)Pawn] = (float)After.Z;
        GlidingSettledFor[(void*)Pawn] = 0;

        FBotGlide& Glide = BotGlides[(void*)Pawn];
        Glide = FBotGlide{};
        Glide.JumpTime = DropTime;
        Glide.JumpZ = (float)After.Z;
        Glide.LastDescentZ = (float)After.Z;
        Glide.LastDescentTime = DropTime;
    }

    static std::string DescribeBotPawn(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn)
            return std::string("no pawn at all");

        char Text[96];
        const FVector At = Pawn->K2_GetActorLocation();
        snprintf(Text, sizeof(Text), "a pawn with %.0f health at Z=%.0f", Pawn->GetHealth(), (float)At.Z);
        return std::string(Text);
    }

    static int GPawnSwapsUndone = 0;
    static int GPawnSwapsNotUndone = 0;
    static int GGamePawnsAdopted = 0;
    static int GLostPawnsRemade = 0;
    static int GBrokenPawnsSwappedIn = 0;
    static int GReplacementsFailed = 0;

    static void KeepBotsOnTheirOwnPawns(AFortGameStateAthena* GameState)
    {
        static bool bWindowClosed = false;

        if (bWindowClosed || !bBusDeparted || !GameState || !GameState->HasPlayerArray() || GoodBotPawns.empty())
            return;

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        static float FirstLook = -1.f;
        if (FirstLook < 0.f)
            FirstLook = Now;

        static float DropEnd = -1.f;
        if (DropEnd <= 0.f)
            DropEnd = BusDropEndTime(GameState);

        const float Until = (DropEnd > 0.f) ? DropEnd + 20.f : FirstLook + 120.f;

        if (Now > Until)
        {

            bWindowClosed = true;

            {
                int StillOnBoard = 0;
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                        continue;

                    auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                    if (PC && IsPlayerControllerBot((AActor*)PC) && PC->IsInAircraft())
                        StillOnBoard++;
                }

                LogLine("[BUS] The bus is gone (bot pawns watched until %.0fs): the game moved a bot off its own pawn %d time(s) -- put "
                    "back on it %d time(s), could NOT be put back %d time(s) | pawns the game gave a bot that it kept: %d | given a "
                    "new pawn by hand after the game took theirs: %d (left on a broken pawn: %d, left with none: %d) | could not be "
                    "given one: %d | bots the game still has on board: %d.",
                    Until, GPawnSwapsUndone + GPawnSwapsNotUndone, GPawnSwapsUndone, GPawnSwapsNotUndone, GGamePawnsAdopted,
                    GBrokenPawnsSwappedIn + GLostPawnsRemade, GBrokenPawnsSwappedIn, GLostPawnsRemade, GReplacementsFailed,
                    StillOnBoard);
            }

            return;
        }

        static int SnapshotsTaken = 0;

        if (DropEnd > 0.f && SnapshotsTaken < 2 && Now >= DropEnd + (SnapshotsTaken == 0 ? 1.f : 5.f))
        {
            SnapshotsTaken++;

            int OnOwn = 0, OnOther = 0, OnNone = 0, Aboard = 0, NotOffYet = 0;

            for (int i = 0; i < GameState->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                    continue;

                auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                if (!PC || !IsPlayerControllerBot((AActor*)PC) || EliminatedBots.find((void*)PC) != EliminatedBots.end())
                    continue;

                if (PC->IsInAircraft())
                    Aboard++;

                auto Own = GoodBotPawns.find((void*)PC);
                auto P = (AFortPlayerPawnAthena*)PC->Pawn;

                if (Own == GoodBotPawns.end())
                    NotOffYet++;
                else if (!P)
                    OnNone++;
                else if (P == Own->second.Pawn)
                    OnOwn++;
                else
                    OnOther++;
            }

            LogLine("[BUS] %.0fs after the end of the bus's drop zone (%.0fs): bots on their own pawn %d, on another pawn %d, with no pawn "
                "%d, never got off %d | bots the game still has on board: %d | put right so far: %d put back on their own pawn, %d "
                "kept the pawn the game gave them, %d given a new one.",
                Now - DropEnd, DropEnd, OnOwn, OnOther, OnNone, NotOffYet, Aboard,
                GPawnSwapsUndone, GGamePawnsAdopted, GBrokenPawnsSwappedIn + GLostPawnsRemade);
        }

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto GoodIt = GoodBotPawns.find((void*)Controller);
            if (GoodIt == GoodBotPawns.end())
                continue;

            FGoodBotPawn& Good = GoodIt->second;
            auto Current = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Current && Current == Good.Pawn)
            {
                if (BotPawnStillExists(Good))
                    Good.LastHealth = Current->GetHealth();

                continue;
            }

            if (BotPawnIsUsable(Good))
            {
                const std::string Other = DescribeBotPawn(Current);

                Controller->Possess(Good.Pawn);
                if (Controller->HasMyFortPawn())
                    Controller->MyFortPawn = Good.Pawn;

                if ((void*)Good.Pawn->PlayerState != (void*)PlayerState)
                {
                    Good.Pawn->PlayerState = PlayerState;
                    Good.Pawn->OnRep_PlayerState();
                }

                const bool bBack = (void*)Controller->Pawn == (void*)Good.Pawn;
                bool bRemoved = false;

                if (bBack && Current && Current != Good.Pawn && !Current->Controller)
                {
                    Current->K2_DestroyActor();
                    bRemoved = true;
                }

                const char* Why = "it was not on board";
                if (bBack && Controller->IsInAircraft())
                    MarkBotOffTheBus(Controller, &Why);

                if (bBack)
                {
                    GPawnSwapsUndone++;
                    BotBeingFixedUntil.erase((void*)Controller);
                }
                else
                {
                    GPawnSwapsNotUndone++;
                }

                const int Swaps = GPawnSwapsUndone + GPawnSwapsNotUndone;

                if (Swaps <= 5 || (Swaps % 25) == 0 || (!bBack && GPawnSwapsNotUndone <= 5))
                    LogLine("[BUS] The game moved '%s' off its own pawn, onto %s -- %s (its own pawn: %.0f health at Z=%.0f)%s | "
                        "the game says it is on the bus: %s (%s) | %d time(s) so far.",
                        SafeNameOf(PlayerState).c_str(), Other.c_str(),
                        bBack ? "put back on its own pawn" : "could NOT be put back on its own pawn",
                        Good.Pawn->GetHealth(), (float)Good.Pawn->K2_GetActorLocation().Z,
                        bRemoved ? ", the other pawn removed" : "",
                        Controller->IsInAircraft() ? "YES" : "no", Why, Swaps);

                Good.LastHealth = Good.Pawn->GetHealth();
                continue;
            }

            if (BotPawnStillExists(Good))
            {
                Good.LastHealth = 0.f;
                continue;
            }

            if (!(Good.LastHealth > 0.f))
                continue;

            {
                auto Mode = (AFortGameMode*)World->AuthorityGameMode;
                bool bStillIn = false;

                if (Mode && Mode->HasAlivePlayers())
                    for (int a = 0; a < Mode->AlivePlayers.Num(); a++)
                        if ((void*)Mode->AlivePlayers[a] == (void*)Controller)
                        {
                            bStillIn = true;
                            break;
                        }

                if (!bStillIn)
                {
                    Good.LastHealth = 0.f;
                    continue;
                }
            }

            const std::string Was = DescribeBotPawn(Current);

            if (Current)
            {
                if (!(Current->GetHealth() > 0.f) && ReadBoolByName((UObject*)Current, "bIsDying") != 1)
                {
                    Current->SetMaxHealth(100.f);
                    Current->SetHealth(100.f);
                }

                if (Current->GetHealth() > 0.f)
                {
                    RememberGoodBotPawn(Controller, Current);

                    const bool bSent = MovedToIsland.find((void*)Current) == MovedToIsland.end();
                    if (bSent)
                        SendBotToItsLandingSpot(World, Current, LandingSpotFor(World, GameState, (void*)Controller));

                    GGamePawnsAdopted++;
                    BotBeingFixedUntil.erase((void*)Controller);

                    if (GGamePawnsAdopted <= 5 || (GGamePawnsAdopted % 25) == 0)
                        LogLine("[BUS] The game took '%s's own pawn away and gave it %s -- kept (health now %.0f)%s (%d so far).",
                            SafeNameOf(PlayerState).c_str(), Was.c_str(), Current->GetHealth(),
                            bSent ? " and sent to its landing spot" : ", left where it is", GGamePawnsAdopted);

                    continue;
                }
            }

            const FVector Spot = LandingSpotFor(World, GameState, (void*)Controller);
            auto Fresh = MakeBotPawnByHand(Controller, PlayerState, Spot);

            if (!Fresh)
            {

                BotBeingFixedUntil[(void*)Controller] = Now + 3.f;
                GReplacementsFailed++;

                if (GReplacementsFailed <= 3)
                    LogLine("[BUS] '%s' lost its own pawn and has %s -- making it a new one by hand FAILED; tried again next tick "
                        "(it counts as alive for 3 s).", SafeNameOf(PlayerState).c_str(), Was.c_str());

                continue;
            }

            bool bRemoved = false;
            if (Current && Current != Fresh && !Current->Controller)
            {
                Current->K2_DestroyActor();
                bRemoved = true;
            }

            RememberGoodBotPawn(Controller, Fresh);
            SendBotToItsLandingSpot(World, Fresh, Spot);
            BotBeingFixedUntil.erase((void*)Controller);

            const char* Why = "it was not on board";
            if (Controller->IsInAircraft())
                MarkBotOffTheBus(Controller, &Why);

            if (Current)
                GBrokenPawnsSwappedIn++;
            else
                GLostPawnsRemade++;

            if ((GBrokenPawnsSwappedIn + GLostPawnsRemade) <= 5 || ((GBrokenPawnsSwappedIn + GLostPawnsRemade) % 25) == 0)
                LogLine("[BUS] The game took '%s's own pawn away and left it with %s -- given a new pawn by hand at its landing spot "
                    "(health read back: %.0f)%s | the game says it is on the bus: %s (%s).",
                    SafeNameOf(PlayerState).c_str(), Was.c_str(), Fresh->GetHealth(), bRemoved ? ", the broken pawn removed" : "",
                    Controller->IsInAircraft() ? "YES" : "no", Why);
        }
    }

    bool MoveSomeBotsToIsland(int MaxThisTick)
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return true;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return true;

        BuildLandingPlan(World, GameState);

        if (LandingPlan.empty())
        {
            LogLine("[BotAI] Landing plan is empty -- can't place bots on the island.\n");
            return true;
        }

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        const float DropStart = BusDropStartTime(GameState);
        const float DropEnd = BusDropEndTime(GameState);

        if (DropStart > 0.f && Now < DropStart)
        {
            static bool bSaidSo = false;

            if (!bSaidSo)
            {
                bSaidSo = true;
                LogLine("[BUS] Bots get off when the doors open: the drop zone runs from %.0fs to %.0fs (now %.0fs).",
                    DropStart, DropEnd, Now);
            }

            return false;
        }

        int movedThisTick = 0;
        bool anyRemaining = false;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            if (movedThisTick >= MaxThisTick)
            {
                anyRemaining = true;
                break;
            }

            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (Pawn && MovedToIsland.find((void*)Pawn) != MovedToIsland.end())
                continue;

            FVector BusAt{};
            const bool bHaveBus = BusPosition(GameState, BusAt);

            if (bHaveBus)
            {
                const FVector Spot = LandingSpotFor(World, GameState, (void*)Controller);
                const double Gx = Spot.X - BusAt.X;
                const double Gy = Spot.Y - BusAt.Y;
                const double ToSpot = sqrt(Gx * Gx + Gy * Gy);

                const double kReachPerHeight = 1.0;
                const double Reach = (BusAt.Z > 3000.0 ? BusAt.Z : 15000.0) * kReachPerHeight;
                const double Range = Reach * 0.6 + (double)(BotPersonalSeed((void*)Controller) % (uint32)(Reach * 0.3));

                auto Closest = BotClosestToSpot.find((void*)Controller);
                const bool bBusMovingAway = Closest != BotClosestToSpot.end() && ToSpot > Closest->second + 800.0;

                if (Closest == BotClosestToSpot.end() || ToSpot < Closest->second)
                    BotClosestToSpot[(void*)Controller] = ToSpot;

                const bool bLastChance = bBusMovingAway || ((DropEnd > 0.f) ? (Now >= DropEnd - 2.f)
                    : (DropStart > 0.f && Now >= DropStart + 90.f));

                const float MyFraction = (float)(BotPersonalSeed((void*)Controller) % 1000) / 1000.f;
                const bool bMyTime = (ToSpot > Reach) && DropStart > 0.f && DropEnd > DropStart &&
                    Now >= DropStart + MyFraction * (DropEnd - DropStart);

                if (ToSpot > Range && !bLastChance && !bMyTime)
                {
                    if (Pawn)
                        Pawn->K2_SetActorLocation(BusAt, false, (void*)nullptr, true);

                    anyRemaining = true;
                    continue;
                }
            }   // end of: if (bHaveBus)

            const bool bThrownOut = Pawn != nullptr;
            bool bJumpedItself = false;
            bool bHandMade = false;

            if (!Pawn)
            {
                const bool bWasAboard = Controller->IsInAircraft();

                static std::set<void*> ReadiedForTheJump;
                if (ReadiedForTheJump.insert((void*)Controller).second)
                    MarkBotReadyLikeAPlayer(Controller, PlayerState, false);

                if (bWasAboard && !GGameJumpBroken && (GGameJumps > 0 || GGameJumpTries < 3))
                {
                    GGameJumpTries++;

                    if (!GameJumpGuarded(Controller))
                    {
                        GGameJumpBroken = true;
                        LogLine("[BUS] The game's own aircraft jump faulted on a bot -- from now on bots get their pawn made by hand.");
                    }

                    Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

                    if (Pawn)
                    {
                        bJumpedItself = true;
                        GGameJumps++;

                        if (GGameJumps <= 3 || (GGameJumps % 25) == 0)
                            LogLine("[BUS] '%s' jumped out of the bus with the game's own jump (%d so far) | still on board after: %s | "
                                "came out at Z=%.0f with %.0f health.",
                                SafeNameOf(PlayerState).c_str(), GGameJumps, Controller->IsInAircraft() ? "YES" : "no",
                                (float)Pawn->K2_GetActorLocation().Z, Pawn->GetHealth());
                    }
                    else
                    {
                        LogLine("[BUS] The game's own jump did not let '%s' off the bus (try %d of 3) | on board: yes | bus doors locked: %s | "
                            "drop zone %.0fs to %.0fs, now %.0fs | loading flags: client %d, server %d, ready %d.",
                            SafeNameOf(PlayerState).c_str(), GGameJumpTries,
                            ReadBoolByName((UObject*)GameState, "bAircraftIsLocked") == 1 ? "YES"
                            : (ReadBoolByName((UObject*)GameState, "bAircraftIsLocked") == 0 ? "no" : "?"),
                            DropStart, DropEnd, Now,
                            ReadBoolByName((UObject*)Controller, "bHasClientFinishedLoading"),
                            ReadBoolByName((UObject*)Controller, "bHasServerFinishedLoading"),
                            ReadBoolByName((UObject*)Controller, "bReadyToStartMatch"));

                        if (GGameJumpTries == 3 && GGameJumps == 0)
                            LogLine("[BUS] The game's own jump turned down the first 3 bots, as it did all of them in the last two logs -- "
                                "every bot now gets its pawn made by hand at its landing spot and is marked off the bus.");
                    }
                }
            }

            FVector Dest = LandingSpotFor(World, GameState, (void*)Controller);

            if (bHaveBus)
            {
                const uint32 S = BotPersonalSeed((void*)Controller);
                const FVector Spot = Dest;
                const double Reach = (BusAt.Z > 3000.0 ? BusAt.Z : 15000.0);
                const double kLeadFraction = 0.7;
                const double Lead = Reach * kLeadFraction;

                const double Bx = BusAt.X - Spot.X;
                const double By = BusAt.Y - Spot.Y;
                const double BLen = sqrt(Bx * Bx + By * By);

                if (BLen > Lead && BLen > 1.0)
                {
                    Dest = FVector(Spot.X + Bx / BLen * Lead, Spot.Y + By / BLen * Lead, BusAt.Z);

                    static int StagedLines = 0;
                    if (++StagedLines <= 10)
                        LogLine("[DROP] '%s' is %.0f m from its spot -- it leaves the bus %.0f m short of it so it can glide there.",
                            SafeNameOf(PlayerState).c_str(), (float)(BLen / 100.0), (float)(Lead / 100.0));
                }
                else
                {
                    Dest = BusAt;
                }

                Dest.X += (double)((int)(S % 600) - 300);
                Dest.Y += (double)((int)((S >> 10) % 600) - 300);
            }

            if (!Pawn)
            {
                Pawn = MakeBotPawnByHand(Controller, PlayerState, Dest);

                if (!Pawn)
                {
                    static int Failures = 0;
                    if (++Failures <= 3)
                        LogLine("[BUS] Making a pawn by hand for '%s' FAILED -- tried again next tick.", SafeNameOf(PlayerState).c_str());

                    anyRemaining = true;
                    continue;
                }

                bHandMade = true;
            }
            else if (bThrownOut)
            {
                GThrownOutByBus++;

                if (GThrownOutByBus <= 3 || (GThrownOutByBus % 25) == 0)
                {
                    const FVector Where = Pawn->K2_GetActorLocation();

                    LogLine("[BUS] '%s' already had a pawn when it got off (it kept its pawn from the island, or the game let it out "
                        "itself): at X=%.0f Y=%.0f Z=%.0f with %.0f health -- moved to its landing spot (%d so far).",
                        SafeNameOf(PlayerState).c_str(), (float)Where.X, (float)Where.Y, (float)Where.Z, Pawn->GetHealth(), GThrownOutByBus);
                }
            }

            if (!bHandMade)
            {
                GiveFreshBotPawnHealth(Controller, Pawn, bJumpedItself ? "off the bus with the game's own jump" : "a pawn it already had");

                if (!(Pawn->GetHealth() > 0.f) && ReadBoolByName((UObject*)Pawn, "bIsDying") != 1)
                {
                    auto Broken = Pawn;
                    auto Fresh = MakeBotPawnByHand(Controller, PlayerState, Dest);

                    if (!Fresh)
                    {
                        BotBeingFixedUntil[(void*)Controller] = Now + 3.f;

                        static int FixFailures = 0;
                        if (++FixFailures <= 3)
                            LogLine("[BUS] '%s' is on a pawn with no health that will not take any, and making it a new one by hand "
                                "FAILED -- tried again next tick (it counts as alive for 3 s).", SafeNameOf(PlayerState).c_str());

                        anyRemaining = true;
                        continue;
                    }

                    {
                        const bool bRemoved = Broken != Fresh && !Broken->Controller;
                        if (bRemoved)
                            Broken->K2_DestroyActor();

                        Pawn = Fresh;
                        bHandMade = true;
                        GBrokenPawnsReplaced++;

                        if (GBrokenPawnsReplaced <= 3 || (GBrokenPawnsReplaced % 25) == 0)
                            LogLine("[BUS] The pawn the game gave '%s' has no health and will not take any -- replaced by a pawn made by "
                                "hand at its landing spot (health read back: %.0f)%s (%d so far).",
                                SafeNameOf(PlayerState).c_str(), Fresh->GetHealth(), bRemoved ? ", the broken one removed" : "",
                                GBrokenPawnsReplaced);
                    }
                }
            }

            const char* Why = "it was not on board";
            bool bOffTheBus = !Controller->IsInAircraft();

            if (!bOffTheBus)
            {
                bOffTheBus = MarkBotOffTheBus(Controller, &Why);

                if (bOffTheBus)
                    GMarkedOffTheBus++;
                else
                    GNotMarkedOffTheBus++;
            }

            if (bHandMade && (GHandMadeBotPawns <= 3 || (GHandMadeBotPawns % 25) == 0))
                LogLine("[BUS] '%s' got its pawn by hand at its landing spot (X=%.0f Y=%.0f Z=%.0f, %.0f health; %d so far) | off the bus "
                    "as far as the game is concerned: %s (%s).",
                    SafeNameOf(PlayerState).c_str(), (float)Dest.X, (float)Dest.Y, (float)Dest.Z, Pawn->GetHealth(),
                    GHandMadeBotPawns, bOffTheBus ? "yes" : "NO", Why);
            else if (!bOffTheBus && GNotMarkedOffTheBus <= 3)
                LogLine("[BUS] '%s' could not be marked off the bus (%s) -- if the bus throws it out at the end of its drop zone, it is "
                    "put back on its own pawn.", SafeNameOf(PlayerState).c_str(), Why);

            if (Pawn->GetHealth() > 0.f)
            {
                RememberGoodBotPawn(Controller, Pawn);
            }
            else
            {
                static int NoHealthReports = 0;
                if (++NoHealthReports <= 3)
                    LogLine("[BUS] '%s's pawn reads %.0f health after it got off the bus (%s) -- it is sent to its landing spot anyway, "
                        "and will be counted as dead.", SafeNameOf(PlayerState).c_str(), Pawn->GetHealth(),
                        bHandMade ? "made by hand" : "made by the game");
            }

            SendBotToItsLandingSpot(World, Pawn, Dest);
            movedThisTick++;
        }   // end of: for loop

        if (!anyRemaining)
        {
            static bool bSummarized = false;

            if (!bSummarized)
            {
                bSummarized = true;

                int StillOnBoard = 0, WithHealth = 0;
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                        continue;

                    auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                    if (!PC || !IsPlayerControllerBot((AActor*)PC))
                        continue;

                    if (PC->IsInAircraft())
                        StillOnBoard++;

                    auto P = (AFortPlayerPawnAthena*)PC->Pawn;
                    if (P && P->GetHealth() > 0.f)
                        WithHealth++;
                }

                LogLine("[BUS] Every bot is off the bus (%.0fs, the drop zone ends at %.0fs): %d with the game's own jump, %d already on "
                    "a pawn, %d given a pawn by hand (%d of those because the game's pawn had no health) | marked off the bus: %d, "
                    "could not be: %d (bytes written %d, refused %d) | bots the game still has on board: %d | bots on a pawn with "
                    "health: %d.",
                    Now, DropEnd, GGameJumps, GThrownOutByBus, GHandMadeBotPawns, GBrokenPawnsReplaced, GMarkedOffTheBus,
                    GNotMarkedOffTheBus, GBusExitLatch.Written, GBusExitLatch.Refused, StillOnBoard, WithHealth);
            }
        }

        return !anyRemaining;
    }

    void ThankAllBots()
    {
        if (!bBusDeparted)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        if ((Now - LastThankTime) < 1.0f)
            return;
        LastThankTime = Now;

        int thanksThisPass = 3;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AActor*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (ThankedBots.find((void*)Controller) != ThankedBots.end())
                continue;
            ThankedBots.insert((void*)Controller);

            if (PlayerState->HasbThankedBusDriver())
            {
                PlayerState->bThankedBusDriver = true;
                PlayerState->OnRep_ThankedBusDriver();

                {
                    const std::string Narrow = SafeNameOf(PlayerState);

                    if (!Narrow.empty())
                        LogLine("[BUS] %s thanked the bus driver.", Narrow.c_str());
                }
            }

            if (--thanksThisPass <= 0)
                break;
        }
    }

    void LogHudState()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray())
            return;

        static float LastHudLog = -1000.f;
        float Now = (float)UGameplayStatics::GetTimeSeconds(World);
        if ((Now - LastHudLog) < 5.0f)
            return;
        LastHudLog = Now;

        int trueAliveBots = 0;
        int trueAliveHumans = 0;
        int botsWithPawn = 0;
        int botsDeadPawn = 0;
        int onTheBus = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS) continue;
            bool bIsBot = PS->HasbIsABot() && PS->bIsABot;

            auto OwnerActor = (AActor*)PS->Owner;
            AFortPlayerPawnAthena* Pw = nullptr;
            if (OwnerActor)
                Pw = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)OwnerActor)->Pawn;

            bool bPawnless = false;
            bool bAlive = PlayerIsStillIn(GameMode, PS, &bPawnless);

            if (bAlive && bPawnless)
                onTheBus++;

            if (bIsBot)
            {
                if (Pw) botsWithPawn++; else botsDeadPawn++;
                if (bAlive) trueAliveBots++;
            }
            else if (bAlive)
            {
                trueAliveHumans++;
            }
        }

        int hudPlayersLeft = GameState->HasPlayersLeft() ? (int)GameState->PlayersLeft : -1;
        int hudTeamsLeft = GameState->HasTeamsLeft() ? (int)GameState->TeamsLeft : -1;
        int aliveArrayNum = GameMode->HasAlivePlayers() ? GameMode->AlivePlayers.Num() : -1;
        int trueAlive = trueAliveBots + trueAliveHumans;

        LogLine("[HUD] PlayersLeft(shown)=%d TeamsLeft(shown)=%d | TRUE alive=%d (bots=%d humans=%d, %d of them with no pawn yet -- on the bus or not spawned) | AlivePlayers[]=%d PlayerArray=%d | bots w/pawn=%d w/o=%d%s\n",
            hudPlayersLeft, hudTeamsLeft, trueAlive, trueAliveBots, trueAliveHumans, onTheBus,
            aliveArrayNum, GameState->PlayerArray.Num(), botsWithPawn, botsDeadPawn,
            (hudPlayersLeft != trueAlive) ? "  <<< MISMATCH: HUD disagrees with reality" : "");

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS) continue;
            bool bIsBot = PS->HasbIsABot() && PS->bIsABot;

            int kills = PS->HasKillScore() ? (int)PS->KillScore : -1;
            int killScore = PS->HasKillScore() ? (int)PS->KillScore : -1;

            if (!bIsBot)
            {
                const std::string nm = SafeNameOf(PS);
                LogLine("[HUD] Human '%s': Kills(shown)=%d KillScore=%d\n", nm.c_str(), kills, killScore);
            }
            else if (kills > 0)
            {
                LogLine("[HUD] Bot has %d kill(s) (KillScore=%d)\n", kills, killScore);
            }
        }
    }

    void UpdateHudCounts()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray() || !GameState->HasPlayersLeft())
            return;

        auto GameMode = CurrentGameMode();

        int trueAlive = 0;
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            if (PlayerIsStillIn(GameMode, PS))
                trueAlive++;
        }

        if (trueAlive <= 0)
            return;

        if ((int)GameState->PlayersLeft != trueAlive)
        {
            int before = (int)GameState->PlayersLeft;
            GameState->PlayersLeft = trueAlive;
            GameState->OnRep_PlayersLeft();
            if (GameState->HasTeamsLeft())
                GameState->TeamsLeft = trueAlive;

            LogLine("[HUD] PlayersLeft corrected %d -> %d (live recount).\n", before, trueAlive);
        }
    }

    static void NoOpExecFunction(void*, void*, void*)
    {
        static int Calls = 0;

        if (++Calls <= 5)
        {
            printf("[Storm] A silenced distant-storm function was called (%d) -- did nothing, as intended.\n", Calls);
            fflush(stdout);
        }
    }

    static void SilenceDistantStorm()
    {
        static bool bDone = false;
        if (bDone)
            return;

        static int Countdown = 0;
        static int Attempts = 0;

        if (Countdown-- > 0)
            return;

        Countdown = 15;

        if (++Attempts > 240)
        {
            bDone = true;
            printf("[SafeZone] Never found the safe zone indicator class -- distant storm left as it is.\n");
            fflush(stdout);
            return;
        }

        const UStruct* IndicatorClass = (const UStruct*)FindClass("SafeZoneIndicator_C");

        if (!IndicatorClass)
        {
            const int Total = TUObjectArray::Num();

            for (int i = 0; i < Total; i++)
            {
                auto Obj = TUObjectArray::GetObjectByIndex(i);

                if (!Obj || !Obj->Class || !(Obj->Class->GetCastFlags() & 0x20))
                    continue;

                auto RawName = Obj->Name.ToString();
                std::string Lowered;
                for (const char* p = RawName.c_str(); *p; p++)
                    Lowered += (char)tolower((unsigned char)*p);

                if (Lowered.find("safezoneindicator") != std::string::npos)
                {
                    IndicatorClass = (const UStruct*)Obj;
                    break;
                }
            }
        }

        if (!IndicatorClass)
            return;

        bDone = true;

        int Silenced = 0;

        for (const UStruct* Cls = IndicatorClass; Cls; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child; Child = Child->GetNext())
            {
                if (!Child->Class || !(Child->Class->GetCastFlags() & 0x80000))
                    continue;

                auto RawName = Child->GetName().ToString();
                std::string Lowered;
                for (const char* p = RawName.c_str(); *p; p++)
                    Lowered += (char)tolower((unsigned char)*p);

                if (Lowered.find("distant") == std::string::npos && Lowered.find("oneshot") == std::string::npos)
                    continue;

                Hooking::ExecHook((UFunction*)Child, (void*)&NoOpExecFunction);
                Silenced++;

                printf("[SafeZone] Silenced cosmetic storm function \"%s\".\n", RawName.c_str());
            }
        }

        if (Silenced == 0)
            printf("[SafeZone] Found the indicator class but no distant-storm functions on it.\n");
        else
            printf("[SafeZone] %d distant-storm function(s) silenced -- that is the one that crashes.\n", Silenced);

        fflush(stdout);
    }

    static const float kZoneWait[] = {
        170.f,
        120.f,
         90.f,
         80.f,
         50.f,
         30.f,
          0.f,
          0.f,
          0.f,
         10.f,
    };
    static const float kZoneShrink[] = {
        210.f,
        120.f,
         90.f,
         70.f,
         60.f,
         60.f,
         55.f,
         50.f,
         75.f,
         10.f,
    };
    static const int kZoneCount = (int)(sizeof(kZoneWait) / sizeof(kZoneWait[0]));

    static void ApplyCustomZoneTimings(AFortGameMode* GameMode)
    {
        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;
        if (!Indicator->HasSafeZonePhases())
            return;

        auto& Phases = Indicator->SafeZonePhases;
        const int N = Phases.Num();
        if (N <= 0)
            return;

        static bool bLogged = false;
        for (int i = 0; i < N; i++)
        {

            int idx = i;
            if (idx >= kZoneCount)
                idx = kZoneCount - 1;

            auto& Phase = Phases.Get(i, FFortSafeZonePhaseInfo::Size());
            if (FFortSafeZonePhaseInfo::HasWaitTime())
            {
                float W = kZoneWait[idx];
                Phase.WaitTime = W;
            }
            if (FFortSafeZonePhaseInfo::HasShrinkTime())
            {
                float S = kZoneShrink[idx];
                Phase.ShrinkTime = S;
            }
        }

        if (!bLogged)
        {
            bLogged = true;
            LogLine("[SafeZone] Applied custom phase timings (%d phases).", N);
        }
    }

    static void EnsureStormTimingData(AFortGameStateAthena* GameState)
    {
        static bool bDone = false;

        if (bDone || !GameState || !GameState->HasMapInfo() || !GameState->MapInfo)
            return;

        auto MapInfo = GameState->MapInfo;

        if (!MapInfo->HasSafeZoneDefinition())
            return;

        bDone = true;

        auto& Definition = MapInfo->SafeZoneDefinition;

        if (FFortSafeZoneDefinition::HasWaitTime())
        {
            auto& WaitTime = Definition.WaitTime;
            WaitTime.Value = kZoneWait[0];
            WaitTime.Curve.CurveTable = nullptr;
            GStormMapDataWasRepaired = true;
            printf("[SafeZone] Definition WaitTime forced to %.0fs (phase 1 form).\n", kZoneWait[0]);
        }

        if (FFortSafeZoneDefinition::HasShrinkTime())
        {
            auto& ShrinkTime = Definition.ShrinkTime;
            ShrinkTime.Value = kZoneShrink[0];
            ShrinkTime.Curve.CurveTable = nullptr;
            GStormMapDataWasRepaired = true;
            printf("[SafeZone] Definition ShrinkTime forced to %.0fs (phase 1 close).\n", kZoneShrink[0]);
        }

        fflush(stdout);
    }

    static bool GetSafeZoneCircle(AFortGameMode* GameMode, FVector& OutCentre, float& OutRadius)
    {
        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return false;

        auto Indicator = GameMode->SafeZoneIndicator;

        bool bHaveCentre = false;

        if (Indicator->HasNextCenter())
        {
            OutCentre = Indicator->NextCenter;
            bHaveCentre = true;
        }
        else if (Indicator->HasLastCenter())
        {
            OutCentre = Indicator->LastCenter;
            bHaveCentre = true;
        }
        else if (Indicator->HasPreviousCenter())
        {
            OutCentre = Indicator->PreviousCenter;
            bHaveCentre = true;
        }

        if (!bHaveCentre)
            return false;

        OutRadius = 0.f;
        if (Indicator->HasNextRadius())
            OutRadius = (float)Indicator->NextRadius;
        if (!(OutRadius > 0.f) && Indicator->HasLastRadius())
            OutRadius = (float)Indicator->LastRadius;
        if (!(OutRadius > 0.f) && Indicator->HasPreviousRadius())
            OutRadius = (float)Indicator->PreviousRadius;

        if (!(OutRadius > 0.f))
            return false;

        if (OutCentre.X == 0.0 && OutCentre.Y == 0.0 && OutCentre.Z == 0.0)
            return false;

        return true;
    }

    static int GetStormPhase(AFortGameMode* GameMode)
    {
        if (!GameMode)
            return 0;

        if (GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator && GameMode->SafeZoneIndicator->HasCurrentPhase())
            return (int)GameMode->SafeZoneIndicator->CurrentPhase;

        if (GameMode->HasSafeZonePhase())
            return (int)GameMode->SafeZonePhase;

        return 0;
    }

    static float StormDamagePerSecond(int Phase)
    {
        static const float Table[] = {
             1.f,
             1.f,
             1.f,
             2.f,
             5.f,
             8.f,
             10.f,
             10.f,
             10.f,
             10.f,
        };

        const int Count = (int)(sizeof(Table) / sizeof(Table[0]));

        if (Phase < 0)
            Phase = 0;
        if (Phase >= Count)
            Phase = Count - 1;

        float Damage = Table[Phase] * FConfiguration::StormDamageScale;

        if (!(Damage > 0.f))
            Damage = 0.f;

        return Damage;
    }

    static int GStormTick = 0;

    static std::map<void*, float> LastKnownHealth;

    static std::map<void*, int> LastPlayerDamageTick;

    static std::map<void*, int> LastOutsideZoneTick;

    static std::map<void*, FVector> LastKnownLocation;

    static void ApplyStormDamage(AFortGameMode* GameMode, AFortGameStateAthena* GameState, int TickNow)
    {
        if (!FConfiguration::bManageStormDamage || FConfiguration::bDisableStorm)
        {
            if (GameState && GameState->HasPlayerArray())
            {
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!PS)
                        continue;

                    auto Owner = (AActor*)PS->Owner;
                    if (!Owner)
                        continue;

                    auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
                    if (!Pawn)
                        continue;

                    if (Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone)
                        LastOutsideZoneTick[(void*)Owner] = TickNow;
                }
            }

            return;
        }

        if (!GameMode || !GameState || !GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        static float LastRunTime = 0.f;
        float Delta = Now - LastRunTime;
        LastRunTime = Now;

        if (!(Delta > 0.f) || Delta > 1.0f)
            return;

        const int Phase = GetStormPhase(GameMode);
        const float PerSecond = StormDamagePerSecond(Phase);
        const float ThisTick = PerSecond * Delta;

        static int Reported = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn)
                continue;
            if (PS->HasbIsABot() && PS->bIsABot && !BotHasLanded((void*)Pawn))
                continue;   // no storm damage on a bot that has not dropped yet
            if (PS->HasbIsABot() && !PS->bIsABot && !NPCs::IsNPC(PS))
                continue;   // real players: the game handles storm damage

            const float Health = Pawn->GetHealth();
            auto Previous = LastKnownHealth.find((void*)Owner);
            const float Before = (Previous != LastKnownHealth.end()) ? Previous->second : Health;

            LastKnownHealth[(void*)Owner] = Health;

            if (Health <= 0.f)
                continue;

            const bool bOutside = Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone;
            if (!bOutside)
                continue;

            LastOutsideZoneTick[(void*)Owner] = TickNow;

            if (Pawn->HasbIsInvulnerable() && Pawn->bIsInvulnerable)
                continue;

            auto Hit = LastPlayerDamageTick.find((void*)Owner);
            const bool bTradingShots = (Hit != LastPlayerDamageTick.end()) && ((TickNow - Hit->second) < 20);

            float Take = ThisTick;

            if (!bTradingShots)
            {
                const float Lost = Before - Health;

                if (Lost > 0.f)
                    Take = ThisTick - Lost;
            }

            if (!(Take > 0.f))
                continue;

            float Target = Health - Take;

            if (Target < 0.f)
                Target = 0.f;

            Pawn->SetHealth(Target);
            LastKnownHealth[(void*)Owner] = Target;

            if (++Reported <= 3)
                LogLine("[ZONE] Storm holding at %.1f health a second (phase %d): took %.2f off this tick%s.",
                    PerSecond, Phase, Take, bTradingShots ? " on top of the shot they just took" : "");
        }
    }

    static void LogStormState(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
    {
        if (!FConfiguration::bStormLog || !(FConfiguration::StormLogIntervalSeconds > 0.f))
            return;

        if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
            return;

        auto Indicator = GameMode->SafeZoneIndicator;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        static float NextReport = 0.f;
        if (Now < NextReport)
            return;
        NextReport = Now + FConfiguration::StormLogIntervalSeconds;

        const int Phase = Indicator->HasCurrentPhase()
            ? (int)Indicator->CurrentPhase
            : (GameMode->HasSafeZonePhase() ? (int)GameMode->SafeZonePhase : -1);

        const float Start = Indicator->HasSafeZoneStartShrinkTime() ? (float)Indicator->SafeZoneStartShrinkTime : -1.f;
        const float Finish = Indicator->HasSafeZoneFinishShrinkTime() ? (float)Indicator->SafeZoneFinishShrinkTime : -1.f;
        const float NextRadius = Indicator->HasNextRadius() ? (float)Indicator->NextRadius : -1.f;

        float CurrentRadius = -1.f;
        if (Indicator->HasPreviousRadius() && Indicator->PreviousRadius > 0.f)
            CurrentRadius = (float)Indicator->PreviousRadius;
        else if (Indicator->HasLastRadius())
            CurrentRadius = (float)Indicator->LastRadius;

        const char* What = (Now < Start) ? "holding" : (Now < Finish ? "CLOSING" : "due to move on");

        printf("[Storm] phase %d, %s -- starts closing in %.1fs, done in %.1fs | radius %.0f -> %.0f\n",
            Phase, What, Start - Now, Finish - Now, CurrentRadius, NextRadius);
        fflush(stdout);
    }

    static void SeasonOneStormTiming(int Phase, float& OutWait, float& OutShrink)
    {
        static const float Wait[] = { 200.f, 200.f, 150.f, 120.f, 120.f, 90.f, 90.f, 60.f, 45.f, 30.f };
        static const float Shrink[] = { 180.f, 180.f, 90.f, 90.f, 60.f, 40.f, 30.f, 30.f, 25.f, 20.f };
        const int Count = (int)(sizeof(Wait) / sizeof(Wait[0]));

        if (Phase < 0)
            Phase = 0;
        if (Phase >= Count)
            Phase = Count - 1;

        OutWait = Wait[Phase];
        OutShrink = Shrink[Phase];
    }

    static void LogStormChanges(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
    {
        if (!GameMode || !GameState)
            return;

        struct FStormSeen
        {
            const void* Indicator = (const void*)1;
            int ModePhase = -999;
            int StatePhase = -999;
            float StormStartsAt = -12345.f;
            float Start = -12345.f;
            float Finish = -12345.f;
            float LastR = -12345.f;
            float NextR = -12345.f;
        };

        static FStormSeen Seen{};
        static int32 StatePhaseOffset = -2;
        static int32 RadiusOffset = -2;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(GameState);

        if (StatePhaseOffset == -2)
        {
            const uint32 Off = ((UObject*)GameState)->GetOffset("SafeZonePhase");
            StatePhaseOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        FStormSeen Current{};
        auto Indicator = GameMode->HasSafeZoneIndicator() ? GameMode->SafeZoneIndicator : nullptr;

        Current.Indicator = (const void*)Indicator;
        Current.ModePhase = GameMode->HasSafeZonePhase() ? (int)GameMode->SafeZonePhase : -1;
        Current.StatePhase = StatePhaseOffset >= 0 ? (int)GetFromOffset<uint8>(GameState, (uint32)StatePhaseOffset) : -1;
        Current.StormStartsAt = GameState->HasSafeZonesStartTime() ? (float)GameState->SafeZonesStartTime : -1.f;

        float RadiusNow = -1.f;

        if (Indicator)
        {
            Current.Start = Indicator->HasSafeZoneStartShrinkTime() ? (float)Indicator->SafeZoneStartShrinkTime : -1.f;
            Current.Finish = Indicator->HasSafeZoneFinishShrinkTime() ? (float)Indicator->SafeZoneFinishShrinkTime : -1.f;
            Current.LastR = Indicator->HasLastRadius() ? (float)Indicator->LastRadius : -1.f;
            Current.NextR = Indicator->HasNextRadius() ? (float)Indicator->NextRadius : -1.f;

            if (RadiusOffset == -2)
            {
                const uint32 Off = ((UObject*)Indicator)->GetOffset("Radius");
                RadiusOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
            }

            if (RadiusOffset >= 0)
                RadiusNow = GetFromOffset<float>(Indicator, (uint32)RadiusOffset);
        }

        auto Moved = [](float A, float B, float Tolerance) { return fabsf(A - B) > Tolerance; };

        const bool bChanged = Current.Indicator != Seen.Indicator || Current.ModePhase != Seen.ModePhase ||
            Current.StatePhase != Seen.StatePhase || Moved(Current.StormStartsAt, Seen.StormStartsAt, 0.05f) ||
            Moved(Current.Start, Seen.Start, 0.05f) || Moved(Current.Finish, Seen.Finish, 0.05f) ||
            Moved(Current.LastR, Seen.LastR, 1.f) || Moved(Current.NextR, Seen.NextR, 1.f);

        if (!bChanged)
            return;

        Seen = Current;

        if (!Indicator)
        {
            LogLine("[STORM] What the clients are told changed (%.1fs): no storm indicator yet | game mode's storm phase %d, game "
                "state's %d | storm starts at %.1f (%s%.1fs).",
                Now, Current.ModePhase, Current.StatePhase, Current.StormStartsAt,
                Current.StormStartsAt >= Now ? "in " : "", fabsf(Current.StormStartsAt - Now));
            return;
        }

        const char* What = (Now < Current.Start) ? "holding" : (Now < Current.Finish ? "CLOSING" : "done -- due to move on");

        LogLine("[STORM] What the clients are told changed (%.1fs): phase %d (game state %d), %s | closes from %.1f (in %.1fs) to "
            "%.1f (in %.1fs) | radius now %.0f, from %.0f to %.0f | storm start time %.1f.",
            Now, Current.ModePhase, Current.StatePhase, What, Current.Start, Current.Start - Now, Current.Finish,
            Current.Finish - Now, RadiusNow, Current.LastR, Current.NextR, Current.StormStartsAt);
    }

    static std::map<void*, std::pair<void*, int>> BotTopDamager;

    static std::map<void*, std::pair<void*, int>> BotLastDamager;

    static std::map<void*, std::pair<void*, int>> BotCodeLastHit;

    static AFortPlayerStateAthena* ResolveDamagerState(AActor* Causer)
    {
        if (!Causer)
            return nullptr;

        if (Causer->IsA<AFortPlayerStateAthena>())
            return (AFortPlayerStateAthena*)Causer;

        if (Causer->IsA<AFortPlayerPawnAthena>())
        {
            auto PawnController = (AActor*)((AFortPlayerPawnAthena*)Causer)->Controller;

            return PawnController ? (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)PawnController)->PlayerState : nullptr;
        }

        if (Causer->GetOffset("PlayerState") != (uint32)-1)
            return (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)Causer)->PlayerState;

        return nullptr;
    }

    static int32 DamagerInfoSize()
    {
        static int32 Size = -1;

        if (Size == -1)
        {
            auto Struct = FindStruct("DamagerInfo");
            Size = Struct ? Struct->GetPropertiesSize() : (int32)sizeof(FDamagerInfo);

            if (Size <= 0 || Size > 0x200)
                Size = (int32)sizeof(FDamagerInfo);
        }

        return Size;
    }

    static void WipeTheSlateAtBusTime(AFortGameStateAthena* GameState)
    {
        static bool bDone = false;

        if (!GameState || !GameState->HasPlayerArray())
            return;

        if (PlayersAreProtected(GameState))
        {
            bDone = false;
            LandedBots.clear();
            return;
        }

        if (bDone)
            return;

        bDone = true;

        int Healed = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasOwner() || !PS->Owner)
                continue;

            auto Owner = (AActor*)PS->Owner;
            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn)
                continue;

            const float Health = Pawn->GetHealth();
            const bool bIsHuman = PS->HasbIsABot() && !PS->bIsABot && !NPCs::IsNPC(PS);

            if (!bIsHuman && Health > 0.f && Health < 100.f)
            {
                float Full = 100.f;
                Pawn->SetHealth(Full);
                Healed++;
            }

            LastKnownHealth.erase((void*)Owner);
            LastPlayerDamageTick.erase((void*)Owner);
            BotTopDamager.erase((void*)Owner);
            BotLastDamager.erase((void*)Owner);
            ShotAtBy.erase((void*)Owner);
            AggroedByHuman.erase((void*)Owner);
        }

        LogLine("[LANDED] Everyone is on the ground. %d player(s) healed back to full and every scratch from the island "
            "and the bus forgotten. Damage and eliminations start counting from here.", Healed);
    }

    static constexpr double kTile = 512.0;
    static constexpr double kFloor = 384.0;
    static constexpr double kSub = 128.0;

    static double SnapTile(double v)
    {
        return round(v / kTile) * kTile;
    }

    static double SnapFloorZ(double v)
    {

        return round(v / kFloor) * kFloor;
    }

    static bool BuildFacesEastWest(double Yaw)
    {
        const long Quarter = lround(Yaw / 90.0);
        return (Quarter % 2) != 0;
    }

    static FVector SnapBuildLocation(const FVector& V, double Yaw)
    {
        if (BuildFacesEastWest(Yaw))
            return FVector{ SnapTile(V.X - 256.0) + 256.0, SnapTile(V.Y), SnapFloorZ(V.Z) };

        return FVector{ SnapTile(V.X), SnapTile(V.Y - 256.0) + 256.0, SnapFloorZ(V.Z) };
    }

    static FRotator SnapBuildRotation(const FRotator& R)
    {
        FRotator Out{};
        Out.Pitch = 0.0;
        Out.Roll = 0.0;
        Out.Yaw = round(R.Yaw / 90.0) * 90.0;
        return Out;
    }

    static UClass* ResolveBuildClass(const wchar_t* Path, const char* ShortName)
    {
        UClass* Cls = const_cast<UClass*>(FindObject<UClass>(Path));
        if (!Cls)
            Cls = const_cast<UClass*>(FindClass(ShortName));
        return Cls;
    }

    static ABuildingSMActor* SpawnBuildPiece(UClass* Class, const FVector& Loc, const FRotator& Rot,
        AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PS)
    {
        if (!Class || !Controller)
            return nullptr;

        FRotator GridRot = SnapBuildRotation(Rot);
        FVector Snapped = SnapBuildLocation(Loc, GridRot.Yaw);

        ABuildingSMActor* Building = UWorld::SpawnActorUnfinished<ABuildingSMActor>(Class, Snapped, GridRot, Controller);
        if (!Building)
            return nullptr;

        Building->InitializeKismetSpawnedBuildingActor(Building, Controller, true, nullptr, false);
        UWorld::FinishSpawnActor(Building, Snapped, GridRot);

        Building->CurrentBuildingLevel = 0;
        Building->OnRep_CurrentBuildingLevel();
        Building->SetMirrored(false);
        Building->bPlayerPlaced = true;

        if (PS && PS->HasTeamIndex())
            Building->Team = PS->TeamIndex;
        if (Building->HasTeamIndex())
            Building->TeamIndex = Building->Team;

        Building->ForceNetUpdate();
        return Building;
    }

    static void TryBotReactiveBuild(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        const int BuildAfterTicks = 0;

        static UClass* WallWood = nullptr;
        static UClass* WallBrick = nullptr;
        static UClass* WallMetal = nullptr;
        static bool bResolved = false;

        if (!bResolved)
        {
            bResolved = true;
            WallWood = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Solid.PBWA_W1_Solid_C", "PBWA_W1_Solid_C");
            WallBrick = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Stone/L1/PBWA_S1_Solid.PBWA_S1_Solid_C", "PBWA_S1_Solid_C");
            WallMetal = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Metal/L1/PBWA_M1_Solid.PBWA_M1_Solid_C", "PBWA_M1_Solid_C");

            if (!WallWood)
                WallWood = ResolveBuildClass(L"/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Brace.PBWA_W1_Brace_C", "PBWA_W1_Brace_C");

            if (!WallWood && !WallBrick && !WallMetal)
            {
                LogLine("[BUILD] Could not resolve any wall class -- reactive building disabled.");
                return;
            }
            LogLine("[BUILD] Tile=512 (5m) Floor=384 | classes ready.");
        }

        if (!WallWood && !WallBrick && !WallMetal)
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            if (!Controller)
                continue;

            if (BotAlreadyBuiltWall.find((void*)Controller) != BotAlreadyBuiltWall.end())
                continue;

            auto Since = BotUnderFireSince.find((void*)Controller);
            if (Since == BotUnderFireSince.end())
                continue;
            if ((GStormTick - Since->second) > 60)
                continue;
            if ((GStormTick - Since->second) < BuildAfterTicks)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            if (!BotHasLanded((void*)Pawn))
                continue;
            if (GlidingBots.find((void*)Pawn) != GlidingBots.end())
                continue;
            if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
                continue;
            if (Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus)
                continue;
            if (Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen)
                continue;

            FVector Loc = Pawn->K2_GetActorLocation();
            if (Loc.Z > 8000.0)
                continue;

            AFortPlayerPawnAthena* Threat = nullptr;
            const char* Toward = "the way it was facing";

            {
                auto Locked = BotCombatTarget.find((void*)Pawn);

                if (Locked != BotCombatTarget.end() && Locked->second && Locked->second->GetHealth() > 0.f)
                {
                    Threat = Locked->second;
                    Toward = "the one it is fighting";
                }
                else
                {
                    auto Shooter = BotLastDamager.find((void*)Controller);

                    if (Shooter != BotLastDamager.end() && Shooter->second.first)
                    {
                        auto ShooterState = (AFortPlayerStateAthena*)Shooter->second.first;

                        if (ShooterState->HasOwner() && ShooterState->Owner)
                        {
                            auto ShooterPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)ShooterState->Owner)->Pawn;

                            if (ShooterPawn && ShooterPawn != Pawn && ShooterPawn->GetHealth() > 0.f)
                            {
                                Threat = ShooterPawn;
                                Toward = "whoever just shot it";
                            }
                        }
                    }
                }
            }

            FRotator Facing = Pawn->K2_GetActorRotation();

            if (Threat)
            {
                const FVector ThreatLoc = Threat->K2_GetActorLocation();
                const double ToX = ThreatLoc.X - Loc.X;
                const double ToY = ThreatLoc.Y - Loc.Y;

                if ((ToX * ToX + ToY * ToY) > 1.0)
                {
                    double ThreatYaw = atan2(ToY, ToX) * (180.0 / 3.14159265358979323846);
                    Facing.Yaw = ThreatYaw;
                }
            }

            FRotator GridFacing = SnapBuildRotation(Facing);

            const double YawRad = GridFacing.Yaw * (3.14159265358979323846 / 180.0);
            const double Fx = cos(YawRad);
            const double Fy = sin(YawRad);
            const bool bEastWest = BuildFacesEastWest(GridFacing.Yaw);

            double Ahead = (double)FConfiguration::BotWallDistance;
            if (!(Ahead > 0.0))
                Ahead = kTile + kTile;

            FVector Wall1{};

            if (!bEastWest)
            {
                Wall1.X = SnapTile(Loc.X + (Fx > 0.0 ? Ahead : -Ahead));
                Wall1.Y = SnapTile(Loc.Y - 256.0) + 256.0;
            }
            else
            {
                Wall1.X = SnapTile(Loc.X - 256.0) + 256.0;
                Wall1.Y = SnapTile(Loc.Y + (Fy > 0.0 ? Ahead : -Ahead));
            }

            const double FloorZ = floor(Loc.Z / kFloor) * kFloor;
            Wall1.Z = FloorZ;

            UClass* WallCls = WallWood;
            int MatRoll = rand() % 100;
            if (MatRoll >= 70 && WallBrick) WallCls = WallBrick;
            else if (MatRoll >= 90 && WallMetal) WallCls = WallMetal;
            if (!WallCls) WallCls = WallWood ? WallWood : (WallBrick ? WallBrick : WallMetal);

            const bool bPlaced = SpawnBuildPiece(WallCls, Wall1, GridFacing, Controller, PS) != nullptr;

            BotAlreadyBuiltWall.insert((void*)Controller);

            if (bPlaced)
            {
                static int WallReports = 0;

                const double Away = bEastWest ? fabs(Wall1.Y - Loc.Y) : fabs(Wall1.X - Loc.X);
                const char* Direction = !bEastWest ? (Fx > 0.0 ? "north" : "south") : (Fy > 0.0 ? "east" : "west");

                const int ModX = ((int)llround(Wall1.X) % 512 + 512) % 512;
                const int ModY = ((int)llround(Wall1.Y) % 512 + 512) % 512;

                if (++WallReports <= 25)
                    LogLine("[BUILD] '%s' put up ONE wall %.1f m to its %s, towards %s | X=%.0f Y=%.0f Z=%.0f "
                        "(X%%512=%d Y%%512=%d -- where your own %s walls snap).",
                        SafeNameOf(PS).c_str(), (float)(Away / 100.0), Direction, Toward,
                        (float)Wall1.X, (float)Wall1.Y, (float)Wall1.Z, ModX, ModY,
                        bEastWest ? "east/west-facing" : "north/south-facing");
            }
        }
    }

    void TrackBotDamage()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        const int32 Stride = DamagerInfoSize();

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AActor*)PS->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Controller)->Pawn;
            if (!Pawn || !Pawn->HasDamagers())
                continue;

            auto& Damagers = Pawn->Damagers;

            if (Damagers.Num() <= 0 || !Damagers.GetData())
                continue;

            void* Causer = nullptr;
            int CauserDamage = 0;

            for (int d = Damagers.Num() - 1; d >= 0; d--)
            {
                auto& Entry = Damagers.Get(d, Stride);

                auto DamagerState = ResolveDamagerState(Entry.DamageCauser);
                if (!DamagerState || DamagerState == PS)
                    continue;

                Causer = (void*)DamagerState;
                CauserDamage = Entry.DamageAmount;
                break;
            }

            {
                auto Previous = BotTopDamager.find((void*)Controller);
                const bool bChanged = (Previous == BotTopDamager.end()) ||
                    (Previous->second.first != Causer) ||
                    (Previous->second.second != CauserDamage);

                if (bChanged && Causer)
                {
                    auto CauserState = (AFortPlayerStateAthena*)Causer;
                    if (CauserState->HasbIsABot() && CauserState->bIsABot)
                    {
                        GCombatBotOnBotHits++;

                        if (CauserState->HasOwner() && CauserState->Owner)
                        {
                            auto ShooterPawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)CauserState->Owner)->Pawn;
                            auto ShooterLock = BotCombatTarget.find((void*)ShooterPawn);

                            if (ShooterLock != BotCombatTarget.end() && ShooterLock->second == Pawn)
                            {
                                GHitsOnLockedTarget++;
                            }
                            else
                            {
                                GHitsOnSomebodyElse++;

                                if (GHitsOnSomebodyElse <= 10)
                                {
                                    auto LockName = BotLockedOnName.find((void*)ShooterPawn);

                                    LogLine("[HITS] '%s' hit '%s', but the one it is locked onto is '%s'.",
                                        SafeNameOf(CauserState).c_str(), SafeNameOf(PS).c_str(),
                                        LockName != BotLockedOnName.end() ? LockName->second.c_str() : "nobody");
                                }
                            }
                        }
                    }
                    else
                    {
                    }

                    AggroedByHuman[(void*)Controller] = { Causer, GStormTick };
                }
            }

            BotTopDamager[(void*)Controller] = { Causer, CauserDamage };

            if (Causer)
            {
                auto PrevLast = BotLastDamager.find((void*)Controller);
                const bool bNewDamager = (PrevLast == BotLastDamager.end()) || (PrevLast->second.first != Causer);

                BotLastDamager[(void*)Controller] = { Causer, GStormTick };

                BotTopDamager[(void*)Controller] = { Causer, CauserDamage };

                BotUnderFireSince[(void*)Controller] = GStormTick;

                if (bNewDamager)
                    BotAlreadyBuiltWall.erase((void*)Controller);
            }
        }
    }

    static void TrackEveryBotAlive(AFortGameStateAthena* GameState, int TickNow)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AActor*)PS->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Controller)->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            if (BotTopDamager.find((void*)Controller) == BotTopDamager.end())
                BotTopDamager[(void*)Controller] = { nullptr, 0 };

            auto Tracked = BotTopDamager.find((void*)Controller);
            if (Tracked != BotTopDamager.end() && Tracked->second.first)
            {
                static std::map<void*, std::pair<void*, int>> LastSeenLedger;

                auto Seen = LastSeenLedger.find((void*)Controller);
                if (Seen == LastSeenLedger.end() || Seen->second != Tracked->second)
                {
                    LastSeenLedger[(void*)Controller] = Tracked->second;
                    LastPlayerDamageTick[(void*)Controller] = TickNow;
                }
            }
        }
    }

    static FGameplayTagContainer* ResolveDamageTags(AFortPlayerPawnAthena* Pawn, FGameplayTagContainer& Fallback)
    {
        if (!Pawn)
            return &Fallback;

        const int32 Anchor = Pawn->GetOffset("MoveSoundStimulusBroadcastInterval");
        if (Anchor == -1)
            return &Fallback;

        const int32 Delta = (VersionInfo.FortniteVersion >= 11 && VersionInfo.FortniteVersion < 18) ? 0x18 : 0x10;
        auto Candidate = (FGameplayTagContainer*)(__int64(Pawn) + Anchor + Delta);

        if (IsBadReadPtr((void*)Candidate))
            return &Fallback;

        return Candidate;
    }

    static bool DiedToTheStorm(void* Controller, int TickNow)
    {
        if (!Controller)
            return false;

        auto Outside = LastOutsideZoneTick.find(Controller);
        if (Outside == LastOutsideZoneTick.end() || (TickNow - Outside->second) > 90)
            return false;

        auto Hit = LastPlayerDamageTick.find(Controller);
        if (Hit != LastPlayerDamageTick.end() && (TickNow - Hit->second) < 150)
            return false;

        return true;
    }

    static const int KillCreditWindowTicks = 15 * 30;

    static AFortPlayerStateAthena* FindBotKiller(AActor* BotController, AFortPlayerStateAthena* VictimState,
        AFortGameStateAthena* GameState, const char** OutHow)
    {
        const char* How = "no damage from another player on record";
        AFortPlayerStateAthena* Killer = nullptr;

        if (PlayersAreProtected(GameState))
        {
            if (OutHow)
                *OutHow = "the match had not started";
            return nullptr;
        }

        auto Last = BotLastDamager.find((void*)BotController);
        if (Last != BotLastDamager.end() && Last->second.first)
        {
            if ((GStormTick - Last->second.second) <= KillCreditWindowTicks)
            {
                Killer = (AFortPlayerStateAthena*)Last->second.first;
                How = "the last player to damage it";
                GElimsFromLedger++;
            }
            else
            {
                How = "its last damage from another player was more than 15 seconds before it died";
            }
        }

        if (!Killer)
        {
            auto Tracked = BotTopDamager.find((void*)BotController);
            auto When = LastPlayerDamageTick.find((void*)BotController);

            if (Tracked != BotTopDamager.end() && Tracked->second.first && When != LastPlayerDamageTick.end() &&
                (GStormTick - When->second) <= KillCreditWindowTicks)
            {
                Killer = (AFortPlayerStateAthena*)Tracked->second.first;
                How = "the game's own damage list";
                GElimsFromLedger++;
            }
        }

        if (Killer == VictimState)
        {
            Killer = nullptr;
            How = "the only damage on record was its own";
        }

        if (OutHow)
            *OutHow = How;

        return Killer;
    }

    static std::set<void*> LootAlreadyDropped;

    static const UFortItemDefinition* PickupDefinition(AActor* Pickup, int* OutCount, int* OutAmmo);
    static std::string BotObjectName(const UObject* Object);

    // Every item type already lying on the ground within 4 m of a spot (the game may have dropped them itself).
    static std::set<const UFortItemDefinition*> ItemsAlreadyOnTheGroundNear(const FVector& At)
    {
        std::set<const UFortItemDefinition*> Found;

        auto World = UWorld::GetWorld();
        static auto PickupClass = FindClass("FortPickupAthena");

        if (!World || !PickupClass)
            return Found;

        TArray<AActor*> All;
        UGameplayStatics::GetAllActorsOfClass(World, PickupClass, &All);

        for (int i = 0; i < All.Num(); i++)
        {
            AActor* Pickup = All[i];

            if (!Pickup || ReadBoolByName((const UObject*)Pickup, "bPickedUp") == 1)
                continue;

            const FVector P = Pickup->K2_GetActorLocation();
            const double Dx = P.X - At.X;
            const double Dy = P.Y - At.Y;

            if (Dx * Dx + Dy * Dy > 400.0 * 400.0)
                continue;

            if (auto Def = PickupDefinition(Pickup, nullptr, nullptr))
                Found.insert(Def);
        }

        return Found;
    }

    static void DropBotLoot(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        if (!Controller)
            return;

        if (LootAlreadyDropped.find((void*)Controller) != LootAlreadyDropped.end())
            return;

        LootAlreadyDropped.insert((void*)Controller);

        auto VictimPS = (AFortPlayerStateAthena*)Controller->PlayerState;

        FVector dropLoc{};
        bool bHaveDropSpot = false;

        if (Pawn)
        {
            dropLoc = Pawn->K2_GetActorLocation();
            bHaveDropSpot = true;
        }
        else
        {
            auto Seen = LastKnownLocation.find((void*)Controller);

            if (Seen != LastKnownLocation.end())
            {
                dropLoc = Seen->second;
                bHaveDropSpot = true;
            }
        }

        if (!bHaveDropSpot)
        {
            if (VictimPS)
                BotInventories.erase((void*)VictimPS);

            static int noSpotReported = 0;
            if (++noSpotReported <= 3)
                LogLine("[DROP] A bot died with no pawn and no last known position -- nothing to drop onto.");

            return;
        }

        dropLoc.Z += 100.0;

        int droppedCount = 0;
        int fromInventory = 0;
        bool bDroppedMaterials = false;

        // what is already on the floor here (the game's own drop), and what we drop ourselves
        const auto AlreadyThere = ItemsAlreadyOnTheGroundNear(dropLoc);
        std::set<const UFortItemDefinition*> DroppedByUs;

        auto DropOne = [&](const UFortItemDefinition* Definition, int Count, int LoadedAmmo)
            {
                if (!Definition || Count <= 0)
                    return;

                if (AlreadyThere.count(Definition) || DroppedByUs.count(Definition))
                {
                    static int SkipLines = 0;
                    if (++SkipLines <= 10)
                        LogLine("[DROP] Skipped a duplicate %s (it is already on the floor or was already dropped).",
                            BotObjectName((const UObject*)Definition).c_str());
                    return;
                }

                DroppedByUs.insert(Definition);

                FVector spot = dropLoc;
                float ang = (float)(rand() % 360) * (3.14159265f / 180.f);
                spot.X += cosf(ang) * (float)(40 + (rand() % 90));
                spot.Y += sinf(ang) * (float)(40 + (rand() % 90));

                AFortInventory::SpawnPickup(spot, Definition, Count, LoadedAmmo,
                    EFortPickupSourceTypeFlag::GetPlayer(),
                    EFortPickupSpawnSource::GetPlayerElimination());
                droppedCount++;
            };

        if (VictimPS)
        {
            auto invIt = BotInventories.find((void*)VictimPS);

            if (invIt != BotInventories.end() && invIt->second)
            {
                auto Inv = invIt->second;
                auto& Entries = Inv->Inventory.ReplicatedEntries;

                for (int e = 0; e < Entries.Num(); e++)
                {
                    auto& Entry = Entries.Get(e, FFortItemEntry::Size());
                    if (!Entry.HasItemDefinition() || !Entry.ItemDefinition)
                        continue;

                    if (Entry.ItemDefinition->Cast<UFortWeaponMeleeItemDefinition>() || !Entry.ItemDefinition->CanBeDropped())
                        continue;

                    int cnt = Entry.HasCount() ? Entry.Count : 1;
                    int ammo = Entry.HasLoadedAmmo() ? Entry.LoadedAmmo : 0;
                    if (cnt <= 0)
                        cnt = 1;

                    DropOne(Entry.ItemDefinition, cnt, ammo);
                    fromInventory++;
                }

                BotInventories.erase(invIt);
            }
        }

        if (droppedCount == 0 && Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
        {
            auto Held = (AFortWeapon*)Pawn->CurrentWeapon;

            if (Held->HasWeaponData() && Held->WeaponData &&
                !((const UObject*)Held->WeaponData)->IsA<UFortWeaponMeleeItemDefinition>())
                DropOne((const UFortItemDefinition*)Held->WeaponData, 1, 30);
        }

        if (droppedCount == 0 && VictimPS)
        {
            auto wep = BotWeapons.find((void*)VictimPS);

            if (wep != BotWeapons.end() && wep->second)
            {
                int Clip = 30;
                auto ClipIt = BotWeaponClipSize.find((void*)VictimPS);

                if (ClipIt != BotWeaponClipSize.end() && ClipIt->second > 0)
                    Clip = ClipIt->second;

                DropOne(wep->second, 1, Clip);

                if (wep->second->Cast<UFortWeaponRangedItemDefinition>())
                {

                    static const wchar_t* SpareAmmo[] = {
                        L"AthenaAmmoDataBulletsMedium",
                        L"AthenaAmmoDataBulletsLight",
                        L"AthenaAmmoDataShells",
                        L"AthenaAmmoDataBulletsHeavy",
                    };

                    auto AmmoDef = FindItemDefByName(SpareAmmo[rand() % (int)(sizeof(SpareAmmo) / sizeof(SpareAmmo[0]))]);
                    DropOne(AmmoDef, 60, 0);
                }

                DropOne(FindItemDefByName(L"Athena_Bandage"), 3, 0);
            }
        }

        if (droppedCount > 0)
        {
            static const wchar_t* Materials[] = { L"WoodItemData", L"StoneItemData", L"MetalItemData" };

            auto MatDef = FindItemDefByName(Materials[rand() % 3]);

            if (MatDef)
            {
                DropOne(MatDef, 30 + (rand() % 60), 0);
                bDroppedMaterials = true;
            }
        }

        if (VictimPS)
        {
            BotWeapons.erase((void*)VictimPS);
            BotWeaponClipSize.erase((void*)VictimPS);
        }

        static int dropsReported = 0;
        if (++dropsReported <= 6)
            LogLine("[DROP] Bot died -- spawned %d pickup(s) at the death location (%d from its inventory; materials %s).",
                droppedCount, fromInventory, bDroppedMaterials ? "yes" : "NO");
    }

    static std::string BotObjectName(const UObject* Object);

    struct FSettledDeath
    {
        float When = -1000.f;
        const void* PawnKey = nullptr;
        bool bCredited = false;
        std::string DecidedBy;
        std::string Outcome;
    };

    static std::map<void*, FSettledDeath> GSettledDeaths;

    static int GElimsCredited = 0;
    static int GElimsToNobody = 0;
    static int GElimsByGameReport = 0;
    static int GElimsBySweep = 0;
    static int GElimsSecondReports = 0;
    static int GElimPopupsSent = 0;
    static int GElimPopupsNotSent = 0;

    static bool DeathCauseIsWorkedOutHere()
    {
        return VersionInfo.EngineVersion < 4.19;
    }

    static std::string DeathCauseName(uint8 Cause)
    {
        static const char* Names[] = { "the storm", "a fall", "a pistol", "a shotgun", "a rifle", "an SMG", "a sniper",
                                       "melee", "a heavy weapon", "bleeding out", "the banhammer", "leaving the game",
                                       "unspecified" };

        if (DeathCauseIsWorkedOutHere() && Cause < (uint8)(sizeof(Names) / sizeof(Names[0])))
            return Names[Cause];

        return "cause " + std::to_string((int)Cause);
    }

    static uint8 DeathCauseFromWeapon(const UObject* WeaponDefinition)
    {
        if (!WeaponDefinition)
            return 255;

        std::string Lower = BotObjectName(WeaponDefinition);
        for (auto& Ch : Lower)
            Ch = (char)tolower((unsigned char)Ch);

        auto Has = [&](const char* Part) { return Lower.find(Part) != std::string::npos; };

        if (Has("pickaxe") || Has("harvest") || Has("melee"))
            return 7;
        if (Has("sniper"))
            return 6;
        if (Has("shotgun"))
            return 3;
        if (Has("smg") || Has("autoheavy") || Has("scavenger") || Has("machinepistol"))
            return 5;
        if (Has("pistol") || Has("revolver") || Has("handcannon") || Has("sixshooter"))
            return 2;
        if (Has("launcher") || Has("rocket") || Has("grenade") || Has("rpg"))
            return 8;
        if (Has("assault") || Has("rifle") || Has("minigun") || Has("lmg"))
            return 4;

        return 255;
    }

    static AFortPlayerPawnAthena* PawnOfPlayerState(AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState || !PlayerState->HasOwner() || !PlayerState->Owner)
            return nullptr;

        auto Owner = (AActor*)PlayerState->Owner;

        if (!Owner->IsA<AFortPlayerControllerAthena>())
            return nullptr;

        return (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
    }

    static const UObject* KillingWeaponDefinition(AActor* DamageCauser, AFortPlayerPawnAthena* KillerPawn)
    {
        if (DamageCauser && DamageCauser->IsA<AFortWeapon>())
        {
            auto Weapon = (AFortWeapon*)DamageCauser;

            if (Weapon->HasWeaponData() && Weapon->WeaponData)
                return (const UObject*)Weapon->WeaponData;
        }

        if (DamageCauser && DamageCauser->IsA<AFortPlayerPawnAthena>() && !KillerPawn)
            KillerPawn = (AFortPlayerPawnAthena*)DamageCauser;

        if (KillerPawn && KillerPawn->HasCurrentWeapon() && KillerPawn->CurrentWeapon)
        {
            auto Weapon = (AFortWeapon*)KillerPawn->CurrentWeapon;

            if (Weapon->HasWeaponData() && Weapon->WeaponData)
                return (const UObject*)Weapon->WeaponData;
        }

        return nullptr;
    }

    static bool ReportKillTo(AFortPlayerStateAthena* Killer, AFortPlayerStateAthena* Victim, const std::string& VictimName)
    {
        static UFunction* Fn = nullptr;
        static int Kind = -1;
        static uint32 ParamOffset = 0;
        static uint32 ParamsSize = 0;

        if (!Killer || !Victim)
            return false;

        if (Kind == -1)
        {
            Kind = 2;
            Fn = Killer->GetFunction("ClientReportKill");

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                ParamsSize = Params.Size;

                if (!Params.NameOffsetMap.empty())
                {
                    const auto& First = Params.NameOffsetMap[0];
                    ParamOffset = First.Offset;

                    if (First.ElementSize == 0x10)
                        Kind = 0;
                    else if (First.ElementSize == 8)
                        Kind = 1;

                    LogLine("[ELIM] ClientReportKill on this build takes '%s' (%u bytes) -- the \"Eliminated\" popup is sent %s.",
                        First.Name.c_str(), (unsigned)First.ElementSize,
                        Kind == 0 ? "the victim's name" : (Kind == 1 ? "the victim's player state" : "nothing (unknown kind -- no popup)"));
                }
            }
            else
            {
                LogLine("[ELIM] ClientReportKill is not on this build -- no \"Eliminated\" popup.");
            }
        }

        alignas(16) uint8 Buffer[0x110] = {};

        if (Kind == 2 || !Fn || ParamsSize == 0 || ParamsSize > 0x100 || ParamOffset + 0x10 > (uint32)sizeof(Buffer))
            return false;

        if (Kind == 1)
        {
            AFortPlayerStateAthena* VictimPointer = Victim;
            memcpy(Buffer + ParamOffset, &VictimPointer, sizeof(VictimPointer));
            Killer->ProcessEvent(Fn, Buffer);
            return true;
        }

        std::wstring Wide;

        if (const FString* Raw = PlayerNameField(Victim))
            if (Raw->CStr() && Raw->Num() > 1)
                Wide = std::wstring(Raw->CStr());

        if (Wide.empty() && !VictimName.empty())
        {
            const int Needed = MultiByteToWideChar(CP_UTF8, 0, VictimName.c_str(), (int)VictimName.size(), nullptr, 0);

            if (Needed > 0)
            {
                Wide.resize((size_t)Needed);
                MultiByteToWideChar(CP_UTF8, 0, VictimName.c_str(), (int)VictimName.size(), &Wide[0], Needed);
            }
        }

        if (Wide.empty())
            return false;

        FString Name(Wide.c_str());
        memcpy(Buffer + ParamOffset, (const void*)&Name, 0x10);

        Killer->ProcessEvent(Fn, Buffer);

        if (Name.Data)
            FMemory::Free(Name.Data);

        return true;
    }

    struct FDeathFacts
    {
        AFortPlayerStateAthena* Victim = nullptr;
        AFortPlayerStateAthena* Killer = nullptr;
        AFortPlayerPawnAthena* VictimPawn = nullptr;
        const void* PawnKey = nullptr;
        AFortPlayerPawnAthena* KillerPawn = nullptr;
        AActor* DamageCauser = nullptr;
        int Storm = -1;
        bool bFromGameReport = false;
        std::string How;
        FVector Where{};
        bool bHaveWhere = false;
    };

    static bool DeathIsAlreadySettled(AFortPlayerStateAthena* Victim, const void* PawnKey, float Now, const FSettledDeath** Out)
    {
        auto Done = GSettledDeaths.find((void*)Victim);

        if (Done == GSettledDeaths.end())
            return false;

        const bool bSameBody = PawnKey && Done->second.PawnKey == PawnKey;
        const bool bJustNow = (Now - Done->second.When) < 10.f;

        if (!bSameBody && !bJustNow)
            return false;

        if (Out)
            *Out = &Done->second;

        return true;
    }

    static bool SettleElimination(FDeathFacts Facts)
    {
        auto Victim = Facts.Victim;
        if (!Victim)
            return false;

        auto World = UWorld::GetWorld();
        auto GameState = (World && World->GameState) ? (AFortGameStateAthena*)World->GameState : nullptr;
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        const char* DecidedBy = Facts.bFromGameReport ? "the game's own death report" : "the bot code's dead-bot sweep";
        const std::string VictimName = SafeNameOf(Victim);

        const FSettledDeath* Earlier = nullptr;

        if (DeathIsAlreadySettled(Victim, Facts.PawnKey, Now, &Earlier))
        {
            GElimsSecondReports++;

            if (GElimsSecondReports <= 20 || (GElimsSecondReports % 25) == 0)
                LogLine("[ELIM] '%s': %s reported this death as well -- %s already decided it %.1fs ago (%s). Nothing changed, "
                    "nobody gets a second kill (%d such second report(s) so far).",
                    VictimName.c_str(), DecidedBy, Earlier->DecidedBy.c_str(), Now - Earlier->When, Earlier->Outcome.c_str(),
                    GElimsSecondReports);
            return false;
        }

        auto Killer = Facts.Killer;
        std::string NoKillerBecause = Facts.How.empty() ? std::string("no damage from another player on record") : Facts.How;

        if (Killer == Victim)
        {
            Killer = nullptr;
            NoKillerBecause = "it was its own doing";
        }

        if (Killer && GameState && PlayersAreProtected(GameState))
        {
            Killer = nullptr;
            NoKillerBecause = "the match had not started";
        }

        if (Killer && !((UObject*)Killer)->IsA<AFortPlayerStateAthena>())
        {
            Killer = nullptr;
            NoKillerBecause = "what the game named as the killer is not a player";
        }

        int Storm = Facts.Storm;

        if (Storm < 0)
            Storm = (Victim->HasOwner() && Victim->Owner && DiedToTheStorm((void*)Victim->Owner, GStormTick)) ? 1 : 0;

        if (Killer && Storm == 1 && !Facts.bFromGameReport)
        {
            Killer = nullptr;
            NoKillerBecause = "it was outside the circle and nobody had hit it in the last 5 seconds -- the storm finished it";
        }

        if (!Killer && Storm == 1 && NoKillerBecause.find("storm") == std::string::npos)
            NoKillerBecause += ", in the storm";

        uint8 Cause = 0;
        std::string CauseFrom;

        if (DeathCauseIsWorkedOutHere())
        {
            if (Killer)
            {
                auto KillerPawn = Facts.KillerPawn ? Facts.KillerPawn : PawnOfPlayerState(Killer);
                const UObject* Weapon = KillingWeaponDefinition(Facts.DamageCauser, KillerPawn);
                const uint8 FromWeapon = DeathCauseFromWeapon(Weapon);

                if (FromWeapon != 255)
                {
                    Cause = FromWeapon;
                    CauseFrom = "from " + BotObjectName(Weapon);
                }
                else
                {
                    Cause = 5;
                    CauseFrom = Weapon ? ("could not tell from " + BotObjectName(Weapon) + ", so an SMG as before")
                        : std::string("no weapon to read, so an SMG as before");
                }
            }
            else
            {
                Cause = (Storm == 1) ? 0 : 1;
                CauseFrom = (Storm == 1) ? "it was outside the circle" : "no killer and not the storm: the fall/void line";
            }
        }
        else
        {
            FGameplayTagContainer EmptyTags{};
            FGameplayTagContainer* SourceTags = ResolveDamageTags(Facts.VictimPawn, EmptyTags);

            Cause = (Storm == 1 && !Killer) ? 0 : ToDeathCause(Facts.VictimPawn, *SourceTags, false);

            if (Cause == 0 && Killer)
                Cause = 5;

            CauseFrom = "the game's own ToDeathCause";
        }

        if (Victim->HasDeathInfo())
        {
            FGameplayTagContainer EmptyTags{};
            FGameplayTagContainer* SourceTags = ResolveDamageTags(Facts.VictimPawn, EmptyTags);

            if (FDeathInfo::HasKiller())
                Victim->DeathInfo.Killer = Killer;
            if (FDeathInfo::HasFinisherOrDowner())
                Victim->DeathInfo.FinisherOrDowner = Killer ? (AActor*)Killer : (AActor*)Victim;
            if (FDeathInfo::HasbDBNO())
                Victim->DeathInfo.bDBNO = false;
            if (FDeathInfo::HasDeathLocation() && Facts.bHaveWhere)
                Victim->DeathInfo.DeathLocation = Facts.Where;
            if (!Facts.bFromGameReport)
            {

                if (FDeathInfo::HasDeathTags())
                    Victim->DeathInfo.DeathTags = *SourceTags;
                if (FDeathInfo::HasVictimTags() && Facts.VictimPawn)
                    Victim->DeathInfo.VictimTags = Facts.VictimPawn->GameplayTags;
            }
            if (FDeathInfo::HasDeathClassSlot())
                Victim->DeathInfo.DeathClassSlot = -1;
            if (FDeathInfo::HasDeathCause())
                Victim->DeathInfo.DeathCause = Cause;
            if (FDeathInfo::HasbInitialized())
                Victim->DeathInfo.bInitialized = true;

            Victim->OnRep_DeathInfo();
        }

        FSettledDeath& Settled = GSettledDeaths[(void*)Victim];
        Settled.When = Now;
        Settled.PawnKey = Facts.PawnKey;
        Settled.DecidedBy = DecidedBy;

        if (Facts.bFromGameReport)
            GElimsByGameReport++;
        else
            GElimsBySweep++;

        if (Killer)
        {
            const std::string KillerName = SafeNameOf(Killer);

            int Before = -1, After = -1;

            if (Killer->HasKillScore())
            {
                Before = (int)Killer->KillScore;
                Killer->KillScore++;
                After = (int)Killer->KillScore;
            }
            else if (Killer->HasKills())
            {
                Before = (int)Killer->Kills;
                Killer->Kills++;
                After = (int)Killer->Kills;
            }

            Killer->OnRep_Kills();

            if (Killer->HasTeamKillScore())
            {
                Killer->TeamKillScore++;
                Killer->OnRep_TeamKillScore();
            }

            const bool bKillerIsHuman = !(Killer->HasbIsABot() && Killer->bIsABot) && !NPCs::IsNPC(Killer);
            const char* Popup = "a bot -- no screen, no popup";

            if (bKillerIsHuman)
            {
                if (ReportKillTo(Killer, Victim, VictimName))
                {
                    GElimPopupsSent++;
                    Popup = "\"Eliminated <name>\" sent with the victim's name";
                }
                else
                {
                    GElimPopupsNotSent++;
                    Popup = "the popup could NOT be sent (see the ClientReportKill line)";
                }

                if (Killer->HasTeamKillScore())
                    Killer->ClientReportTeamKill(Killer->TeamKillScore);
            }

            const bool bVictimIsBot = Victim->HasbIsABot() && Victim->bIsABot;

            if (bVictimIsBot && !bKillerIsHuman && FConfiguration::SiphonAmount > 0)
            {
                auto KillerPawn = PawnOfPlayerState(Killer);

                if (KillerPawn && KillerPawn->GetHealth() > 0.f)
                {
                    float Health = KillerPawn->GetHealth();
                    float Shield = KillerPawn->GetShield();
                    float Remaining = (float)FConfiguration::SiphonAmount;

                    if (Health < 100.f)
                    {
                        float Need = 100.f - Health;
                        float Give = Remaining < Need ? Remaining : Need;
                        Health += Give;
                        Remaining -= Give;
                    }

                    if (Remaining > 0.f && Shield < 100.f)
                    {
                        float Need = 100.f - Shield;
                        float Give = Remaining < Need ? Remaining : Need;
                        Shield += Give;
                    }

                    KillerPawn->SetHealth(Health);
                    KillerPawn->SetShield(Shield);
                }
            }

            GElimsCredited++;
            Settled.bCredited = true;
            Settled.Outcome = "'" + KillerName + "' got the elim";

            LogLine("[ELIM] '%s' eliminated '%s' with %s (%s) -- decided by %s (%s) | %s's kills: %d -> %d | %s.",
                KillerName.c_str(), VictimName.c_str(), DeathCauseName(Cause).c_str(), CauseFrom.c_str(), DecidedBy,
                Facts.How.empty() ? "the killer it named" : Facts.How.c_str(), KillerName.c_str(), Before, After, Popup);

            return true;
        }

        const FString* Raw = PlayerNameField(Victim);
        const int Pick = NoKillerMessages::PickFor((Raw && Raw->CStr()) ? Raw->CStr() : L"", Raw ? Raw->Num() : 0);
        const std::string Line = NoKillerMessages::LineUtf8(Pick, VictimName.empty() ? std::string("someone") : VictimName);

        GElimsToNobody++;
        Settled.bCredited = false;
        Settled.Outcome = "nobody got the elim: \"" + Line + "\"";

        LogLine("[ELIM] %s -- nobody gets the elim (%s; decided by %s) | killfeed: %s (%s), no kill for anyone.",
            Line.c_str(), NoKillerBecause.c_str(), DecidedBy, DeathCauseName(Cause).c_str(), CauseFrom.c_str());

        return false;
    }

    struct FSweepDeath
    {
        AFortPlayerStateAthena* Victim = nullptr;
        FWeakObjectPtr VictimWeak{};
        AFortPlayerStateAthena* Killer = nullptr;
        FWeakObjectPtr KillerWeak{};
        const void* PawnKey = nullptr;
        std::string How;
        int Storm = 0;
        FVector Where{};
        bool bHaveWhere = false;
        float DecideAt = 0.f;
    };

    static std::vector<FSweepDeath> GSweepDeaths;

    static bool WeakStillThere(const FWeakObjectPtr& Weak, const void* Pointer)
    {
        if (!Pointer || Weak.ObjectIndex < 0 || Weak.Get() != (const UObject*)Pointer)
            return false;

        if (VersionInfo.EngineVersion < 5.0)
        {
            auto Item = TUObjectArray::GetItemByIndex(Weak.ObjectIndex);

            if (!Item || (Item->Flags & (0x20000000 | 0x10000000)))
                return false;
        }

        return true;
    }

    static void QueueSweepDeath(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, AFortGameStateAthena* GameState)
    {
        if (!Controller)
            return;

        auto Victim = (AFortPlayerStateAthena*)Controller->PlayerState;
        if (!Victim)
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        FSweepDeath Death;
        Death.Victim = Victim;
        Death.VictimWeak = FWeakObjectPtr((const UObject*)Victim);
        Death.PawnKey = (const void*)Pawn;

        const char* How = "";
        Death.Killer = FindBotKiller((AActor*)Controller, Victim, GameState, &How);
        Death.How = How ? How : "";

        if (Death.Killer)
            Death.KillerWeak = FWeakObjectPtr((const UObject*)Death.Killer);

        Death.Storm = DiedToTheStorm((void*)Controller, GStormTick) ? 1 : 0;

        if (Pawn)
        {
            Death.Where = Pawn->K2_GetActorLocation();
            Death.bHaveWhere = true;
        }
        else
        {
            auto Seen = LastKnownLocation.find((void*)Controller);

            if (Seen != LastKnownLocation.end())
            {
                Death.Where = Seen->second;
                Death.bHaveWhere = true;
            }
        }

        Death.DecideAt = Now + 1.0f;

        static int Reported = 0;
        if (++Reported <= 12)
            LogLine("[ELIM] '%s' is down (the bot code's sweep saw it%s) -- waiting a second for the game's own death report; the "
                "sweep's own reading: %s (%s)%s.",
                SafeNameOf(Victim).c_str(), Pawn ? "" : ", its body already gone",
                Death.Killer ? ("'" + SafeNameOf(Death.Killer) + "'").c_str() : "nobody", Death.How.c_str(),
                Death.Storm ? ", in the storm" : "");

        GSweepDeaths.push_back(Death);
    }

    static void DecideSweepDeaths()
    {
        if (GSweepDeaths.empty())
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        std::vector<FSweepDeath> StillWaiting;

        for (auto& Death : GSweepDeaths)
        {
            if (Now < Death.DecideAt)
            {
                StillWaiting.push_back(Death);
                continue;
            }

            if (!WeakStillThere(Death.VictimWeak, Death.Victim))
            {
                LogLine("[ELIM] A bot the sweep saw go down is gone from the game before it could be decided -- nobody credited.");
                continue;
            }

            FDeathFacts Facts;
            Facts.Victim = Death.Victim;
            Facts.Killer = (Death.Killer && WeakStillThere(Death.KillerWeak, Death.Killer)) ? Death.Killer : nullptr;
            Facts.PawnKey = Death.PawnKey;
            Facts.Storm = Death.Storm;
            Facts.bFromGameReport = false;
            Facts.How = (Death.Killer && !Facts.Killer) ? std::string("the one who damaged it has left the game") : Death.How;
            Facts.Where = Death.Where;
            Facts.bHaveWhere = Death.bHaveWhere;

            SettleElimination(Facts);
        }

        GSweepDeaths.swap(StillWaiting);
    }

    bool SettleEliminationFromGameReport(AFortPlayerStateAthena* Victim, AFortPlayerStateAthena* Killer, AFortPlayerPawnAthena* VictimPawn,
        AFortPlayerPawnAthena* KillerPawn, AActor* DamageCauser)
    {
        FDeathFacts Facts;
        Facts.Victim = Victim;
        Facts.Killer = Killer;
        Facts.VictimPawn = VictimPawn;
        Facts.PawnKey = (const void*)VictimPawn;
        Facts.KillerPawn = KillerPawn;
        Facts.DamageCauser = DamageCauser;
        Facts.Storm = -1;
        Facts.bFromGameReport = true;
        Facts.How = Killer ? "the killer the game named" : "the game named no killer";

        if (!Killer && Victim && Victim->HasOwner() && Victim->Owner)
        {
            auto Hit = BotCodeLastHit.find((void*)Victim->Owner);

            if (Hit != BotCodeLastHit.end() && Hit->second.first && Hit->second.first != (void*)Victim &&
                (GStormTick - Hit->second.second) <= 5 * 30)
            {
                Facts.Killer = (AFortPlayerStateAthena*)Hit->second.first;
                Facts.KillerPawn = PawnOfPlayerState(Facts.Killer);
                Facts.How = "the game named no killer -- the bot that hit it with the bot code's own damage in the 5 seconds before";
            }
        }

        if (VictimPawn)
        {
            Facts.Where = VictimPawn->K2_GetActorLocation();
            Facts.bHaveWhere = true;
        }

        return SettleElimination(Facts);
    }

    static void LogEliminationSummary()
    {
        const int Decided = GElimsByGameReport + GElimsBySweep;

        if (Decided == 0 && GElimsSecondReports == 0)
            return;

        LogLine("[ELIM] Deaths decided: %d (by the game's own death report %d, by the bot code's sweep %d) | kills credited %d | "
            "to nobody %d | second reports of a decided death, ignored %d | \"Eliminated <name>\" popups sent %d, not sent %d | "
            "sweep kills traced to real damage %d.",
            Decided, GElimsByGameReport, GElimsBySweep, GElimsCredited, GElimsToNobody, GElimsSecondReports,
            GElimPopupsSent, GElimPopupsNotSent, GElimsFromLedger);
    }

    void LogFromClient(AFortPlayerControllerAthena* Controller, const char* Text)
    {
        if (!Controller || !Text)
            return;

        struct FBudget
        {
            float WindowStart = -100.f;
            int Used = 0;
            int Dropped = 0;
        };

        static std::map<void*, FBudget> Budgets;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        auto PS = (AFortPlayerStateAthena*)Controller->PlayerState;
        const std::string Who = PS ? SafeNameOf(PS) : std::string("?");

        FBudget& Budget = Budgets[(void*)Controller];

        if (Now - Budget.WindowStart >= 10.f)
        {
            if (Budget.Dropped > 0)
                LogLine("[CLIENT] ('%s' sent %d more line(s) in those 10 seconds -- not written, flood guard.)", Who.c_str(), Budget.Dropped);

            Budget.WindowStart = Now;
            Budget.Used = 0;
            Budget.Dropped = 0;
        }

        if (++Budget.Used > 40)
        {
            Budget.Dropped++;
            return;
        }

        std::string Clean;

        for (const char* p = Text; *p && Clean.size() < 600; p++)
        {
            const unsigned char Ch = (unsigned char)*p;
            Clean += (Ch >= 0x20 && Ch != 0x7F) ? (char)Ch : ' ';
        }

        LogLine("[CLIENT] '%s': %s", Who.c_str(), Clean.c_str());
    }

    static void HandleBotDeath(AFortPlayerControllerAthena* DeadBotController)
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !DeadBotController)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        auto DeadPawn = (AFortPlayerPawnAthena*)DeadBotController->Pawn;

        QueueSweepDeath(DeadBotController, DeadPawn, GameState);

        DropBotLoot(DeadBotController, DeadPawn);

        BotTopDamager.erase((void*)DeadBotController);
        BotLastDamager.erase((void*)DeadBotController);
        BotUnderFireSince.erase((void*)DeadBotController);
        BotAlreadyBuiltWall.erase((void*)DeadBotController);
        LastPlayerDamageTick.erase((void*)DeadBotController);
        LastKnownHealth.erase((void*)DeadBotController);
        LastOutsideZoneTick.erase((void*)DeadBotController);

        ShotAtBy.erase((void*)DeadBotController);
        AggroedByHuman.erase((void*)DeadBotController);

        LandedBots.erase((void*)DeadBotController);
        LastKnownLocation.erase((void*)DeadBotController);
    }

    static bool StillOnTheSpawnIsland(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasGamePhase())
            return false;

        return GameState->GamePhase < 3;
    }

    void ApplyDamageProtection()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS)
                continue;

            auto Owner = (AActor*)PS->Owner;
            if (!Owner)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)Owner)->Pawn;
            if (!Pawn)
                continue;

            HasLanded((void*)Owner, Pawn);

            const bool bKnownBot = PS->HasbIsABot() && PS->bIsABot;
            const bool bKnownHuman = PS->HasbIsABot() && !PS->bIsABot && !NPCs::IsNPC(PS);

            // Fall damage: always off on the spawn island and while a bot is still gliding down from the bus,
            // otherwise it follows the config toggle.
            const bool bBotStillDropping = bKnownBot && !BotHasLanded((void*)Pawn);
            const bool bStopFallDamage = !FConfiguration::bFallDamage || StillOnTheSpawnIsland(GameState) ||
                !AircraftPhaseReached() || bBotStillDropping;

            if (bStopFallDamage)
            {
                if (Pawn->HasbIgnoreNextFallingDamage())
                    Pawn->bIgnoreNextFallingDamage = true;

                Pawn->ResetFallingHeight();

                if (Pawn->HasLastFallDistance())
                    Pawn->LastFallDistance = 0.f;
            }
            else if (Pawn->HasbIgnoreNextFallingDamage() && Pawn->bIgnoreNextFallingDamage)
            {
                // clear it, but only when the pawn is actually on the ground, so the landing from the bus stays safe
                if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasMovementMode() &&
                    Pawn->CharacterMovement->MovementMode == 1)
                    Pawn->bIgnoreNextFallingDamage = false;
            }

            const bool bProtected = !AircraftPhaseReached() || StillOnTheSpawnIsland(GameState);

            static bool bProtectionSeen = false;
            static bool bProtectionLifted = false;

            if (bProtected)
            {
                if (!bProtectionSeen)
                {
                    bProtectionSeen = true;
                    LogLine("[PROTECT] Spawn island: nobody can be damaged until the bus leaves (game phase %d).",
                        GameState->HasGamePhase() ? (int)GameState->GamePhase : -1);
                }

                if (Pawn->HasbCanBeDamaged())
                    Pawn->bCanBeDamaged = false;

                if (Pawn->HasbIsInvulnerable())
                    Pawn->bIsInvulnerable = true;

                const float Health = Pawn->GetHealth();

                if (bKnownBot && Health < 100.f)
                {
                    static int HurtReports = 0;

                    if (++HurtReports <= 5)
                        LogLine("[PROTECT] '%s' lost health on the spawn island even so (%.0f left, invulnerable: %s) -- %s.",
                            SafeNameOf(PS).c_str(), Health,
                            (Pawn->HasbIsInvulnerable() && Pawn->bIsInvulnerable) ? "yes" : "NO",
                            Health > 0.f ? "healed back to 100" : "it is dead, the protection did NOT stop it");
                }

                if (!bKnownHuman && Health > 0.f && Health < 100.f)
                {
                    float Full = 100.f;
                    Pawn->SetHealth(Full);
                }

                continue;
            }

            if (bProtectionSeen && !bProtectionLifted)
            {
                bProtectionLifted = true;
                LogLine("[PROTECT] The bus is in the air (game phase %d) -- spawn island protection off, everyone can be damaged from here.",
                    GameState->HasGamePhase() ? (int)GameState->GamePhase : -1);
            }

            if (bKnownBot && !BotHasLanded((void*)Pawn))
            {
                // Bots stay unhurtable until they touch down (capped at 150 s so a stuck bot can't be immortal forever).
                static std::map<void*, int> FirstUnlandedTick;
                auto SeenAt = FirstUnlandedTick.find((void*)Owner);

                if (SeenAt == FirstUnlandedTick.end())
                    SeenAt = FirstUnlandedTick.emplace((void*)Owner, GStormTick).first;

                if ((GStormTick - SeenAt->second) < 150 * 30)
                {
                    if (Pawn->HasbIsInvulnerable())
                        Pawn->bIsInvulnerable = true;
                    if (Pawn->HasbCanBeDamaged())
                        Pawn->bCanBeDamaged = false;

                    continue;
                }
            }

            if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
                Pawn->bCanBeDamaged = true;

            if (Pawn->HasbIsInvulnerable())
            {
                if (bKnownHuman && FConfiguration::bGodModeForRealPlayer)
                {
                    Pawn->bIsInvulnerable = true;
                }
                else if (Pawn->bIsInvulnerable)
                {
                    Pawn->bIsInvulnerable = false;

                    if (bKnownBot)
                        GBotsMadeKillableAgain++;
                }
            }
        }

        static bool bAnnounced = false;
        if (!bAnnounced)
        {
            bAnnounced = true;
            LogLine("[PROTECT] Fall damage off for everyone, and nothing takes damage until the aircraft phase "
                "starts. From that moment nobody is invulnerable and everyone can be shot out of the sky.");
        }

        static int LastReported = 0;
        if (GBotsMadeKillableAgain > LastReported)
        {
            LastReported = GBotsMadeKillableAgain;
            if (LastReported <= 10)
                LogLine("[PROTECT] Found a bot stuck on god mode and made it killable again (%d so far).", LastReported);
        }
    }

    void RemoveDeadBots()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray() || !GameMode->HasAlivePlayers())
            return;

        DecideSweepDeaths();

        int removed = 0;
        float lowestHealthSeen = 100.0f;
        int botsChecked = 0;

        {
            std::set<void*> presentNow;
            for (int j = 0; j < GameMode->AlivePlayers.Num(); j++)
            {
                auto Ctrl = (AFortPlayerControllerAthena*)GameMode->AlivePlayers[j];
                if (!Ctrl || !Ctrl->PlayerState || !Ctrl->PlayerState->bIsABot || NPCs::IsNPC(Ctrl))
                    continue;

                auto P = (AFortPlayerPawnAthena*)Ctrl->Pawn;
                if (P)
                {
                    HasLanded((void*)Ctrl, P);
                    LastKnownLocation[(void*)Ctrl] = P->K2_GetActorLocation();
                }
                bool bAlive = ((P != nullptr) && (P->GetHealth() > 0.f)) || BotIsBeingFixed((void*)Ctrl);

                if (bAlive)
                    presentNow.insert((void*)Ctrl);
            }

            if (!PlayersAreProtected(GameState))
            {
                for (auto prev : SeenAliveBots)
                {
                    if (presentNow.find(prev) != presentNow.end())
                        continue;
                    if (EliminatedBots.find(prev) != EliminatedBots.end())
                        continue;

                    if (LandedBots.find(prev) == LandedBots.end())
                        continue;
                    if (((AFortPlayerControllerAthena*)prev)->IsInAircraft())
                        continue;   // the game put it on the bus: not dead

                    EliminatedBots.insert(prev);
                    removed++;

                    HandleBotDeath((AFortPlayerControllerAthena*)prev);
                }
            }

            SeenAliveBots = presentNow;
        }

        for (int j = GameMode->AlivePlayers.Num() - 1; j >= 0; j--)
        {
            auto Controller = (AFortPlayerControllerAthena*)GameMode->AlivePlayers[j];
            if (!Controller || !Controller->PlayerState || !Controller->PlayerState->bIsABot || NPCs::IsNPC(Controller))
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (Pawn)
            {
                HasLanded((void*)Controller, Pawn);
                botsChecked++;
                float health = Pawn->GetHealth();
                if (health < lowestHealthSeen)
                    lowestHealthSeen = health;
            }

            bool bDead = (!Pawn || Pawn->GetHealth() <= 0.0f) && !PlayersAreProtected(GameState) &&
                LandedBots.find((void*)Controller) != LandedBots.end() && !BotIsBeingFixed((void*)Controller) &&
                !Controller->IsInAircraft();

            if (bDead && EliminatedBots.find((void*)Controller) == EliminatedBots.end())
            {
                EliminatedBots.insert((void*)Controller);
                removed++;

                QueueSweepDeath(Controller, Pawn, GameState);

                DropBotLoot(Controller, Pawn);

                BotTopDamager.erase((void*)Controller);
                BotLastDamager.erase((void*)Controller);
                LastPlayerDamageTick.erase((void*)Controller);
                LastKnownHealth.erase((void*)Controller);
                LastOutsideZoneTick.erase((void*)Controller);
                ShotAtBy.erase((void*)Controller);
                LandedBots.erase((void*)Controller);

                if (Pawn)
                {
                    BotLastZoneDistance.erase((void*)Pawn);
                    BotZoneMoveTick.erase((void*)Pawn);
                    BotFire.erase((void*)Pawn);
                    BotAim.erase((void*)Pawn);
                    BotFacingYaw.erase((void*)Pawn);
                    BotReloads.erase((void*)Pawn);
                }

                {
                    auto LastAt = LastKnownLocation.find((void*)Controller);

                    LogLine("[BotAI] Bot eliminated | pawn: %s, health %.0f | game phase %d | last Z=%.0f | outside the circle: %s",
                        Pawn ? "yes" : "no", Pawn ? Pawn->GetHealth() : -1.f,
                        GameState->HasGamePhase() ? (int)GameState->GamePhase : -1,
                        LastAt != LastKnownLocation.end() ? (float)LastAt->second.Z : -1.f,
                        (Pawn && Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone) ? "YES" : "no");
                }
            }
        }

        static int reportCounter = 0;
        if (((++reportCounter) % 300) == 0 && botsChecked > 0)
            LogLine("[BotAI] RemoveDeadBots: %d bot(s) checked, lowest health seen = %.1f, removed this pass = %d.\n",
                botsChecked, lowestHealthSeen, removed);
    }

    void GiveBotPickaxes()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        static auto DefaultPickaxe = FindObject<UFortItemDefinition>(
            L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

        static bool bReported = false;
        if (!bReported)
        {
            bReported = true;
            LogLine("[BotAI] Default pickaxe asset: %s\n", DefaultPickaxe ? "found" : "NOT FOUND");
        }
        if (!DefaultPickaxe)
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto OwnerActor = (AActor*)PlayerState->Owner;
            if (!OwnerActor)
                continue;

            if (PickaxeGiven.find((void*)OwnerActor) != PickaxeGiven.end())
                continue;

            AFortInventory* Inventory = nullptr;

            if (OwnerActor->IsA<AFortPlayerControllerAthena>())
            {
                Inventory = ((AFortPlayerControllerAthena*)OwnerActor)->WorldInventory;
            }
            else
            {
                Inventory = EnsureBotInventory(OwnerActor);
            }

            if (!Inventory)
            {
                static int noInvReported = 0;
                if (++noInvReported <= 3)
                    LogLine("[BotAI] Could not create an inventory for bot yet (no real player joined?).");
                continue;
            }

            PickaxeGiven.insert((void*)OwnerActor);
            Inventory->GiveItem(DefaultPickaxe);

            static int given = 0;
            if (++given <= 3)
                LogLine("[BotAI] Gave pickaxe to bot #%d.\n", given);
        }
    }

    static uint8 AllocateFreeTeam(AFortGameStateAthena* GameState)
    {
        for (int attempt = 0; attempt < 251; attempt++)
        {
            if (AFortGameMode::CurrentTeam < 3 || AFortGameMode::CurrentTeam > 253)
                AFortGameMode::CurrentTeam = 3;

            const uint8 candidate = AFortGameMode::CurrentTeam;

            AFortGameMode::CurrentTeam++;
            AFortGameMode::PlayersOnCurTeam = 0;

            bool bTaken = false;
            if (GameState->HasPlayerArray())
            {
                for (int i = 0; i < GameState->PlayerArray.Num(); i++)
                {
                    auto Other = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                    if (!Other || !Other->HasTeamIndex())
                        continue;

                    if ((uint8)Other->TeamIndex == candidate)
                    {
                        bTaken = true;
                        break;
                    }
                }
            }

            if (!bTaken)
                return candidate;
        }

        return 3;
    }

    void AssignBotTeams()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Ctrl = (AActor*)PS->Owner;
            if (!Ctrl)
                continue;

            if (BotTeamsAssigned.find((void*)Ctrl) != BotTeamsAssigned.end())
                continue;
            BotTeamsAssigned.insert((void*)Ctrl);

            uint8 team = AllocateFreeTeam(GameState);

            uint8 oldTeam = PS->HasTeamIndex() ? PS->TeamIndex : 0;

            if (PS->HasTeamIndex())
                PS->TeamIndex = team;
            if (PS->HasSquadId())
            {
                PS->SquadId = team;
                PS->OnRep_SquadId();
            }

            static int reported = 0;
            if (++reported <= 5)
                LogLine("[TEAM] Bot assigned team %d (was %d).", (int)team, (int)oldTeam);
        }
    }

    static const UFortItemDefinition* FindItemDefByName(const wchar_t* name)
    {
        if (!name || !*name)
            return nullptr;

        static std::map<std::wstring, const UFortItemDefinition*> cache;
        static std::map<std::wstring, std::chrono::steady_clock::time_point> missedAt;

        auto cached = cache.find(name);
        if (cached != cache.end())
            return cached->second;

        const auto NowTp = std::chrono::steady_clock::now();

        auto missed = missedAt.find(name);
        if (missed != missedAt.end() &&
            std::chrono::duration_cast<std::chrono::seconds>(NowTp - missed->second).count() < 30)
            return nullptr;

        std::wstring n(name);
        const UFortItemDefinition* found = nullptr;

        static auto AnyClass = UObject::StaticClass();

        auto TryPath = [&](const std::wstring& Path) -> bool
            {
                found = (const UFortItemDefinition*)FindObject(Path.c_str(), AnyClass);
                return found != nullptr;
            };

        auto StartsWith = [&](const wchar_t* Prefix) -> bool
            {
                return n.rfind(Prefix, 0) == 0;
            };

        const char* How = "";

        if (n[0] == L'/')
        {

            if (TryPath(n))
                How = "full path";
        }
        else
        {

            if (TryPath(n + L"." + n))
                How = "short path";

            std::wstring Likely;

            if (StartsWith(L"AthenaAmmoData") || StartsWith(L"AmmoData"))
                Likely = L"/Game/Athena/Items/Ammo/";
            else if (n == L"WoodItemData" || n == L"StoneItemData" || n == L"MetalItemData")
                Likely = L"/Game/Items/ResourcePickups/";
            else if (StartsWith(L"Athena_") && n.size() > 7)
                Likely = L"/Game/Athena/Items/Consumables/" + n.substr(7) + L"/";
            else if (StartsWith(L"WID_"))
                Likely = L"/Game/Athena/Items/Weapons/";

            if (!found && !Likely.empty() && TryPath(Likely + n + L"." + n))
                How = "its own folder";

            if (!found)
            {
                static const wchar_t* folders[] = {
                    L"/Game/Athena/Items/Weapons/",
                    L"/Game/Items/Weapons/",
                    L"/Game/Weapons/",
                    L"/Game/Athena/Items/Consumables/",
                    L"/Game/Items/Consumables/",
                    L"/Game/Athena/Items/Ammo/",
                    L"/Game/Items/Ammo/",
                    L"/Game/Items/ResourcePickups/",
                    L"/Game/Athena/Items/",
                };

                for (auto f : folders)
                {
                    if (Likely == f)
                        continue;

                    if (TryPath(std::wstring(f) + n + L"." + n))
                    {
                        How = "folder search";
                        break;
                    }
                }
            }

            if (!found && TryPath(n))
                How = "bare name";

            if (!found)
            {
                std::string narrowName(n.begin(), n.end());
                found = TUObjectArray::FindObject<UFortItemDefinition>(narrowName.c_str());
                if (found)
                    How = "already loaded";
            }
        }

        std::string narrow(n.begin(), n.end());

        if (found)
        {
            cache[n] = found;
            missedAt.erase(n);

            static int hitsReported = 0;
            if (++hitsReported <= 15)
                LogLine("[ASSET] Lookup '%s' -> FOUND (%s)", narrow.c_str(), How);
        }
        else
        {
            missedAt[n] = NowTp;

            static int missesReported = 0;
            if (++missesReported <= 30)
                LogLine("[ASSET] Lookup '%s' -> NOT FOUND in any known folder (will try again in 30s)", narrow.c_str());
        }

        return found;
    }

    static AFortInventory* EnsureBotInventory(AActor* BotControllerActor)
    {
        if (!BotControllerActor)
            return nullptr;

        auto BotCtrl = (AFortAthenaAIBotController*)BotControllerActor;

        if (BotCtrl->HasInventory() && BotCtrl->Inventory)
            return BotCtrl->Inventory;

        static TSubclassOf<AFortInventory> CachedInvClass{};
        static bool bClassResolved = false;

        if (!bClassResolved)
        {
            auto World = UWorld::GetWorld();
            if (World && World->GameState)
            {
                auto GS = (AFortGameStateAthena*)World->GameState;
                if (GS->HasPlayerArray())
                {
                    for (int i = 0; i < GS->PlayerArray.Num(); i++)
                    {
                        auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
                        if (!PS || (PS->HasbIsABot() && PS->bIsABot))
                            continue;

                        auto PC = (AActor*)PS->Owner;
                        if (!PC || !PC->IsA<AFortPlayerControllerAthena>())
                            continue;

                        auto RealPC = (AFortPlayerControllerAthena*)PC;
                        if (RealPC->HasWorldInventoryClass() && RealPC->WorldInventoryClass.ClassPtr)
                        {
                            CachedInvClass = RealPC->WorldInventoryClass;
                            bClassResolved = true;
                            LogLine("[INV] Resolved WorldInventoryClass from the real player -- can now build bot inventories.");
                        }
                        break;
                    }
                }
            }
        }

        if (!CachedInvClass.ClassPtr)
        {
            static bool bTriedDirectClass = false;
            if (!bTriedDirectClass)
            {
                bTriedDirectClass = true;

                auto DirectClass = FindClass("FortInventory");
                if (DirectClass)
                {
                    CachedInvClass = DirectClass;
                    LogLine("[INV] Resolved inventory class directly via FindClass(\"FortInventory\").");
                }
                else
                {
                    LogLine("[INV] FAILED: could not resolve an inventory class either from a player "
                        "controller or via FindClass. Bots cannot carry items.");
                }
            }
        }

        if (!CachedInvClass.ClassPtr)
            return nullptr;

        auto NewInv = UWorld::SpawnActor<AFortInventory>(CachedInvClass, FVector{}, FRotator{}, BotControllerActor);
        if (!NewInv)
        {
            static bool bSpawnFailReported = false;
            if (!bSpawnFailReported)
            {
                bSpawnFailReported = true;
                LogLine("[INV] SpawnActor<AFortInventory> returned null -- class resolved but the actor could not be created.");
            }
            return nullptr;
        }

        if (NewInv->HasInventoryType())
            NewInv->InventoryType = 0;

        if (BotCtrl->HasInventory())
            BotCtrl->Inventory = NewInv;

        static int created = 0;
        if (++created <= 3)
            LogLine("[INV] Created inventory for bot #%d.", created);

        return NewInv;
    }

    enum EBotGunKind
    {
        BotGun_None = 0,
        BotGun_Launcher,
        BotGun_Sniper,
        BotGun_Pistol,
        BotGun_Shotgun,
        BotGun_Smg,
        BotGun_Rifle,
        BotGun_KindCount
    };

    static const char* BotGunKindName(int Kind)
    {
        static const char* Names[] = { "none", "launcher", "sniper", "pistol", "shotgun", "SMG", "rifle" };
        return (Kind >= 0 && Kind < BotGun_KindCount) ? Names[Kind] : "?";
    }

    static std::string LowerObjectName(const UObject* Object)
    {
        std::string Name = Object ? BotObjectName(Object) : std::string();

        for (auto& Ch : Name)
            Ch = (char)tolower((unsigned char)Ch);

        return Name;
    }

    static int BotGunKindOf(const UFortItemDefinition* Def)
    {
        if (!Def || !((const UObject*)Def)->IsA<UFortWeaponItemDefinition>() || ((const UObject*)Def)->IsA<UFortWeaponMeleeItemDefinition>())
            return BotGun_None;

        const std::string Name = LowerObjectName((const UObject*)Def);
        auto Has = [&](const char* Part) { return Name.find(Part) != std::string::npos; };

        if (Name.rfind("wid_", 0) != 0 || Has("harvest") || Has("pickaxe") || Has("trap"))
            return BotGun_None;
        if (Has("grenade") && !Has("launcher"))
            return BotGun_None;

        if (Has("launcher") || Has("rocket") || Has("rpg"))
            return BotGun_Launcher;
        if (Has("sniper"))
            return BotGun_Sniper;
        if (Has("shotgun"))
            return BotGun_Shotgun;
        if (Has("smg") || Has("autoheavy") || Has("scavenger") || Has("machinepistol"))
            return BotGun_Smg;
        if (Has("pistol") || Has("revolver") || Has("handcannon") || Has("sixshooter"))
            return BotGun_Pistol;
        if (Has("assault") || Has("rifle") || Has("minigun") || Has("lmg"))
            return BotGun_Rifle;

        return BotGun_None;
    }

    static int BotGunRarity(const UFortItemDefinition* Def)
    {
        const std::string Name = LowerObjectName((const UObject*)Def);

        if (Name.find("_sr_") != std::string::npos)
            return 5;
        if (Name.find("_vr_") != std::string::npos)
            return 4;
        if (Name.find("_uc_") != std::string::npos)
            return 2;
        if (Name.find("_r_") != std::string::npos)
            return 3;
        if (Name.find("_c_") != std::string::npos)
            return 1;

        return 0;
    }

    static int BotGunScore(const UFortItemDefinition* Def)
    {
        const int Kind = BotGunKindOf(Def);
        return Kind == BotGun_None ? -1 : Kind * 10 + BotGunRarity(Def);
    }

    static int BotGunMagazine(const UFortItemDefinition* Def)
    {
        auto Weapon = Def ? Def->Cast<UFortWeaponItemDefinition>() : nullptr;
        auto Stats = Weapon ? AFortInventory::GetStats(Weapon) : nullptr;

        const int Clip = Stats ? (int)Stats->ClipSize : 0;
        return (Clip > 0 && Clip <= 1000) ? Clip : 1;
    }

    static UFortWorldItemDefinition* BotGunAmmo(const UFortItemDefinition* Def)
    {
        auto World = Def ? Def->Cast<UFortWorldItemDefinition>() : nullptr;
        if (!World || !World->GetFunction("GetAmmoWorldItemDefinition_BP"))
            return nullptr;

        return World->GetAmmoWorldItemDefinition_BP();
    }

    static bool InventoryHolds(AFortInventory* Inventory, const UFortItemDefinition* Def)
    {
        if (!Inventory || !Def)
            return false;

        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            if (Entries.Get(e, FFortItemEntry::Size()).ItemDefinition == Def)
                return true;
        }

        return false;
    }

    static bool GBotLootGroupResolved = false;
    static bool GBotLootGroupFound = false;
    static FName GBotLootGroup{};
    static int GBotKitsFromLoot = 0;
    static int GBotKitsFixed = 0;
    static int GBotLootGuns[BotGun_KindCount] = {};
    static int GBotLootRarity[6] = {};

    static bool ResolveBotLootGroup()
    {
        if (GBotLootGroupResolved)
            return GBotLootGroupFound;

        GBotLootGroupResolved = true;

        static const wchar_t* Candidates[] = { L"Loot_AthenaTreasure", L"Loot_Treasure", L"Loot_AthenaFloorLoot" };

        for (auto Candidate : Candidates)
        {
            FName Group(Candidate);
            auto It = TierDataMap.find((int32)Group.ComparisonIndex);

            if (It != TierDataMap.end() && It->second.Num() > 0)
            {
                GBotLootGroup = Group;
                GBotLootGroupFound = true;

                std::wstring Wide(Candidate);
                LogLine("[LOOT] Bots get their loot from the game's own loot tables: tier group %s (%d tier row(s)) -- the chests' "
                    "lootpool.", std::string(Wide.begin(), Wide.end()).c_str(), (int)It->second.Num());
                return true;
            }
        }

        LogLine("[LOOT] None of this build's chest tier groups (Loot_AthenaTreasure, Loot_Treasure, Loot_AthenaFloorLoot) has any "
            "loot table rows loaded (%zu tier group(s) loaded in all) -- bots get the old fixed kit instead.", TierDataMap.size());
        return false;
    }

    static void RollBotLoot(std::vector<const UFortItemDefinition*>& OutGuns, std::vector<std::pair<const UFortItemDefinition*, int>>& OutOther,
        int& OutRolls)
    {
        OutRolls = 0;

        if (!ResolveBotLootGroup())
            return;

        bool KindTaken[BotGun_KindCount] = {};

        for (int Roll = 0; Roll < 8 && OutGuns.size() < 3; Roll++)
        {
            TArray<FFortItemEntry*> Drops{};
            UFortLootPackage::ChooseLootForContainer(Drops, GBotLootGroup);
            OutRolls++;

            for (int d = 0; d < Drops.Num(); d++)
            {
                FFortItemEntry* Entry = Drops[d];
                if (!Entry)
                    continue;

                const UFortItemDefinition* Def = Entry->ItemDefinition;
                const int Count = Entry->Count;
                free(Entry);

                if (!Def)
                    continue;

                const int Kind = BotGunKindOf(Def);

                if (Kind != BotGun_None)
                {
                    if (!KindTaken[Kind] && OutGuns.size() < 3)
                    {
                        KindTaken[Kind] = true;
                        OutGuns.push_back(Def);
                    }

                    continue;
                }

                if (((const UObject*)Def)->IsA<UFortAmmoItemDefinition>() || ((const UObject*)Def)->IsA<UFortResourceItemDefinition>() ||
                    ((const UObject*)Def)->IsA<UFortWeaponItemDefinition>() || LowerObjectName((const UObject*)Def).find("trap") != std::string::npos)
                    continue;

                if (OutOther.size() < 2)
                    OutOther.push_back({ Def, Count > 0 ? Count : 1 });
            }

            Drops.Free();

            if (Roll >= 2 && OutGuns.size() >= 2)
                break;
        }
    }

    static std::string GiveBotSpareRounds(AFortInventory* Inventory, const UFortItemDefinition* Gun)
    {
        auto Ammo = BotGunAmmo(Gun);
        if (!Ammo)
            return " + no ammo type found";

        if (InventoryHolds(Inventory, Ammo))
            return std::string();

        int Spare = BotGunMagazine(Gun) * 4;
        if (Spare < 6)
            Spare = 6;

        const int MaxStack = Ammo->GetMaxStackSize();
        if (MaxStack > 0 && Spare > MaxStack)
            Spare = MaxStack;

        Inventory->GiveItem(Ammo, Spare, 0);
        return " + " + BotObjectName((const UObject*)Ammo) + " x" + std::to_string(Spare);
    }

    static int GiveBotLootpoolKit(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory)
            return 0;

        std::vector<const UFortItemDefinition*> Guns;
        std::vector<std::pair<const UFortItemDefinition*, int>> Other;
        int Rolls = 0;

        RollBotLoot(Guns, Other, Rolls);

        if (Guns.empty())
            return 0;

        std::string Carried;

        for (auto Gun : Guns)
        {
            const int Clip = BotGunMagazine(Gun);
            Inventory->GiveItem(Gun, 1, Clip);

            GBotLootGuns[BotGunKindOf(Gun)]++;
            GBotLootRarity[BotGunRarity(Gun)]++;

            if (!Carried.empty())
                Carried += ", ";
            Carried += BotObjectName((const UObject*)Gun) + " (" + std::to_string(Clip) + ")";
            Carried += GiveBotSpareRounds(Inventory, Gun);
        }

        for (auto& [Def, Count] : Other)
        {
            int Give = Count;
            const int MaxStack = Def->GetMaxStackSize();

            if (MaxStack > 0 && Give > MaxStack)
                Give = MaxStack;

            Inventory->GiveItem(Def, Give, 0);
            Carried += ", " + BotObjectName((const UObject*)Def) + " x" + std::to_string(Give);
        }

        GBotKitsFromLoot++;

        if (GBotKitsFromLoot <= 12 || (GBotKitsFromLoot % 25) == 0)
            LogLine("[LOOT] '%s' looted %d chest(s)' worth: %s.", SafeNameOf(PlayerState).c_str(), Rolls, Carried.c_str());

        return (int)Guns.size();
    }

    static void LogBotLootSummary()
    {
        if (GBotKitsFromLoot == 0 && GBotKitsFixed == 0)
            return;

        LogLine("[LOOT] Bot kits from the lootpool: %d, the old fixed kit: %d | guns handed out: rifles %d, SMGs %d, shotguns %d, "
            "pistols %d, snipers %d, launchers %d | rarity: common %d, uncommon %d, rare %d, epic %d, legendary %d.",
            GBotKitsFromLoot, GBotKitsFixed, GBotLootGuns[BotGun_Rifle], GBotLootGuns[BotGun_Smg], GBotLootGuns[BotGun_Shotgun],
            GBotLootGuns[BotGun_Pistol], GBotLootGuns[BotGun_Sniper], GBotLootGuns[BotGun_Launcher], GBotLootRarity[1],
            GBotLootRarity[2], GBotLootRarity[3], GBotLootRarity[4], GBotLootRarity[5]);
    }

    static void NoteFixedBotKit(AFortPlayerStateAthena* PlayerState, const char* Why)
    {
        GBotKitsFixed++;

        if (GBotKitsFixed <= 5 || (GBotKitsFixed % 25) == 0)
            LogLine("[LOOT] '%s' got the old fixed kit (%s) -- %d bot(s) so far.", PlayerState ? SafeNameOf(PlayerState).c_str() : "a bot",
                Why ? Why : "the loot tables gave no gun", GBotKitsFixed);
    }

    void GiveBotLoadouts()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        static const wchar_t* FixedWeapons[] = {
            L"WID_Assault_AutoHigh_Athena_SR_Ore_T03",
            L"WID_Shotgun_Standard_Athena_C_Ore_T03",
            L"WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03",
        };
        static const wchar_t* FixedConsumables[] = {
            L"Athena_PurpleStuff",
            L"Athena_PurpleStuff",
            L"Athena_Grenade",
        };

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto OwnerActor = PS->Owner;
            if (!OwnerActor)
                continue;

            if (LoadoutGiven.find((void*)PS) != LoadoutGiven.end())
                continue;
            if (!kBotsSpawnWithGuns)
            {
                LoadoutGiven.insert((void*)PS);
                continue;
            }

            auto Controller = OwnerActor->Cast<AFortPlayerControllerAthena>();
            if (!Controller || !Controller->Pawn)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn)
                continue;

            if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
                continue;

            if (Controller->WorldInventory && GiveBotLootpoolKit(Controller, PS) > 0)
            {
                BotInventories[(void*)PS] = Controller->WorldInventory;
                LoadoutGiven.insert((void*)PS);
                continue;
            }

            auto Inv = EnsureBotInventory(OwnerActor);
            if (Inv)
                BotInventories[(void*)PS] = Inv;

            bool gaveAny = false;
            FGuid primaryGuid{};

            for (auto WName : FixedWeapons)
            {
                auto WeaponDef = FindItemDefByName(WName);
                if (!WeaponDef)
                {
                    std::wstring wn(WName);
                    std::string nn(wn.begin(), wn.end());
                    static int miss = 0;
                    if (miss++ < 8)
                        LogLine("[LOADOUT] Missing weapon '%s' on this build.", nn.c_str());
                    continue;
                }

                int ClipSize = 30;
                if (auto RangedDef = WeaponDef->Cast<UFortWeaponItemDefinition>())
                {
                    if (auto Stats = AFortInventory::GetStats(RangedDef))
                        if (Stats->ClipSize > 0)
                            ClipSize = Stats->ClipSize;
                }

                if (Inv)
                {
                    auto Item = Inv->GiveItem(WeaponDef, 1, ClipSize);
                    if (Item && Item->HasItemEntry() && FFortItemEntry::HasItemGuid())
                    {
                        if (!gaveAny)
                            primaryGuid = Item->ItemEntry.ItemGuid;
                        gaveAny = true;
                    }

                    GiveBotSpareRounds(Inv, WeaponDef);
                }

                if (!gaveAny || primaryGuid.A == 0)
                {
                    auto Equipped = Pawn->EquipWeaponDefinition(WeaponDef, primaryGuid);
                    (void)Equipped;
                }
            }

            if (auto FirstDef = FindItemDefByName(FixedWeapons[0]))
            {
                Pawn->EquipWeaponDefinition(FirstDef, primaryGuid);
                BotWeaponClipSize[(void*)PS] = 30;
            }

            for (auto CName : FixedConsumables)
            {
                auto ConsDef = FindItemDefByName(CName);
                if (!ConsDef)
                {
                    std::wstring wn(CName);
                    std::string nn(wn.begin(), wn.end());
                    static int missc = 0;
                    if (missc++ < 6)
                        LogLine("[LOADOUT] Missing consumable '%s' on this build.", nn.c_str());
                    continue;
                }
                if (Inv)
                    Inv->GiveItem(ConsDef, 1, 0);
            }

            LoadoutGiven.insert((void*)PS);
            NoteFixedBotKit(PS, Controller->WorldInventory ? "the loot tables gave no gun" : "it has no inventory of its own");
        }
    }

    void RegisterBotsAsAlive()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray() || !GameMode->HasAlivePlayers())
            return;

        int newlyAdded = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AActor*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            bool alreadyIn = false;
            for (int j = 0; j < GameMode->AlivePlayers.Num(); j++)
            {
                if (GameMode->AlivePlayers[j] == Controller)
                {
                    alreadyIn = true;
                    break;
                }
            }
            if (!alreadyIn)
            {
                if (!bBusDeparted)
                {
                    GameMode->AlivePlayers.Add(Controller);
                    newlyAdded++;
                }
            }
        }

        if (!bBusDeparted && GameState->HasPlayersLeft())
        {
            int32 realCount = GameMode->AlivePlayers.Num();
            if (GameState->PlayersLeft != realCount)
            {
                GameState->PlayersLeft = realCount;

                GameState->OnRep_PlayersLeft();

                if (GameState->HasTeamsLeft())
                    GameState->TeamsLeft = realCount;
            }
        }

        if (newlyAdded > 0)
            if (!bBusDeparted)
                LogLine("[BotAI] Registered %d new bot(s); player count now %d.\n", newlyAdded, GameMode->AlivePlayers.Num());
    }

    void RenameUnnamedBots()
    {
        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameMode)
            return;

        auto& Players = GameState->PlayerArray;

        static const std::vector<std::string> SpecialNames = {
            "&darkBeast&",
            "90cranker7",
            "A Sweaty Dog",
            "AboveMule633",
            "AimLikeIdaho",
            "AngryDuck51",
            "AthenaOrApollo",
            "AtTheBeach321",
            "BagelBoy82",
            "BatTacos",
            "Beebitme",
            "BellyFlop40",
            "Blackjack31",
            "BliceCake",
            "BlinkImGone44",
            "BoatingIsLife",
            "BobDobaleena",
            "BoldPrediction",
            "BraddleRoyale80",
            "BrainInvader",
            "Brauny Banana",
            "CactusDad80",
            "ColonelChugJug",
            "ConstantThorn",
            "CrazyPea96",
            "CrepeSalad",
            "DoctorLobby92",
            "DoubleDaring",
            "DoubleDuel75",
            "DoubleRainbow96",
            "DrPlanet",
            "DrumGunnar",
            "ElPollo85",
            "Discord.gg/4e0h",
            "FlavorCaptain",
            "FlimsyGoat",
            "FlossPatrol82",
            "Gatordile81",
            "GetItGotItGood",
            "GhostChicken12",
            "GnomeRider16",
            "Gooddoggo80",
            "Goosezilla13",
            "Grandma40",
            "HashtagToad57",
            "HeliumHog",
            "HeyThereFriend81",
            "Hoodwinked12",
            "HotelBlankets",
            "HowAreMy90s",
            "iHazHighGround",
            "JonseyForever35",
            "JustABitEpic",
            "KittyCat80",
            "Kregore73",
            "LetsBePals23",
            "LewtGoblin7",
            "LootTrooper51",
            "LousyCentaur",
            "LuckyNumber4279",
            "McCucumber71",
            "Meshuncle",
            "MintElephant26",
            "Mouthful95",
            "N0nDa1ry",
            "NotAPalidome",
            "Number141",
            "OldWaterBottle28",
            "ParanoidCactus",
            "PortableOx",
            "PositiveFeels",
            "PrancingPwnee",
            "PrinceWombat",
            "PurpleCrayon85",
            "Mehmehmeh",
            "QueenBeet74",
            "SergentSummer",
            "ShadowArrow58",
            "Shepard52",
            "ShieldHorse63",
            "ShootyMcGee40",
            "Neo_loves_overtonight",
            "SilverySilver",
            "SirTricksALot21",
            "SoggyCookie26",
            "SpiffyPowder6866",
            "SteelGoose18",
            "SweetPenguin16",
            "TAgYOuRIt9",
            "TheFreezer21",
            "ThermalDragon39",
            "TimeToGo80",
            "TooManyBeets",
            "WalkInThePark66",
            "WildCactusBob",
            "Wondertail",
            "Yeetman57",
            "AtomicPossum",
            "BananaBandit77",
            "CosmicBadger",
            "DiscoLizard42",
            "ElectricMoose",
            "FancyRaccoon",
            "GiantPanda88",
            "HappyTurnip",
            "IronOstrich",
            "JellyfishKing",
            "KillerToast",
            "LazyRocket",
            "MightyMuffin",
            "NinjaSquid",
            "AthenaFortPlayerController",
            "OrangeLlama",
            "PricklyPear55",
            "QuickQuokka",
            "RobotChicken",
            "SneakyPigeon",
            "TinyTornado",
            "UltraWombat",
            "VelvetShark",
            "WaffleKnight",
            "XenonTurtle",
            "YellowYeti",
            "ZanyZebra",
            "AmberAxolotl",
            "BlueberryBat",
            "CrankyCrab",
            "DizzyDolphin",
            "EmberFox",
            "FrostyFrog",
            "Overtonight",
            "GlowingGoat",
            "HiddenHedgehog",
            "IvoryIbis",
            "JollyJaguar",
            "KoalaKrusader",
            "LunarLemur",
            "MossyMole",
            "NeonNewt",
            "OceanOtter",
            "PinkPenguin",
            "QuietQuail",
            "RedRaccoon",
            "SilverSloth",
            "TurboToucan",
            "UrbanUmbreon",
            "VioletVulture",
            "WanderingWolf",
            "XRayYak",
            "YawningYak",
            "ZippyZorua",
            "AcornArmadillo",
            "BouncyBeaver",
            "CheerfulCheetah",
            "DapperDingo",
            "ElegantEmu",
            "FuzzyFerret",
            "GentleGiraffe",
            "HumbleHippo",
            "IcyIguana",
            "JeffDaKiller",
            "JazzyJackal",
            "KindKangaroo",
            "LuckyLynx",
            "MerryMeerkat",
            "NobleNarwhal",
            "OddOcelot",
            "PlayfulPanda",
            "QuirkyQuoll",
            "RowdyRhino",
            "SleepySeal",
            "TameTapir",
            "UniqueUrial",
            "VigilantVicuna",
            "WiseWalrus",
            "ZestyZebu",
            "BraveBison",
            "CleverCamel",
            "DaringDeer",
            "EagerEagle",
            "CoffeeGuzzler38",
            "FearlessFalcon",
            "GallantGazelle",
            "HonestHawk",
            "InvincibleIbis",
            "JoyfulJay",
            "KeenKiwi",
            "LoyalLion",
            "MysticMoth",
            "NimbleNumbat",
            "OptimisticOwl",
            "PatientParrot",
            "RadiantRaven",
            "SwiftSparrow",
            "TrustyTiger",
            "UpbeatUrchin",
            "ValiantVulture",
            "WildWoodpecker",
            "XenialXerus",
            "YoungYak",
            "ZealousZebra",
            "SoggyWaffle",
            "CrispyBacon",
            "JefferyEinstein.uasset",
            "SpicyNoodle",
            "FrozenPizza",
            "MeltyCheese",
            "CrunchyTaco",
            "ButteryBiscuit",
            "SaltyPretzel",
            "SweetMuffin",
            "SourLemon",
            "TangyOrange",
            "BitterMelon",
            "SavorySteak",
            "JuicyBurger",
            "CreamySoup",
            "ToastyBread",
            "StickyRice",
            "FluffyPancake",
            "GummyBear",
            "ChocolateMoose",
            "VanillaYeti",
            "StrawberrySlime",
            "BlueberryBison",
            "RaspberryRhino",
            "BananaBoat",
            "CherryBomb",
            "GrapeApe",
            "MangoMonkey",
            "PeachPanda",
            "PearParrot",
            "PlumPuppy",
            "KiwiKnight",
            "LemonLlama",
            "LimeLizard",
            "MelonMan",
            "OliveOtter",
            "PapayaPirate",
            "GuavaGoblin",
            "FigFalcon",
            "DateDragon",
            "LarryFizzlepot 2.0",
            "ApricotAngel",
            "CoconutCrab",
            "PecanPilot",
            "WalnutWizard",
            "AlmondArmadillo",
            "CashewCoyote",
            "PistachioPixie",
            "HazelnutHawk",
            "MacadamiaMole",
            "PeanutPanda",
            "CoffeeCat",
            "TeaTiger",
            "SodaShark",
            "JuiceJaguar",
            "MilkMoose",
            "WaterWombat",
            "FireFerret",
            "EarthEmu",
            "WindWolf",
            "StormStag",
            "ThunderTurtle",
            "LightningLynx",
            "SolarSquid",
            "LunarLobster",
            "StarfishSteve",
            "GalaxyGoat",
            "NebulaNewt",
            "CometCrab",
            "MeteorMoth",
            "AsteroidApe",
            "PlanetPigeon",
            "OrbitOtter",
            "RocketRaccoon",
            "CometCoyote",
            "GravityGoose",
            "CosmoCat",
            "AstroAnt",
            "NovaNewt",
            "QuasarQuail",
            "PulsarPug",
            "ShadowSheep",
            "GhostGecko",
            "DonaldMustardPlsComeBack",
            "PhantomPanda",
            "SpiritSparrow",
            "HauntedHamster",
            "SpookySquid",
            "CreepyCrab",
            "MysteriousMole",
            "SecretSquirrel",
            "HiddenHawk",
            "SilentSnake",
            "QuietQuokka",
            "WhisperWolf",
            "MurmurMoth",
            "EchoEmu",
            "BreezeBadger",
            "FogFox",
            "MistMoose",
            "CloudCoyote",
            "RainRaccoon",
            "SunnySalamander",
            "CloudyCat",
            "WindyWeasel",
            "RainyRabbit",
            "SnowySeal",
            "StormySwan",
            "FoggyFerret",
            "MistyMouse",
            "DustyDingo",
            "SandySquirrel",
            "MuddyMule",
            "RockyRaccoon",
            "PebblePenguin",
            "BoulderBear",
            "CrystalCrab",
            "DiamondDove",
            "RubyRabbit",
            "EmeraldEel",
            "SapphireSnake",
            "TopazTiger",
            "BronzeBadger",
            "SilverShark",
            "GoldenGoose",
            "PlatinumPanda",
            "CopperCoyote",
            "IronIguana",
            "SteelSeal",
            "TitaniumTurtle",
            "ChromeChicken",
            "NickelNewt",
            "BrassBeaver",
            "CobaltCat",
            "ZincZebra",
            "LeadLemur",
            "MercuryMoth",
            "NeonNarwhal",
            "ArgonApe",
            "HeliumHedgehog",
            "OxygenOtter",
            "CarbonCrab",
            "PixelPanda",
            "VectorVulture",
            "MatrixMole",
            "BinaryBear",
            "CyberCat",
            "DigitalDog",
            "AnalogAnt",
            "LaserLizard",
            "PlasmaPig",
            "QuantumQuail",
            "NanoNewt",
            "MicroMoose",
            "SophieRain",
            "MegaMoth",
            "GigaGoat",
            "TerraTiger",
            "ByteBadger",
            "DataDingo",
            "CodeCoyote",
            "GlitchGoose",
            "BugBunny",
            "AlphaAlpaca",
            "BetaBadger",
            "GammaGoose",
            "DeltaDuck",
            "EpsilonEmu",
            "ZetaZebra",
            "EtaEagle",
            "ThetaTiger",
            "IotaIbis",
            "KappaKoala",
            "LambdaLion",
            "MuMoose",
            "NuNewt",
            "XiXerus",
            "OmicronOwl",
            "PiPanda",
            "RhoRabbit",
            "SigmaSnake",
            "TauTurtle",
            "UpsilonUrchin",
            "PhiFox",
            "ChiCheetah",
            "PsiPenguin",
            "OmegaOtter",
            "PrimePanda",
            "ZeroZebra",
            "OneOtter",
            "Four_E_Zero_H",
            "TwoTiger",
            "ThreeThrush",
            "FourFox",
            "FiveFalcon",
            "SixSnake",
            "SevenSeal",
            "EightEagle",
            "NineNewt",
            "TenTurtle",
            "ElevenEmu",
            "TwelveToucan",
            "LuckyLlama",
            "HappyHippo",
            "GrumpyGoat",
            "SleepySloth",
            "DopeyDog",
            "BashfulBear",
            "SneezySnake",
            "DocDuck",
            "HappyHawk",
            "FunnyFox",
            "SillySeal",
            "CrazyCrab",
            "WackyWombat",
            "GoofyGoose",
            "ClumsyCat",
            "BouncyBear",
            "JumpyJaguar",
            "SkippySquid",
            "HoppingHare",
            "LeapingLizard",
            "DashingDeer",
            "PrancingPony",
            "GallopingGoat",
            "RunningRabbit",
            "FlyingFish",
            "SwimmingSwan",
            "DivingDolphin",
            "ClimbingCat",
            "CrawlingCrab",
            "SlitheringSnake",
            "WaddlingWalrus",
            "MarchingMoose",
            "DancingDingo",
            "SingingSparrow",
            "HummingHawk",
            "WhistlingWolf",
            "LaughingLlama",
            "SmilingSheep",
            "WinkingWolf",
            "BlinkingBat",
            "StaringStag",
            "GazingGoat",
            "NinjaNarwhal",
            "PiratePanda",
            "VikingVulture",
            "KnightKiwi",
            "WizardWeasel",
            "MageMoth",
            "RogueRabbit",
            "RangerRaccoon",
            "ClericCat",
            "DruidDuck",
            "BardBadger",
            "MonkMole",
            "PaladinPug",
            "WarlockWolf",
            "SorcererSnake",
            "AlchemistAnt",
            "NecromancerNewt",
            "SummonerSquid",
            "ArcherArmadillo",
            "HunterHawk",
            "SamuraiSwan",
            "CowboyCoyote",
            "SheriffShark",
            "DeputyDog",
            "MarshalMoose",
            "OutlawOtter",
            "BanditBadger",
            "RustlerRabbit",
            "GunfighterGoose",
            "SniperSparrow",
            "ScoutSquirrel",
            "MedicMoth",
            "EngineerEmu",
            "SoldierSnake",
            "CaptainCrab",
            "MajorMole",
            "GeneralGoat",
            "AdmiralApe",
            "CommanderCat",
            "SergeantSeal",
            "DoctorDove",
            "ProfessorPanda",
            "TeacherTiger",
            "StudentSquid",
            "ScientistSnail",
            "InventorIbis",
            "ExplorerEel",
            "AdventurerAnt",
            "TravelerToucan",
            "WandererWolf",
            "DreamerDuck",
            "ThinkerThrush",
            "BuilderBear",
            "MakerMoose",
            "CrafterCrab",
            "PainterPenguin",
            "SingerSwan",
            "DancerDeer",
            "WriterWren",
            "PlayerPanda",
            "PrintHelloWorld",
            "Alper is the goat",
        };

        static std::vector<std::string> ActiveNames;
        static size_t ActiveNamesUsed = 0;
        static bool bActiveNamesReady = false;

        static const char* AlwaysInTheLobby = "Alper is the goat";

        if (!bActiveNamesReady && !SpecialNames.empty())
        {
            ActiveNames = SpecialNames;

            for (size_t i = ActiveNames.size(); i > 1; --i)
            {
                size_t j = (size_t)(rand() % (int)i);
                std::swap(ActiveNames[i - 1], ActiveNames[j]);
            }
            const size_t Keep = ActiveNames.size() < 99 ? ActiveNames.size() : 99;
            ActiveNames.resize(Keep);

            if (std::find(ActiveNames.begin(), ActiveNames.end(), std::string(AlwaysInTheLobby)) == ActiveNames.end() &&
                !ActiveNames.empty())
                ActiveNames[(size_t)(rand() % (int)ActiveNames.size())] = AlwaysInTheLobby;

            bActiveNamesReady = true;
            LogLine("[NAMES] Picked %zu random names out of %zu for this match ('%s' is always one of them).",
                ActiveNames.size(), SpecialNames.size(), AlwaysInTheLobby);
        }

        for (int i = 0; i < Players.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)Players[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller)
                continue;

            if (AssignedBotNames.find((void*)Controller) != AssignedBotNames.end())
                continue;

            if (ActiveNames.empty())
                continue;

            std::string NewName;
            {
                size_t idx = ActiveNamesUsed % ActiveNames.size();
                size_t pass = ActiveNamesUsed / ActiveNames.size();
                ActiveNamesUsed++;

                NewName = ActiveNames[idx];
                if (pass > 0)
                    NewName += " " + std::to_string(pass + 1);
            }

            AssignedBotNames[(void*)Controller] = NewName;

            std::wstring WideName(NewName.begin(), NewName.end());

            GameMode->ChangeName(Controller, FString(WideName.c_str()), true);
            PlayerState->OnRep_PlayerName();

            static int NamedSoFar = 0;
            const std::string ReadBack = NameAsStored(PlayerState);

            LogLine("[NAMES] Bot #%d is now '%s' (the game reads back '%s'%s).", ++NamedSoFar, NewName.c_str(), ReadBack.c_str(),
                ReadBack == NewName ? "" : " -- NOT the name it was given");
        }
    }

    static const wchar_t* GSeasonOneOutfits[] = {
        L"CID_001_Athena_Commando_F_Default", L"CID_002_Athena_Commando_F_Default", L"CID_003_Athena_Commando_F_Default",
        L"CID_004_Athena_Commando_F_Default", L"CID_005_Athena_Commando_M_Default", L"CID_006_Athena_Commando_M_Default",
        L"CID_007_Athena_Commando_M_Default", L"CID_008_Athena_Commando_M_Default",
        L"CID_009_Athena_Commando_M", L"CID_010_Athena_Commando_M", L"CID_011_Athena_Commando_M", L"CID_012_Athena_Commando_M",
        L"CID_013_Athena_Commando_F", L"CID_014_Athena_Commando_F", L"CID_015_Athena_Commando_F", L"CID_016_Athena_Commando_F",
        L"CID_017_Athena_Commando_M", L"CID_018_Athena_Commando_M", L"CID_019_Athena_Commando_M", L"CID_020_Athena_Commando_M",
        L"CID_021_Athena_Commando_F", L"CID_022_Athena_Commando_F", L"CID_023_Athena_Commando_F", L"CID_024_Athena_Commando_F",
        L"CID_025_Athena_Commando_M", L"CID_026_Athena_Commando_M", L"CID_027_Athena_Commando_F", L"CID_028_Athena_Commando_F",
        L"CID_029_Athena_Commando_F_Halloween", L"CID_030_Athena_Commando_M_Halloween",
    };

    struct FBotOutfit
    {
        const UObject* Hero = nullptr;
        const UObject* Parts[6] = {};
        uint8 Gender = 2;
        uint8 BodyType = 0;
        std::string Name;
    };

    template <typename T>
    static const UObject* SoftArrayAt(TArray<TSoftObjectPtr<T>>& Array, int Index, const UClass* Class)
    {
        const int32 Stride = (VersionInfo.EngineVersion <= 4.16) ? 0x20 : (int32)FSoftObjectPtr::Size();
        return Array.Get(Index, Stride).InternalGet(Class);
    }

    static std::string OutfitPartName(const UObject* Part)
    {
        if (!Part)
            return std::string("none");

        auto Name = Part->Name.ToString();
        return std::string(Name.c_str());
    }

    static uint8 PartBodyTypes(const UObject* Part)
    {
        static int32 Offset = -2;

        if (!Part)
            return 0;

        if (Offset == -2)
            Offset = (int32)Part->GetOffset("BodyTypesPermitted");

        if (Offset <= 0)
            return 0;

        const uint8 Mask = GetFromOffset<uint8>(Part, (uint32)Offset);
        return (Mask >= 1 && Mask <= 7) ? Mask : 0;
    }

    static bool ResolveOneOutfitRaw(const wchar_t* Id, FBotOutfit* Out, int* Reason)
    {
        std::wstring Wide(Id);
        std::wstring Path = L"/Game/Athena/Items/Cosmetics/Characters/" + Wide + L"." + Wide;
        std::string Narrow(Wide.begin(), Wide.end());

        auto CID = (UAthenaCharacterItemDefinition*)FindObject<UAthenaCharacterItemDefinition>(Path.c_str());
        if (!CID)
            CID = (UAthenaCharacterItemDefinition*)TUObjectArray::FindObject<UAthenaCharacterItemDefinition>(Narrow.c_str());

        if (!CID)
        {
            *Reason = 1;
            return false;
        }

        auto Hero = CID->HeroDefinition;
        if (!Hero)
        {
            *Reason = 2;
            return false;
        }

        if (!Hero->HasSpecializations() || Hero->Specializations.Num() <= 0 || Hero->Specializations.Num() > 16)
        {
            *Reason = 3;
            return false;
        }

        Out->Hero = (const UObject*)Hero;
        Out->Name = Narrow;
        Out->Gender = (Narrow.find("_Commando_M") != std::string::npos) ? 1 : 2;

        for (int s = 0; s < Hero->Specializations.Num(); s++)
        {
            auto Spec = (UFortHeroSpecialization*)SoftArrayAt(Hero->Specializations, s, UFortHeroSpecialization::StaticClass());
            if (!Spec || Spec->CharacterParts.Num() <= 0 || Spec->CharacterParts.Num() > 16)
                continue;

            for (int j = 0; j < Spec->CharacterParts.Num(); j++)
            {
                auto Part = (UCustomCharacterPart*)SoftArrayAt(Spec->CharacterParts, j, UCustomCharacterPart::StaticClass());

                if (Part && Part->CharacterPartType < 6 && !Out->Parts[Part->CharacterPartType])
                    Out->Parts[Part->CharacterPartType] = (const UObject*)Part;
            }
        }

        if (!Out->Parts[1])
        {
            *Reason = 4;
            return false;
        }

        return true;
    }

    static bool ResolveOneOutfitGuarded(const wchar_t* Id, FBotOutfit* Out, int* Reason)
    {
        *Reason = 0;

        __try
        {
            return ResolveOneOutfitRaw(Id, Out, Reason);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Reason = 9;
            return false;
        }
    }

    static std::vector<FBotOutfit> GBotOutfits;
    static bool GBotOutfitsResolved = false;
    static std::map<void*, int> BotOutfitOf;
    static std::map<void*, void*> DressedBotPawns;
    static int GBotsDressed = 0;
    static bool GPartChoiceBroken = false;
    static bool GCustomizationBroken = false;
    static bool GDressNotifyBroken = false;

    static void ResolveSeasonOneOutfits()
    {
        if (GBotOutfitsResolved)
            return;

        GBotOutfitsResolved = true;

        static const char* Reasons[] = { "?", "not on this build", "no hero", "no specializations", "no body", "?", "?", "?", "?", "faulted" };

        std::vector<FBotOutfit> Found;
        std::string Skipped;
        int Faults = 0;

        for (auto Id : GSeasonOneOutfits)
        {
            FBotOutfit Outfit{};
            int Reason = 0;

            if (ResolveOneOutfitGuarded(Id, &Outfit, &Reason))
            {
                Found.push_back(Outfit);
                continue;
            }

            if (Reason == 9)
                Faults++;

            std::wstring Wide(Id);
            if (!Skipped.empty())
                Skipped += ", ";
            Skipped += std::string(Wide.begin(), Wide.end()) + " (" + Reasons[(Reason >= 0 && Reason <= 9) ? Reason : 0] + ")";
        }

        const UObject* FemaleHead = nullptr;
        const UObject* MaleHead = nullptr;

        for (auto& Outfit : Found)
        {
            if (!Outfit.Parts[0])
                continue;

            if (Outfit.Gender == 1 && !MaleHead)
                MaleHead = Outfit.Parts[0];
            if (Outfit.Gender == 2 && !FemaleHead)
                FemaleHead = Outfit.Parts[0];
        }

        if (!FemaleHead)
            FemaleHead = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");

        int HeadsLent = 0;

        for (auto& Outfit : Found)
        {
            if (!Outfit.Parts[0])
            {
                Outfit.Parts[0] = (Outfit.Gender == 1) ? MaleHead : FemaleHead;
                if (Outfit.Parts[0])
                    HeadsLent++;
            }

            if (!Outfit.Parts[0])
            {
                if (!Skipped.empty())
                    Skipped += ", ";
                Skipped += Outfit.Name + " (no head)";
                continue;
            }

            if (VersionInfo.EngineVersion <= 4.16)
            {
                uint8 Allowed = 7;
                const uint8 HeadTypes = PartBodyTypes(Outfit.Parts[0]);
                const uint8 BodyTypes = PartBodyTypes(Outfit.Parts[1]);

                if (HeadTypes)
                    Allowed &= HeadTypes;
                if (BodyTypes)
                    Allowed &= BodyTypes;

                Outfit.BodyType = (Allowed & 2) ? 2 : ((Allowed & 1) ? 1 : ((Allowed & 4) ? 4 : 2));
            }

            GBotOutfits.push_back(Outfit);
        }

        if (GBotOutfits.empty())
        {

            FBotOutfit Default{};
            Default.Hero = FindObject<UObject>(L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F");
            if (!Default.Hero)
                Default.Hero = FindObject<UObject>(L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01");
            Default.Parts[0] = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
            Default.Parts[1] = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
            Default.Parts[3] = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");
            Default.Gender = 2;
            Default.BodyType = (VersionInfo.EngineVersion <= 4.16) ? 2 : 0;
            Default.Name = "the default commando";

            if (Default.Parts[0] && Default.Parts[1])
                GBotOutfits.push_back(Default);
        }

        LogLine("[DRESS] %d Season 1 outfit(s) with a head and a body found for the bots (%d borrowed a head from another outfit)%s%s%s.",
            (int)GBotOutfits.size(), HeadsLent,
            GBotOutfits.empty() ? " -- NONE, the bots cannot be dressed" : "",
            Skipped.empty() ? "" : " | not used: ", Skipped.c_str());

        if (Faults > 0)
            LogLine("[DRESS] Reading %d CID(s) faulted -- caught, those outfits are left out.", Faults);

        for (size_t i = 0; i < GBotOutfits.size() && i < 6; i++)
        {
            const auto& Outfit = GBotOutfits[i];

            LogLine("[DRESS]   %s: head %s, body %s, hat %s, backpack %s | %s, body type %d.", Outfit.Name.c_str(),
                OutfitPartName(Outfit.Parts[0]).c_str(), OutfitPartName(Outfit.Parts[1]).c_str(),
                OutfitPartName(Outfit.Parts[2]).c_str(), OutfitPartName(Outfit.Parts[3]).c_str(),
                Outfit.Gender == 1 ? "male" : "female", (int)Outfit.BodyType);
        }
    }

    static bool ChoosePartRaw(AFortPlayerPawnAthena* Pawn, uint8 Slot, const UObject* Part)
    {
        UObject* Chosen = (UObject*)Part;
        Pawn->ServerChoosePart(Slot, Chosen);
        return true;
    }

    static bool ChoosePartGuarded(AFortPlayerPawnAthena* Pawn, uint8 Slot, const UObject* Part)
    {
        __try
        {
            return ChoosePartRaw(Pawn, Slot, Part);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ApplyCustomizationGuarded(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
    {
        __try
        {
            ((void (*)(AActor*, AActor*))ApplyCharacterCustomization)((AActor*)PlayerState, (AActor*)Pawn);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool InvokeNoArgsGuarded(const UObject* Object, const char* FunctionName)
    {
        __try
        {
            InvokeNoArgs(Object, FunctionName);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool InvokeOneByteRaw(const UObject* Object, const char* FunctionName, uint8 Value)
    {
        auto Fn = Object ? Object->GetFunction(FunctionName) : nullptr;
        if (!Fn)
            return false;

        const int32 Size = Fn->GetPropertiesSize();
        if (Size < 1 || Size > 0x100)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));
        Params[0] = Value;

        Object->ProcessEvent(Fn, Params);
        return true;
    }

    static bool InvokeOneByteGuarded(const UObject* Object, const char* FunctionName, uint8 Value, bool* OutFaulted)
    {
        *OutFaulted = false;

        __try
        {
            return InvokeOneByteRaw(Object, FunctionName, Value);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *OutFaulted = true;
            return false;
        }
    }

    static bool SetReflectedBool(UObject* Object, const char* Name)
    {
        auto Prop = Object ? Object->GetProperty(Name, 0x20000) : nullptr;
        if (!Prop)
            return false;

        const uint32 Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
        const uint8 Mask = Prop->GetFieldMask();

        if (!Mask || Offset >= 0x10000)
            return false;

        GetFromOffset<uint8>(Object, Offset) |= Mask;
        return true;
    }

    static void DressBotSeasonOne(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
    {
        if (!PlayerState || !Pawn)
            return;

        ResolveSeasonOneOutfits();
        if (GBotOutfits.empty())
            return;

        int Index;
        auto Picked = BotOutfitOf.find((void*)PlayerState);

        if (Picked != BotOutfitOf.end())
        {
            Index = Picked->second;
        }
        else
        {
            Index = rand() % (int)GBotOutfits.size();
            BotOutfitOf[(void*)PlayerState] = Index;
        }

        const FBotOutfit& Outfit = GBotOutfits[(size_t)Index];

        static int32 StatePartsOffset = -2, PawnPartsOffset = -2, GenderOffset = -2, BodyTypeOffset = -2;

        if (StatePartsOffset == -2)
            StatePartsOffset = (int32)((UObject*)PlayerState)->GetOffset("CharacterParts");
        if (PawnPartsOffset == -2)
            PawnPartsOffset = (int32)((UObject*)Pawn)->GetOffset("CharacterParts");
        if (GenderOffset == -2)
            GenderOffset = (int32)((UObject*)PlayerState)->GetOffset("CharacterGender");
        if (BodyTypeOffset == -2)
            BodyTypeOffset = (int32)((UObject*)PlayerState)->GetOffset("CharacterBodyType");

        if (StatePartsOffset < 0)
        {
            static bool bSaid = false;
            if (!bSaid)
            {
                bSaid = true;
                LogLine("[DRESS] The PlayerState has no CharacterParts on this build -- the bots cannot be dressed.");
            }
            return;
        }

        auto WriteStateParts = [&]()
            {
                auto Slots = (const UObject**)((uint8*)PlayerState + StatePartsOffset);
                for (int s = 0; s < 6; s++)
                    Slots[s] = Outfit.Parts[s];
            };

        WriteStateParts();

        if (PlayerState->HasHeroType() && Outfit.Hero)
        {
            const UObject* Hero = Outfit.Hero;
            PlayerState->HeroType = Hero;
        }

        if (GenderOffset >= 0)
            GetFromOffset<uint8>(PlayerState, (uint32)GenderOffset) = Outfit.Gender;

        if (BodyTypeOffset >= 0 && Outfit.BodyType)
            GetFromOffset<uint8>(PlayerState, (uint32)BodyTypeOffset) = Outfit.BodyType;

        SetReflectedBool((UObject*)PlayerState, "bShowHeroBackpack");
        SetReflectedBool((UObject*)PlayerState, "bShowHeroHeadAccessories");

        int Chosen = 0;
        if (!GPartChoiceBroken)
        {
            for (int s = 0; s < 6; s++)
            {
                if (!Outfit.Parts[s])
                    continue;

                if (!ChoosePartGuarded(Pawn, (uint8)s, Outfit.Parts[s]))
                {
                    GPartChoiceBroken = true;
                    LogLine("[DRESS] The game's ServerChoosePart faulted on a bot -- skipped from now on (the PlayerState's parts still dress them).");
                    break;
                }

                Chosen++;
            }
        }

        if (!GPartChoiceBroken)
        {
            bool bFaulted = false;
            InvokeOneByteGuarded((UObject*)Pawn, "ServerChooseGender", Outfit.Gender, &bFaulted);

            if (bFaulted)
            {
                GPartChoiceBroken = true;
                LogLine("[DRESS] The game's ServerChooseGender faulted on a bot -- part and gender choices skipped from now on.");
            }
        }

        bool bApplied = false;
        if (ApplyCharacterCustomization && !GCustomizationBroken)
        {
            bApplied = ApplyCustomizationGuarded(PlayerState, Pawn);

            if (!bApplied)
            {
                GCustomizationBroken = true;
                LogLine("[DRESS] The game's ApplyCharacterCustomization faulted on a bot -- skipped from now on.");
            }
        }

        WriteStateParts();

        if (!GDressNotifyBroken)
        {
            const bool bOk = InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_HeroType") &&
                InvokeNoArgsGuarded((UObject*)Pawn, "OnRep_PlayerState");

            WriteStateParts();

            const bool bOk2 = bOk &&
                InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_CharacterParts") &&
                InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_ShowHeroBackpack") &&
                InvokeNoArgsGuarded((UObject*)PlayerState, "OnRep_ShowHeroHeadAccessories");

            if (!bOk2)
            {
                GDressNotifyBroken = true;
                LogLine("[DRESS] One of the game's look notifications faulted on a bot -- skipped from now on.");
            }
        }

        if (PawnPartsOffset >= 0)
        {
            auto PawnSlots = (const UObject**)((uint8*)Pawn + PawnPartsOffset);
            for (int s = 0; s < 6; s++)
                PawnSlots[s] = Outfit.Parts[s];
        }

        if (!GDressNotifyBroken)
            InvokeNoArgsGuarded((UObject*)Pawn, "OnCharacterPartsReinitialized");

        PlayerState->ForceNetUpdate();
        Pawn->ForceNetUpdate();

        GBotsDressed++;

        if (GBotsDressed <= 3 || (GBotsDressed % 25) == 0)
        {
            int Parts = 0;
            for (int s = 0; s < 6; s++)
                if (Outfit.Parts[s])
                    Parts++;

            auto Slots = (const UObject**)((uint8*)PlayerState + StatePartsOffset);

            LogLine("[DRESS] '%s' dressed as %s (%d part(s), %d chosen on the pawn, customization %s) | the PlayerState now holds: head %s, "
                "body %s, body type %d -- %d bot(s) dressed so far.",
                SafeNameOf(PlayerState).c_str(), Outfit.Name.c_str(), Parts, Chosen,
                bApplied ? "applied" : (ApplyCharacterCustomization ? "not applied" : "not on this build"),
                OutfitPartName(Slots[0]).c_str(), OutfitPartName(Slots[1]).c_str(),
                BodyTypeOffset >= 0 ? (int)GetFromOffset<uint8>(PlayerState, (uint32)BodyTypeOffset) : -1, GBotsDressed);
        }
    }

    void DressUndressedBots()
    {
        static const std::vector<std::wstring> DefaultSkins = {
            L"/Game/Athena/Items/Cosmetics/Characters/CID_922_Athena_Commando_M_ParcelPrank.CID_922_Athena_Commando_M_ParcelPrank",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_921_Athena_Commando_F_ParcelPetal.CID_921_Athena_Commando_F_ParcelPetal",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_957_Athena_Commando_F_Ponytail.CID_957_Athena_Commando_F_Ponytail",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_925_Athena_Commando_F_TapDance.CID_925_Athena_Commando_F_TapDance",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_891_Athena_Commando_M_LunchBox.CID_891_Athena_Commando_M_LunchBox",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_886_Athena_Commando_M_ChOneRenegade.CID_886_Athena_Commando_M_ChOneRenegade",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_757_Athena_Commando_F_WildCat.CID_757_Athena_Commando_F_WildCat",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_902_Athena_Commando_M_PumpkinPunk.CID_902_Athena_Commando_M_PumpkinPunk",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_903_Athena_Commando_F_Frankie.CID_903_Athena_Commando_F_Frankie",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_919_Athena_Commando_F_BabaYaga.CID_919_Athena_Commando_F_BabaYaga",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_913_Athena_Commando_F_York_D.CID_913_Athena_Commando_F_York_D",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_914_Athena_Commando_F_York_E.CID_914_Athena_Commando_F_York_E",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_917_Athena_Commando_M_DurrburgerSkull.CID_917_Athena_Commando_M_DurrburgerSkull",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_920_Athena_Commando_M_PartyTrooper.CID_920_Athena_Commando_M_PartyTrooper",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_897_Athena_Commando_F_DarkBomberSummer.CID_897_Athena_Commando_F_DarkBomberSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_893_Athena_Commando_F_BlackWidowJacket.CID_893_Athena_Commando_F_BlackWidowJacket",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_892_Athena_Commando_F_VampireCasual.CID_892_Athena_Commando_F_VampireCasual",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_899_Athena_Commando_F_Poison.CID_899_Athena_Commando_F_Poison",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_895_Athena_Commando_M_DeliSandwich.CID_895_Athena_Commando_M_DeliSandwich",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_828_Athena_Commando_F_Valet.CID_828_Athena_Commando_F_Valet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_829_Athena_Commando_M_Valet.CID_829_Athena_Commando_M_Valet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_896_Athena_Commando_F_SpookyNeon.CID_896_Athena_Commando_F_SpookyNeon",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_869_Athena_Commando_F_Cavalry.CID_869_Athena_Commando_F_Cavalry",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_894_Athena_Commando_M_Palespooky.CID_894_Athena_Commando_M_Palespooky",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_871_Athena_Commando_F_StreetFashionGarnet.CID_871_Athena_Commando_F_StreetFashionGarnet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_825_Athena_Commando_F_SportsFashion.CID_825_Athena_Commando_F_SportsFashion",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_868_Athena_Commando_M_Backspin_3U6CA.CID_868_Athena_Commando_M_Backspin_3U6CA",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_852_Athena_Commando_F_BlackWidowCorrupt.CID_852_Athena_Commando_F_BlackWidowCorrupt",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_853_Athena_Commando_F_SniperHoodCorrupt.CID_853_Athena_Commando_F_SniperHoodCorrupt",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_827_Athena_Commando_M_MultibotStealth.CID_827_Athena_Commando_M_MultibotStealth",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_679_Athena_Commando_M_ModernMilitaryEclipse.CID_679_Athena_Commando_M_ModernMilitaryEclipse",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_806_Athena_Commando_F_GreenJacket.CID_806_Athena_Commando_F_GreenJacket",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_544_Athena_Commando_M_Kurohomura.CID_544_Athena_Commando_M_Kurohomura",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_604_Athena_Commando_F_Razor.CID_604_Athena_Commando_F_Razor",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_763_Athena_Commando_F_ShinyJacket.CID_763_Athena_Commando_F_ShinyJacket",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_864_Athena_Commando_F_Elastic_E.CID_864_Athena_Commando_F_Elastic_E",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_840_Athena_Commando_M_HightowerGrape.CID_840_Athena_Commando_M_HightowerGrape",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_841_Athena_Commando_M_HightowerWasabi.CID_841_Athena_Commando_M_HightowerWasabi",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_842_Athena_Commando_F_HightowerHoneydew.CID_842_Athena_Commando_F_HightowerHoneydew",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_843_Athena_Commando_M_HightowerTomato_Casual.CID_843_Athena_Commando_M_HightowerTomato_Casual",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_844_Athena_Commando_F_HightowerMango.CID_844_Athena_Commando_F_HightowerMango",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_845_Athena_Commando_M_HightowerTapas.CID_845_Athena_Commando_M_HightowerTapas",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_846_Athena_Commando_M_HightowerDate.CID_846_Athena_Commando_M_HightowerDate",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_824_Athena_Commando_F_RaiderPink.CID_824_Athena_Commando_F_RaiderPink",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_800_Athena_Commando_M_Robro.CID_800_Athena_Commando_M_Robro",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_822_Athena_Commando_F_Angler.CID_822_Athena_Commando_F_Angler",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_823_Athena_Commando_F_Islander.CID_823_Athena_Commando_F_Islander",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_836_Athena_Commando_M_JonesyFlare.CID_836_Athena_Commando_M_JonesyFlare",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_833_Athena_Commando_F_TripleScoop.CID_833_Athena_Commando_F_TripleScoop",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_832_Athena_Commando_F_AntiLlama.CID_832_Athena_Commando_F_AntiLlama",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_826_Athena_Commando_M_FloatillaCaptain.CID_826_Athena_Commando_M_FloatillaCaptain",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_834_Athena_Commando_M_Axl.CID_834_Athena_Commando_M_Axl",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_798_Athena_Commando_M_JonesyVagabond.CID_798_Athena_Commando_M_JonesyVagabond",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_815_Athena_Commando_F_DurrburgerHero.CID_815_Athena_Commando_F_DurrburgerHero",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_795_Athena_Commando_M_Dummeez.CID_795_Athena_Commando_M_Dummeez",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_796_Athena_Commando_F_Tank.CID_796_Athena_Commando_F_Tank",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_816_Athena_Commando_F_DirtyDocks.CID_816_Athena_Commando_F_DirtyDocks",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_817_Athena_Commando_M_DirtyDocks.CID_817_Athena_Commando_M_DirtyDocks",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_814_Athena_Commando_M_BananaSummer.CID_814_Athena_Commando_M_BananaSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_810_Athena_Commando_M_MilitaryFashionSummer.CID_810_Athena_Commando_M_MilitaryFashionSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_775_Athena_Commando_F_StreetRacerCobraGold.CID_775_Athena_Commando_F_StreetRacerCobraGold",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_813_Athena_Commando_M_TeriyakiAtlantis.CID_813_Athena_Commando_M_TeriyakiAtlantis",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_801_Athena_Commando_F_GolfSummer.CID_801_Athena_Commando_F_GolfSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_802_Athena_Commando_F_HeartBreaker.CID_802_Athena_Commando_F_HeartBreaker",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_797_Athena_Commando_F_Taco.CID_797_Athena_Commando_F_Taco",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_799_Athena_Commando_F_CupidDark.CID_799_Athena_Commando_F_CupidDark",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_786_Athena_Commando_F_CavalryBandit_Ghost.CID_786_Athena_Commando_F_CavalryBandit_Ghost",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_787_Athena_Commando_M_Heist_Ghost.CID_787_Athena_Commando_M_Heist_Ghost",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_803_Athena_Commando_F_SharkSuit.CID_803_Athena_Commando_F_SharkSuit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_808_Athena_Commando_F_ConstellationSun.CID_808_Athena_Commando_F_ConstellationSun",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_805_Athena_Commando_F_PunkDevilSummer.CID_805_Athena_Commando_F_PunkDevilSummer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_847_Athena_Commando_M_Soy_2AS3C.CID_847_Athena_Commando_M_Soy_2AS3C",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_784_Athena_Commando_F_RenegadeRaiderFire.CID_784_Athena_Commando_F_RenegadeRaiderFire",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_749_Athena_Commando_F_GraffitiAssassin.CID_749_Athena_Commando_F_GraffitiAssassin",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_764_Athena_Commando_F_Loofah.CID_764_Athena_Commando_F_Loofah",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_778_Athena_Commando_M_Gator.CID_778_Athena_Commando_M_Gator",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_785_Athena_Commando_F_Python.CID_785_Athena_Commando_F_Python",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_767_Athena_Commando_F_BlackKnight.CID_767_Athena_Commando_F_BlackKnight",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_776_Athena_Commando_M_ProfessorPup.CID_776_Athena_Commando_M_ProfessorPup",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_717_Athena_Commando_F_BlueFlames.CID_717_Athena_Commando_F_BlueFlames",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_771_Athena_Commando_F_OceanRider.CID_771_Athena_Commando_F_OceanRider",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_770_Athena_Commando_F_MechanicalEngineer.CID_770_Athena_Commando_F_MechanicalEngineer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_747_Athena_Commando_M_BadEgg.CID_747_Athena_Commando_M_BadEgg",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_751_Athena_Commando_F_NeonCatSpy.CID_751_Athena_Commando_F_NeonCatSpy",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_726_Athena_Commando_M_TargetPractice.CID_726_Athena_Commando_M_TargetPractice",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_758_Athena_Commando_M_TechExplorer.CID_758_Athena_Commando_M_TechExplorer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_759_Athena_Commando_F_RapVillainess.CID_759_Athena_Commando_F_RapVillainess",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_752_Athena_Commando_M_Comet.CID_752_Athena_Commando_M_Comet",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_753_Athena_Commando_F_Hostile.CID_753_Athena_Commando_F_Hostile",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_703_Athena_Commando_M_Cyclone.CID_703_Athena_Commando_M_Cyclone",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_761_Athena_Commando_M_CycloneSpace.CID_761_Athena_Commando_M_CycloneSpace",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_662_Athena_Commando_M_FlameSkull.CID_662_Athena_Commando_M_FlameSkull",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_748_Athena_Commando_F_Hitman.CID_748_Athena_Commando_F_Hitman",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_736_Athena_Commando_F_DonutDish.CID_736_Athena_Commando_F_DonutDish",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_724_Athena_Commando_M_SpyTech.CID_724_Athena_Commando_M_SpyTech",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_744_Athena_Commando_F_DuckHero.CID_744_Athena_Commando_F_DuckHero",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_741_Athena_Commando_F_HalloweenBunnySpring.CID_741_Athena_Commando_F_HalloweenBunnySpring",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_740_Athena_Commando_F_CardboardCrew.CID_740_Athena_Commando_F_CardboardCrew",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_728_Athena_Commando_M_MinotaurLuck.CID_728_Athena_Commando_M_MinotaurLuck",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_722_Athena_Commando_M_TeriyakiFishAssassin.CID_722_Athena_Commando_M_TeriyakiFishAssassin",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_725_Athena_Commando_F_AgentX.CID_725_Athena_Commando_F_AgentX",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_715_Athena_Commando_F_TwinDark.CID_715_Athena_Commando_F_TwinDark",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_720_Athena_Commando_F_StreetFashionEmerald.CID_720_Athena_Commando_F_StreetFashionEmerald",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_714_Athena_Commando_M_AnarchyAcresFarmer.CID_714_Athena_Commando_M_AnarchyAcresFarmer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_674_Athena_Commando_F_HoodieBandit.CID_674_Athena_Commando_F_HoodieBandit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_697_Athena_Commando_F_GraffitiFuture.CID_697_Athena_Commando_F_GraffitiFuture",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_688_Athena_Commando_F_AgentRogue.CID_688_Athena_Commando_F_AgentRogue",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_695_Athena_Commando_F_DesertOpsCamo.CID_695_Athena_Commando_F_DesertOpsCamo",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_691_Athena_Commando_F_TNTina.CID_691_Athena_Commando_F_TNTina",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_690_Athena_Commando_F_Photographer.CID_690_Athena_Commando_F_Photographer",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_699_Athena_Commando_F_BrokenHeart.CID_699_Athena_Commando_F_BrokenHeart",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_704_Athena_Commando_F_LollipopTrickster.CID_704_Athena_Commando_F_LollipopTrickster",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_625_Athena_Commando_F_PinkTrooper.CID_625_Athena_Commando_F_PinkTrooper",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_623_Athena_Commando_M_Frogman.CID_623_Athena_Commando_M_Frogman",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_683_Athena_Commando_F_TigerFashion.CID_683_Athena_Commando_F_TigerFashion",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_670_Athena_Commando_F_ArcticCamo.CID_670_Athena_Commando_F_ArcticCamo",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_673_Athena_Commando_F_ArcticCamo_Slate.CID_673_Athena_Commando_F_ArcticCamo_Slate",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_605_Athena_Commando_M_TourBus.CID_605_Athena_Commando_M_TourBus",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_577_Athena_Commando_F_CODSquadPlaid.CID_577_Athena_Commando_F_CODSquadPlaid",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_648_Athena_Commando_F_MsAlpine.CID_648_Athena_Commando_F_MsAlpine",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_650_Athena_Commando_F_HolidayPJ_B.CID_650_Athena_Commando_F_HolidayPJ_B",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_634_Athena_Commando_F_GalileoRocket_ARVEH.CID_634_Athena_Commando_F_GalileoRocket_ARVEH",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_616_Athena_Commando_F_CavalryBandit.CID_616_Athena_Commando_F_CavalryBandit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_617_Athena_Commando_F_ForestQueen.CID_617_Athena_Commando_F_ForestQueen",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_614_Athena_Commando_M_MissingLink.CID_614_Athena_Commando_M_MissingLink",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_510_Athena_Commando_F_AngelEclipse.CID_510_Athena_Commando_F_AngelEclipse",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_574_Athena_Commando_F_CubeRockerPunk.CID_574_Athena_Commando_F_CubeRockerPunk",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_586_Athena_Commando_F_PunkDevil.CID_586_Athena_Commando_F_PunkDevil",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_575_Athena_Commando_F_BulletBlue.CID_575_Athena_Commando_F_BulletBlue",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_593_Athena_Commando_F_SoccerZombieA.CID_593_Athena_Commando_F_SoccerZombieA",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_598_Athena_Commando_M_Mastermind.CID_598_Athena_Commando_M_Mastermind",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_583_Athena_Commando_F_NoshHunter.CID_583_Athena_Commando_F_NoshHunter",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_578_Athena_Commando_F_Fisherman.CID_578_Athena_Commando_F_Fisherman",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_571_Athena_Commando_F_Sheath.CID_571_Athena_Commando_F_Sheath",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_546_Athena_Commando_F_TacticalRed.CID_546_Athena_Commando_F_TacticalRed",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_545_Athena_Commando_F_SushiNinja.CID_545_Athena_Commando_F_SushiNinja",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_523_Athena_Commando_F_Cupid.CID_523_Athena_Commando_F_Cupid",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_479_Athena_Commando_F_Davinci.CID_479_Athena_Commando_F_Davinci",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_482_Athena_Commando_F_PizzaPit.CID_482_Athena_Commando_F_PizzaPit",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_530_Athena_Commando_F_BlackMonday_1BV6J.CID_530_Athena_Commando_F_BlackMonday_1BV6J",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_524_Athena_Commando_F_FutureBikerWhite.CID_524_Athena_Commando_F_FutureBikerWhite",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_532_Athena_Commando_F_Punchy.CID_532_Athena_Commando_F_Punchy",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_511_Athena_Commando_M_CubePaintWildCard.CID_511_Athena_Commando_M_CubePaintWildCard",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_519_Athena_Commando_M_RaptorBlackOps.CID_519_Athena_Commando_M_RaptorBlackOps",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_514_Athena_Commando_F_ToxicKitty.CID_514_Athena_Commando_F_ToxicKitty",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_503_Athena_Commando_F_TacticalWoodlandFuture.CID_503_Athena_Commando_F_TacticalWoodlandFuture",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_480_Athena_Commando_F_Bubblegum.CID_480_Athena_Commando_F_Bubblegum",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_497_Athena_Commando_F_WildWest.CID_497_Athena_Commando_F_WildWest",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_483_Athena_Commando_F_GraffitiRemix.CID_483_Athena_Commando_F_GraffitiRemix",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_405_Athena_Commando_F_Masako.CID_405_Athena_Commando_F_Masako",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_028_Athena_Commando_F.CID_028_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_017_Athena_Commando_M.CID_017_Athena_Commando_M",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_029_Athena_Commando_F_Halloween.CID_029_Athena_Commando_F_Halloween",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_022_Athena_Commando_F.CID_022_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_030_Athena_Commando_M_Halloween.CID_030_Athena_Commando_M_Halloween",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_027_Athena_Commando_F.CID_027_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_024_Athena_Commando_F.CID_024_Athena_Commando_F",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_226_Athena_Commando_F_Octoberfest.CID_226_Athena_Commando_F_Octoberfest",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_278_Athena_Commando_M_Yeti.CID_278_Athena_Commando_M_Yeti",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_309_Athena_Commando_M_StreetGoth.CID_309_Athena_Commando_M_StreetGoth",
            L"/Game/Athena/Items/Cosmetics/Characters/CID_333_Athena_Commando_M_Squishy.CID_333_Athena_Commando_M_Squishy",
        };

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        if (!GameState->HasPlayerArray())
            return;

        int DressedThisTick = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            if (!PlayerState->HasCharacterData())
            {
                auto SeasonOneController = (AFortPlayerControllerAthena*)PlayerState->Owner;
                auto SeasonOnePawn = SeasonOneController ? (AFortPlayerPawnAthena*)SeasonOneController->Pawn : nullptr;

                if (SeasonOnePawn && DressedThisTick < 6)
                {
                    auto Dressed = DressedBotPawns.find((void*)SeasonOnePawn);

                    if (Dressed == DressedBotPawns.end() || Dressed->second != (void*)PlayerState)
                    {
                        DressedBotPawns[(void*)SeasonOnePawn] = (void*)PlayerState;
                        DressBotSeasonOne(PlayerState, SeasonOnePawn);
                        DressedThisTick++;
                    }
                }

                continue;
            }

            auto* CharData = &PlayerState->CharacterData;
            if (!CharData)
                continue;

            auto** Parts = (UCustomCharacterPart**)((uint8_t*)CharData + 0x08);

            if (Parts[0] || Parts[1])
                continue;

            auto CID = FindObject<UAthenaCharacterItemDefinition>(DefaultSkins[rand() % DefaultSkins.size()].c_str());
            if (!CID)
            {
                static bool bReported = false;
                if (!bReported) { bReported = true; LogLine("[BotAI] Default CID asset NOT FOUND -- bots will be invisible\n"); }
                continue;
            }

            auto HeroType = CID->HeroDefinition;
            if (!HeroType || !HeroType->HasSpecializations() || HeroType->Specializations.Num() == 0)
                continue;

            auto SpecSoftPtr = HeroType->Specializations[0];
            auto Spec = (UFortHeroSpecialization*)SpecSoftPtr.Get();
            if (!Spec)
                continue;

            for (int j = 0; j < Spec->CharacterParts.Num() && j < 6; j++)
            {
                auto PartSoftPtr = Spec->CharacterParts[j];
                auto Part = (UCustomCharacterPart*)PartSoftPtr.Get();
                if (Part && (int)Part->CharacterPartType < 6)
                {
                    int slot = (int)Part->CharacterPartType;
                    Parts[slot] = Part;
                }
            }

            static UObject* KismetLibCDO = nullptr;
            static bool bKismetLooked = false;
            if (!bKismetLooked)
            {
                bKismetLooked = true;
                KismetLibCDO = (UObject*)FindObject<UObject>(L"/Script/FortniteGame.Default__FortKismetLibrary");
                LogLine("[BotAI] FortKismetLibrary CDO: %s\n", KismetLibCDO ? "found" : "not found");
            }
            if (KismetLibCDO)
            {
                static auto UpdateVisFn = KismetLibCDO->GetFunction("UpdatePlayerCustomCharacterPartsVisualization");
                if (UpdateVisFn)
                    KismetLibCDO->Call<void>(UpdateVisFn, PlayerState);
            }
            PlayerState->OnRep_CharacterData();

            static int dressed = 0;
            if (++dressed <= 3)
                LogLine("[BotAI] Dressed bot #%d via CharacterData path.\n", dressed);
        }
    }

    static std::map<void*, FVector> BotWanderGoals;

    static std::map<void*, FVector> BotAimError;
    static std::map<void*, int> BotAimErrorTick;

    static int GZoneOrdersIssued = 0;
    static int GZoneBotsInStorm = 0;
    static int GZoneBotsClosing = 0;
    static int GZoneBotsStuck = 0;

    static int GWanderUnstuck = 0;

    static void TopUpBotMagazine(void* PlayerStateKey)
    {
        if (!FConfiguration::bBotsNeverRunOutOfAmmo)
            return;

        auto InvIt = BotInventories.find(PlayerStateKey);
        if (InvIt == BotInventories.end() || !InvIt->second)
            return;

        auto WeaponIt = BotWeapons.find(PlayerStateKey);
        if (WeaponIt == BotWeapons.end() || !WeaponIt->second)
            return;

        auto ClipIt = BotWeaponClipSize.find(PlayerStateKey);

        int ClipSize = (ClipIt != BotWeaponClipSize.end() && ClipIt->second > 0) ? ClipIt->second : 30;

        auto Inventory = InvIt->second;
        if (!Inventory->HasInventory())
            return;

        for (int i = 0; i < Inventory->Inventory.ItemInstances.Num(); i++)
        {
            auto Item = Inventory->Inventory.ItemInstances[i];
            if (!Item || !Item->HasItemEntry())
                continue;

            auto& Entry = Item->ItemEntry;

            if (!FFortItemEntry::HasItemDefinition() || Entry.ItemDefinition != WeaponIt->second)
                continue;

            if (!FFortItemEntry::HasLoadedAmmo())
                return;

            int RefillBelow = ClipSize / 3;
            if (RefillBelow < 1)
                RefillBelow = 1;

            if (Entry.LoadedAmmo >= RefillBelow)
                return;

            Entry.LoadedAmmo = ClipSize;
            Inventory->Update(&Entry);
            GMagazinesRefilled++;
            return;
        }
    }

    static bool BotIsHoldingAWeapon(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon())
            return false;

        return Pawn->CurrentWeapon != nullptr;
    }

    static int32* WeaponAmmoCount(AActor* Weapon)
    {
        if (!Weapon)
            return nullptr;

        static int32 Offset = -2;

        if (Offset == -2)
        {
            Offset = (int32)Weapon->GetOffset("AmmoCount");
            LogLine("[RELOAD] The weapon's live round counter (AmmoCount) %s.",
                Offset >= 0 ? "was found -- bots will reload" : "is NOT on this build's weapon class");
        }

        if (Offset < 0)
            return nullptr;

        return &GetFromOffset<int32>(Weapon, (uint32)Offset);
    }

    static bool BotWeaponIsEmpty(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon() || !Pawn->CurrentWeapon)
            return false;

        auto Ammo = WeaponAmmoCount((AActor*)Pawn->CurrentWeapon);

        return Ammo && *Ammo <= 0;
    }

    static int BotMagazineSize(void* PlayerStateKey)
    {
        auto ClipIt = BotWeaponClipSize.find(PlayerStateKey);
        int ClipSize = (ClipIt != BotWeaponClipSize.end() && ClipIt->second > 0) ? ClipIt->second : 30;

        if (ClipSize < 4)
            ClipSize = 4;

        return ClipSize;
    }

    class BAIPerceptionComponent : public UObject
    {
    public:
        UCLASS_COMMON_MEMBERS(BAIPerceptionComponent);

        DEFINE_FUNC(SetSenseEnabled, void);
        DEFINE_FUNC(ForgetAll, void);
    };

    static std::set<void*> BotSensesSilenced;
    static int GSensesSilencedBots = 0;
    static bool GSensesFaulted = false;

    static void SilenceSensesRaw(UObject* Perception, UClass** Classes, int Count)
    {
        auto Senses = (BAIPerceptionComponent*)Perception;

        for (int s = 0; s < Count; s++)
        {
            if (!Classes[s])
                continue;

            UClass* SenseClass = Classes[s];
            bool bEnable = false;
            Senses->SetSenseEnabled(SenseClass, bEnable);
        }

        Senses->ForgetAll();
    }

    static bool SilenceSensesGuarded(UObject* Perception, UClass** Classes, int Count)
    {
        __try
        {
            SilenceSensesRaw(Perception, Classes, Count);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GSensesFaulted = true;
            return false;
        }
    }

    static void SilenceNativeSenses(::AAIController* AIController)
    {
        if (!AIController || GSensesFaulted || BotSensesSilenced.find((void*)AIController) != BotSensesSilenced.end())
            return;

        BotSensesSilenced.insert((void*)AIController);

        static int32 PerceptionOffset = -2;
        static int32 BlueprintPerceptionOffset = -2;

        if (PerceptionOffset == -2)
        {
            PerceptionOffset = (int32)AIController->GetOffset("PerceptionComponent");
            BlueprintPerceptionOffset = (int32)AIController->GetOffset("AIPerception");
        }

        UObject* Perception = nullptr;

        if (PerceptionOffset >= 0)
            Perception = GetFromOffset<UObject*>(AIController, (uint32)PerceptionOffset);

        if (!Perception && BlueprintPerceptionOffset >= 0)
            Perception = GetFromOffset<UObject*>(AIController, (uint32)BlueprintPerceptionOffset);

        if (!Perception)
        {
            static bool bSaid = false;

            if (!bSaid)
            {
                bSaid = true;
                LogLine("[SENSES] This bot controller has no built-in senses to switch off -- nothing to do.");
            }

            return;
        }

        static const char* SenseNames[] = { "AISense_Sight", "AISense_Hearing", "AthenaAISense_Hearing", "AISense_Damage",
                                            "AISense_Team", "AISense_Touch", "AISense_Prediction", "AISense_Blueprint" };
        static const int SenseCount = (int)(sizeof(SenseNames) / sizeof(SenseNames[0]));
        static UClass* SenseClasses[8] = {};
        static int SensesFound = 0;
        static bool bLookedUp = false;

        if (!bLookedUp)
        {
            bLookedUp = true;

            for (int s = 0; s < SenseCount && s < 8; s++)
            {
                SenseClasses[s] = const_cast<UClass*>(FindClass(SenseNames[s]));

                if (SenseClasses[s])
                    SensesFound++;
            }
        }

        if (!SilenceSensesGuarded(Perception, SenseClasses, SenseCount < 8 ? SenseCount : 8))
        {
            LogLine("[SENSES] Switching off the game's own built-in bot senses faulted on this build -- they are left alone "
                "(the bot code still picks its own targets; the aim focus still keeps its bursts on them).");
            return;
        }

        GSensesSilencedBots++;

        if (GSensesSilencedBots == 1)
            LogLine("[SENSES] Switched off the game's own built-in bot senses (%d kinds found: sight, hearing, being shot...). "
                "They fed Epic's own target picker, which kept turning bots to aim at whoever it picked -- only the "
                "bot code's lock-on picks targets now.", SensesFound);
    }

    static int32 GActorInfoOffset = -2;
    static int GActorInfoSearches = 0;
    static uint32 GAscOwnerOffset = 0;
    static int GActorInfoCleared = 0;
    static std::set<void*> GBotsRunOnServer;

    static void FindBotActorInfo(UObject* Asc, AActor* Owner, AActor* Controller)
    {
        GActorInfoOffset = -1;
        GActorInfoSearches++;

        uint32 Size = Asc->Class ? (uint32)Asc->Class->GetPropertiesSize() : 0;
        if (Size < 0x100 || Size > 0x4000)
            Size = 0x1800;

        int Matches = 0;
        int32 First = -1;

        for (uint32 Off = 0x28; Off + 8 <= Size; Off += 8)
        {
            const uint8* Info = *(const uint8* const*)((const uint8*)Asc + Off);
            const uintptr_t Raw = (uintptr_t)Info;

            if (Raw < 0x10000 || Raw > 0x7FFFFFFFFFFFull || (Raw & 7))
                continue;

            if (IsBadReadPtr((void*)Info) || IsBadReadPtr((void*)(Info + 0x27)))
                continue;

            if (*(const int32*)(Info + 0x08) != Owner->Index || *(const int32*)(Info + 0x18) != Controller->Index ||
                *(const int32*)(Info + 0x20) != Asc->Index)
                continue;

            Matches++;

            if (First < 0)
                First = (int32)Off;
        }

        GActorInfoOffset = First;

        if (First >= 0)
            LogLine("[FIRE] Found the bots' ability-system \"actor info\" (at +0x%X in the ability system component; %d match(es), "
                "looked on %d bot(s)) -- once a bot is on the ground, the player controller is taken out of it.",
                (unsigned)First, Matches, GActorInfoSearches);
        else if (GActorInfoSearches >= 5)
            LogLine("[FIRE] The bots' ability-system \"actor info\" was NOT found (looked on 5 bots) -- a bot's trigger still asks a "
                "game client it does not have to fire the gun; the [FIRE] lines below show whether any round goes off.");
    }

    static int LetTheServerRunBotAbilities(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState)
    {
        if (!Controller || !PlayerState || !PlayerState->HasAbilitySystemComponent())
            return 0;

        auto Asc = (UObject*)PlayerState->AbilitySystemComponent;
        if (!Asc)
            return 0;

        AActor* Owner = (AActor*)PlayerState;

        if (GAscOwnerOffset == 0)
        {
            const uint32 Off = Asc->GetOffset("OwnerActor");
            GAscOwnerOffset = (Off == (uint32)-1 || Off == 0) ? (uint32)-1 : Off;
        }

        if (GAscOwnerOffset != (uint32)-1)
        {
            auto Reflected = *(AActor**)((uint8*)Asc + GAscOwnerOffset);

            if (Reflected)
                Owner = Reflected;
        }

        if (GActorInfoOffset < 0 && GActorInfoSearches < 5)
            FindBotActorInfo(Asc, Owner, (AActor*)Controller);

        if (GActorInfoOffset < 0)
            return 0;

        uint8* Info = *(uint8**)((uint8*)Asc + GActorInfoOffset);

        static std::map<void*, void*> KnownReadable;

        if (!Info)
            return 0;

        auto Known = KnownReadable.find((void*)Asc);

        if (Known == KnownReadable.end() || Known->second != (void*)Info)
        {
            if (IsBadReadPtr((void*)Info) || IsBadReadPtr((void*)(Info + 0x27)))
                return 0;

            KnownReadable[(void*)Asc] = (void*)Info;
        }

        if (*(int32*)(Info + 0x08) != Owner->Index || *(int32*)(Info + 0x20) != Asc->Index)
            return 0;

        int32* PlayerControllerWeak = (int32*)(Info + 0x18);

        if (PlayerControllerWeak[0] == Controller->Index)
        {
            PlayerControllerWeak[0] = -1;
            PlayerControllerWeak[1] = 0;
            GActorInfoCleared++;

            if (GBotsRunOnServer.insert((void*)Controller).second && GBotsRunOnServer.size() <= 3)
                LogLine("[FIRE] '%s' is on the ground: the player controller is out of its ability system now (read back: %s), so "
                    "the ability system treats it like the game's own AI and should run its gun on the server -- whether a round "
                    "really goes off is in the [FIRE] Burst lines (%zu bot(s) so far).",
                    SafeNameOf(PlayerState).c_str(), PlayerControllerWeak[0] == -1 ? "cleared" : "NOT cleared",
                    GBotsRunOnServer.size());
        }

        return PlayerControllerWeak[0] == -1 ? 1 : 0;
    }

    static bool GCameraResolved = false;
    static int32 GCamManagerOffset = -1, GCamCacheOffset = -1, GCamStampOffset = -1, GCamPovOffset = -1;
    static int32 GPovLocationOffset = -1, GPovRotationOffset = -1, GPovFovOffset = -1;

    static void KeepBotCameraOnItsEyes(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FRotator Aim)
    {
        if (!Controller || !Pawn)
            return;

        if (!GCameraResolved)
        {

            const uint32 ManagerOff = ((UObject*)Controller)->GetOffset("PlayerCameraManager");
            auto Manager = (ManagerOff != (uint32)-1) ? *(UObject**)((uint8*)Controller + ManagerOff) : nullptr;

            if (ManagerOff != (uint32)-1 && !Manager)
                return;

            GCameraResolved = true;

            auto ToInt = [](uint32 Off) -> int32 { return Off == (uint32)-1 ? -1 : (int32)Off; };

            auto Entry = FindStruct("CameraCacheEntry");
            auto View = FindStruct("MinimalViewInfo");

            GCamManagerOffset = ToInt(ManagerOff);
            GCamCacheOffset = Manager ? ToInt(Manager->GetOffset("CameraCache")) : -1;
            GCamStampOffset = Entry ? ToInt(Entry->GetOffset("TimeStamp")) : -1;
            GCamPovOffset = Entry ? ToInt(Entry->GetOffset("POV")) : -1;
            GPovLocationOffset = View ? ToInt(View->GetOffset("Location")) : -1;
            GPovRotationOffset = View ? ToInt(View->GetOffset("Rotation")) : -1;
            GPovFovOffset = View ? ToInt(View->GetOffset("FOV")) : -1;

            const bool bUsable = GCamManagerOffset >= 0 && GCamCacheOffset >= 0 && GCamStampOffset >= 0 && GCamPovOffset >= 0 &&
                GPovLocationOffset >= 0 && GPovRotationOffset >= 0;

            LogLine("[FIRE] A bot's camera %s.", bUsable ? "can be kept on its eyes while it aims"
                : "could NOT be found on this build -- left where it is");

            if (!bUsable)
                GCamManagerOffset = -1;
        }

        if (GCamManagerOffset < 0)
            return;

        uint8* Manager = *(uint8**)((uint8*)Controller + GCamManagerOffset);
        if (!Manager)
            return;

        uint8* Cache = Manager + GCamCacheOffset;
        uint8* Pov = Cache + GCamPovOffset;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 1.f;

        const FVector At = Pawn->K2_GetActorLocation();
        FVector Eyes(At.X, At.Y, At.Z + 60.0);

        memcpy(Pov + GPovLocationOffset, (const void*)&Eyes, (size_t)FVector::Size());
        memcpy(Pov + GPovRotationOffset, (const void*)&Aim, (size_t)FRotator::Size());

        if (GPovFovOffset >= 0 && !(*(float*)(Pov + GPovFovOffset) >= 1.f))
            *(float*)(Pov + GPovFovOffset) = 90.f;

        *(float*)(Cache + GCamStampOffset) = (Now > 0.f) ? Now : 1.f;
    }

    static int GTriggerMode = 0;
    static int32 GLocalFlagOffset = -2;

    static bool TriggerRaw(AFortPlayerPawnAthena* Pawn, bool bPull)
    {
        if (bPull)
            Pawn->PawnStartFire(0);
        else
            Pawn->PawnStopFire(0);

        return true;
    }

    static bool TriggerGuarded(AFortPlayerPawnAthena* Pawn, bool bPull)
    {
        __try
        {
            return TriggerRaw(Pawn, bPull);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void BotTrigger(AFortPlayerPawnAthena* Pawn, bool bPull)
    {
        if (!Pawn)
            return;

        auto Controller = (UObject*)Pawn->Controller;

        if (GTriggerMode == 1 && Controller && GLocalFlagOffset == -2)
        {
            const uint32 Off = Controller->GetOffset("bIsLocalPlayerController");
            GLocalFlagOffset = (Off == (uint32)-1) ? -1 : (int32)Off;

            if (GLocalFlagOffset < 0)
                LogLine("[FIRE] bIsLocalPlayerController is not on this build's controller -- the trigger stays the plain one.");
        }

        if (GTriggerMode != 1 || !Controller || GLocalFlagOffset < 0 || !IsPlayerControllerBot((AActor*)Controller))
        {
            TriggerRaw(Pawn, bPull);
            return;
        }

        uint8& Local = *((uint8*)Controller + GLocalFlagOffset);
        const uint8 Was = Local;

        Local = 1;
        const bool bOk = TriggerGuarded(Pawn, bPull);
        Local = Was;

        if (!bOk)
        {
            GTriggerMode = 2;
            LogLine("[FIRE] Pulling the trigger with the bot counted as a local player faulted -- back to the plain trigger.");
        }
    }

    struct FBotGunNumbers
    {
        bool bRead = false;
        bool bValid = false;
        float Dmg[4] = {};
        float Rng[4] = {};
        int Pellets = 1;
        float ReloadTime = 0.f;
        float FireRate = 0.f;
        bool bOneRoundAtATime = false;
    };

    static std::map<const void*, FBotGunNumbers> GBotGunNumbers;

    static const FBotGunNumbers& BotGunNumbers(const UFortItemDefinition* Def)
    {
        FBotGunNumbers& Numbers = GBotGunNumbers[(const void*)Def];

        if (Numbers.bRead)
            return Numbers;

        Numbers.bRead = true;

        static const char* Fields[11] = { "DmgPB", "DmgMid", "DmgLong", "DmgMaxRange", "RngPB", "RngMid", "RngLong", "RngMax",
                                          "BulletsPerCartridge", "ReloadTime", "FiringRate" };
        static int32 Offsets[11] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        static int32 OneAtATimeOffset = -1;
        static uint8 OneAtATimeMask = 0;
        static bool bResolved = false;

        if (!bResolved)
        {
            bResolved = true;

            auto Struct = FindStruct("FortRangedWeaponStats");
            int Found = 0;

            for (int f = 0; f < 11; f++)
            {
                const uint32 Off = Struct ? Struct->GetOffset(Fields[f]) : (uint32)-1;
                Offsets[f] = (Off == (uint32)-1 || Off > 0x1000) ? -1 : (int32)Off;

                if (Offsets[f] >= 0)
                    Found++;
            }

            if (auto Prop = Struct ? Struct->GetProperty("bReloadIndividualBullets") : nullptr)
            {
                const uint32 Off = Struct->GetOffset("bReloadIndividualBullets");
                OneAtATimeOffset = (Off == (uint32)-1 || Off > 0x1000) ? -1 : (int32)Off;
                OneAtATimeMask = Prop->GetFieldMask();
            }

            LogLine("[HITS] The weapon data table's fields: %d of 11 found on this build (DmgPB at %d, BulletsPerCartridge at %d, "
                "ReloadTime at %d, FiringRate at %d, bReloadIndividualBullets at %d).",
                Found, Offsets[0], Offsets[8], Offsets[9], Offsets[10], OneAtATimeOffset);
        }

        auto Weapon = Def ? Def->Cast<UFortWeaponItemDefinition>() : nullptr;
        auto Stats = Weapon ? (const uint8*)AFortInventory::GetStats(Weapon) : nullptr;

        if (!Stats)
            return Numbers;

        auto ReadFloat = [&](int f) -> float
            {
                if (Offsets[f] < 0)
                    return 0.f;

                const float Value = *(const float*)(Stats + Offsets[f]);
                return (Value >= 0.f && Value < 1000000.f) ? Value : 0.f;
            };

        for (int f = 0; f < 4; f++)
        {
            Numbers.Dmg[f] = ReadFloat(f);
            Numbers.Rng[f] = ReadFloat(4 + f);
        }

        Numbers.Pellets = (Offsets[8] >= 0) ? *(const int32*)(Stats + Offsets[8]) : 1;
        if (Numbers.Pellets < 1 || Numbers.Pellets > 32)
            Numbers.Pellets = 1;

        Numbers.ReloadTime = ReadFloat(9);
        Numbers.FireRate = ReadFloat(10);
        Numbers.bOneRoundAtATime = OneAtATimeOffset >= 0 && (Stats[OneAtATimeOffset] & (OneAtATimeMask ? OneAtATimeMask : (uint8)0xFF)) != 0;
        Numbers.bValid = Numbers.Dmg[0] >= 1.f && Numbers.Dmg[0] < 1000.f;

        static int Reported = 0;
        if (++Reported <= 12)
            LogLine("[HITS] %s: %.1f damage up to %.0f m, %.1f at %.0f m, %.1f at %.0f m, %.1f at %.0f m | %d pellet(s) a shot | %.2f shots a "
                "second | reload %.2fs%s%s.",
                BotObjectName((const UObject*)Def).c_str(), Numbers.Dmg[0], Numbers.Rng[0] / 100.f, Numbers.Dmg[1], Numbers.Rng[1] / 100.f,
                Numbers.Dmg[2], Numbers.Rng[2] / 100.f, Numbers.Dmg[3], Numbers.Rng[3] / 100.f, Numbers.Pellets, Numbers.FireRate,
                Numbers.ReloadTime, Numbers.bOneRoundAtATime ? " a round" : "",
                Numbers.bValid ? "" : " -- NOT usable, the bot code deals no damage with this gun");

        return Numbers;
    }

    static float BotRoundDamage(const FBotGunNumbers& Numbers, float DistanceUnits)
    {
        if (!Numbers.bValid)
            return 0.f;

        auto Lerp = [](float A, float B, float T) { return A + (B - A) * (T < 0.f ? 0.f : (T > 1.f ? 1.f : T)); };

        const float* D = Numbers.Dmg;
        const float* R = Numbers.Rng;
        float Damage = D[0];

        if (R[0] > 0.f && DistanceUnits > R[0] && D[1] > 0.f && R[1] > R[0])
        {
            if (DistanceUnits <= R[1])
                Damage = Lerp(D[0], D[1], (DistanceUnits - R[0]) / (R[1] - R[0]));
            else if (D[2] > 0.f && R[2] > R[1] && DistanceUnits <= R[2])
                Damage = Lerp(D[1], D[2], (DistanceUnits - R[1]) / (R[2] - R[1]));
            else if (D[2] > 0.f && R[2] > R[1] && D[3] > 0.f && R[3] > R[2])
                Damage = (DistanceUnits <= R[3]) ? Lerp(D[2], D[3], (DistanceUnits - R[2]) / (R[3] - R[2])) : D[3];
            else
                Damage = (D[2] > 0.f && R[2] > R[1]) ? D[2] : D[1];
        }

        if (Numbers.Pellets > 1)
        {
            float Share = 0.95f - DistanceUnits / 2500.f;
            if (Share < 0.2f)
                Share = 0.2f;

            Damage = (Damage * (float)Numbers.Pellets > 300.f) ? Damage * Share : Damage * (float)Numbers.Pellets * Share;
        }

        if (Damage > 150.f)
            Damage = 150.f;

        return Damage;
    }

    enum EBotDamageMode
    {
        BotDamage_Watching = 0,
        BotDamage_ByGame,
        BotDamage_ByCode,
    };

    static int GBotDamageMode[2] = { BotDamage_ByCode, BotDamage_ByCode };   // [0] = bots, [1] = real players
    static int GBotDamageSamples[2] = { 0, 0 };
    static int GBotDamageSamplesLanded[2] = { 0, 0 };
    static int GCodeHits[2] = { 0, 0 };
    static double GCodeDamage[2] = { 0.0, 0.0 };
    static int GCodeKills[2] = { 0, 0 };
    static int GCodeKillsNotByForceKill = 0;
    static int GCodeHitLines = 0;

    static const char* TargetKindName(int Kind)
    {
        return Kind == 1 ? "real players" : "bots";
    }

    static int PawnTargetKind(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->Controller)
            return -1;

        auto State = (AFortPlayerStateAthena*)((AFortPlayerControllerAthena*)Pawn->Controller)->PlayerState;
        if (!State || NPCs::IsNPC(State))
            return -1;

        return (State->HasbIsABot() && State->bIsABot) ? 0 : 1;
    }

    struct FBotShotAim
    {
        void* Target = nullptr;
        bool bOnTarget = false;
        bool bCanShoot = false;
        float Distance = 0.f;
    };

    static std::map<void*, FBotShotAim> BotShotAim;

    static UFunction* GForceKillFn = nullptr;
    static bool GForceKillLooked = false;
    static bool GForceKillBroken = false;
    static uint32 GForceKillController = 0, GForceKillActor = 0;

    static bool ForceKillRaw(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        Pawn->ProcessEvent(Fn, Params);
        return true;
    }

    static bool ForceKillGuarded(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        __try
        {
            return ForceKillRaw(Pawn, Fn, Params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool BotForceKill(AFortPlayerPawnAthena* Target, AFortPlayerControllerAthena* KillerController, AFortPlayerPawnAthena* KillerPawn)
    {
        if (GForceKillBroken || !Target)
            return false;

        if (!GForceKillLooked)
        {
            GForceKillLooked = true;

            auto Fn = ((UObject*)Target)->GetFunction("ForceKill");
            int Found = 0;
            uint32 Size = 0;

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                Size = Params.Size;

                for (auto& P : Params.NameOffsetMap)
                {
                    if (P.Name == "DeathReason" && P.ElementSize == 8)
                        Found |= 1;
                    else if (P.Name == "KillerController" && P.ElementSize == 8)
                    {
                        GForceKillController = P.Offset;
                        Found |= 2;
                    }
                    else if (P.Name == "KillerActor" && P.ElementSize == 8)
                    {
                        GForceKillActor = P.Offset;
                        Found |= 4;
                    }
                }
            }

            if (Fn && Found == 7 && Size > 0 && Size <= 0x100)
                GForceKillFn = Fn;

            LogLine("[HITS] The game's own ForceKill %s.", GForceKillFn ? "was found -- a bot's finishing shot eliminates through it, naming the bot"
                : "is NOT usable on this build -- a bot's finishing shot sets the health to 0");
        }

        if (!GForceKillFn)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        *(void**)(Params + GForceKillController) = (void*)KillerController;
        *(void**)(Params + GForceKillActor) = (void*)KillerPawn;

        if (!ForceKillGuarded((UObject*)Target, GForceKillFn, Params))
        {
            GForceKillBroken = true;
            LogLine("[HITS] The game's ForceKill faulted -- a bot's finishing shot sets the health to 0 from now on.");
            return false;
        }

        return true;
    }

    static UFunction* GApplyDamageFn = nullptr;
    static UObject* GGameplayStaticsCDO = nullptr;
    static bool GApplyDamageLooked = false;
    static bool GApplyDamageBroken = false;
    static uint32 GAD_Damaged = 0, GAD_Damage = 0, GAD_Instigator = 0, GAD_Causer = 0, GAD_Size = 0;

    static bool ApplyDamageGuarded(UFunction* Fn, uint8* Params)
    {
        __try
        {
            GGameplayStaticsCDO->ProcessEvent(Fn, Params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Sends damage through the game's own damage system (hurt sound, indicator, real shield handling).
    static bool BotApplyGameDamage(AFortPlayerPawnAthena* Target, AFortPlayerControllerAthena* ShooterController,
        AFortPlayerPawnAthena* Shooter, float Damage)
    {
        if (GApplyDamageBroken || !Target)
            return false;

        if (!GApplyDamageLooked)
        {
            GApplyDamageLooked = true;

            GGameplayStaticsCDO = (UObject*)FindObject<UObject>(L"/Script/Engine.Default__GameplayStatics");
            auto Fn = GGameplayStaticsCDO ? GGameplayStaticsCDO->GetFunction("ApplyDamage") : nullptr;
            int Found = 0;

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                GAD_Size = Params.Size;

                for (auto& P : Params.NameOffsetMap)
                {
                    if (P.Name == "DamagedActor") { GAD_Damaged = P.Offset;    Found |= 1; }
                    else if (P.Name == "BaseDamage") { GAD_Damage = P.Offset;     Found |= 2; }
                    else if (P.Name == "EventInstigator") { GAD_Instigator = P.Offset; Found |= 4; }
                    else if (P.Name == "DamageCauser") { GAD_Causer = P.Offset;     Found |= 8; }
                }
            }

            if (Fn && Found == 15 && GAD_Size > 0 && GAD_Size <= 0x100)
                GApplyDamageFn = Fn;

            LogLine("[HITS] The game's own ApplyDamage %s.",
                GApplyDamageFn ? "was found -- bot hits go through it (hurt sound, shield)" : "is NOT usable on this build -- bot hits write health and shield directly");
        }

        if (!GApplyDamageFn)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        *(void**)(Params + GAD_Damaged) = (void*)Target;
        *(float*)(Params + GAD_Damage) = Damage;
        *(void**)(Params + GAD_Instigator) = (void*)ShooterController;
        *(void**)(Params + GAD_Causer) = (void*)Shooter;

        const float LifeBefore = Target->GetHealth() + Target->GetShield();

        if (!ApplyDamageGuarded(GApplyDamageFn, Params))
        {
            GApplyDamageBroken = true;
            LogLine("[HITS] The game's ApplyDamage faulted -- bot hits write health and shield directly from now on.");
            return false;
        }

        const float LifeAfter = Target->GetHealth() + Target->GetShield();

        static int Checked = 0;
        if (++Checked <= 6)
            LogLine("[HITS] ApplyDamage test: %.0f damage, health+shield %.0f -> %.0f (%s).", Damage, LifeBefore, LifeAfter,
                LifeAfter < LifeBefore ? "the game took it" : "the game IGNORED it -- falling back");

        return LifeAfter < LifeBefore;
    }

    static UObject* GShieldHitSound = nullptr;
    static UObject* GHealthHitSound = nullptr;
    static UObject* GShieldBreakSound = nullptr;
    // If you know the exact asset names (from the [FX-HIT] log lines), put them here. Empty = auto-guess.
    static const char* kShieldHitOverride = "Cons_Shield_Impacts_Athena_Cue";
    static const char* kHealthHitOverride = "Player_Impact_Flesh_Cue";
    static const char* kShakeOverride = "";      // a camera shake class, e.g. "CS_Damage_C"

    static void DumpNamesMatching(const UObject* Object, const char* Label, const char* const* Keys, int KeyCount);
    static bool GBreakSoundLooked = false;
    static bool GSoundsLooked = false;
    static UFunction* GPlaySoundFn = nullptr;
    static uint32 GPS_Sound = 0, GPS_Volume = 0, GPS_Pitch = 0, GPS_Size = 0;
    static bool GPlaySoundBroken = false;

    static bool PlaySoundGuarded(UObject* Controller, UFunction* Fn, uint8* Params)
    {
        __try
        {
            Controller->ProcessEvent(Fn, Params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static UObject* FindSoundByName(const char* Name);
    static std::string LowerText(std::string S);

    // ---- hit sound test mode ----
    // true  = hits play the NEXT candidate sound (about every 1.5 s) and log its name. Listen for the right one.
    // false = normal play, uses kHealthHitOverride / kShieldHitOverride.
    static const bool kSoundTestMode = false;

    static std::vector<UObject*> GHitCandidates;
    static int GTestIndex = -1;
    static int GTestLastTick = -100000;

    static void FindHitSounds()
    {
        GSoundsLooked = true;

        // names you filled in yourself win
        if (kShieldHitOverride[0])
            GShieldHitSound = FindSoundByName(kShieldHitOverride);
        if (kHealthHitOverride[0])
            GHealthHitSound = FindSoundByName(kHealthHitOverride);

        HitLog("Override health '%s' -> %s | shield '%s' -> %s",
            kHealthHitOverride, GHealthHitSound ? "FOUND" : "none",
            kShieldHitOverride, GShieldHitSound ? "FOUND" : "none");

        // every loaded sound that could be a hit/hurt sound (for test mode)
        static const char* Keys[] = { "player_impact", "flesh", "shield_impact" };

        for (int i = 0; i < TUObjectArray::Num() && GHitCandidates.size() < 200; i++)
        {
            const UObject* Object = TUObjectArray::GetObjectByIndex(i);

            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            const std::string ClassName = Object->Class->Name.ToString().c_str();

            if (ClassName.find("SoundCue") == std::string::npos && ClassName.find("SoundWave") == std::string::npos)
                continue;

            const std::string Lower = LowerText(Object->Name.ToString().c_str());
            bool bMatch = false;

            for (auto Key : Keys)
                if (Lower.find(Key) != std::string::npos) { bMatch = true; break; }

            if (!bMatch)
                continue;

            // footsteps, bullets hitting walls etc. are never the right one
            if (Lower.find("step") != std::string::npos || Lower.find("bullet") != std::string::npos ||
                Lower.find("wall") != std::string::npos || Lower.find("foley") != std::string::npos)
                continue;

            GHitCandidates.push_back((UObject*)Object);
        }

        HitLog("%d candidate hit sound(s) are loaded on this server.", (int)GHitCandidates.size());

        std::string Line;
        int InLine = 0;

        for (auto Candidate : GHitCandidates)
        {
            Line += BotObjectName(Candidate) + ", ";

            if (++InLine >= 8)
            {
                HitLog("candidates: %s", Line.c_str());
                Line.clear();
                InLine = 0;
            }
        }

        if (!Line.empty())
            HitLog("candidates: %s", Line.c_str());
    }

    static UObject* FindSoundByName(const char* Name);
    static std::string LowerText(std::string S);

    // Finds the shield-break sound by name. If you know the real name from the log, put it in kBreakOverride.
    static void FindShieldBreakSound()
    {
        GBreakSoundLooked = true;

        static const char* kBreakOverride = "";   // e.g. "Shield_Break_Cue" once the [SOUND] log shows the real name

        if (kBreakOverride[0])
            GShieldBreakSound = FindSoundByName(kBreakOverride);

        std::string Listed;
        int Count = 0;
        UObject* Pick = nullptr;

        for (int i = 0; !GShieldBreakSound && i < TUObjectArray::Num(); i++)
        {
            const UObject* Object = TUObjectArray::GetObjectByIndex(i);

            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (LowerText(BotClassName(Object)).find("sound") == std::string::npos)
                continue;

            const std::string Name = BotObjectName(Object);
            const std::string Lower = LowerText(Name);

            if (Lower.find("shield") == std::string::npos)
                continue;

            if (Count++ < 40)
                Listed += Name + ", ";

            const bool bBreaky = Lower.find("break") != std::string::npos || Lower.find("down") != std::string::npos ||
                Lower.find("deplet") != std::string::npos || Lower.find("shatter") != std::string::npos ||
                Lower.find("pop") != std::string::npos || Lower.find("destroy") != std::string::npos;

            if (!Pick && bBreaky)
                Pick = (UObject*)Object;
        }

        if (!GShieldBreakSound)
            GShieldBreakSound = Pick;

        LogLine("[SOUND] Shield-break sound: %s | every sound with 'shield' in its name (%d): %s",
            GShieldBreakSound ? BotObjectName(GShieldBreakSound).c_str() : "NONE (the shield-hit sound is used instead)",
            Count, Listed.c_str());
    }

    // Lists every loaded sound that could be a hit/hurt sound, and every camera shake class. Runs once.
    static void DumpHitFeedbackCandidates(UObject* HitController)
    {
        static bool bDone = false;
        if (bDone)
            return;
        bDone = true;

        static const char* SoundKeys[] = { "hit", "damage", "hurt", "pain", "impact", "grunt", "flinch", "shield", "bullet" };
        std::string Line;
        int InLine = 0, Count = 0;

        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            const UObject* Object = TUObjectArray::GetObjectByIndex(i);
            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (LowerText(BotClassName(Object)).find("sound") == std::string::npos)
                continue;

            const std::string Name = BotObjectName(Object);
            const std::string Lower = LowerText(Name);
            bool bMatch = false;

            for (auto Key : SoundKeys)
                if (Lower.find(Key) != std::string::npos) { bMatch = true; break; }

            if (!bMatch || Count >= 120)
                continue;

            Count++;
            Line += Name + ", ";

            if (++InLine >= 10)
            {
                LogLine("[FX-HIT] sounds: %s", Line.c_str());
                Line.clear();
                InLine = 0;
            }
        }

        if (!Line.empty())
            LogLine("[FX-HIT] sounds: %s", Line.c_str());

        LogLine("[FX-HIT] %d sound(s) listed above.", Count);

        Line.clear();
        InLine = 0;
        int Shakes = 0;

        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            const UObject* Object = TUObjectArray::GetObjectByIndex(i);
            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (LowerText(BotClassName(Object)).find("class") == std::string::npos)
                continue;

            const std::string Name = BotObjectName(Object);
            if (LowerText(Name).find("shake") == std::string::npos || Shakes >= 60)
                continue;

            Shakes++;
            Line += Name + ", ";

            if (++InLine >= 10)
            {
                LogLine("[FX-HIT] camera shakes: %s", Line.c_str());
                Line.clear();
                InLine = 0;
            }
        }

        if (!Line.empty())
            LogLine("[FX-HIT] camera shakes: %s", Line.c_str());

        LogLine("[FX-HIT] %d camera shake class(es) listed above.", Shakes);

        static const char* ControllerKeys[] = { "damage", "hit", "shake", "indicator", "hurt", "flinch", "camera", "sound" };
        DumpNamesMatching(HitController, "The hit player's controller", ControllerKeys, 8);
    }

    // ---- camera shake on the hit player's screen ----
    static UFunction* GShakeFn = nullptr;
    static bool GShakeLooked = false, GShakeBroken = false;
    static uint32 GSH_Shake = 0, GSH_Scale = 0;
    static UObject* GShakeClass = nullptr;
    static bool GShakeClassLooked = false;
    static std::map<void*, int> GShakeLastTick;

    static bool PlayShakeGuarded(UObject* Controller, UFunction* Fn, uint8* Params)
    {
        __try
        {
            Controller->ProcessEvent(Fn, Params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void FindHitShake()
    {
        GShakeClassLooked = true;

        if (kShakeOverride[0])
        {
            GShakeClass = (UObject*)TUObjectArray::FindObject<UObject>(kShakeOverride);
        }

        UObject* Best = nullptr;
        int BestRank = 0;

        for (int i = 0; !GShakeClass && i < TUObjectArray::Num(); i++)
        {
            const UObject* Object = TUObjectArray::GetObjectByIndex(i);
            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (LowerText(BotClassName(Object)).find("class") == std::string::npos)
                continue;

            const std::string Lower = LowerText(BotObjectName(Object));
            if (Lower.find("shake") == std::string::npos)
                continue;

            int Rank = 0;
            if (Lower.find("damage") != std::string::npos) Rank = 4;
            else if (Lower.find("hurt") != std::string::npos) Rank = 3;
            else if (Lower.find("hit") != std::string::npos) Rank = 2;
            else if (Lower.find("impact") != std::string::npos) Rank = 1;

            if (Rank > BestRank)
            {
                BestRank = Rank;
                Best = (UObject*)Object;
            }
        }

        if (!GShakeClass)
            GShakeClass = Best;

        LogLine("[FX-HIT] Hit camera shake: %s", GShakeClass ? BotObjectName(GShakeClass).c_str()
            : "NONE found -- read the camera shake list above and put the right one in kShakeOverride");
    }

    static void BotShakeHitScreen(AFortPlayerPawnAthena* Target, float Damage)
    {
        if (GShakeBroken || !Target || !Target->Controller)
            return;

        auto Controller = (UObject*)Target->Controller;

        // several rounds in one tick (a shotgun) shake once, not five times
        int& Last = GShakeLastTick[(void*)Controller];
        if (GStormTick - Last < 3)
            return;
        Last = GStormTick;

        if (!GShakeClassLooked)
            FindHitShake();

        if (!GShakeClass)
            return;

        if (!GShakeLooked)
        {
            GShakeLooked = true;

            // 4.16 calls it ClientPlayCameraShake, newer engines ClientStartCameraShake
            UFunction* Fn = Controller->GetFunction("ClientPlayCameraShake");
            if (!Fn)
                Fn = Controller->GetFunction("ClientStartCameraShake");

            int Found = 0;
            uint32 Size = 0;

            if (Fn)
            {
                auto P = Fn->GetParamsNamed();
                Size = P.Size;

                for (auto& E : P.NameOffsetMap)
                {
                    if (E.Name == "Shake" && E.ElementSize == 8) { GSH_Shake = E.Offset; Found |= 1; }
                    else if (E.Name == "Scale" && E.ElementSize == 4) { GSH_Scale = E.Offset; Found |= 2; }
                }

                if (Found == 3 && Size > 0 && Size <= 0x100)
                    GShakeFn = Fn;
            }

            LogLine("[FX-HIT] The controller's camera shake call %s.", GShakeFn ? "was found -- bots' hits shake the hit player's screen"
                : "is NOT usable on this build -- no camera shake");
        }

        if (!GShakeFn)
            return;

        float Scale = Damage / 25.f;
        if (Scale < 0.4f) Scale = 0.4f;
        if (Scale > 1.5f) Scale = 1.5f;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        *(void**)(Params + GSH_Shake) = (void*)GShakeClass;
        *(float*)(Params + GSH_Scale) = Scale;

        if (!PlayShakeGuarded(Controller, GShakeFn, Params))
        {
            GShakeBroken = true;
            LogLine("[FX-HIT] The camera shake call faulted -- off from now on.");
        }
    }

    // Kind: 0 = health hit, 1 = shield hit, 2 = shield break. Plays on the hit player's own screen.
    static void BotPlayHitFeedback(AFortPlayerPawnAthena* Target, int Kind, float Damage = 20.f)
    {
        if (GPlaySoundBroken || !Target || !Target->Controller)
            return;

        auto Controller = (UObject*)Target->Controller;

        BotShakeHitScreen(Target, Damage);

        if (!GSoundsLooked)
            FindHitSounds();

        if (Kind == 2 && !GBreakSoundLooked)
            FindShieldBreakSound();

        UObject* Sound = nullptr;

        if (kSoundTestMode)
        {
            if (GHitCandidates.empty())
            {
                static bool bSaidEmpty = false;
                if (!bSaidEmpty)
                {
                    bSaidEmpty = true;
                    HitLog("Test mode: NO candidate sounds are loaded -- the right sound is not in memory on the server.");
                }
                return;
            }

            // next candidate every ~1.5 s (45 ticks); a burst keeps replaying the same one so you can hear it
            if (GTestIndex < 0 || (GStormTick - GTestLastTick) >= 30)
            {
                GTestIndex = (GTestIndex + 1) % (int)GHitCandidates.size();
                GTestLastTick = GStormTick;

                HitLog("TEST #%d of %d: %s  (kind %d: 0=health 1=shield 2=shield break)",
                    GTestIndex + 1, (int)GHitCandidates.size(), BotObjectName(GHitCandidates[GTestIndex]).c_str(), Kind);
            }

            Sound = GHitCandidates[GTestIndex];
        }
        else
        {
            if (Kind == 2)
                Sound = GShieldBreakSound ? GShieldBreakSound : (GShieldHitSound ? GShieldHitSound : GHealthHitSound);
            else if (Kind == 1)
                Sound = GShieldHitSound ? GShieldHitSound : GHealthHitSound;
            else
                Sound = GHealthHitSound;

            if (!Sound)
            {
                static int NoSoundLines = 0;
                if (++NoSoundLines <= 3)
                    HitLog("No sound set for kind %d -- fill in kHealthHitOverride / kShieldHitOverride.", Kind);
                return;
            }
        }

        if (!GPlaySoundFn)
        {
            auto Fn = Controller->GetFunction("ClientPlaySound");
            if (!Fn)
            {
                GPlaySoundBroken = true;
                HitLog("ClientPlaySound is NOT on this controller -- cannot play hit sounds this way.");
                return;
            }

            auto Params = Fn->GetParamsNamed();
            GPS_Size = Params.Size;
            int Found = 0;

            for (auto& P : Params.NameOffsetMap)
            {
                if (P.Name == "Sound") { GPS_Sound = P.Offset;  Found |= 1; }
                else if (P.Name == "VolumeMultiplier") { GPS_Volume = P.Offset; Found |= 2; }
                else if (P.Name == "PitchMultiplier") { GPS_Pitch = P.Offset;  Found |= 4; }
            }

            if (Found != 7 || GPS_Size == 0 || GPS_Size > 0x100)
            {
                GPlaySoundBroken = true;
                HitLog("ClientPlaySound has unexpected parameters (found mask %d, size %u).", Found, (unsigned)GPS_Size);
                return;
            }

            GPlaySoundFn = Fn;
            HitLog("ClientPlaySound found and ready.");
        }

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        *(void**)(Params + GPS_Sound) = (void*)Sound;
        *(float*)(Params + GPS_Volume) = 1.f;
        *(float*)(Params + GPS_Pitch) = 1.f;

        if (!PlaySoundGuarded(Controller, GPlaySoundFn, Params))
        {
            GPlaySoundBroken = true;
            HitLog("ClientPlaySound faulted -- no hit sounds from now on.");
            return;
        }

        if (!kSoundTestMode)
        {
            static int Lines = 0;
            if (++Lines <= 10)
                HitLog("Played %s (kind %d).", BotObjectName(Sound).c_str(), Kind);
        }
    }

    static int32 GShieldOffset = -1;
    static int32 GShieldBaseOffset = -1;
    static int32 GHealthOffset = -1;

    // SetShield does nothing on this build, so write the value into the health set ourselves.
    static void ForceShield(AFortPlayerPawnAthena* Pawn, float OldShield, float NewShield)
    {
        Pawn->SetShield(NewShield);

        if (fabsf(Pawn->GetShield() - NewShield) < 0.5f)
            return;   // the game took it

        auto HealthSet = (uint8*)Pawn->HealthSet;
        if (!HealthSet)
            return;

        // we already know where the shield lives
        if (GShieldOffset >= 0)
        {
            *(float*)(HealthSet + GShieldOffset) = NewShield;

            if (GShieldBaseOffset >= 0)
                *(float*)(HealthSet + GShieldBaseOffset) = NewShield;

            return;
        }

        const float HealthBefore = Pawn->GetHealth();

        for (int Off = 0x30; Off < 0x200; Off += 4)
        {
            float& Value = *(float*)(HealthSet + Off);

            if (fabsf(Value - OldShield) > 0.001f)
                continue;

            const float Saved = Value;
            const float BaseBefore = *(float*)(HealthSet + Off - 4);
            Value = NewShield;

            // keep it only if the shield changed AND health did not move
            if (fabsf(Pawn->GetShield() - NewShield) < 0.5f && fabsf(Pawn->GetHealth() - HealthBefore) < 0.5f)
            {
                GShieldOffset = Off;

                // the float just before it was the base value (it matched the old shield too)
                if (fabsf(BaseBefore - OldShield) < 0.001f)
                {
                    GShieldBaseOffset = Off - 4;
                    *(float*)(HealthSet + GShieldBaseOffset) = NewShield;
                }
                LogLine("[SHIELD] SetShield did nothing. Found the shield at HealthSet+0x%X. Floats around it: %.1f | %.1f | %.1f | %.1f.",
                    Off, *(float*)(HealthSet + Off - 4), Value, *(float*)(HealthSet + Off + 4), *(float*)(HealthSet + Off + 8));
                return;
            }

            Value = Saved;   // wrong float, put it back and keep looking
        }

        LogLine("[SHIELD] Could not change shield %.0f -> %.0f by any method.", OldShield, NewShield);
    }

    // same idea for health
    static void ForceHealth(AFortPlayerPawnAthena* Pawn, float OldHealth, float NewHealth)
    {
        Pawn->SetHealth(NewHealth);

        if (fabsf(Pawn->GetHealth() - NewHealth) < 0.5f)
            return;

        auto HealthSet = (uint8*)Pawn->HealthSet;
        if (!HealthSet)
            return;

        if (GHealthOffset >= 0)
        {
            *(float*)(HealthSet + GHealthOffset) = NewHealth;
            return;
        }

        const float ShieldBefore = Pawn->GetShield();

        for (int Off = 0x30; Off < 0x200; Off += 4)
        {
            float& Value = *(float*)(HealthSet + Off);

            if (fabsf(Value - OldHealth) > 0.001f)
                continue;

            const float Saved = Value;
            Value = NewHealth;

            if (fabsf(Pawn->GetHealth() - NewHealth) < 0.5f && fabsf(Pawn->GetShield() - ShieldBefore) < 0.5f)
            {
                GHealthOffset = Off;
                LogLine("[HEALTH] SetHealth did nothing. Found health at HealthSet+0x%X.", Off);
                return;
            }

            Value = Saved;
        }

        LogLine("[HEALTH] Could not change health %.0f -> %.0f by any method.", OldHealth, NewHealth);
    }

    static bool BotHitTarget(AFortPlayerPawnAthena* Shooter, AFortPlayerControllerAthena* ShooterController, AFortPlayerStateAthena* ShooterState,
        AFortPlayerPawnAthena* Target, float Damage, int Kind, const UFortItemDefinition* Gun)
    {
        if (!Target || !(Damage > 0.f) || Kind < 0)
            return false;

        if (Target->HasbIsInvulnerable() && Target->bIsInvulnerable)
        {
            static int InvulnLines = 0;
            if (++InvulnLines <= 5)
                LogLine("[DMG] Hit refused: the target is invulnerable (check bGodModeForRealPlayer in your config).");
            return false;
        }

        const float Health = Target->GetHealth();
        if (Health <= 0.f)
            return true;

        const float Shield = Target->GetShield();

        // shield takes the hit first, whatever is left goes to health
        const float FromShield = (Shield > 0.f) ? (Shield < Damage ? Shield : Damage) : 0.f;
        const float FromHealth = Damage - FromShield;
        const float NewShield = Shield - FromShield;
        const float NewHealth = Health - FromHealth;

        // who hit whom (kill credit depends on this)
        if (auto TargetController = (AActor*)Target->Controller)
        {
            BotCodeLastHit[(void*)TargetController] = { (void*)ShooterState, GStormTick };

            if (Kind == 0)
            {
                BotLastDamager[(void*)TargetController] = { (void*)ShooterState, GStormTick };
                LastPlayerDamageTick[(void*)TargetController] = GStormTick;

                // the bot that was just hit fights back against whoever shot it
                AggroedByHuman[(void*)TargetController] = { (void*)ShooterState, GStormTick };
            }
        }

        GCodeHits[Kind]++;
        GCodeDamage[Kind] += (double)Damage;

        // finishing hit (shield is already empty if health is taking damage)
        if (NewHealth <= 0.5f)
        {
            if (FromShield > 0.f)
                ForceShield(Target, Shield, 0.f);

            if (!BotForceKill(Target, ShooterController, Shooter))
            {
                GCodeKillsNotByForceKill++;
                Target->SetHealth(0.f);
            }

            GCodeKills[Kind]++;
            return true;
        }

        if (FromShield > 0.f)
        {
            ForceShield(Target, Shield, NewShield);

            if (Kind == 1)
            {
                if (NewShield <= 0.5f)
                    BotPlayHitFeedback(Target, 2);       // the hit that took the shield to zero
                else
                    BotPlayHitFeedback(Target, 1);       // an ordinary shield hit
            }
        }

        if (FromHealth > 0.f)
        {
            BotApplyGameDamage(Target, ShooterController, Shooter, FromHealth);

            // if the game ignored it, write the health ourselves
            if (Target->GetHealth() > Health - FromHealth + 0.5f)
                ForceHealth(Target, Health, NewHealth);

            if (Kind == 1)
                BotPlayHitFeedback(Target, 0);           // health hit sound on the player's screen
        }

        // what actually happened (first 20 hits on a real player)
        if (Kind == 1)
        {
            static int Lines = 0;
            if (++Lines <= 20)
                LogLine("[DMG] %.0f damage: shield %.0f -> %.0f (wanted %.0f), health %.0f -> %.0f (wanted %.0f).",
                    Damage, Shield, Target->GetShield(), NewShield, Health, Target->GetHealth(), NewHealth);
        }

        return false;
    }

    static void LogBotDamageSummary()
    {
        static const char* ModeNames[] = { "still watching", "the game's own (it lands)", "the bot code's" };

        LogLine("[HITS] Bot bullets: on bots %s (watched %d, landed %d) -- the bot code's hits %d, %.0f damage, %d finished off | on real "
            "players %s (watched %d, landed %d) -- the bot code's hits %d, %.0f damage, %d finished off | finishing hits not through "
            "ForceKill: %d.",
            ModeNames[GBotDamageMode[0]], GBotDamageSamples[0], GBotDamageSamplesLanded[0], GCodeHits[0], GCodeDamage[0], GCodeKills[0],
            ModeNames[GBotDamageMode[1]], GBotDamageSamples[1], GBotDamageSamplesLanded[1], GCodeHits[1], GCodeDamage[1], GCodeKills[1],
            GCodeKillsNotByForceKill);
    }

    struct FBurstCheck
    {
        bool bOpen = false;
        void* Weapon = nullptr;
        int RoundsAtStart = -1;
        int RoundsLast = -1;
        int RoundsSpent = 0;
        bool bAbilityRan = false;
        bool bServerRuns = false;
        void* Target = nullptr;
        std::string TargetName;
        float TargetLifeAtStart = -1.f;
        float Distance = 0.f;
        int Mode = 0;
        int Ticks = 0;
        bool bWeaponChanged = false;
        int EquippingAtStart = -1;
        int ReloadingAtStart = -1;
        int OnTargetRounds = 0;
        int TargetKind = -1;
        float Damage = 0.f;
    };

    static int GDryBurstsEquipping = 0;
    static int GDryBurstsReloading = 0;
    static int GDryBurstsNeither = 0;

    static int GunIsReloading(AActor* Weapon);
    static int GunIsEquipping(AActor* Weapon);

    static std::map<void*, FBurstCheck> BotBurstChecks;
    static int GBurstsChecked = 0;
    static int GBurstsThatFired = 0;
    static int GBurstsInMode[2] = { 0, 0 };
    static int GFiredInMode[2] = { 0, 0 };
    static int32 GActiveAbilityOffset = -2;

    static bool WeaponAbilityIsRunning(AActor* Weapon)
    {
        if (!Weapon)
            return false;

        if (GActiveAbilityOffset == -2)
        {
            const uint32 Off = Weapon->GetOffset("ActiveAbility");
            GActiveAbilityOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        if (GActiveAbilityOffset < 0)
            return false;

        return *(void**)((uint8*)Weapon + GActiveAbilityOffset) != nullptr;
    }

    static float PawnLife(AFortPlayerPawnAthena* Pawn)
    {
        return Pawn ? Pawn->GetHealth() + Pawn->GetShield() : -1.f;
    }

    static void StartBurstCheck(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Target,
        float DistanceMetres)
    {
        if (!Pawn)
            return;

        FBurstCheck& Check = BotBurstChecks[(void*)Pawn];
        Check = FBurstCheck{};

        auto Weapon = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;
        int32* Ammo = WeaponAmmoCount(Weapon);

        Check.bOpen = true;
        Check.Weapon = (void*)Weapon;
        Check.RoundsAtStart = Ammo ? *Ammo : -1;
        Check.RoundsLast = Check.RoundsAtStart;
        Check.bAbilityRan = WeaponAbilityIsRunning(Weapon);
        Check.bServerRuns = GBotsRunOnServer.find((void*)Controller) != GBotsRunOnServer.end();
        Check.Target = (void*)Target;
        Check.TargetName = BotPawnName(Target);
        Check.TargetLifeAtStart = PawnLife(Target);
        Check.Distance = DistanceMetres;
        Check.Mode = (GTriggerMode == 1) ? 1 : 0;
        Check.EquippingAtStart = Weapon ? GunIsEquipping(Weapon) : -1;
        Check.ReloadingAtStart = Weapon ? GunIsReloading(Weapon) : -1;
        Check.TargetKind = PawnTargetKind(Target);
    }

    static void BotRoundsFired(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, FBurstCheck& Check, int Rounds, AActor* Weapon,
        const std::set<void*>& LivePawns)
    {
        if (Rounds <= 0 || !Weapon || !PlayerState || Pawn->GetHealth() <= 0.f)
            return;

        auto AimIt = BotShotAim.find((void*)Pawn);
        if (AimIt == BotShotAim.end())
            return;

        const FBotShotAim Aim = AimIt->second;

        if (!Aim.bCanShoot || !Aim.bOnTarget || !Aim.Target || Aim.Target != Check.Target || LivePawns.find(Aim.Target) == LivePawns.end())
            return;

        Check.OnTargetRounds += Rounds;

        const int Kind = Check.TargetKind;
        if (Kind < 0 || GBotDamageMode[Kind] != BotDamage_ByCode)
            return;

        auto Gun = ((AFortWeapon*)Weapon)->HasWeaponData() ? (const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData : nullptr;
        if (!Gun)
            return;

        const float PerRound = BotRoundDamage(BotGunNumbers(Gun), Aim.Distance);
        if (!(PerRound > 0.f))
            return;

        auto Target = (AFortPlayerPawnAthena*)Aim.Target;
        auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;

        for (int r = 0; r < Rounds; r++)
        {
            Check.Damage += PerRound;

            if (BotHitTarget(Pawn, Controller, PlayerState, Target, PerRound, Kind, Gun))
                break;
        }
    }

    static std::map<void*, int> BotGunJams;
    static std::set<void*> BotVerifyNextBurst;
    static std::map<void*, float> BotLastReissue;
    static std::map<void*, int> BotReissuesNoFire;
    static std::set<void*> BotReissueGivenUp;
    static std::map<void*, float> BotLastShotAt;
    static int GDryBurstsStillCycling = 0;
    static int GGunJamsSeen = 0;
    static int GGunsFireAfterGameReload = 0;
    static int GGunsDeadAfterGameReload = 0;

    static bool BotReissueGun(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState, int Rounds,
        const char* Why);

    static void UpdateBurstCheck(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, const std::set<void*>& LivePawns)
    {
        auto It = BotBurstChecks.find((void*)Pawn);
        if (It == BotBurstChecks.end() || !It->second.bOpen)
            return;

        FBurstCheck& Check = It->second;
        Check.Ticks++;

        auto Weapon = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;

        if ((void*)Weapon != Check.Weapon)
            Check.bWeaponChanged = true;

        if (Weapon && (void*)Weapon == Check.Weapon)
        {
            int32* Ammo = WeaponAmmoCount(Weapon);

            if (Ammo)
            {
                if (Check.RoundsLast >= 0 && *Ammo < Check.RoundsLast)
                {
                    const int ThisTick = Check.RoundsLast - *Ammo;
                    Check.RoundsSpent += ThisTick;

                    auto World = UWorld::GetWorld();
                    BotLastShotAt[(void*)Pawn] = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

                    BotRoundsFired(Pawn, PlayerState, Check, ThisTick, Weapon, LivePawns);
                }

                Check.RoundsLast = *Ammo;
            }

            if (WeaponAbilityIsRunning(Weapon))
                Check.bAbilityRan = true;
        }

        auto FireIt = BotFire.find((void*)Pawn);
        if (FireIt != BotFire.end() && FireIt->second.bFiring)
            return;

        Check.bOpen = false;

        const bool bFired = Check.RoundsSpent > 0;
        const int Mode = Check.Mode;

        const bool bConclusive = Check.RoundsAtStart > 0 && !Check.bWeaponChanged && Check.Ticks >= 4;

        GBurstsChecked++;

        if (bFired)
            GBurstsThatFired++;

        if (bConclusive)
        {
            GBurstsInMode[Mode]++;

            if (bFired)
                GFiredInMode[Mode]++;
            else if (Check.EquippingAtStart == 1)
                GDryBurstsEquipping++;
            else if (Check.ReloadingAtStart == 1)
                GDryBurstsReloading++;
            else
                GDryBurstsNeither++;
        }

        const bool bTargetStillHere = Check.Target && LivePawns.find(Check.Target) != LivePawns.end();
        const float LifeNow = bTargetStillHere ? PawnLife((AFortPlayerPawnAthena*)Check.Target) : -1.f;

        if (bFired && Check.OnTargetRounds > 0 && Check.TargetKind >= 0 && GBotDamageMode[Check.TargetKind] == BotDamage_Watching &&
            Check.Distance <= 15.f && bTargetStillHere && Check.TargetLifeAtStart > 0.f)
        {
            const int Kind = Check.TargetKind;
            const float Lost = Check.TargetLifeAtStart - LifeNow;
            const bool bLanded = Lost >= 5.f;

            GBotDamageSamples[Kind]++;

            if (bLanded)
                GBotDamageSamplesLanded[Kind]++;

            LogLine("[HITS] Watching bot bullets on %s: '%s' fired %d round(s) at '%s' from %.0f m, %d of them with its aim on them -- they "
                "lost %.0f (%s). Watched %d of 5.",
                TargetKindName(Kind), SafeNameOf(PlayerState).c_str(), Check.RoundsSpent, Check.TargetName.c_str(), Check.Distance,
                Check.OnTargetRounds, Lost, bLanded ? "the game's own shot landed" : "nothing landed", GBotDamageSamples[Kind]);

            if (bLanded)
            {
                GBotDamageMode[Kind] = BotDamage_ByGame;
                LogLine("[HITS] The game's own bot bullets DO land on %s -- the bot code never adds damage of its own on them.",
                    TargetKindName(Kind));
            }
            else if (GBotDamageSamples[Kind] >= 5)
            {
                GBotDamageMode[Kind] = BotDamage_ByCode;
                LogLine("[HITS] 5 bursts at %s with the aim on them, at 15 m or less, and the game's own shots landed nothing -- the bot "
                    "code deals the damage on %s from now on (each round on target hits for the gun's own damage).",
                    TargetKindName(Kind), TargetKindName(Kind));
            }
        }

        if (GBurstsChecked <= 8 || (GBurstsChecked % 50) == 0)
        {
            char Life[64];

            if (bTargetStillHere)
                snprintf(Life, sizeof(Life), "%.0f -> %.0f", Check.TargetLifeAtStart, LifeNow);
            else
                snprintf(Life, sizeof(Life), "%.0f -> down (or gone)", Check.TargetLifeAtStart);

            auto FlagText = [](int Flag) { return Flag == 1 ? "YES" : (Flag == 0 ? "no" : "?"); };

            LogLine("[FIRE] Burst #%d: '%s' at '%s' (%.0f m, %d tick(s)) -- the gun lost %d round(s)%s | its ability ran: %s | "
                "player controller out of its ability system: %s | trigger: %s | the gun when the trigger went down: %d round(s), "
                "equipping %s, reloading %s | on target: %d round(s), the bot code's damage %.0f | the target's health + shield: %s.",
                GBurstsChecked, SafeNameOf(PlayerState).c_str(), Check.TargetName.c_str(), Check.Distance, Check.Ticks,
                Check.RoundsSpent,
                Check.RoundsAtStart < 0 ? " (its round counter cannot be read)"
                : (Check.RoundsAtStart == 0 ? " (it started with an empty gun)"
                    : (Check.bWeaponChanged ? " (it changed what it was holding)" : "")),
                Check.bAbilityRan ? "yes" : "no", Check.bServerRuns ? "yes" : "NO",
                Mode == 1 ? "as a local player" : "plain", Check.RoundsAtStart, FlagText(Check.EquippingAtStart),
                FlagText(Check.ReloadingAtStart), Check.OnTargetRounds, Check.Damage, Life);
        }

        if (!bConclusive)
            return;

        if (Mode == 1 && GTriggerMode == 1)
        {
            if (bFired)
            {
                BotGunJams.erase((void*)Pawn);
                BotReissuesNoFire.erase((void*)Pawn);

                if (BotVerifyNextBurst.erase((void*)Pawn) > 0)
                {
                    GGunsFireAfterGameReload++;

                    if (GGunsFireAfterGameReload <= 5)
                        LogLine("[RELOAD] '%s's gun fires after the game's own reload (%d round(s) in its first burst) -- %d gun(s) checked "
                            "like that so far.", SafeNameOf(PlayerState).c_str(), Check.RoundsSpent, GGunsFireAfterGameReload);
                }
            }
            else if (Check.EquippingAtStart != 1 && Check.ReloadingAtStart != 1)
            {
                auto Gun = (Weapon && ((AFortWeapon*)Weapon)->HasWeaponData()) ? (const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData
                    : nullptr;

                const float FireRate = Gun ? BotGunNumbers(Gun).FireRate : 0.f;
                const float Between = (FireRate > 0.05f) ? 1.f / FireRate : 0.f;
                auto LastShot = BotLastShotAt.find((void*)Pawn);
                auto World = UWorld::GetWorld();
                const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;
                const bool bStillCycling = Between > 0.25f && LastShot != BotLastShotAt.end() && Now - LastShot->second < Between + 0.5f;

                if (bStillCycling)
                {
                    GDryBurstsStillCycling++;
                }
                else
                {
                    GGunJamsSeen++;

                    const bool bAfterGameReload = BotVerifyNextBurst.erase((void*)Pawn) > 0;
                    int& Jams = BotGunJams[(void*)Pawn];
                    Jams++;

                    if (bAfterGameReload)
                        GGunsDeadAfterGameReload++;

                    if (bAfterGameReload || Jams >= 2)
                    {
                        Jams = 0;

                        BotReissueGun(Pawn, (AFortPlayerControllerAthena*)PlayerState->Owner, PlayerState, Gun ? BotGunMagazine(Gun) : 30,
                            bAfterGameReload ? "it would not fire after the game's own reload"
                            : "two full bursts in a row with rounds in it fired nothing");
                    }
                }
            }
        }

        if (GTriggerMode == 0 && GBurstsInMode[0] >= 4 && GFiredInMode[0] == 0)
        {
            GTriggerMode = 1;
            LogLine("[FIRE] 4 full bursts (rounds in the gun, the trigger held) and not one round fired with the plain trigger -- from "
                "now on the trigger is pulled with the bot counted as a local player for that moment.");
        }
        else if (GTriggerMode == 1 && GBurstsInMode[1] >= 4 && GFiredInMode[1] == 0)
        {
            GTriggerMode = 2;
            LogLine("[FIRE] 4 more full bursts and still not one round fired that way either -- back to the plain trigger. The [FIRE] "
                "lines above say how far it got.");
        }
    }

    static void WatchBotTrigger(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, int TickNow)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon() || !Pawn->CurrentWeapon)
            return;

        if (BotIsHealing(Pawn))
            return;   // a consumable in use is not a stray gun shot

        auto Weapon = (AActor*)Pawn->CurrentWeapon;

        int32* Ammo = WeaponAmmoCount(Weapon);
        if (!Ammo)
            return;

        const int Rounds = *Ammo;

        auto Seen = BotAmmoSeen.find((void*)Pawn);

        if (Seen != BotAmmoSeen.end() && Seen->second.first == (void*)Weapon && Rounds < Seen->second.second)
        {
            auto FireIt = BotFire.find((void*)Pawn);
            const bool bBursting = FireIt != BotFire.end() && FireIt->second.bFiring;

            auto LetGo = BotTriggerUpTick.find((void*)Pawn);
            const bool bJustLetGo = LetGo != BotTriggerUpTick.end() && (TickNow - LetGo->second) <= 15;

            if (bBursting || bJustLetGo)
            {
                GShotTicksOurs++;
            }
            else
            {
                GShotTicksStray++;

                BotTrigger(Pawn, false);
                BotTriggerUpTick[(void*)Pawn] = TickNow;

                if (GShotTicksStray <= 10)
                    LogLine("[TRIGGER] '%s' fired its gun without the bot code pulling the trigger -- let go of it.",
                        SafeNameOf(PlayerState).c_str());
            }
        }

        BotAmmoSeen[(void*)Pawn] = { (void*)Weapon, Rounds };
    }

    static int GReloadsByGame = 0;
    static int GReloadsByBotCode = 0;
    static int GReloadsRefilledByGame = 0;
    static int GReloadsRefilledByBotCode = 0;
    static int GReloadsCancelled = 0;
    static int GReloadsTopOff = 0;
    static int GStuckReloadFlags = 0;
    static int GStuckEquipFlags = 0;
    static int GReloadLines = 0;

    struct FGunFlag
    {
        int32 Offset = -2;
        uint8 Mask = 0;
    };

    static FGunFlag GGunReloadingFlag;
    static FGunFlag GGunEquippingFlag;

    static void FindGunFlag(AActor* Weapon, FGunFlag& Flag, const char* First, const char* Second)
    {
        if (Flag.Offset != -2 || !Weapon)
            return;

        Flag.Offset = -1;

        for (const char* Name : { First, Second })
        {
            if (!Name)
                continue;

            auto Prop = Weapon->GetProperty(Name, 0x20000);

            if (Prop)
            {
                Flag.Offset = (int32)GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
                Flag.Mask = Prop->GetFieldMask();

                LogLine("[RELOAD] The gun's \"%s\" flag is at +0x%X (mask 0x%02X).", Name, (unsigned)Flag.Offset, (unsigned)Flag.Mask);
                break;
            }
        }

        if (Flag.Offset < 0 || Flag.Mask == 0)
        {
            Flag.Offset = -1;
            LogLine("[RELOAD] The gun has no \"%s\" flag on this build.", First);
        }
    }

    static int ReadGunFlag(AActor* Weapon, const FGunFlag& Flag)
    {
        if (!Weapon || Flag.Offset < 0)
            return -1;

        return (GetFromOffset<uint8>(Weapon, (uint32)Flag.Offset) & Flag.Mask) ? 1 : 0;
    }

    static void WriteGunFlag(AActor* Weapon, const FGunFlag& Flag, bool bOn)
    {
        if (!Weapon || Flag.Offset < 0)
            return;

        uint8& Byte = GetFromOffset<uint8>(Weapon, (uint32)Flag.Offset);
        Byte = bOn ? (uint8)(Byte | Flag.Mask) : (uint8)(Byte & ~Flag.Mask);
    }

    static int GunIsReloading(AActor* Weapon)
    {
        FindGunFlag(Weapon, GGunReloadingFlag, "bIsReloadingWeapon", "bIsReloading");
        return ReadGunFlag(Weapon, GGunReloadingFlag);
    }

    static int GunIsEquipping(AActor* Weapon)
    {
        FindGunFlag(Weapon, GGunEquippingFlag, "bIsEquippingWeapon", "bIsEquipping");
        return ReadGunFlag(Weapon, GGunEquippingFlag);
    }

    static int GunMagazine(AActor* Weapon, void* PlayerStateKey)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Weapon)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("GetBulletsPerClip");
        }

        int Magazine = 0;

        if (Fn && Weapon)
            Magazine = Weapon->Call<int32>(Fn);

        if (Magazine <= 0 || Magazine > 1000)
            Magazine = BotMagazineSize(PlayerStateKey);

        return Magazine;
    }

    static float GunReloadTime(AActor* Weapon)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Weapon)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("GetReloadTime");
        }

        float Seconds = 0.f;

        if (Fn && Weapon)
            Seconds = Weapon->Call<float>(Fn);

        if (!(Seconds >= 0.3f && Seconds <= 8.f) && Weapon && ((AFortWeapon*)Weapon)->HasWeaponData() && ((AFortWeapon*)Weapon)->WeaponData)
            Seconds = BotGunNumbers((const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData).ReloadTime;

        if (!(Seconds >= 0.3f && Seconds <= 8.f))
            Seconds = 2.0f;

        return Seconds;
    }

    static float GunReloadAllowance(AActor* Weapon, int Rounds, int Magazine)
    {
        const float Each = GunReloadTime(Weapon);
        const int Missing = Magazine - Rounds;

        auto Def = (Weapon && ((AFortWeapon*)Weapon)->HasWeaponData()) ? (const UFortItemDefinition*)((AFortWeapon*)Weapon)->WeaponData : nullptr;

        if (Def && Missing > 1 && BotGunNumbers(Def).bOneRoundAtATime)
            return Each * (float)Missing;

        return Each;
    }

    static bool CallGameReloadRaw(AActor* Weapon, UFunction* Fn, uint8* LocalFlag)
    {
        uint8 Was = 0;

        if (LocalFlag)
        {
            Was = *LocalFlag;
            *LocalFlag = 1;
        }

        Weapon->ProcessEvent(Fn, nullptr);

        if (LocalFlag)
            *LocalFlag = Was;

        return true;
    }

    static bool CallGameReloadGuarded(AActor* Weapon, UFunction* Fn, uint8* LocalFlag)
    {
        __try
        {
            return CallGameReloadRaw(Weapon, Fn, LocalFlag);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GGameReloadFaulted = false;

    static bool StartGameReload(AFortPlayerControllerAthena* Controller, AActor* Weapon)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Weapon)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("Reload");
            LogLine("[RELOAD] The game's own AFortWeapon::Reload %s.", Fn ? "was found -- bots reload with it first" : "is NOT on this build");
        }

        if (!Fn || !Weapon || GGameReloadFaulted)
            return false;

        uint8* LocalFlag = nullptr;

        if (Controller && IsPlayerControllerBot((AActor*)Controller))
        {
            if (GLocalFlagOffset == -2)
            {
                const uint32 Off = ((UObject*)Controller)->GetOffset("bIsLocalPlayerController");
                GLocalFlagOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
            }

            if (GLocalFlagOffset >= 0)
                LocalFlag = (uint8*)Controller + GLocalFlagOffset;
        }

        if (!CallGameReloadGuarded(Weapon, Fn, LocalFlag))
        {
            GGameReloadFaulted = true;

            if (LocalFlag)
                *LocalFlag = 0;

            LogLine("[RELOAD] The game's own Reload faulted on a bot's gun -- bots reload with the bot code's own reload from now on.");
            return false;
        }

        return GunIsReloading(Weapon) == 1;
    }

    static int GGunsReissued = 0;
    static int GGunsReissueRefused = 0;
    static int GAmmoSyncs = 0;
    static int GAmmoSyncLines = 0;

    static uint8* BotLocalFlag(AFortPlayerControllerAthena* Controller)
    {
        if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
            return nullptr;

        if (GLocalFlagOffset == -2)
        {
            const uint32 Off = ((UObject*)Controller)->GetOffset("bIsLocalPlayerController");
            GLocalFlagOffset = (Off == (uint32)-1) ? -1 : (int32)Off;
        }

        return GLocalFlagOffset >= 0 ? (uint8*)Controller + GLocalFlagOffset : nullptr;
    }

    static int GunLocalRounds(AActor* Weapon)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Weapon)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("GetLocalMagazineAmmoCount");
        }

        return (Fn && Weapon) ? (int)Weapon->Call<int32>(Fn) : -1;
    }

    static bool OnRepAmmoRaw(AActor* Weapon, UFunction* Fn, uint8* LocalFlag)
    {
        uint8 Was = 0;

        if (LocalFlag)
        {
            Was = *LocalFlag;
            *LocalFlag = 1;
        }

        Weapon->ProcessEvent(Fn, nullptr);

        if (LocalFlag)
            *LocalFlag = Was;

        return true;
    }

    static bool OnRepAmmoGuarded(AActor* Weapon, UFunction* Fn, uint8* LocalFlag)
    {
        __try
        {
            return OnRepAmmoRaw(Weapon, Fn, LocalFlag);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool GAmmoSyncBroken = false;

    static void SyncGunAfterRefill(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState, AActor* Weapon)
    {
        if (!Weapon || GAmmoSyncBroken)
            return;

        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked)
        {
            bLooked = true;
            Fn = Weapon->GetFunction("OnRep_AmmoCount");
        }

        if (!Fn)
            return;

        int32* Ammo = WeaponAmmoCount(Weapon);
        const int LocalBefore = GunLocalRounds(Weapon);

        SetBoolByName((UObject*)Weapon, "bUpdateLocalAmmoCount", true);

        if (!OnRepAmmoGuarded(Weapon, Fn, BotLocalFlag(Controller)))
        {
            GAmmoSyncBroken = true;

            if (uint8* Flag = BotLocalFlag(Controller))
                *Flag = 0;

            LogLine("[RELOAD] The gun's OnRep_AmmoCount faulted on a bot -- not run after reloads from now on.");
            return;
        }

        GAmmoSyncs++;

        if (++GAmmoSyncLines <= 8)
            LogLine("[RELOAD] '%s's gun after its reload: %d round(s) in it; what the gun says it holds for its own player: %d before its "
                "OnRep_AmmoCount, %d after.", SafeNameOf(PlayerState).c_str(), Ammo ? *Ammo : -1, LocalBefore, GunLocalRounds(Weapon));
    }

    static bool ActorIsLive(const UObject* Object)
    {
        if (!Object || Object->Index < 0 || Object->Index >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(Object->Index);
        return Item && Item->Object == Object && !(Item->Flags & (0x20000000 | 0x10000000));
    }

    static bool BotReissueGun(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState, int Rounds,
        const char* Why)
    {
        if (!Pawn || !Controller || !PlayerState || !Controller->WorldInventory || !Pawn->HasCurrentWeapon() || !Pawn->CurrentWeapon)
            return false;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        if (BotReissueGivenUp.find((void*)Pawn) != BotReissueGivenUp.end())
            return false;

        auto Last = BotLastReissue.find((void*)Pawn);
        if (Last != BotLastReissue.end() && Now - Last->second < 3.f)
        {
            GGunsReissueRefused++;
            return false;
        }

        int& NoFire = BotReissuesNoFire[(void*)Pawn];
        if (NoFire >= 3)
        {
            BotReissueGivenUp.insert((void*)Pawn);
            LogLine("[RELOAD] '%s': three fresh copies of its gun did not fire either -- no more for this one (the [FIRE] lines say how its "
                "bursts go).", SafeNameOf(PlayerState).c_str());
            return false;
        }

        auto Weapon = (AFortWeapon*)Pawn->CurrentWeapon;
        auto Def = Weapon->HasWeaponData() ? (const UFortItemDefinition*)Weapon->WeaponData : nullptr;

        if (!Def || ((const UObject*)Def)->IsA<UFortWeaponMeleeItemDefinition>())
            return false;

        auto Inventory = Controller->WorldInventory;
        FGuid OldGuid = Weapon->HasItemEntryGuid() ? Weapon->ItemEntryGuid : FGuid{};

        bool bInInventory = false;
        {
            auto& Entries = Inventory->Inventory.ReplicatedEntries;

            for (int e = 0; e < Entries.Num(); e++)
            {
                if (Entries.Get(e, FFortItemEntry::Size()).ItemGuid == OldGuid)
                {
                    bInInventory = true;
                    break;
                }
            }
        }

        if (Rounds < 1)
            Rounds = 1;

        if (bInInventory)
            Inventory->Remove(OldGuid);

        auto Item = Inventory->GiveItem(Def, 1, Rounds);
        if (!Item || !Item->HasItemEntry() || !FFortItemEntry::HasItemGuid())
            return false;

        const FGuid NewGuid = Item->ItemEntry.ItemGuid;
        auto NewWeapon = (AActor*)Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Def, NewGuid);

        if ((AActor*)Weapon != (AActor*)Pawn->CurrentWeapon && ActorIsLive((const UObject*)Weapon))
            ((AActor*)Weapon)->K2_DestroyActor();

        BotWeapons[(void*)PlayerState] = Def;
        BotInventories[(void*)PlayerState] = Inventory;
        BotLastReissue[(void*)Pawn] = Now;
        BotGunJams.erase((void*)Pawn);
        NoFire++;

        GGunsReissued++;

        if (GGunsReissued <= 12 || (GGunsReissued % 50) == 0)
            LogLine("[RELOAD] '%s' got a fresh %s out of its inventory (%s): %d round(s) in it | holding it now: %s | old item %s | %d "
                "fresh copies so far.",
                SafeNameOf(PlayerState).c_str(), BotObjectName((const UObject*)Def).c_str(), Why, Rounds,
                (NewWeapon && (AActor*)Pawn->CurrentWeapon == NewWeapon) ? "yes" : "NO",
                bInInventory ? "taken out of its inventory" : "was not in its inventory", GGunsReissued);

        return NewWeapon != nullptr;
    }

    static bool BotIsReloading(AFortPlayerPawnAthena* Pawn)
    {
        auto It = BotReloads.find((void*)Pawn);
        return It != BotReloads.end() && It->second.bActive;
    }

    static void LetGoOfTheTrigger(AFortPlayerPawnAthena* Pawn, int TickNow)
    {
        auto FireIt = BotFire.find((void*)Pawn);

        if (FireIt != BotFire.end() && FireIt->second.bFiring)
        {
            BotTrigger(Pawn, false);
            BotTriggerUpTick[(void*)Pawn] = TickNow;

            FireIt->second.bFiring = false;
            FireIt->second.BurstTicksLeft = 0;
        }
    }

    static void FinishBotReloadNow(FBotReload& Reload, AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller,
        AFortPlayerStateAthena* PlayerState, AActor* Weapon, bool bGameRefilled, float Now)
    {
        int32* Ammo = WeaponAmmoCount(Weapon);
        const int RoundsAtEnd = Ammo ? *Ammo : -1;
        const std::string GunName = BotObjectName((const UObject*)((AFortWeapon*)Weapon)->WeaponData);

        const char* How = "";
        int RoundsNow = RoundsAtEnd;

        if (bGameRefilled)
        {

            SyncGunAfterRefill(Controller, PlayerState, Weapon);
            BotVerifyNextBurst.insert((void*)Pawn);

            How = RoundsAtEnd >= Reload.Magazine ? "the game's own reload refilled it" : "the game's own reload part-filled it";
        }
        else
        {

            WriteGunFlag(Weapon, GGunReloadingFlag, false);

            if (BotReissueGun(Pawn, Controller, PlayerState, Reload.Magazine,
                Reload.bGameReload ? "the game's own reload never refilled it" : "the bot code's own reload"))
            {
                How = Reload.bGameReload ? "the game's reload never refilled it, so it got a fresh copy of the gun, full"
                    : "a fresh copy of the gun, full, after the gun's reload time";
                RoundsNow = Reload.Magazine;
            }
            else
            {

                if (Ammo)
                    *Ammo = Reload.Magazine;

                How = "the bot code wrote the round count (no fresh copy could be made just now)";
                RoundsNow = Ammo ? *Ammo : -1;
            }
        }

        TopUpBotMagazine((void*)PlayerState);

        GMagazinesRefilled++;

        if (bGameRefilled)
            GReloadsRefilledByGame++;
        else
            GReloadsRefilledByBotCode++;

        if (++GReloadLines <= 20 || (GReloadLines % 50) == 0)
            LogLine("[RELOAD] '%s' finished reloading its %s after %.1fs (%s) -- %d -> %d/%d rounds, %s.", SafeNameOf(PlayerState).c_str(),
                GunName.c_str(), Now - Reload.StartedAt, Reload.Why, Reload.RoundsBefore, RoundsNow, Reload.Magazine, How);

        Reload.bActive = false;
        Reload.bGameReload = false;
        Reload.Weapon = nullptr;
    }

    static void TickBotReload(AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, AFortPlayerControllerAthena* Controller,
        bool bFighting, int TickNow)
    {
        if (!Pawn || !PlayerState)
            return;

        auto World = UWorld::GetWorld();
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        FBotReload& Reload = BotReloads[(void*)Pawn];

        AActor* Weapon = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;

        if (bFighting)
            Reload.NoFightSince = -1.f;
        else if (Reload.NoFightSince < 0.f)
            Reload.NoFightSince = Now;

        if (Reload.bActive && (void*)Weapon != Reload.Weapon)
        {
            GReloadsCancelled++;

            if (++GReloadLines <= 20)
                LogLine("[RELOAD] '%s' stopped reloading -- %s.", SafeNameOf(PlayerState).c_str(),
                    Weapon ? "it is holding a different gun now" : "it has nothing in its hands now");

            Reload.bActive = false;
            Reload.bGameReload = false;
            Reload.Weapon = nullptr;
        }

        if (!Weapon)
            return;

        if (BotIsHealing(Pawn))
            return;   // the bandage in its hands has no magazine to reload

        int32* Ammo = WeaponAmmoCount(Weapon);
        if (!Ammo)
            return;

        if (auto Gun = (AFortWeapon*)Weapon)
            if (Gun->HasWeaponData() && Gun->WeaponData && ((const UObject*)Gun->WeaponData)->IsA<UFortWeaponMeleeItemDefinition>())
                return;

        const int Rounds = *Ammo;
        const int ReloadingFlag = GunIsReloading(Weapon);
        const int EquippingFlag = GunIsEquipping(Weapon);

        if (EquippingFlag == 1)
        {
            if (Reload.EquipFlagSince < 0.f)
                Reload.EquipFlagSince = Now;
            else if (Now - Reload.EquipFlagSince > 3.f)
            {
                WriteGunFlag(Weapon, GGunEquippingFlag, false);
                GStuckEquipFlags++;

                if (GStuckEquipFlags <= 10 || (GStuckEquipFlags % 25) == 0)
                    LogLine("[RELOAD] '%s's gun was still \"equipping\" %.1fs after it was taken out -- the flag is cleared so it can fire "
                        "(%d time(s) so far).", SafeNameOf(PlayerState).c_str(), Now - Reload.EquipFlagSince, GStuckEquipFlags);

                Reload.EquipFlagSince = -1.f;
            }
        }
        else
        {
            Reload.EquipFlagSince = -1.f;
        }

        if (!Reload.bActive && ReloadingFlag == 1)
        {
            const int Magazine = (Reload.MagazineOf == (void*)Weapon && Reload.MagazineSize > 0) ? Reload.MagazineSize
                : GunMagazine(Weapon, (void*)PlayerState);

            if (Reload.ReloadFlagSince < 0.f)
                Reload.ReloadFlagSince = Now;
            else if (Now - Reload.ReloadFlagSince > GunReloadAllowance(Weapon, Rounds, Magazine) + 1.5f)
            {
                WriteGunFlag(Weapon, GGunReloadingFlag, false);

                GStuckReloadFlags++;

                if (GStuckReloadFlags <= 10 || (GStuckReloadFlags % 25) == 0)
                    LogLine("[RELOAD] '%s's gun had the game's \"reloading\" flag stuck on for %.1fs with no reload going -- cleared, and "
                        "a fresh copy of the gun with %d round(s) (%d time(s) so far).",
                        SafeNameOf(PlayerState).c_str(), Now - Reload.ReloadFlagSince, Magazine, GStuckReloadFlags);

                Reload.ReloadFlagSince = -1.f;

                if (!BotReissueGun(Pawn, Controller, PlayerState, Magazine, "the game's \"reloading\" flag was stuck on") && *Ammo < Magazine)
                    *Ammo = Magazine;

                return;
            }
        }
        else
        {
            Reload.ReloadFlagSince = -1.f;
        }

        if (Reload.bActive)
        {
            if (Reload.bGameReload)
            {
                const bool bRefilled = Rounds >= Reload.Magazine || (ReloadingFlag == 0 && Rounds > Reload.RoundsBefore && Now - Reload.StartedAt > 0.2f);

                if (bRefilled)
                    FinishBotReloadNow(Reload, Pawn, Controller, PlayerState, Weapon, true, Now);
                else if (Now - Reload.StartedAt > Reload.Duration + 1.5f)
                    FinishBotReloadNow(Reload, Pawn, Controller, PlayerState, Weapon, false, Now);
            }
            else if (Now - Reload.StartedAt >= Reload.Duration)
            {
                FinishBotReloadNow(Reload, Pawn, Controller, PlayerState, Weapon, false, Now);
            }

            return;
        }

        if (Reload.MagazineOf != (void*)Weapon)
        {
            Reload.MagazineOf = (void*)Weapon;
            Reload.MagazineSize = GunMagazine(Weapon, (void*)PlayerState);
        }

        const int Magazine = Reload.MagazineSize;
        const char* Why = nullptr;

        if (Rounds <= 0)
            Why = bFighting ? "its magazine ran dry mid-fight" : "its magazine was empty";
        else if (!bFighting && Rounds < Magazine && Reload.NoFightSince >= 0.f && Now - Reload.NoFightSince >= 1.5f)
            Why = "topping off after the fight";

        if (!Why)
            return;

        if (ReloadingFlag == 1)
            return;

        LetGoOfTheTrigger(Pawn, TickNow);

        Reload.bActive = true;
        Reload.Weapon = (void*)Weapon;
        Reload.RoundsBefore = Rounds;
        Reload.Magazine = Magazine;
        Reload.StartedAt = Now;
        Reload.Duration = GunReloadAllowance(Weapon, Rounds, Magazine);
        Reload.Why = Why;

        Reload.bGameReload = StartGameReload(Controller, Weapon);

        if (Reload.bGameReload)
        {
            GReloadsByGame++;
        }
        else
        {
            GReloadsByBotCode++;
            WriteGunFlag(Weapon, GGunReloadingFlag, true);
        }

        GReloadsStarted++;

        if (Rounds > 0)
            GReloadsTopOff++;

        if (++GReloadLines <= 20 || (GReloadLines % 50) == 0)
            LogLine("[RELOAD] '%s' is reloading its %s (%s, %d/%d rounds) -- %s, %.1fs reload.",
                SafeNameOf(PlayerState).c_str(), BotObjectName((const UObject*)((AFortWeapon*)Weapon)->WeaponData).c_str(), Why,
                Rounds, Magazine,
                Reload.bGameReload ? "the game's own reload started" : "the game's own reload did not start, the bot code does it",
                Reload.Duration);
    }

    static void LogReloadSummary()
    {
        if (GReloadsStarted == 0 && GStuckReloadFlags == 0 && GStuckEquipFlags == 0)
            return;

        LogLine("[RELOAD] Reloads started %d (the game's own %d, the bot code's %d; topping off after a fight %d) | refilled by the game "
            "%d, by the bot code %d | stopped by a gun change %d | stuck \"reloading\" flags cleared %d, stuck \"equipping\" "
            "flags cleared %d.",
            GReloadsStarted, GReloadsByGame, GReloadsByBotCode, GReloadsTopOff, GReloadsRefilledByGame,
            GReloadsRefilledByBotCode, GReloadsCancelled, GStuckReloadFlags, GStuckEquipFlags);

        LogLine("[RELOAD] After the game's own reload: %d gun(s) fired their next burst, %d did not | OnRep_AmmoCount run after a reload %d "
            "time(s) | full bursts with rounds in the gun that fired nothing %d (not counted: %d from a pump or bolt-action still "
            "between shots) | fresh copies of a gun %d (%d held back: one every 3 seconds at most) | bots left be after 3 fresh "
            "copies that did not fire %zu.",
            GGunsFireAfterGameReload, GGunsDeadAfterGameReload, GAmmoSyncs, GGunJamsSeen, GDryBurstsStillCycling, GGunsReissued,
            GGunsReissueRefused, BotReissueGivenUp.size());
    }

    static bool GFocusFaulted = false;
    static bool GAimReadFaulted = false;

    static void BotFocusRaw(::AAIController* AIController, int Mode, AActor* Focus, const FVector* Point)
    {
        if (Mode == 1)
        {
            AActor* NewFocus = Focus;
            AIController->K2_SetFocus(NewFocus);
        }
        else if (Mode == 2)
        {
            FVector FocalPoint = *Point;
            AIController->K2_SetFocalPoint(FocalPoint);
        }
        else
        {
            AIController->K2_ClearFocus();
        }
    }

    static void BotFocusGuarded(::AAIController* AIController, int Mode, AActor* Focus, const FVector* Point)
    {
        __try
        {
            BotFocusRaw(AIController, Mode, Focus, Point);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GFocusFaulted = true;
        }
    }

    static bool BotReadAimRaw(::AAIController* AIController, double* OutYaw)
    {
        const FRotator Actual = AIController->GetControlRotation();
        *OutYaw = (double)Actual.Yaw;
        return true;
    }

    static bool BotReadAimGuarded(::AAIController* AIController, double* OutYaw)
    {
        __try
        {
            return BotReadAimRaw(AIController, OutYaw);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GAimReadFaulted = true;
            return false;
        }
    }

    static bool GStopFaulted = false;

    static void BotStopWalkingRaw(::AAIController* AIController)
    {

        if (IsPlayerControllerBot((AActor*)AIController))
        {
            auto Pawn = ((AFortPlayerControllerAthena*)AIController)->Pawn;
            auto It = Pawn ? BotBodies.find((void*)Pawn) : BotBodies.end();

            if (It != BotBodies.end())
                It->second.bHasGoal = false;

            return;
        }

        AIController->StopMovement();
    }

    static uint8 BotMoveTo(::AAIController* AIController, AFortPlayerPawnAthena* Pawn, FVector Goal, float AcceptRadius)
    {
        if (IsPlayerControllerBot((AActor*)AIController))
        {
            if (!Pawn)
                return 0;

            auto& Body = BotBodies[(void*)Pawn];
            Body.bHasGoal = true;
            Body.Goal = Goal;
            Body.AcceptRadius = AcceptRadius;
            return 2;
        }

        return AIController->MoveToLocation(Goal, AcceptRadius, true, false, false, false, (UClass*)nullptr, true);
    }

    static void BotStopWalkingGuarded(::AAIController* AIController)
    {
        __try
        {
            BotStopWalkingRaw(AIController);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GStopFaulted = true;
        }
    }

    static void BotAimAt(::AAIController* AIController, AFortPlayerPawnAthena* OnTarget, const FVector& From,
        double AimYaw, double AimPitch, double Distance)
    {
        if (!AIController || GFocusFaulted)
            return;

        static int Available = -1;

        if (Available == -1)
        {
            Available = (AIController->GetFunction("K2_SetFocus") && AIController->GetFunction("K2_SetFocalPoint") &&
                AIController->GetFunction("K2_ClearFocus")) ? 1 : 0;

            LogLine("[AIM] The engine's aim focus %s.",
                Available ? "is available -- a bot's whole burst goes where the bot code aims it"
                : "is NOT on this build -- only the first bullet of a burst follows the bot code's aim");
        }

        if (Available != 1)
            return;

        if (OnTarget)
        {
            BotFocusGuarded(AIController, 1, (AActor*)OnTarget, nullptr);
        }
        else
        {
            const double R = 3.14159265358979323846 / 180.0;

            FVector Point{};
            Point.X = From.X + cos(AimPitch * R) * cos(AimYaw * R) * Distance;
            Point.Y = From.Y + cos(AimPitch * R) * sin(AimYaw * R) * Distance;
            Point.Z = From.Z + sin(AimPitch * R) * Distance;

            BotFocusGuarded(AIController, 2, nullptr, &Point);
        }

        if (GFocusFaulted)
        {
            LogLine("[AIM] The engine's aim focus faulted on this build -- switched off; only the first bullet of a burst "
                "follows the bot code's aim from here on.");
            return;
        }

        BotHasFocus.insert((void*)AIController);
    }

    static void BotStopAiming(::AAIController* AIController)
    {
        if (!AIController)
            return;

        auto It = BotHasFocus.find((void*)AIController);
        if (It == BotHasFocus.end())
            return;

        BotHasFocus.erase(It);

        if (!GFocusFaulted)
            BotFocusGuarded(AIController, 0, nullptr, nullptr);
    }

    class BKismetSystemLibrary : public UObject
    {
    public:
        UCLASS_COMMON_MEMBERS(BKismetSystemLibrary);

        DEFINE_STATIC_FUNC(LineTraceSingle, bool);
    };

    struct FBotHit
    {
        bool bHit = false;
        AActor* Actor = nullptr;
    };

    static bool GTraceFaulted = false;

    static bool BotTraceAvailable()
    {
        static int Available = -1;

        if (Available == -1)
        {
            auto Library = BKismetSystemLibrary::GetDefaultObj();
            Available = (Library && Library->GetFunction("LineTraceSingle")) ? 1 : 0;

            LogLine("[EYES] The engine's line trace %s.",
                Available ? "is available -- bots also check the ground is not in the way before they shoot"
                : "is NOT on this build -- only the game's line-of-sight check decides what a bot can see");
        }

        return Available == 1 && !GTraceFaulted;
    }

    static bool BotTraceRaw(AActor* Self, const FVector& Start, const FVector& End, uint8 Channel, uint8* HitBuffer,
        AActor* AlsoIgnore)
    {
        struct FColour
        {
            float R, G, B, A;
        };

        UObject* Context = (UObject*)Self;
        TArray<AActor*> Ignore{};

        AActor* IgnoreList[1] = { AlsoIgnore };

        if (AlsoIgnore)
        {
            Ignore.Data = IgnoreList;
            Ignore.NumElements = 1;
            Ignore.MaxElements = 1;
        }

        bool bComplex = false;
        uint8 DrawDebug = 0;
        void* HitOut = (void*)HitBuffer;
        bool bIgnoreSelf = true;
        FColour TraceColour{ 1.f, 0.f, 0.f, 1.f };
        FColour HitColour{ 0.f, 1.f, 0.f, 1.f };
        float DrawTime = 0.f;

        return BKismetSystemLibrary::LineTraceSingle(Context, Start, End, Channel, bComplex, Ignore, DrawDebug,
            HitOut, bIgnoreSelf, TraceColour, HitColour, DrawTime);
    }

    static bool BotTraceGuarded(AActor* Self, const FVector* Start, const FVector* End, uint8 Channel, uint8* HitBuffer,
        AActor* AlsoIgnore = nullptr)
    {
        __try
        {
            return BotTraceRaw(Self, *Start, *End, Channel, HitBuffer, AlsoIgnore);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GTraceFaulted = true;
            return false;
        }
    }

    static FBotHit BotTrace(AActor* Self, const FVector& Start, const FVector& End, uint8 Channel, AActor* AlsoIgnore = nullptr)
    {
        FBotHit Out{};

        if (!Self || !BotTraceAvailable())
            return Out;

        static int32 ActorOffset = -2;

        if (ActorOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            ActorOffset = HitStruct ? (int32)HitStruct->GetOffset("Actor") : -1;

            LogLine("[EYES] Line trace hit results: what was hit %s.", ActorOffset >= 0 ? "is readable" : "is NOT readable");
        }

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        Out.bHit = BotTraceGuarded(Self, &Start, &End, Channel, HitBuffer, AlsoIgnore);

        if (GTraceFaulted)
        {
            LogLine("[EYES] The engine's line trace faulted on this build -- switched off; the game's line-of-sight check "
                "alone decides what a bot can see from here on.");
            return FBotHit{};
        }

        if (Out.bHit && ActorOffset >= 0 && ActorOffset < 0x3F0)
            Out.Actor = (AActor*)((FWeakObjectPtr*)(HitBuffer + ActorOffset))->Get();

        return Out;
    }

    static std::string BotObjectName(const UObject* Object)
    {
        if (!Object)
            return std::string("nothing");

        auto Name = Object->Name.ToString();
        return std::string(Name.c_str());
    }

    static std::string BotClassName(const UObject* Object)
    {
        if (!Object || !Object->Class)
            return std::string("?");

        auto Name = Object->Class->Name.ToString();
        return std::string(Name.c_str());
    }

    static bool BotStraightLineSees(AFortPlayerPawnAthena* Me, AActor* Them, std::string* WhatIsInTheWay)
    {
        if (!Me || !Them)
            return false;

        if (!BotTraceAvailable())
            return true;

        const FVector MyLoc = Me->K2_GetActorLocation();
        const FVector ThemLoc = Them->K2_GetActorLocation();

        FVector Eyes(MyLoc.X, MyLoc.Y, MyLoc.Z + 60.0);

        auto MyGun = (Me->HasCurrentWeapon() && Me->CurrentWeapon) ? (AActor*)Me->CurrentWeapon : nullptr;

        const double Heights[2] = { 30.0, 70.0 };
        std::string FirstInTheWay;

        for (int h = 0; h < 2; h++)
        {
            FVector Point(ThemLoc.X, ThemLoc.Y, ThemLoc.Z + Heights[h]);

            const FBotHit Hit = BotTrace((AActor*)Me, Eyes, Point, 0, MyGun);

            if (GTraceFaulted)
                return true;

            if (!Hit.bHit)
                return true;

            if (Hit.Actor)
            {
                if (Hit.Actor == Them)
                    return true;

                if ((AActor*)Hit.Actor->Owner == Them)
                    return true;
            }

            if (FirstInTheWay.empty())
                FirstInTheWay = Hit.Actor ? (BotObjectName(Hit.Actor) + " (" + BotClassName(Hit.Actor) + ")")
                : std::string("something that is not an actor");
        }

        if (WhatIsInTheWay)
            *WhatIsInTheWay = FirstInTheWay;

        return false;
    }

    static bool BotGroundInTheWay(AFortPlayerPawnAthena* Me, const FVector& MyLoc, AFortPlayerPawnAthena* Them,
        const FVector& ThemLoc, std::string* WhatWasHit = nullptr)
    {
        if (!Me || !Them || !BotTraceAvailable())
            return false;

        FVector Eyes = MyLoc;
        Eyes.Z = MyLoc.Z + 60.0;

        FVector Chest = ThemLoc;
        Chest.Z = ThemLoc.Z + 30.0;

        const uint8 Channels[2] = { 1, 0 };

        for (int c = 0; c < 2; c++)
        {
            const FBotHit Hit = BotTrace((AActor*)Me, Eyes, Chest, Channels[c]);

            if (!Hit.bHit || !Hit.Actor || Hit.Actor == (AActor*)Them)
                continue;

            const std::string ClassName = BotClassName(Hit.Actor);

            if (ClassName.find("Landscape") != std::string::npos)
            {
                if (WhatWasHit)
                    *WhatWasHit = BotObjectName(Hit.Actor);

                return true;
            }
        }

        return false;
    }

    static std::map<void*, std::pair<int, bool>> BotGroundCheck;

    static float TimeAccum = 0.f;
    static float LastFillTime = 0.f;
    static bool bMovedToIsland = false;
    static float BusStartedTime = 0.f;

    static int GBodyVerdict = 0;
    static int GBodyChecks = 0;
    static int GBodyChecksFell = 0;
    static int GBodyScriptedOnTheirOwn = 0;
    static int GBodyMovesSent = 0;
    static int GBodyScriptedBlocked = 0;
    static float GLastBodyTick = -1.f;

    static UFunction* GServerMoveFn = nullptr;
    static bool GServerMoveResolved = false;
    static uint32 GSM_TimeStamp = 0, GSM_InAccel = 0, GSM_ClientLoc = 0, GSM_Flags = 0, GSM_Roll = 0, GSM_View = 0, GSM_Mode = 0, GSM_Size = 0;

    static bool ResolveServerMove(AFortPlayerPawnAthena* Pawn)
    {
        if (GServerMoveResolved)
            return GServerMoveFn != nullptr;

        GServerMoveResolved = true;

        auto Fn = Pawn ? ((UObject*)Pawn)->GetFunction("ServerMoveNoBase") : nullptr;
        if (!Fn)
        {
            LogLine("[BODY] ServerMoveNoBase is not on this build -- every bot is moved by script.");
            return false;
        }

        auto Params = Fn->GetParamsNamed();
        int Found = 0;

        for (auto& P : Params.NameOffsetMap)
        {
            if (P.Name == "TimeStamp" && P.ElementSize == 4) { GSM_TimeStamp = P.Offset; Found |= 1; }
            else if (P.Name == "InAccel" && P.ElementSize == 12) { GSM_InAccel = P.Offset;   Found |= 2; }
            else if (P.Name == "ClientLoc" && P.ElementSize == 12) { GSM_ClientLoc = P.Offset; Found |= 4; }
            else if (P.Name == "CompressedMoveFlags" && P.ElementSize == 1) { GSM_Flags = P.Offset;     Found |= 8; }
            else if (P.Name == "ClientRoll" && P.ElementSize == 1) { GSM_Roll = P.Offset;      Found |= 16; }
            else if (P.Name == "View" && P.ElementSize == 4) { GSM_View = P.Offset;      Found |= 32; }
            else if (P.Name == "ClientMovementMode" && P.ElementSize == 1) { GSM_Mode = P.Offset;      Found |= 64; }
        }

        GSM_Size = Params.Size;

        if (Found != 127 || GSM_Size == 0 || GSM_Size > 0x200)
        {
            int Count = 0;
            for (int Bit = 0; Bit < 7; Bit++)
                if (Found & (1 << Bit))
                    Count++;

            LogLine("[BODY] ServerMoveNoBase does not look the way it should on this build (found %d of 7 arguments, %u bytes) -- "
                "every bot is moved by script.", Count, GSM_Size);
            return false;
        }

        GServerMoveFn = Fn;
        LogLine("[BODY] Found the game's ServerMoveNoBase (%u-byte move) -- bots send their moves the way a real player's game does.", GSM_Size);
        return true;
    }

    static bool SendMoveRaw(AFortPlayerPawnAthena* Pawn, uint8* Params)
    {
        ((UObject*)Pawn)->ProcessEvent(GServerMoveFn, Params);
        return true;
    }

    static bool SendMoveGuarded(AFortPlayerPawnAthena* Pawn, uint8* Params)
    {
        __try
        {
            return SendMoveRaw(Pawn, Params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static uint16 CompressAxis(double Angle)
    {
        double A = fmod(Angle, 360.0);
        if (A < 0.0)
            A += 360.0;

        return (uint16)(((long)lround(A * 65536.0 / 360.0)) & 0xFFFF);
    }

    static double BotHalfHeight(AFortPlayerPawnAthena* Pawn)
    {
        static double Cached = -1.0;
        if (Cached > 0.0 || !Pawn)
            return Cached > 0.0 ? Cached : 90.0;

        Cached = 90.0;

        auto Root = (UObject*)Pawn->RootComponent;
        if (Root)
        {
            const int32 Offset = (int32)Root->GetOffset("CapsuleHalfHeight");
            if (Offset > 0)
            {
                const float Half = GetFromOffset<float>(Root, (uint32)Offset);
                if (Half > 20.f && Half < 300.f)
                    Cached = Half;
            }
        }

        LogLine("[BODY] A bot is %.0f units from its middle to its feet (capsule half-height).", Cached);
        return Cached;
    }

    static float BotMaxAcceleration(AFortPlayerPawnAthena* Pawn)
    {
        static float Cached = -1.f;
        if (Cached > 0.f || !Pawn)
            return Cached > 0.f ? Cached : 2048.f;

        Cached = 2048.f;

        auto Move = Pawn->HasCharacterMovement() ? (UObject*)Pawn->CharacterMovement : nullptr;
        if (Move)
        {
            const int32 Offset = (int32)Move->GetOffset("MaxAcceleration");
            if (Offset > 0)
            {
                const float Accel = GetFromOffset<float>(Move, (uint32)Offset);
                if (Accel > 100.f && Accel < 100000.f)
                    Cached = Accel;
            }
        }

        return Cached;
    }

    static int32 GHitPointOffset = -2;

    static bool BotGroundBelow(AFortPlayerPawnAthena* Pawn, double X, double Y, double FromZ, double ToZ, double& OutZ)
    {
        if (!Pawn || !BotTraceAvailable())
            return false;

        if (GHitPointOffset == -2)
        {
            auto HitStruct = FindStruct("HitResult");
            GHitPointOffset = HitStruct ? (int32)HitStruct->GetOffset("ImpactPoint") : -1;

            if (GHitPointOffset < 0 && HitStruct)
                GHitPointOffset = (int32)HitStruct->GetOffset("Location");

            LogLine("[BODY] Ground traces: where the trace hit %s.", GHitPointOffset >= 0 ? "is readable"
                : "is NOT readable -- scripted movement cannot find the ground");
        }

        if (GHitPointOffset < 0 || GHitPointOffset > 0x3C0)
            return false;

        alignas(16) uint8 HitBuffer[0x400];
        memset(HitBuffer, 0, sizeof(HitBuffer));

        FVector Start(X, Y, FromZ);
        FVector End(X, Y, ToZ);

        if (!BotTraceGuarded((AActor*)Pawn, &Start, &End, 0, HitBuffer))
            return false;

        if (VersionInfo.FortniteVersion >= 20)
            OutZ = *(double*)(HitBuffer + GHitPointOffset + 16);
        else
            OutZ = (double)*(float*)(HitBuffer + GHitPointOffset + 8);

        return true;
    }

    static int GPathDetours = 0;
    static int GPathBoxedIn = 0;
    static int GStuckEvents = 0;

    // Is it safe to walk this way? Looks for walls, trees, buildings, cliffs and steep faces.
    // Steps = how many 1.2 m ground probes to look ahead (4 normally, more when a bot is coming back from a detour).
    static bool BotDirectionSafe(AFortPlayerPawnAthena* Pawn, const FVector& Loc, double Half, double Dx, double Dy, int Steps = 4)
    {
        const double Feet = Loc.Z - Half;

        // 1) something solid straight ahead at knee height or head height
        if (BotTraceAvailable())
        {
            const double Heights[2] = { 50.0, 110.0 };
            const double Reach = 220.0 + (double)(Steps - 4) * 120.0;

            for (int h = 0; h < 2; h++)
            {
                FVector Start(Loc.X, Loc.Y, Feet + Heights[h]);
                FVector End(Loc.X + Dx * Reach, Loc.Y + Dy * Reach, Feet + Heights[h]);

                const FBotHit Hit = BotTrace((AActor*)Pawn, Start, End, 0);

                if (!Hit.bHit)
                    continue;

                if (Hit.Actor)
                {
                    if (BotClassName(Hit.Actor).find("Landscape") != std::string::npos)
                        continue;   // slopes are judged by the ground probes below

                    if (BotLooksLikeDoor(Hit.Actor))
                        continue;   // a door is opened, not walked around
                }

                return false;
            }
        }

        // 2) the ground ahead: no big drop, no slope steeper than a player can climb
        double PrevZ = Feet;

        for (int s = 1; s <= Steps; s++)
        {
            const double Dist = 120.0 * s;
            const double X = Loc.X + Dx * Dist;
            const double Y = Loc.Y + Dy * Dist;
            double Ground = 0.0;

            if (!BotGroundBelow(Pawn, X, Y, PrevZ + 260.0, PrevZ - 1500.0, Ground) &&
                !BotGroundBelow(Pawn, X, Y, PrevZ + 700.0, PrevZ - 1500.0, Ground))
                return false;   // no ground at all (void / water)

            if (Ground < PrevZ - 250.0)
                return false;   // drop-off (a walkable slope drops ~120 per 120)

            if (Ground > PrevZ + 130.0)
                return false;   // steeper than about 47 degrees: a player can't walk that either

            PrevZ = Ground;
        }

        return true;
    }

    static int GLedgeStops = 0;

    // True if there is a drop, or no ground at all, within ~6 m in this direction.
    // Walls do NOT count (a stuck bot may still hop over a small lip), only ledges.
    static bool BotLedgeAhead(AFortPlayerPawnAthena* Pawn, const FVector& Loc, double Dx, double Dy)
    {
        if (!BotTraceAvailable())
            return false;

        const double Len = sqrt(Dx * Dx + Dy * Dy);
        if (Len < 0.001)
            return false;

        Dx /= Len;
        Dy /= Len;

        double PrevZ = Loc.Z - BotHalfHeight(Pawn);

        for (int s = 1; s <= 4; s++)
        {
            const double X = Loc.X + Dx * 150.0 * (double)s;
            const double Y = Loc.Y + Dy * 150.0 * (double)s;
            double Ground = 0.0;

            if (!BotGroundBelow(Pawn, X, Y, PrevZ + 300.0, PrevZ - 2000.0, Ground) &&
                !BotGroundBelow(Pawn, X, Y, PrevZ + 800.0, PrevZ - 2000.0, Ground))
                return true;   // nothing below: void

            if (Ground < PrevZ - 250.0)
                return true;   // cliff

            PrevZ = Ground;
        }

        return false;
    }

    struct FSteerMemory
    {
        int Side = 0;
        int Tick = -1000;
    };

    static std::map<void*, FSteerMemory> BotSteerMemory;

    // Finds a safe direction as close to the wanted one as possible. False if everything is blocked.
    // A bot that has gone round an obstacle on one side keeps going that side for ~4 seconds (no more left/right flipping),
    // and only turns back towards its goal once the way is clear a lot further ahead.
    static bool BotPickSafeDirection(AFortPlayerPawnAthena* Pawn, const FVector& Loc, double Half, double WantX, double WantY,
        int PreferSide, double& OutX, double& OutY, double& OutOffset)
    {
        static const double Angles[] = { 0.0, 20.0, 40.0, 60.0, 80.0, 105.0, 130.0, 155.0, 180.0 };
        const double Rad = 3.14159265358979323846 / 180.0;

        FSteerMemory& Mem = BotSteerMemory[(void*)Pawn];
        const bool bCommitted = Mem.Side != 0 && (GStormTick - Mem.Tick) < 120;
        const int Side = bCommitted ? Mem.Side : (PreferSide >= 0 ? 1 : -1);

        // order to try: straight, then the committed side up to 105 degrees, then the other side up to 105, then the sharp turns
        double Order[16];
        int Count = 0;

        Order[Count++] = 0.0;

        for (int i = 1; i <= 5; i++)
            Order[Count++] = Angles[i] * (double)Side;

        for (int i = 1; i <= 5; i++)
            Order[Count++] = -Angles[i] * (double)Side;

        for (int i = 6; i <= 8; i++)
        {
            Order[Count++] = Angles[i] * (double)Side;

            if (Angles[i] < 179.0)
                Order[Count++] = -Angles[i] * (double)Side;
        }

        for (int n = 0; n < Count; n++)
        {
            const double Signed = Order[n];
            const double C = cos(Signed * Rad);
            const double S = sin(Signed * Rad);
            const double Dx = WantX * C - WantY * S;
            const double Dy = WantX * S + WantY * C;

            const int Steps = (Signed == 0.0 && bCommitted) ? 7 : 4;

            if (!BotDirectionSafe(Pawn, Loc, Half, Dx, Dy, Steps))
                continue;

            OutX = Dx;
            OutY = Dy;
            OutOffset = Signed;

            if (Signed != 0.0)
            {
                Mem.Side = (Signed > 0.0) ? 1 : -1;
                Mem.Tick = GStormTick;
            }

            return true;
        }

        return false;
    }

    // Picks a wander destination whose straight line does not run into a mountain, a cliff or the void.
    // It samples the ground every 5 m along 8 random directions and takes the one that stays walkable the longest
    // (cut short just before the first bad spot), so bots walk round mountains instead of into them.
    static FVector PickWalkableWanderDest(AFortPlayerPawnAthena* Pawn, const FVector& MyLoc)
    {
        const double Half = BotHalfHeight(Pawn);
        const double PI_ = 3.14159265358979323846;

        FVector Best = MyLoc;
        double BestReach = -1.0;

        for (int Try = 0; Try < 8; Try++)
        {
            const double Angle = (double)(rand() % 360) * PI_ / 180.0;
            const double Wanted = 3000.0 + (double)(rand() % 8000);
            const double Dx = cos(Angle);
            const double Dy = sin(Angle);

            double PrevZ = MyLoc.Z - Half;
            double Reach = 0.0;
            const int Samples = (int)(Wanted / 500.0);

            for (int s = 1; s <= Samples; s++)
            {
                const double X = MyLoc.X + Dx * 500.0 * (double)s;
                const double Y = MyLoc.Y + Dy * 500.0 * (double)s;
                double Ground = 0.0;

                if (!BotGroundBelow(Pawn, X, Y, PrevZ + 4000.0, PrevZ - 4000.0, Ground))
                    break;   // void / water

                const double Rise = Ground - PrevZ;

                if (Rise > 330.0 || Rise < -900.0)
                    break;   // mountain face or cliff

                PrevZ = Ground;
                Reach = 500.0 * (double)s;
            }

            if (Reach > BestReach)
            {
                BestReach = Reach;
                Best = FVector(MyLoc.X + Dx * Reach, MyLoc.Y + Dy * Reach, MyLoc.Z);
            }

            if (Reach >= Wanted - 500.0)
                break;   // a clear walk all the way: take it
        }

        if (BestReach < 1000.0)
        {
            // everything nearby is blocked: just try a short hop in the best direction found
            const double Dx = Best.X - MyLoc.X;
            const double Dy = Best.Y - MyLoc.Y;
            const double Len = sqrt(Dx * Dx + Dy * Dy);

            if (Len > 1.0)
                Best = FVector(MyLoc.X + Dx / Len * 1000.0, MyLoc.Y + Dy / Len * 1000.0, MyLoc.Z);
            else
                Best = FVector(MyLoc.X + 1000.0, MyLoc.Y, MyLoc.Z);
        }

        return Best;
    }

    // A coarse local planner for the storm: tries 11 directions around the straight line to the goal, walks each one
// along the ground (400 unit samples), stops at the first cliff / wall / void, and takes the one that ends closest to the goal.
    static FVector PickWalkableHopToward(AFortPlayerPawnAthena* Pawn, const FVector& MyLoc, const FVector& Goal)
    {
        const double Gx = Goal.X - MyLoc.X;
        const double Gy = Goal.Y - MyLoc.Y;
        const double GLen = sqrt(Gx * Gx + Gy * Gy);

        if (GLen < 800.0 || !BotTraceAvailable())
            return Goal;

        const double PI_ = 3.14159265358979323846;
        const double BaseYaw = atan2(Gy, Gx);
        const double Half = BotHalfHeight(Pawn);
        const double MaxReach = GLen < 3200.0 ? GLen : 3200.0;

        static const double Offsets[] = { 0.0, 20.0, -20.0, 45.0, -45.0, 70.0, -70.0, 100.0, -100.0, 135.0, -135.0 };

        FVector Best = Goal;
        double BestScore = -1e18;

        for (double OffDeg : Offsets)
        {
            const double Angle = BaseYaw + OffDeg * PI_ / 180.0;
            const double Dx = cos(Angle);
            const double Dy = sin(Angle);

            double PrevZ = MyLoc.Z - Half;
            double Reach = 0.0;

            for (double D = 400.0; D <= MaxReach; D += 400.0)
            {
                double Ground = 0.0;

                if (!BotGroundBelow(Pawn, MyLoc.X + Dx * D, MyLoc.Y + Dy * D, PrevZ + 800.0, PrevZ - 1500.0, Ground))
                    break;   // void

                const double Rise = Ground - PrevZ;

                if (Rise > 300.0 || Rise < -300.0)
                    break;   // mountain face or cliff

                PrevZ = Ground;
                Reach = D;
            }

            if (Reach < 400.0)
                continue;

            const double Ex = MyLoc.X + Dx * Reach - Goal.X;
            const double Ey = MyLoc.Y + Dy * Reach - Goal.Y;
            const double Score = -sqrt(Ex * Ex + Ey * Ey) - fabs(OffDeg) * 3.0;

            if (Score > BestScore)
            {
                BestScore = Score;
                Best = FVector(MyLoc.X + Dx * Reach, MyLoc.Y + Dy * Reach, MyLoc.Z);
            }
        }

        return Best;
    }

    // ======================= DOORS AND WALLS =======================
    struct FBotBreak
    {
        AActor* Target = nullptr;
        AFortPlayerPawnAthena* Owner = nullptr;
        bool bDoor = false;
        int Hits = 0;
        int NextHitTick = 0;
        int EndTick = 0;
        float StartHealth = -1.f;
        bool bSwinging = false;
        int SwingEndTick = 0;
        bool bFallback = false;
        float SimHealth = 200.f;
    };

    static std::map<void*, FBotBreak> BotBreaks;
    static std::set<AActor*> GIgnoredObstacles;
    static std::map<AActor*, int> GDoorTries;
    static int GDoorsOpened = 0;
    static int GWallsMined = 0;
    static int GWallsForced = 0;
    static bool GDoorNamesDumped = false;

    // doors that are open now are ignored for a while so a bot never presses E on them again (that would close them)
    static std::map<AActor*, int> GIgnoredUntil;

    static bool ObstacleIgnored(AActor* A)
    {
        auto It = GIgnoredUntil.find(A);
        if (It == GIgnoredUntil.end())
            return false;

        if (GStormTick >= It->second)
        {
            GIgnoredUntil.erase(It);
            return false;
        }

        return true;
    }

    // 1 = open, 0 = closed, -1 = no such flag
    static int DoorIsOpen(AActor* D)
    {
        static const char* Flags[] = { "bDoorOpen", "bIsDoorOpen", "bIsOpen", "bOpen" };

        for (auto F : Flags)
        {
            const int V = ReadBoolByName((const UObject*)D, F);
            if (V != -1)
                return V;
        }

        return -1;
    }

    static bool BotBreakActive(AFortPlayerPawnAthena* Pawn)
    {
        auto It = BotBreaks.find((void*)Pawn);
        return It != BotBreaks.end() && It->second.Target != nullptr;
    }

    static std::string LowerText(std::string S)
    {
        for (auto& Ch : S)
            Ch = (char)tolower((unsigned char)Ch);
        return S;
    }


    static bool BotLooksLikeDoor(AActor* Actor)
    {
        if (!Actor)
            return false;

        // Names that contain "door" but are NOT something you open.
        static const char* NotDoors[] = { "outdoor", "doormat", "doorway", "doorframe", "door_frame", "doorjamb", "doorknob" };

        for (const UStruct* Cls = (const UStruct*)Actor->Class; Cls; Cls = Cls->GetSuper())
        {
            const std::string Lower = LowerText(BotObjectName((const UObject*)Cls));

            if (Lower.find("door") == std::string::npos)
                continue;

            bool bExcluded = false;

            for (auto Bad : NotDoors)
            {
                if (Lower.find(Bad) != std::string::npos)
                {
                    bExcluded = true;
                    break;
                }
            }

            if (!bExcluded)
                return true;
        }

        return false;
    }

    // Only walls, windows, fences and solid pieces are worth mining. Props, trees, rocks and vehicles are walked around.
    static bool BotIsBreakableStructure(AActor* Actor)
    {
        if (!Actor)
            return false;

        if (BotLooksLikeDoor(Actor))
            return false;

        const std::string Text = LowerText(BotClassName((const UObject*)Actor) + " " + BotObjectName((const UObject*)Actor));
        static const char* Keys[] = { "wall", "window", "fence", "storefront", "glass", "solid" };

        for (auto Key : Keys)
            if (Text.find(Key) != std::string::npos)
                return true;

        return false;
    }

    // Looks straight ahead (knee and head height) for a door or a building piece. Doors win over walls.
    static AActor* BotFindObstacle(AFortPlayerPawnAthena* Pawn, const FVector& Loc, double Half, double Dx, double Dy, bool* OutDoor, double Reach = 260.0)
    {
        *OutDoor = false;

        if (!BotTraceAvailable())
            return nullptr;

        const double Feet = Loc.Z - Half;
        const double Heights[2] = { 50.0, 110.0 };
        AActor* Found = nullptr;

        for (int h = 0; h < 2; h++)
        {
            FVector Start(Loc.X, Loc.Y, Feet + Heights[h]);
            FVector End(Loc.X + Dx * Reach, Loc.Y + Dy * Reach, Feet + Heights[h]);

            const FBotHit Hit = BotTrace((AActor*)Pawn, Start, End, 0);

            if (!Hit.bHit || !Hit.Actor)
                continue;

            if (Hit.Actor->IsA<AFortPlayerPawnAthena>() || GIgnoredObstacles.find(Hit.Actor) != GIgnoredObstacles.end() ||
                ObstacleIgnored(Hit.Actor))
                continue;

            const std::string Cls = LowerText(BotClassName(Hit.Actor));
            const std::string Nm = LowerText(BotObjectName(Hit.Actor));

            if (Cls.find("landscape") != std::string::npos)
                continue;

            // an open doorway or archway is an opening: walk through it, never mine it
            if (Cls.find("doorway") != std::string::npos || Nm.find("doorway") != std::string::npos ||
                Cls.find("archway") != std::string::npos || Nm.find("archway") != std::string::npos)
                continue;

            const bool bDoor = BotLooksLikeDoor(Hit.Actor);

            if (!bDoor && !(Hit.Actor->IsA<ABuildingSMActor>() && BotIsBreakableStructure(Hit.Actor)))
                continue;

            if (bDoor)
            {
                *OutDoor = true;
                return Hit.Actor;
            }

            if (!Found)
                Found = Hit.Actor;
        }

        return Found;
    }

    static void ListNamesContaining(UObject* Object, const char* Label)
    {
        if (!Object || !Object->Class)
            return;

        static const char* Keys[] = { "door", "open", "interact", "lock" };
        std::string Found;
        int Count = 0;

        for (const UStruct* Cls = (const UStruct*)Object->Class; Cls && Found.size() < 900; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child && Found.size() < 900; Child = Child->GetNext())
            {
                auto RawName = Child->GetName().ToString();
                const std::string Lowered = LowerText(std::string(RawName.c_str()));

                for (auto Key : Keys)
                {
                    if (Lowered.find(Key) != std::string::npos)
                    {
                        Found += std::string(RawName.c_str()) + ", ";
                        Count++;
                        break;
                    }
                }
            }
        }

        LogLine("[DOOR] %s (class %s) names with door/open/interact/lock (%d): %s", Label, BotClassName(Object).c_str(), Count, Found.c_str());
    }

    static std::map<AActor*, int> GDoorRestore;

    static bool SetActorCollision(AActor* Actor, bool bOn)
    {
        auto Fn = Actor ? ((UObject*)Actor)->GetFunction("SetActorEnableCollision") : nullptr;
        if (!Fn)
            return false;

        alignas(16) uint8 Params[0x20];
        memset(Params, 0, sizeof(Params));
        Params[0] = bOn ? 1 : 0;

        ((UObject*)Actor)->ProcessEvent(Fn, Params);
        return true;
    }

    static void RestoreDoorCollisions()
    {
        for (auto It = GDoorRestore.begin(); It != GDoorRestore.end();)
        {
            if (!ActorIsLive((const UObject*)It->first))
            {
                It = GDoorRestore.erase(It);
                continue;
            }

            if (GStormTick >= It->second)
            {
                SetActorCollision(It->first, true);
                It = GDoorRestore.erase(It);
                continue;
            }

            ++It;
        }
    }

    // Tries the common "is open" flags. Returns true only if it really flipped one from closed to open.
    static bool BotOpenDoor(AActor* Door, std::string* How)
    {
        if (!BotLooksLikeDoor(Door))
            return false;   // never flip "open" flags on walls, fences or windows

        static const char* Flags[] = { "bDoorOpen", "bIsDoorOpen", "bIsOpen", "bOpen", "bOpened", "bDoorOpened" };

        for (auto Flag : Flags)
        {
            if (ReadBoolByName((const UObject*)Door, Flag) != 0)
                continue;   // missing property (-1) or already open (1)

            SetBoolByName((UObject*)Door, Flag, true);

            const std::string OnRep = std::string("OnRep_") + Flag;
            InvokeNoArgsGuarded((const UObject*)Door, OnRep.c_str());
            InvokeNoArgs((const UObject*)Door, "ForceNetUpdate");

            *How = Flag;
            return true;
        }

        if (!GDoorNamesDumped)
        {
            GDoorNamesDumped = true;
            ListNamesContaining((UObject*)Door, "The door");
        }

        return false;
    }

    static float BuildingHealth(AActor* Building)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false;

        if (!bLooked && Building)
        {
            bLooked = true;
            Fn = Building->GetFunction("GetHealth");
        }

        return (Fn && Building) ? Building->Call<float>(Fn) : -1.f;
    }

    // Damage through the game's ApplyDamage (same function the bot bullets use).
    static bool BotDamageActor(AActor* Target, float Damage, AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        if (!Target || GApplyDamageBroken)
            return false;

        if (!GApplyDamageLooked)
        {
            GApplyDamageLooked = true;

            GGameplayStaticsCDO = (UObject*)FindObject<UObject>(L"/Script/Engine.Default__GameplayStatics");
            auto Fn = GGameplayStaticsCDO ? GGameplayStaticsCDO->GetFunction("ApplyDamage") : nullptr;
            int Found = 0;

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                GAD_Size = Params.Size;

                for (auto& P : Params.NameOffsetMap)
                {
                    if (P.Name == "DamagedActor") { GAD_Damaged = P.Offset;    Found |= 1; }
                    else if (P.Name == "BaseDamage") { GAD_Damage = P.Offset;     Found |= 2; }
                    else if (P.Name == "EventInstigator") { GAD_Instigator = P.Offset; Found |= 4; }
                    else if (P.Name == "DamageCauser") { GAD_Causer = P.Offset;     Found |= 8; }
                }
            }

            if (Fn && Found == 15 && GAD_Size > 0 && GAD_Size <= 0x100)
                GApplyDamageFn = Fn;
        }

        if (!GApplyDamageFn)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));

        *(void**)(Params + GAD_Damaged) = (void*)Target;
        *(float*)(Params + GAD_Damage) = Damage;
        *(void**)(Params + GAD_Instigator) = (void*)Controller;
        *(void**)(Params + GAD_Causer) = (void*)Pawn;

        if (!ApplyDamageGuarded(GApplyDamageFn, Params))
        {
            GApplyDamageBroken = true;
            return false;
        }

        return true;
    }

    static UFunction* GInteractFn = nullptr;
    static bool GInteractLooked = false;
    static bool GInteractBroken = false;
    static uint32 GInt_Actor = 0, GInt_Size = 0;

    static bool InteractGuarded(UObject* Controller, UFunction* Fn, uint8* Params)
    {
        __try
        {
            Controller->ProcessEvent(Fn, Params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Same call a real player's E press makes on the server.
    static bool BotInteractWith(AFortPlayerControllerAthena* Controller, AActor* Target)
    {
        if (!Controller || !Target || GInteractBroken)
            return false;

        if (!GInteractLooked)
        {
            GInteractLooked = true;

            auto Fn = ((UObject*)Controller)->GetFunction("ServerAttemptInteract");
            bool bFound = false;

            if (Fn)
            {
                auto Params = Fn->GetParamsNamed();
                GInt_Size = Params.Size;

                for (auto& P : Params.NameOffsetMap)
                    if (P.Name == "ReceivingActor" && P.ElementSize == 8)
                    {
                        GInt_Actor = P.Offset;
                        bFound = true;
                    }
            }

            if (Fn && bFound && GInt_Size > 0 && GInt_Size <= 0x100)
                GInteractFn = Fn;

            LogLine("[DOOR] The game's ServerAttemptInteract %s.", GInteractFn ? "was found -- bots open doors with it first"
                : "is NOT usable on this build -- bots fall back to the open flags");
        }

        if (!GInteractFn)
            return false;

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));
        *(void**)(Params + GInt_Actor) = (void*)Target;

        uint8* LocalFlag = BotLocalFlag(Controller);
        const uint8 WasLocal = LocalFlag ? *LocalFlag : 0;

        if (LocalFlag)
            *LocalFlag = 1;

        const bool bOk = InteractGuarded((UObject*)Controller, GInteractFn, Params);

        if (LocalFlag)
            *LocalFlag = WasLocal;

        if (!bOk)
        {
            GInteractBroken = true;
            LogLine("[DOOR] ServerAttemptInteract faulted -- not used from now on.");
            return false;
        }

        return true;
    }

    static bool BotHoldingMelee(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon() || !Pawn->CurrentWeapon)
            return false;

        auto Weapon = (AFortWeapon*)Pawn->CurrentWeapon;
        return Weapon->HasWeaponData() && Weapon->WeaponData && ((const UObject*)Weapon->WeaponData)->IsA<UFortWeaponMeleeItemDefinition>();
    }

    static bool BotTakeOutPickaxe(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory)
            return false;

        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            auto& Entry = Entries.Get(e, FFortItemEntry::Size());

            if (!Entry.ItemDefinition || !((const UObject*)Entry.ItemDefinition)->IsA<UFortWeaponMeleeItemDefinition>())
                continue;

            auto Def = Entry.ItemDefinition->Cast<UFortWeaponItemDefinition>();
            if (!Def)
                continue;

            Pawn->EquipWeaponDefinition(Def, Entry.ItemGuid);
            return true;
        }

        return false;
    }

    static void BotFaceActor(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, const FVector& Loc, AActor* Target)
    {
        const FVector At = Target->K2_GetActorLocation();
        const double Dx = At.X - Loc.X;
        const double Dy = At.Y - Loc.Y;

        if ((Dx * Dx + Dy * Dy) < 1.0)
            return;

        FRotator Look{};
        Look.Pitch = 0.0;
        Look.Yaw = atan2(Dy, Dx) * (180.0 / 3.14159265358979323846);
        Look.Roll = 0.0;

        Pawn->K2_SetActorRotation(Look, false);
        ((::AAIController*)Controller)->SetControlRotation(Look);
    }

    static void BotEndBreak(FBotBreak& B, FBotBody& Body)
    {
        if (B.bSwinging && B.Owner)
            BotTrigger(B.Owner, false);

        B.bSwinging = false;
        B.Target = nullptr;
        Body.BoxedUntil = -1;
        Body.StuckLevel = 0;
        Body.StuckTick = -1;
        Body.OverrideUntil = -1;
        Body.bSteerValid = false;
        Body.SteerCheckTick = -1000;
        Body.ProgressTick = -1;
    }

    static bool BotBeginBreak(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc,
        AActor* Obstacle, bool bDoor)
    {
        if (!Obstacle)
            return false;

        FBotBreak& B = BotBreaks[(void*)Pawn];
        B = FBotBreak{};
        B.Target = Obstacle;
        B.Owner = Pawn;
        B.bDoor = bDoor;
        B.NextHitTick = GStormTick;
        B.EndTick = GStormTick + 30 * 30;

        static int Lines = 0;
        if (++Lines <= 20)
            LogLine("[BREAK] '%s' is blocked by %s (%s) -- %s.", BotPawnName(Pawn).c_str(), BotObjectName(Obstacle).c_str(),
                BotClassName(Obstacle).c_str(), bDoor ? "trying to open it" : "mining it with its pickaxe");

        BotFaceActor(Controller, Pawn, Loc, Obstacle);
        return true;
    }

    static bool BotStartBreak(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc,
        double Gx, double Gy)
    {
        bool bDoor = false;
        AActor* Obstacle = BotFindObstacle(Pawn, Loc, BotHalfHeight(Pawn), Gx, Gy, &bDoor);
        return BotBeginBreak(Controller, Pawn, Body, Loc, Obstacle, bDoor);
    }

    static bool NameHasAny(const std::string& Lower, const char* const* Keys, int Count)
    {
        for (int k = 0; k < Count; k++)
            if (Lower.find(Keys[k]) != std::string::npos)
                return true;
        return false;
    }

    // Lists functions/properties on an object's class chain whose name contains one of the keys.
    static void DumpNamesMatching(const UObject* Object, const char* Label, const char* const* Keys, int KeyCount)
    {
        if (!Object || !Object->Class)
            return;

        std::string Funcs, Props;
        int FuncCount = 0, PropCount = 0;

        for (const UStruct* Cls = (const UStruct*)Object->Class; Cls; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child; Child = Child->GetNext())
            {
                auto Raw = Child->GetName().ToString();
                const std::string Name(Raw.c_str());

                if (!NameHasAny(LowerText(Name), Keys, KeyCount))
                    continue;

                const bool bFunction = Child->Class && (Child->Class->GetCastFlags() & 0x80000);
                std::string& Into = bFunction ? Funcs : Props;
                int& Count = bFunction ? FuncCount : PropCount;

                if (Into.size() < 1400)
                    Into += Name + " [" + BotObjectName((const UObject*)Cls) + "], ";

                Count++;
            }
        }

        LogLine("[FX] %s (class %s) FUNCTIONS (%d): %s", Label, BotClassName(Object).c_str(), FuncCount, Funcs.c_str());
        LogLine("[FX] %s (class %s) PROPERTIES (%d): %s", Label, BotClassName(Object).c_str(), PropCount, Props.c_str());
    }

    // Lists loaded assets (sounds, particles, cues) whose class and name match.
    static void DumpAssetsMatching(const char* Label, const char* const* ClassKeys, int ClassKeyCount,
        const char* const* NameKeys, int NameKeyCount)
    {
        std::string Found;
        int Count = 0;

        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            const UObject* Object = TUObjectArray::GetObjectByIndex(i);
            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (!NameHasAny(LowerText(BotClassName(Object)), ClassKeys, ClassKeyCount))
                continue;

            const std::string Name = BotObjectName(Object);
            if (!NameHasAny(LowerText(Name), NameKeys, NameKeyCount))
                continue;

            if (Count++ < 60)
                Found += Name + " (" + BotClassName(Object) + "), ";
        }

        LogLine("[FX] %s -- %d asset(s) found: %s", Label, Count, Found.c_str());
    }

    // Runs once per building class.
    static void LogMiningDiscovery(AFortPlayerPawnAthena* Pawn, AActor* Target)
    {
        static std::set<std::string> Seen;
        static bool bAssetsDumped = false;

        static const char* FxKeys[] = { "hit", "damage", "effect", "sound", "audio", "cue", "impact", "fx", "particle",
                                        "harvest", "pickaxe", "swing", "multicast", "play", "spawn", "emitter",
                                        "health", "crack", "material" };
        const int FxKeyCount = (int)(sizeof(FxKeys) / sizeof(FxKeys[0]));

        const std::string Key = BotClassName((const UObject*)Target);

        static int Dumps = 0;
        if (Dumps >= 2 || !Seen.insert(Key).second)
            return;

        Dumps++;

        LogLine("[FX] ===== mining discovery for %s (%s) =====", BotObjectName((const UObject*)Target).c_str(), Key.c_str());

        DumpNamesMatching((const UObject*)Target, "The wall/building", FxKeys, FxKeyCount);

        if (Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
            DumpNamesMatching((const UObject*)Pawn->CurrentWeapon, "The pickaxe (weapon actor)", FxKeys, FxKeyCount);

        DumpNamesMatching((const UObject*)Pawn, "The pawn", FxKeys, FxKeyCount);

        if (Pawn && Pawn->Controller)
            DumpNamesMatching((const UObject*)Pawn->Controller, "The controller", FxKeys, FxKeyCount);

        if (!bAssetsDumped)
        {
            bAssetsDumped = true;

            static const char* SoundClasses[] = { "soundcue", "soundwave" };
            static const char* SoundNames[] = { "pickaxe", "harvest", "melee", "swing" };
            DumpAssetsMatching("Loaded SOUNDS that could be the mining sound", SoundClasses, 2, SoundNames,
                (int)(sizeof(SoundNames) / sizeof(SoundNames[0])));

            static const char* FxClasses[] = { "particlesystem", "niagara", "gameplaycue", "gcn", "blueprintgeneratedclass" };
            static const char* FxNames[] = { "pickaxe", "harvest", "melee", "impact", "buildinghit", "gcn_", "wood", "stone", "metal" };
            DumpAssetsMatching("Loaded EFFECTS / GAMEPLAY CUES that could be the mining effect", FxClasses, 5, FxNames,
                (int)(sizeof(FxNames) / sizeof(FxNames[0])));
        }
    }

    // ======================= SOUNDS =======================
    static bool PlayAtLocRaw(UObject* Controller, UFunction* Fn, uint8* Params)
    {
        Controller->ProcessEvent(Fn, Params);
        return true;
    }

    static bool PlayAtLocGuarded(UObject* Controller, UFunction* Fn, uint8* Params)
    {
        __try
        {
            return PlayAtLocRaw(Controller, Fn, Params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Reads a sound pointer straight off an actor (DeathSound, DoorOpeningSound ...). Null if it is not a sound.
    static UObject* ReadSoundProperty(AActor* Actor, const char* Name)
    {
        if (!Actor)
            return nullptr;

        const uint32 Off = ((UObject*)Actor)->GetOffset(Name);
        if (Off == (uint32)-1 || Off > 0x4000)
            return nullptr;

        UObject* Sound = *(UObject**)((uint8*)Actor + Off);
        if (!Sound || IsBadReadPtr((void*)Sound))
            return nullptr;

        if (LowerText(BotClassName(Sound)).find("sound") == std::string::npos)
            return nullptr;

        return Sound;
    }

    static UObject* FindSoundByName(const char* Name)
    {
        static std::map<std::string, UObject*> Cache;

        auto It = Cache.find(Name);
        if (It != Cache.end() && It->second)
            return It->second;

        UObject* Found = (UObject*)TUObjectArray::FindObject<UObject>(Name);

        if (Found)
            Cache[Name] = Found;

        static int Lines = 0;
        if (++Lines <= 20)
            LogLine("[SOUND] Lookup '%s' -> %s", Name, Found ? "FOUND" : "NOT FOUND (will retry)");

        return Found;
    }

    // Plays a sound at a place on the screen of every real player within Range.
    static void PlaySoundNearby(UObject* Sound, const FVector& At, double Range)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false, bBroken = false;
        static uint32 O_Sound = 0, O_Loc = 0, O_Vol = 0, O_Pitch = 0, LocSize = 0;
        static int Played = 0;

        if (!Sound || bBroken)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState)
            return;

        auto GS = (AFortGameStateAthena*)World->GameState;
        if (!GS->HasPlayerArray())
            return;

        for (int i = 0; i < GS->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GS->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || PS->bIsABot || NPCs::IsNPC(PS) || !PS->Owner)
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            auto ListenerPawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!ListenerPawn)
                continue;

            const FVector Me = ListenerPawn->K2_GetActorLocation();
            const double Dx = Me.X - At.X, Dy = Me.Y - At.Y, Dz = Me.Z - At.Z;
            if (Dx * Dx + Dy * Dy + Dz * Dz > Range * Range)
                continue;

            if (!bLooked)
            {
                bLooked = true;
                Fn = ((UObject*)Controller)->GetFunction("ClientPlaySoundAtLocation");
                int Found = 0;

                if (Fn)
                {
                    auto P = Fn->GetParamsNamed();

                    for (auto& E : P.NameOffsetMap)
                    {
                        if (E.Name == "Sound") { O_Sound = E.Offset; Found |= 1; }
                        else if (E.Name == "Location") { O_Loc = E.Offset; LocSize = E.ElementSize; Found |= 2; }
                        else if (E.Name == "VolumeMultiplier") { O_Vol = E.Offset; Found |= 4; }
                        else if (E.Name == "PitchMultiplier") { O_Pitch = E.Offset; Found |= 8; }
                    }

                    if (Found != 15 || (LocSize != 12 && LocSize != 24) || P.Size == 0 || P.Size > 0x100)
                        Fn = nullptr;
                }

                LogLine("[SOUND] ClientPlaySoundAtLocation %s.", Fn ? "was found -- bot sounds play on nearby players' screens"
                    : "is NOT usable on this build -- no bot sounds");
            }

            if (!Fn)
            {
                bBroken = true;
                return;
            }

            alignas(16) uint8 Params[0x100];
            memset(Params, 0, sizeof(Params));

            *(void**)(Params + O_Sound) = (void*)Sound;

            if (LocSize == 12)
            {
                float* L = (float*)(Params + O_Loc);
                L[0] = (float)At.X; L[1] = (float)At.Y; L[2] = (float)At.Z;
            }
            else
            {
                double* L = (double*)(Params + O_Loc);
                L[0] = At.X; L[1] = At.Y; L[2] = At.Z;
            }

            *(float*)(Params + O_Vol) = 1.f;
            *(float*)(Params + O_Pitch) = 1.f;

            if (!PlayAtLocGuarded((UObject*)Controller, Fn, Params))
            {
                bBroken = true;
                LogLine("[SOUND] ClientPlaySoundAtLocation faulted -- no bot sounds from now on.");
                return;
            }

            if (++Played <= 8)
                LogLine("[SOUND] Played %s for '%s' (%.0f m away).", BotObjectName(Sound).c_str(), SafeNameOf(PS).c_str(),
                    (float)(sqrt(Dx * Dx + Dy * Dy + Dz * Dz) / 100.0));
        }
    }

    // The wall's own break sound; falls back to the generic demolish sound for its material.
    static void PlayWallBreakSound(AActor* Wall)
    {
        if (!Wall)
            return;

        UObject* Sound = ReadSoundProperty(Wall, "DeathSound");

        if (!Sound)
        {
            const std::string Text = LowerText(BotClassName((const UObject*)Wall) + " " + BotObjectName((const UObject*)Wall));
            const char* Name = "Demo_Wood_ImpactFinal_Cue";

            if (Text.find("brick") != std::string::npos || Text.find("stone") != std::string::npos ||
                Text.find("crypt") != std::string::npos || Text.find("ruins") != std::string::npos)
                Name = "Demo_Rock_ImpactFinal_Cue";
            else if (Text.find("metal") != std::string::npos || Text.find("factory") != std::string::npos)
                Name = "Demo_Metal_ImpactFinal_Cue";

            Sound = FindSoundByName(Name);
        }

        PlaySoundNearby(Sound, Wall->K2_GetActorLocation(), 9000.0);
    }

    static void PlayDoorSound(AActor* Door, bool bOpening)
    {
        PlaySoundNearby(ReadSoundProperty(Door, bOpening ? "DoorOpeningSound" : "DoorClosingSound"),
            Door->K2_GetActorLocation(), 6000.0);
    }

    // Finds the pickaxe-hits-this-material sound. If you already know the real name, put it in Override.
    // Otherwise it scans every loaded SoundCue and LOGS the candidates, so you can read the right name from the log.
    static UObject* FindImpactSound(const char* Material, const char* Override)
    {
        static std::map<std::string, UObject*> Cache;
        static std::map<std::string, int> RetryAt;

        auto Hit = Cache.find(Material);
        if (Hit != Cache.end() && Hit->second)
            return Hit->second;

        if (GStormTick < RetryAt[Material])
            return nullptr;

        UObject* Pick = (Override && Override[0]) ? FindSoundByName(Override) : nullptr;

        if (!Pick)
        {
            const std::string Mat = LowerText(Material);
            UObject* Pickaxe = nullptr;
            UObject* Demo = nullptr;
            UObject* AnyOne = nullptr;
            std::string Listed;
            int Count = 0;

            for (int i = 0; i < TUObjectArray::Num(); i++)
            {
                const UObject* Object = TUObjectArray::GetObjectByIndex(i);
                if (!Object || !Object->Class || Object->IsDefaultObject())
                    continue;

                if (LowerText(BotClassName(Object)).find("soundcue") == std::string::npos)
                    continue;

                const std::string Name = BotObjectName(Object);
                const std::string Lower = LowerText(Name);

                if (Lower.find(Mat) == std::string::npos)
                    continue;
                if (Lower.find("impact") == std::string::npos && Lower.find("hit") == std::string::npos)
                    continue;
                if (Lower.find("final") != std::string::npos || Lower.find("bullet") != std::string::npos ||
                    Lower.find("step") != std::string::npos || Lower.find("arrow") != std::string::npos)
                    continue;

                if (!Pickaxe && Lower.find("pick") != std::string::npos) Pickaxe = (UObject*)Object;
                if (!Demo && Lower.find("demo") != std::string::npos)    Demo = (UObject*)Object;
                if (!AnyOne)                                             AnyOne = (UObject*)Object;

                if (Count++ < 40)
                    Listed += Name + ", ";
            }

            Pick = Pickaxe ? Pickaxe : (Demo ? Demo : AnyOne);

            LogLine("[SOUND] Impact sounds for '%s' (%d candidate(s)): %s -> using %s", Material, Count, Listed.c_str(),
                Pick ? BotObjectName(Pick).c_str() : "NOTHING");
        }

        if (Pick)
            Cache[Material] = Pick;
        else
            RetryAt[Material] = GStormTick + 300;   // look again in 10 s (assets may not be loaded yet)

        return Pick;
    }

    // Pickaxe swing + the hit sound of the material it hit.
    static void PlayWallHitSound(AActor* Wall)
    {
        if (!Wall)
            return;

        static const char* kSwingSoundName = "PickAxe_Impact_Default_Cue";
        // Fill these in once the [SOUND] log shows the real names, e.g. "Demo_Wood_Impact_Cue". Empty = auto-scan.
        static const char* kImpactOverride[3] = { "", "", "" };   // wood, stone, metal

        const FVector At = Wall->K2_GetActorLocation();

        PlaySoundNearby(FindSoundByName(kSwingSoundName), At, 7000.0);

        const std::string Text = LowerText(BotClassName((const UObject*)Wall) + " " + BotObjectName((const UObject*)Wall));
        int Slot = 0;
        const char* Material = "Wood";

        if (Text.find("brick") != std::string::npos || Text.find("stone") != std::string::npos ||
            Text.find("crypt") != std::string::npos || Text.find("ruins") != std::string::npos)
        {
            Slot = 1;
            Material = "Rock";
        }
        else if (Text.find("metal") != std::string::npos || Text.find("factory") != std::string::npos)
        {
            Slot = 2;
            Material = "Metal";
        }

        PlaySoundNearby(FindImpactSound(Material, kImpactOverride[Slot]), At, 7000.0);
    }

    static bool CallFnGuarded(UObject* Object, UFunction* Fn, uint8* Params)
    {
        __try
        {
            Object->ProcessEvent(Fn, Params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // 1 = the building's own OnDamageServer, 2 = SetHealth + damage effects, 3 = ApplyDamage. Remembers what worked.
    static bool BotDamageBuilding(AActor* Target, float Damage, AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        static int Working = 0;
        static int Lines = 0;

        if (!Target)
            return false;

        const float Before = BuildingHealth(Target);

        for (int Method = (Working ? Working : 1); Method <= 3; Method++)
        {
            alignas(16) uint8 Params[0x300];
            memset(Params, 0, sizeof(Params));

            if (Method == 1)
            {
                auto Fn = ((UObject*)Target)->GetFunction("OnDamageServer");
                if (!Fn)
                    continue;

                auto P = Fn->GetParamsNamed();
                if (P.Size == 0 || P.Size > 0x300)
                    continue;

                std::string Names;
                for (auto& E : P.NameOffsetMap)
                {
                    Names += std::string(E.Name.c_str()) + "(" + std::to_string(E.ElementSize) + "), ";

                    if (E.Name == "Damage" && E.ElementSize == 4)
                        *(float*)(Params + E.Offset) = Damage;
                    else if ((E.Name == "InstigatedBy" || E.Name == "EventInstigator") && E.ElementSize == 8)
                        *(void**)(Params + E.Offset) = (void*)Controller;
                    else if (E.Name == "DamageCauser" && E.ElementSize == 8)
                        *(void**)(Params + E.Offset) = (void*)Pawn;
                }

                if (Lines == 0)
                    LogLine("[BREAK] OnDamageServer takes: %s", Names.c_str());

                CallFnGuarded((UObject*)Target, Fn, Params);
            }
            else if (Method == 2)
            {
                auto Fn = ((UObject*)Target)->GetFunction("SetHealth");
                if (!Fn || Before <= 0.f)
                    continue;

                auto P = Fn->GetParamsNamed();
                if (P.Size == 0 || P.Size > 0x300 || P.NameOffsetMap.empty())
                    continue;

                *(float*)(Params + P.NameOffsetMap[0].Offset) = Before - Damage;
                CallFnGuarded((UObject*)Target, Fn, Params);
                InvokeNoArgsGuarded((const UObject*)Target, "OnRep_Health");
                InvokeNoArgsGuarded((const UObject*)Target, "OnDamagePlayEffects");
            }
            else if (Method == 3)
            {
                BotDamageActor(Target, Damage, Controller, Pawn);
            }
            else
            {
                // Method 4: write the wall's Health value ourselves, then make the screens update
                const uint32 HealthOff = ((UObject*)Target)->GetOffset("Health");

                if (HealthOff == (uint32)-1 || HealthOff > 0x4000 || Before <= 0.f)
                    continue;

                *(float*)((uint8*)Target + HealthOff) = Before - Damage;

                InvokeNoArgsGuarded((const UObject*)Target, "OnRep_Health");
                InvokeNoArgsGuarded((const UObject*)Target, "OnDamagePlayEffects");
                InvokeNoArgs((const UObject*)Target, "ForceNetUpdate");
            }

            const float After = BuildingHealth(Target);

            if (Lines < 12)
            {
                Lines++;
                LogLine("[BREAK] Damage method %d on %s: health %.0f -> %.0f (%s).", Method, BotObjectName((const UObject*)Target).c_str(),
                    Before, After, After < Before - 0.1f ? "WORKED" : "no effect");
            }

            if (After < Before - 0.1f || (Before > 0.f && After <= 0.f))
            {
                Working = Method;
                return true;
            }

            if (Working)
                return false;   // the remembered method failed this time: don't walk through the others every swing
        }

        return false;
    }

    static void ShowWallCracks(AActor* Wall)
    {
        if (!Wall)
            return;

        // Tries every name a build might use. A name that does not exist is skipped silently.
        static const char* Notifies[] = {
            "OnRep_Health", "OnRep_ReplicatedHealth", "OnRep_BuildingHealth",
            "OnHealthChanged", "OnDamagePlayEffects", "UpdateDamageVisuals", "UpdateBuildingDamageVisuals"
        };

        static std::set<std::string> Ran;
        static int Lines = 0;

        for (auto Name : Notifies)
        {
            if (!((UObject*)Wall)->GetFunction(Name))
                continue;

            InvokeNoArgsGuarded((const UObject*)Wall, Name);

            if (Ran.insert(Name).second && ++Lines <= 10)
                LogLine("[CRACKS] Ran %s on %s after a swing.", Name, BotObjectName((const UObject*)Wall).c_str());
        }

        InvokeNoArgs((const UObject*)Wall, "ForceNetUpdate");
    }

    // Lists every function/property on a wall with health/damage/crack/overlay in its name.
// 6 names per line so the log line is never cut off. Runs once per wall class.
    static void LogWallHealthNames(AActor* Wall)
    {
        if (!Wall || !Wall->Class)
            return;

        static std::set<std::string> Done;
        static int Classes = 0;

        const std::string Key = BotClassName((const UObject*)Wall);

        if (!Done.insert(Key).second || ++Classes > 3)
            return;

        static const char* Keys[] = { "health", "damage", "crack", "overlay", "hitpoint" };
        std::string Line;
        int InLine = 0;

        for (const UStruct* Cls = (const UStruct*)Wall->Class; Cls; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child; Child = Child->GetNext())
            {
                auto Raw = Child->GetName().ToString();
                const std::string Name(Raw.c_str());

                if (!NameHasAny(LowerText(Name), Keys, 5))
                    continue;

                const bool bFunction = Child->Class && (Child->Class->GetCastFlags() & 0x80000);

                Line += std::string(bFunction ? "fn " : "prop ") + Name + " [" + BotObjectName((const UObject*)Cls) + "]";

                if (!bFunction)
                {
                    const uint32 Off = ((UObject*)Wall)->GetOffset(Name.c_str());

                    if (Off != (uint32)-1 && Off < 0x4000)
                    {
                        char Tmp[64];
                        snprintf(Tmp, sizeof(Tmp), " @0x%X=%.1f", Off, *(float*)((uint8*)Wall + Off));
                        Line += Tmp;
                    }
                }

                Line += " | ";

                if (++InLine >= 6)
                {
                    LogLine("[HEALTH-NAMES] %s: %s", Key.c_str(), Line.c_str());
                    Line.clear();
                    InLine = 0;
                }
            }
        }

        if (!Line.empty())
            LogLine("[HEALTH-NAMES] %s: %s", Key.c_str(), Line.c_str());
    }

    // Lowers a wall's health with the game's own ForceBuildingHealth (OnDamageServer / ApplyDamage do nothing on these walls).
    static void BotForceWallHealth(AActor* Wall, float HealthBefore, float Damage)
    {
        if (!Wall || HealthBefore <= 0.f)
            return;

        // something else already lowered it this swing: leave it
        if (BuildingHealth(Wall) < HealthBefore - 0.1f)
            return;

        static UFunction* Fn = nullptr;
        static bool bLooked = false, bBroken = false;
        static uint32 OHealth = 0, Size = 0;
        static int Lines = 0;

        if (bBroken)
            return;

        if (!bLooked)
        {
            bLooked = true;
            Fn = ((UObject*)Wall)->GetFunction("ForceBuildingHealth");

            if (Fn)
            {
                auto P = Fn->GetParamsNamed();
                Size = P.Size;

                std::string Names;
                bool bFound = false;

                for (auto& E : P.NameOffsetMap)
                {
                    Names += std::string(E.Name.c_str()) + "(" + std::to_string(E.ElementSize) + "), ";

                    if (!bFound && E.ElementSize == 4)
                    {
                        OHealth = E.Offset;
                        bFound = true;
                    }
                }

                LogLine("[BREAK] ForceBuildingHealth takes: %s", Names.c_str());

                if (!bFound || Size == 0 || Size > 0x100)
                    Fn = nullptr;
            }

            LogLine("[BREAK] The game's ForceBuildingHealth %s.", Fn ? "was found -- walls lose health through it" : "is NOT usable here");
        }

        if (!Fn)
            return;

        float NewHealth = HealthBefore - Damage;
        if (NewHealth < 1.f)
            NewHealth = 1.f;   // the last swing destroys it, so it never dies early on its own

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));
        *(float*)(Params + OHealth) = NewHealth;

        if (!CallFnGuarded((UObject*)Wall, Fn, Params))
        {
            bBroken = true;
            LogLine("[BREAK] ForceBuildingHealth faulted -- not used from now on.");
            return;
        }

        if (++Lines <= 12)
            LogLine("[BREAK] ForceBuildingHealth on %s: health %.0f -> %.0f (asked for %.0f).",
                BotObjectName((const UObject*)Wall).c_str(), HealthBefore, BuildingHealth(Wall), NewHealth);
    }

    // Plays the wall's damage effects (the cracks) with real arguments, the way the game's own damage path does.
    // Plays the wall's damage effects (the cracks) the way the game's own damage path does.
    static void BotPlayWallDamageEffects(AActor* Wall, float Damage, AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn)
    {
        if (!Wall)
            return;

        static int Lines = 0;

        // 1) OnDamagePlayEffects (takes no arguments on some builds, so size 0 is fine)
        if (auto Fn = ((UObject*)Wall)->GetFunction("OnDamagePlayEffects"))
        {
            auto P = Fn->GetParamsNamed();

            if (P.Size <= 0x300)
            {
                alignas(16) uint8 Params[0x300];
                memset(Params, 0, sizeof(Params));

                std::string Listed;

                for (auto& E : P.NameOffsetMap)
                {
                    Listed += std::string(E.Name.c_str()) + "(" + std::to_string(E.ElementSize) + "), ";

                    if ((E.Name == "Damage" || E.Name == "DamageAmount") && E.ElementSize == 4)
                        *(float*)(Params + E.Offset) = Damage;
                    else if ((E.Name == "InstigatedBy" || E.Name == "EventInstigator") && E.ElementSize == 8)
                        *(void**)(Params + E.Offset) = (void*)Controller;
                    else if (E.Name == "DamageCauser" && E.ElementSize == 8)
                        *(void**)(Params + E.Offset) = (void*)Pawn;
                }

                const bool bOk = CallFnGuarded((UObject*)Wall, Fn, Params);

                if (++Lines <= 8)
                    LogLine("[CRACKS] Called OnDamagePlayEffects on %s (%s), %u byte(s) of arguments: %s",
                        BotObjectName((const UObject*)Wall).c_str(), bOk ? "ok" : "FAULTED", (unsigned)P.Size,
                        Listed.empty() ? "none" : Listed.c_str());
            }
        }

        // 2) GameplayCue_Damage with the event "Executed" and the wall's position in the parameters
        if (auto Fn = ((UObject*)Wall)->GetFunction("GameplayCue_Damage"))
        {
            auto P = Fn->GetParamsNamed();

            if (P.Size > 0 && P.Size <= 0x300)
            {
                static int32 LocOff = -2, RawOff = -2, NormOff = -2;

                if (LocOff == -2)
                {
                    auto Struct = FindStruct("GameplayCueParameters");
                    auto ToInt = [](uint32 Off) -> int32 { return (Off == (uint32)-1 || Off > 0x300) ? -1 : (int32)Off; };

                    LocOff = Struct ? ToInt(Struct->GetOffset("Location")) : -1;
                    RawOff = Struct ? ToInt(Struct->GetOffset("RawMagnitude")) : -1;
                    NormOff = Struct ? ToInt(Struct->GetOffset("NormalizedMagnitude")) : -1;

                    LogLine("[CRACKS] GameplayCueParameters: Location at %d, RawMagnitude at %d, NormalizedMagnitude at %d.", LocOff, RawOff, NormOff);
                }

                alignas(16) uint8 Params[0x300];
                memset(Params, 0, sizeof(Params));

                for (auto& E : P.NameOffsetMap)
                {
                    if (E.Name == "EventType" && E.ElementSize == 1)
                    {
                        Params[E.Offset] = 2;   // Executed
                    }
                    else if (E.Name == "Parameters")
                    {
                        const FVector At = Wall->K2_GetActorLocation();
                        uint8* Cue = Params + E.Offset;

                        if (LocOff >= 0 && LocOff + 12 <= (int32)E.ElementSize)
                        {
                            float* L = (float*)(Cue + LocOff);
                            L[0] = (float)At.X; L[1] = (float)At.Y; L[2] = (float)At.Z;
                        }

                        if (RawOff >= 0 && RawOff + 4 <= (int32)E.ElementSize)
                            *(float*)(Cue + RawOff) = Damage;

                        if (NormOff >= 0 && NormOff + 4 <= (int32)E.ElementSize)
                            *(float*)(Cue + NormOff) = 1.f;
                    }
                }

                const bool bOk = CallFnGuarded((UObject*)Wall, Fn, Params);

                if (++Lines <= 8)
                    LogLine("[CRACKS] Called GameplayCue_Damage (Executed) on %s (%s).", BotObjectName((const UObject*)Wall).c_str(),
                        bOk ? "ok" : "FAULTED");
            }
        }

        InvokeNoArgs((const UObject*)Wall, "ForceNetUpdate");
    }

    static void TickBotBreak(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc)
    {
        RestoreDoorCollisions();
        auto It = BotBreaks.find((void*)Pawn);
        if (It == BotBreaks.end() || !It->second.Target)
            return;

        FBotBreak& B = It->second;
        AActor* Target = B.Target;

        // gone already (mined by someone, or opened and removed)
        if (!ActorIsLive((const UObject*)Target))
        {
            BotEndBreak(B, Body);
            return;
        }

        // a fight always wins: back to the gun
        auto Fight = BotCombatTarget.find(Pawn);
        if (Fight != BotCombatTarget.end() && Fight->second)
        {
            BotEndBreak(B, Body);
            return;
        }

        if (GStormTick > B.EndTick)
        {
            GIgnoredObstacles.insert(Target);
            BotEndBreak(B, Body);
            LogLine("[BREAK] '%s' gave up on %s after 30 s -- ignoring it from now on.", BotPawnName(Pawn).c_str(), BotObjectName(Target).c_str());
            return;
        }

        Body.BoxedUntil = GStormTick + 6;   // stand still while working
        BotFaceActor(Controller, Pawn, Loc, Target);

        if (GStormTick < B.NextHitTick)
            return;

        if (B.bDoor)
        {
            const int Try = ++B.Hits;

            const FVector DoorAt = Target->K2_GetActorLocation();
            double Dx = DoorAt.X - Loc.X, Dy = DoorAt.Y - Loc.Y;
            const double DLen = sqrt(Dx * Dx + Dy * Dy);

            if (DLen > 1.0)
            {
                Dx /= DLen;
                Dy /= DLen;
            }

            // already open: do NOT press E again (that closes it), just walk through
            if (Try == 1 && DoorIsOpen(Target) == 1)
            {
                GIgnoredUntil[Target] = GStormTick + 30 * 20;
                BotEndBreak(B, Body);
                return;
            }

            if (Try >= 2 && Try <= 3)
            {
                const int OpenFlag = DoorIsOpen(Target);
                bool bOpenNow;

                if (OpenFlag != -1)
                {
                    bOpenNow = (OpenFlag == 1);
                }
                else
                {
                    bool bStillDoor = false;
                    AActor* Still = BotFindObstacle(Pawn, Loc, BotHalfHeight(Pawn), Dx, Dy, &bStillDoor, 260.0);
                    bOpenNow = (Still != Target);
                }

                if (bOpenNow)
                {
                    GIgnoredUntil[Target] = GStormTick + 30 * 20;   // leave it open, let the bot walk through
                    GDoorsOpened++;

                    if (GDoorsOpened <= 15)
                        LogLine("[DOOR] '%s': %s is really open now (step %d worked) -- %d door(s) opened by bots.",
                            BotPawnName(Pawn).c_str(), BotObjectName(Target).c_str(), Try - 1, GDoorsOpened);

                    BotEndBreak(B, Body);
                    return;
                }
            }

            if (Try == 1)
            {
                // step 1: the E press, as a local player
                const bool bSent = BotInteractWith(Controller, Target);

                static int InteractLines = 0;
                if (++InteractLines <= 10)
                    LogLine("[DOOR] '%s' pressed interact on %s (%s) -- call %s. Checking in 0.7 s if it is open.",
                        BotPawnName(Pawn).c_str(), BotObjectName(Target).c_str(), BotClassName(Target).c_str(),
                        bSent ? "went through" : "NOT sent");

                B.NextHitTick = GStormTick + 20;
                return;
            }

            if (Try == 2)
            {
                // step 2: the open flag, plus the door's own interact events
                std::string How;
                BotOpenDoor(Target, &How);
                InvokeNoArgsGuarded((const UObject*)Target, "BlueprintOnInteract");
                InvokeNoArgsGuarded((const UObject*)Target, "OnRep_bDoorOpen");

                if (DoorIsOpen(Target) == 1)
                    PlayDoorSound(Target, true);

                static int Step2Lines = 0;
                if (++Step2Lines <= 10)
                    LogLine("[DOOR] '%s': interact did not open %s -- tried the open flag (%s) and its own events. Checking again.",
                        BotPawnName(Pawn).c_str(), BotObjectName(Target).c_str(), How.empty() ? "no flag found" : How.c_str());

                B.NextHitTick = GStormTick + 20;
                return;
            }

            SetActorCollision(Target, false);
            GDoorRestore[Target] = GStormTick + 30 * 8;
            GIgnoredUntil[Target] = GStormTick + 30 * 10;
            PlayDoorSound(Target, true);
            GDoorsOpened++;

            static int PassLines = 0;
            if (++PassLines <= 10)
                LogLine("[DOOR] '%s': %s (%s) would not open -- collision off for 8 s so the bot walks through. Door names: see the line below.",
                    BotPawnName(Pawn).c_str(), BotObjectName(Target).c_str(), BotClassName(Target).c_str());

            static bool bDumped = false;
            if (!bDumped)
            {
                bDumped = true;
                ListNamesContaining((UObject*)Target, "The door");
            }

            BotEndBreak(B, Body);
            return;
        }

        if (!BotHoldingMelee(Pawn))
        {
            BotTakeOutPickaxe(Controller, Pawn);
            B.NextHitTick = GStormTick + 20;
            return;
        }

        if (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon && GunIsEquipping((AActor*)Pawn->CurrentWeapon) == 1)
            WriteGunFlag((AActor*)Pawn->CurrentWeapon, GGunEquippingFlag, false);

        if (B.bSwinging)
        {
            if (GStormTick >= B.SwingEndTick)
            {
                BotTrigger(Pawn, false);
                BotTriggerUpTick[(void*)Pawn] = GStormTick;
                B.bSwinging = false;
                B.NextHitTick = GStormTick + 14;
            }
            return;
        }

        if (B.Hits == 0)
        {
            LogMiningDiscovery(Pawn, Target);
            LogWallHealthNames(Target);
        }

        BotTrigger(Pawn, true);
        B.bSwinging = true;
        B.SwingEndTick = GStormTick + 4;
        B.Hits++;
        PlayWallHitSound(Target);

        const int SwingsToBreak = 8;

        // damage is spread so the 8th swing kills it through the game's own damage path
        if (B.StartHealth < 0.f)
            B.StartHealth = BuildingHealth(Target);

        const float PerSwing = (B.StartHealth > 0.f) ? (B.StartHealth / (float)SwingsToBreak) + 1.f : 25.f;
        const float HealthBeforeSwing = BuildingHealth(Target);
        BotDamageBuilding(Target, PerSwing, Controller, Pawn);
        BotForceWallHealth(Target, HealthBeforeSwing, PerSwing);
        BotPlayWallDamageEffects(Target, PerSwing, Controller, Pawn);

        static int SwingLines = 0;
        if (++SwingLines <= 25)
            LogLine("[BREAK] '%s' swing %d of %d at %s (%.0f damage, its health now %.0f of %.0f).", BotPawnName(Pawn).c_str(), B.Hits,
                SwingsToBreak, BotObjectName(Target).c_str(), PerSwing, BuildingHealth(Target), B.StartHealth);

        if (B.Hits >= SwingsToBreak)
        {
            PlayWallBreakSound(Target);   // read DeathSound BEFORE the wall is destroyed
            const float Left = BuildingHealth(Target);

            if (ActorIsLive((const UObject*)Target) && !(Left >= 0.f && Left <= 0.5f))
            {
                SetActorCollision(Target, false);
                Target->K2_DestroyActor();
            }


            GWallsMined++;
            LogLine("[BREAK] %s broke after %d swings (health left: %.0f).", BotObjectName(Target).c_str(), B.Hits, Left);

            BotEndBreak(B, Body);
            return;
        }
    }

    // ---- doors: found by searching the level, not by a line trace ----
    static std::vector<AActor*> GDoorCache;
    static float GDoorCacheAt = -1000.f;

    static const std::vector<AActor*>& GetDoors(UWorld* World)
    {
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        if (World && (Now - GDoorCacheAt) > 15.f)
        {
            GDoorCacheAt = Now;
            GDoorCache.clear();

            static auto DoorClass = FindClass("BuildingDoor");
            static auto BaseClass = DoorClass ? DoorClass : FindClass("BuildingSMActor");

            std::map<std::string, int> Candidates;

            if (BaseClass)
            {
                TArray<AActor*> All;
                UGameplayStatics::GetAllActorsOfClass(World, BaseClass, &All);

                for (int i = 0; i < All.Num(); i++)
                {
                    if (!All[i])
                        continue;

                    if (BotLooksLikeDoor(All[i]))
                        GDoorCache.push_back(All[i]);

                    const std::string ClassName = BotClassName((const UObject*)All[i]);

                    if (LowerText(ClassName).find("door") != std::string::npos)
                        Candidates[ClassName]++;
                }
            }

            static int Reports = 0;
            if (++Reports <= 3)
            {
                std::string Listed;
                int Shown = 0;

                for (auto& C : Candidates)
                    if (Shown++ < 30)
                        Listed += C.first + " x" + std::to_string(C.second) + ", ";

                LogLine("[DOOR] Found %d real door(s) in the level (searched %s). Classes with 'door' in the name: %s",
                    (int)GDoorCache.size(), DoorClass ? "BuildingDoor" : (BaseClass ? "BuildingSMActor" : "nothing"),
                    Listed.empty() ? "NONE" : Listed.c_str());

                if (!GDoorCache.empty())
                    ListNamesContaining((UObject*)GDoorCache[0], "First door found");
            }
        }

        return GDoorCache;
    }

    // A bot walking somewhere checks for a closed door within 3.5 m in front of it and opens it BEFORE bumping into it.
    static bool BotOpenDoorsAhead(UWorld* World, AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn,
        FBotBody& Body, const FVector& Loc)
    {
        if (!Body.bHasGoal || BotBreakActive(Pawn))
            return false;

        static std::map<void*, int> NextLook;
        int& Next = NextLook[(void*)Pawn];

        if (GStormTick < Next)
            return false;

        Next = GStormTick + 8;

        auto Fight = BotCombatTarget.find(Pawn);
        if (Fight != BotCombatTarget.end() && Fight->second)
            return false;   // a fight always wins

        double Gx = Body.Goal.X - Loc.X;
        double Gy = Body.Goal.Y - Loc.Y;
        const double GLen = sqrt(Gx * Gx + Gy * Gy);

        if (GLen < 1.0)
            return false;

        Gx /= GLen;
        Gy /= GLen;

        AActor* Best = nullptr;
        double BestD2 = 350.0 * 350.0;

        for (AActor* Door : GetDoors(World))
        {
            if (!Door || !ActorIsLive((const UObject*)Door) || ObstacleIgnored(Door) ||
                GIgnoredObstacles.find(Door) != GIgnoredObstacles.end())
                continue;

            if (DoorIsOpen(Door) == 1)
                continue;   // already open: pressing E would CLOSE it

            const FVector At = Door->K2_GetActorLocation();
            const double Dx = At.X - Loc.X;
            const double Dy = At.Y - Loc.Y;
            const double D2 = Dx * Dx + Dy * Dy;

            if (D2 >= BestD2 || fabs(At.Z - Loc.Z) > 250.0)
                continue;

            const double D = sqrt(D2);

            if (D > 1.0 && (Dx / D * Gx + Dy / D * Gy) < 0.3)
                continue;   // not in front of the bot

            Best = Door;
            BestD2 = D2;
        }

        return Best && BotBeginBreak(Controller, Pawn, Body, Loc, Best, true);
    }

    // The bot is stuck (or boxed in). Each time it happens in a row, try something stronger.
    static void BotGotStuck(AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc, double Half, const char* Why)
    {
        Body.StuckLevel++;
        GStuckEvents++;

        double GX = Body.Goal.X - Loc.X;
        double GY = Body.Goal.Y - Loc.Y;
        double GL = sqrt(GX * GX + GY * GY);

        if (GL < 1.0)
        {
            GX = 1.0;
            GY = 0.0;
            GL = 1.0;
        }

        GX /= GL;
        GY /= GL;

        if (!BotBreakActive(Pawn))
        {
            auto BreakController = (AFortPlayerControllerAthena*)Pawn->Controller;

            if (BreakController && BotStartBreak(BreakController, Pawn, Body, Loc, GX, GY))
            {
                Body.StuckLevel = 0;
                Body.StuckTick = GStormTick;
                Body.StuckPos = FVector(Loc.X, Loc.Y, Loc.Z);
                return;
            }
        }

        const int Level = Body.StuckLevel;
        const char* Did = "";
        double OutX = 0.0, OutY = 0.0, Off = 0.0;

        if (Level == 1)
        {
            if (BotLedgeAhead(Pawn, Loc, GX, GY))
            {
                Did = "did not jump (a ledge is ahead)";
            }
            else
            {
                FVector Hopup{};
                Hopup.X = GX * 120.0;
                Hopup.Y = GY * 120.0;
                Hopup.Z = 520.0;
                Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);
                Did = "jumped";
            }
        }
        else if (Level == 2 || Level == 3)
        {
            const double Side = (rand() % 2) ? 1.0 : -1.0;

            if (BotPickSafeDirection(Pawn, Loc, Half, -GY * Side, GX * Side, 1, OutX, OutY, Off))
            {
                Body.OverrideX = OutX;
                Body.OverrideY = OutY;
                Body.OverrideUntil = GStormTick + 36;
                Did = "stepped sideways";
            }
            else
            {
                Did = "no safe sidestep";
            }
        }
        else if (Level == 4)
        {
            if (BotPickSafeDirection(Pawn, Loc, Half, -GX, -GY, 1, OutX, OutY, Off))
            {
                Body.OverrideX = OutX;
                Body.OverrideY = OutY;
                Body.OverrideUntil = GStormTick + 45;
                Did = "backed off";
            }
            else
            {
                Did = "no safe way back";
            }

            Body.bHasGoal = false;   // pick a new destination afterwards
        }
        else if (Level == 5 && Body.bHaveGoodPos)
        {
            FVector To(Body.LastGoodPos.X, Body.LastGoodPos.Y, Body.LastGoodPos.Z + 30.0);
            Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);
            Body.bHasGoal = false;
            Did = "put back where it last walked freely";
        }
        else
        {
            double Top = 0.0;

            if (BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z + 5000.0, Loc.Z - 200.0, Top))
            {
                FVector To(Loc.X, Loc.Y, Top + Half + 10.0);
                Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);
                Did = "lifted out onto the highest surface above it";
            }
            else
            {
                Did = "nothing worked";
            }

            Body.bHasGoal = false;
            Body.StuckLevel = 0;
        }

        Body.StuckTick = GStormTick;
        Body.StuckPos = FVector(Loc.X, Loc.Y, Loc.Z);
        Body.bSteerValid = false;
        Body.SteerCheckTick = -1000;

        if (GStuckEvents <= 20)
            LogLine("[PATH] '%s' is stuck (%s, level %d): %s.", BotPawnName(Pawn).c_str(), Why, Level, Did);
    }

    // Once a second while a bot is trying to walk: did it actually get anywhere?
    static void BotStuckCheck(AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc, uint8 Mode)
    {
        if (BotBreakActive(Pawn))
        {
            Body.StuckTick = GStormTick;
            Body.StuckPos = FVector(Loc.X, Loc.Y, Loc.Z);
            Body.ProgressTick = GStormTick;
            return;
        }
        if (!Body.bHasGoal || Mode != 1)
        {
            Body.StuckTick = -1;
            Body.ProgressTick = -1;
            return;
        }

        // ---- 1) is the bot actually getting closer to where it wants to go? ----
        {
            const double Gx = Body.Goal.X - Loc.X;
            const double Gy = Body.Goal.Y - Loc.Y;
            const double GoalDist = sqrt(Gx * Gx + Gy * Gy);

            const bool bNewGoal = Body.ProgressTick < 0 ||
                fabs(Body.Goal.X - Body.ProgressGoalX) + fabs(Body.Goal.Y - Body.ProgressGoalY) > 300.0;

            if (bNewGoal)
            {
                Body.ProgressTick = GStormTick;
                Body.BestGoalDist = GoalDist;
                Body.ProgressGoalX = Body.Goal.X;
                Body.ProgressGoalY = Body.Goal.Y;
                Body.DetourCount = 0;
            }
            else if (GStormTick < Body.OverrideUntil)
            {
                // a detour is running: pause the watchdog and start counting from here once it ends
                Body.ProgressTick = GStormTick;
                Body.BestGoalDist = GoalDist;
            }
            else if (GoalDist < Body.BestGoalDist - 250.0)
            {
                Body.BestGoalDist = GoalDist;
                Body.ProgressTick = GStormTick;
                Body.DetourCount = 0;
            }
            else if ((GStormTick - Body.ProgressTick) > 120)
            {
                // 4 seconds without getting 2.5 m closer
                Body.ProgressTick = GStormTick;
                Body.BestGoalDist = GoalDist;
                Body.DetourCount++;
                if (Body.DetourCount >= 2 && !BotBreakActive(Pawn))
                {
                    auto BreakController = (AFortPlayerControllerAthena*)Pawn->Controller;
                    const double L = GoalDist > 1.0 ? GoalDist : 1.0;

                    if (BreakController && BotStartBreak(BreakController, Pawn, Body, Loc, Gx / L, Gy / L))
                    {
                        Body.DetourCount = 0;
                        return;
                    }
                }

                static int GaveUpLines = 0;

                if (Body.DetourCount > 3)
                {
                    // this goal can't be reached from here: drop it, the AI picks a new one
                    Body.bHasGoal = false;
                    Body.DetourCount = 0;
                    Body.ProgressTick = -1;
                    Body.bSteerValid = false;
                    Body.SteerCheckTick = -1000;

                    if (++GaveUpLines <= 20)
                        LogLine("[PATH] '%s' could not get any closer to its goal (%.0f m away) after 3 detours -- dropping it for a new one.",
                            BotPawnName(Pawn).c_str(), (float)(GoalDist / 100.0));

                    return;
                }

                if (Body.DetourCount == 1)
                    Body.DetourSide = (rand() % 2) ? 1 : -1;

                const double Len = GoalDist > 1.0 ? GoalDist : 1.0;
                const double PerpX = (-Gy / Len) * (double)Body.DetourSide;
                const double PerpY = (Gx / Len) * (double)Body.DetourSide;

                double OutX = 0.0, OutY = 0.0, Off = 0.0;

                if (BotPickSafeDirection(Pawn, Loc, BotHalfHeight(Pawn), PerpX, PerpY, Body.DetourSide, OutX, OutY, Off))
                {
                    Body.OverrideX = OutX;
                    Body.OverrideY = OutY;
                    Body.OverrideUntil = GStormTick + 60 * Body.DetourCount;   // 2 s, then 4 s, then 6 s sideways
                    Body.bSteerValid = false;
                    Body.SteerCheckTick = -1000;
                }

                if (++GaveUpLines <= 20)
                    LogLine("[PATH] '%s' is not getting closer to its goal (%.0f m away) -- detour %d, walking sideways for %d s.",
                        BotPawnName(Pawn).c_str(), (float)(GoalDist / 100.0), Body.DetourCount, 2 * Body.DetourCount);

                return;
            }
        }

        // ---- 2) is it moving at all? (the original check) ----
        if (Body.StuckTick < 0)
        {
            Body.StuckTick = GStormTick;
            Body.StuckPos = FVector(Loc.X, Loc.Y, Loc.Z);
            return;
        }

        if ((GStormTick - Body.StuckTick) < 30)
            return;

        const double Mx = Loc.X - Body.StuckPos.X;
        const double My = Loc.Y - Body.StuckPos.Y;
        const double Moved = sqrt(Mx * Mx + My * My);

        if (Moved >= 120.0)
        {
            Body.StuckLevel = 0;
            Body.LastGoodPos = FVector(Loc.X, Loc.Y, Loc.Z);
            Body.bHaveGoodPos = true;
            Body.StuckTick = GStormTick;
            Body.StuckPos = FVector(Loc.X, Loc.Y, Loc.Z);
            return;
        }

        BotGotStuck(Pawn, Body, Loc, BotHalfHeight(Pawn), "barely moved in a second");
    }

    static bool BotGaitWantsRun(AFortPlayerPawnAthena* Pawn);
    static uint8 GSprintFlagBit = 0x10;

    static void BodyRealStep(AFortPlayerControllerAthena* Controller, AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc, float Dt)
    {
        if (Controller->AcknowledgedPawn != (AActor*)Pawn)
            Controller->AcknowledgedPawn = (AActor*)Pawn;

        uint8 Mode = 0;
        if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasMovementMode())
            Mode = Pawn->CharacterMovement->MovementMode;

        double AX = 0.0, AY = 0.0;

        if (Body.bHasGoal)
        {
            const double DX = Body.Goal.X - Loc.X;
            const double DY = Body.Goal.Y - Loc.Y;
            const double Dist = sqrt(DX * DX + DY * DY);

            if (Dist <= (double)Body.AcceptRadius)
            {
                Body.bHasGoal = false;
                Body.bSteerValid = false;
                Body.SteerCheckTick = -1000;
            }
            else
            {
                double DirX = DX / Dist;
                double DirY = DY / Dist;
                bool bStand = false;

                if (Mode == 1 && !Body.bDirectMove)   // steering only matters on the ground
                {
                    const double Half = BotHalfHeight(Pawn);

                    if (GStormTick < Body.BoxedUntil)
                    {
                        bStand = true;   // boxed in a moment ago: wait for the recovery to do its thing
                    }
                    else if (GStormTick < Body.OverrideUntil)
                    {
                        if ((GStormTick - Body.SteerCheckTick) >= 6)
                        {
                            Body.SteerCheckTick = GStormTick + (int)(BotPersonalSeed((void*)Pawn) % 4);

                            double OutX = 0.0, OutY = 0.0, Offset = 0.0;

                            if (BotPickSafeDirection(Pawn, Loc, Half, Body.OverrideX, Body.OverrideY, Body.SteerSide, OutX, OutY, Offset))
                            {
                                Body.OverrideX = OutX;
                                Body.OverrideY = OutY;
                            }
                            else
                            {
                                Body.OverrideUntil = -1;   // nothing safe this way: end the detour
                            }
                        }

                        if (GStormTick < Body.OverrideUntil)
                        {
                            DirX = Body.OverrideX;
                            DirY = Body.OverrideY;
                        }
                    }
                    else
                    {
                        if (fabs(Body.Goal.X - Body.SteerGoalX) + fabs(Body.Goal.Y - Body.SteerGoalY) > 300.0)
                            Body.SteerCheckTick = -1000;   // new destination: look again right away

                        if ((GStormTick - Body.SteerCheckTick) >= 6)
                        {
                            Body.SteerCheckTick = GStormTick + (int)(BotPersonalSeed((void*)Pawn) % 4);
                            Body.SteerGoalX = Body.Goal.X;
                            Body.SteerGoalY = Body.Goal.Y;

                            double OutX = 0.0, OutY = 0.0, Offset = 0.0;

                            if (BotPickSafeDirection(Pawn, Loc, Half, DirX, DirY, Body.SteerSide, OutX, OutY, Offset))
                            {
                                Body.bSteerValid = true;
                                Body.SteerX = OutX;
                                Body.SteerY = OutY;

                                if (Offset != 0.0)
                                {
                                    Body.SteerSide = (Offset > 0.0) ? 1 : -1;
                                    GPathDetours++;

                                    // stick with this detour for ~1 s before testing the straight line again
                                    Body.SteerCheckTick = GStormTick + 24;
                                }
                            }
                            else
                            {
                                // every direction is a wall or a drop
                                Body.bSteerValid = false;
                                Body.BoxedUntil = GStormTick + 20;
                                Body.SteerCheckTick = GStormTick + 20;
                                GPathBoxedIn++;

                                if (Body.StuckLevel < 1)
                                    Body.StuckLevel = 1;   // skip the jump, it could carry it off an edge

                                BotGotStuck(Pawn, Body, Loc, Half, "boxed in");
                                bStand = true;
                            }
                        }

                        if (!bStand && Body.bSteerValid)
                        {
                            DirX = Body.SteerX;
                            DirY = Body.SteerY;
                        }
                    }
                }

                if (!bStand && Body.bHasGoal)
                {
                    const double Accel = (double)BotMaxAcceleration(Pawn);
                    AX = DirX * Accel;
                    AY = DirY * Accel;
                }
            }
        }

        // Last line of defence: whatever the steering decided, never run off a ledge.
        if (Mode == 1 && (AX != 0.0 || AY != 0.0) && !Body.bDirectMove)
        {
            if ((GStormTick - Body.EdgeCheckTick) >= 3)
            {
                Body.EdgeCheckTick = GStormTick;
                Body.bEdgeAhead = BotLedgeAhead(Pawn, Loc, AX, AY);
            }

            if (Body.bEdgeAhead)
            {
                AX = 0.0;
                AY = 0.0;

                // make the steering look again right away, and cancel a detour that points at the edge
                Body.bSteerValid = false;
                Body.SteerCheckTick = -1000;
                Body.OverrideUntil = -1;

                if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasVelocity())
                {
                    FVector V = Pawn->CharacterMovement->Velocity;
                    V.X = 0.0;
                    V.Y = 0.0;
                    Pawn->CharacterMovement->Velocity = V;
                }

                if (++GLedgeStops <= 10)
                    LogLine("[LEDGE] '%s' stopped at an edge and re-planned (%d stop(s) so far).", BotPawnName(Pawn).c_str(), GLedgeStops);
            }
        }

        if (!Body.bDirectMove)
            BotStuckCheck(Pawn, Body, Loc, Mode);

        const FRotator Aim = Controller->GetControlRotation();
        const uint32 View = ((uint32)CompressAxis(Aim.Yaw) << 16) | (uint32)CompressAxis(Aim.Pitch);

        Body.Clock += (Dt > 0.125f) ? 0.125f : Dt;

        // Speed governor: on the ground, push the bot's real speed towards MaxWalkSpeed (run or walk).
        if (Mode == 1 && (AX != 0.0 || AY != 0.0) && Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
        {
            auto GovMove = Pawn->CharacterMovement;
            const double AccLen = sqrt(AX * AX + AY * AY);

            if (GovMove->HasVelocity() && GovMove->HasMaxWalkSpeed() && AccLen > 1.0)
            {
                const double Target = (double)GovMove->MaxWalkSpeed;
                FVector V = GovMove->Velocity;
                const double Flat = sqrt(V.X * V.X + V.Y * V.Y);

                const double MaxChange = 2500.0 * (double)Dt;
                double Change = Target - Flat;
                if (Change > MaxChange)  Change = MaxChange;
                if (Change < -MaxChange) Change = -MaxChange;

                const double NewSpeed = Flat + Change;
                V.X = AX / AccLen * NewSpeed;
                V.Y = AY / AccLen * NewSpeed;
                GovMove->Velocity = V;

                static int GaitCalls = 0, GaitLines = 0;
                if ((++GaitCalls % 600) == 0 && GaitLines < 12)
                {
                    GaitLines++;
                    LogLine("[GAIT] A bot's target speed is %.0f, it is moving at %.0f.", (float)Target, (float)Flat);
                }
            }
        }

        alignas(16) uint8 Params[0x200];
        memset(Params, 0, sizeof(Params));

        *(float*)(Params + GSM_TimeStamp) = Body.Clock;

        float* Accel = (float*)(Params + GSM_InAccel);
        Accel[0] = (float)AX;
        Accel[1] = (float)AY;
        Accel[2] = 0.f;

        float* ClientLoc = (float*)(Params + GSM_ClientLoc);
        ClientLoc[0] = 1.f;
        ClientLoc[1] = 2.f;
        ClientLoc[2] = 3.f;

        Params[GSM_Flags] = 0;
        Params[GSM_Roll] = 0;
        *(uint32*)(Params + GSM_View) = View;
        Params[GSM_Mode] = Mode;

        if (!SendMoveGuarded(Pawn, Params))
        {
            GBodyVerdict = 2;
            LogLine("[BODY] Sending a bot's move faulted -- every bot is moved by script from now on.");
            return;
        }

        GBodyMovesSent++;
    }

    static void BodyScriptedStep(AFortPlayerPawnAthena* Pawn, FBotBody& Body, const FVector& Loc, float Dt, bool bAirborne)
    {
        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? Pawn->CharacterMovement : nullptr;
        const double Half = BotHalfHeight(Pawn);

        auto SetVelocity = [&](double VX, double VY, double VZ)
            {
                if (Move && Move->HasVelocity())
                {
                    FVector Velocity(VX, VY, VZ);
                    Move->Velocity = Velocity;
                }
            };

        bool bFalling = bAirborne;

        if (!bAirborne && !Body.bOnGround)
        {
            if (!Body.bGroundKnown || fabs(Body.GroundFor.X - Loc.X) > 300.0 || fabs(Body.GroundFor.Y - Loc.Y) > 300.0)
            {
                double Ground = 0.0;
                Body.GroundZ = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, Ground) ? Ground : 0.0;
                Body.bGroundKnown = true;
                Body.GroundFor = FVector(Loc.X, Loc.Y, Loc.Z);
            }

            if (((double)Loc.Z - Half) - Body.GroundZ > 150.0)
                bFalling = true;
            else
                Body.bOnGround = true;
        }

        if (bFalling)
        {
            Body.bOnGround = false;

            const bool bGlider = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;
            const double Down = bGlider ? 700.0 : 2400.0;

            if (!Body.bGroundKnown || fabs(Body.GroundFor.X - Loc.X) > 300.0 || fabs(Body.GroundFor.Y - Loc.Y) > 300.0)
            {
                double Ground = 0.0;
                Body.GroundZ = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, Ground) ? Ground : 0.0;
                Body.bGroundKnown = true;
                Body.GroundFor = FVector(Loc.X, Loc.Y, Loc.Z);
            }

            const double Floor = Body.GroundZ + Half + 5.0;
            double NewZ = (double)Loc.Z - Down * (double)Dt;
            bool bOnTheGround = false;

            if (NewZ <= Floor)
            {
                NewZ = Floor;
                bOnTheGround = true;
            }

            FVector To(Loc.X, Loc.Y, NewZ);
            Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);

            SetVelocity(0.0, 0.0, bOnTheGround ? 0.0 : -Down);

            if (bOnTheGround)
            {
                Body.bOnGround = true;

                if (Move && Move->HasMovementMode() && Move->MovementMode != 1)
                    Move->SetMovementMode((uint8)1, (uint8)0);
            }

            return;
        }

        if (!Body.bHasGoal)
        {
            SetVelocity(0.0, 0.0, 0.0);
            return;
        }

        const double DX = Body.Goal.X - Loc.X;
        const double DY = Body.Goal.Y - Loc.Y;
        const double Dist = sqrt(DX * DX + DY * DY);

        if (Dist <= (double)Body.AcceptRadius)
        {
            Body.bHasGoal = false;
            SetVelocity(0.0, 0.0, 0.0);
            return;
        }

        double Speed = 510.0;
        if (Move && Move->HasMaxWalkSpeed() && Move->MaxWalkSpeed > 100.f)
            Speed = (double)Move->MaxWalkSpeed;

        const double Step = (Dist < Speed * (double)Dt) ? Dist : Speed * (double)Dt;
        const double NX = Loc.X + DX / Dist * Step;
        const double NY = Loc.Y + DY / Dist * Step;

        const double Feet = (double)Loc.Z - Half;
        double Ground = 0.0;

        if (!BotGroundBelow(Pawn, NX, NY, Feet + 60.0, Feet - 3000.0, Ground) || Ground > Feet + 55.0 || Ground < Feet - 250.0)
        {
            GBodyScriptedBlocked++;
            SetVelocity(0.0, 0.0, 0.0);
            return;
        }

        FVector To(NX, NY, Ground + Half + 2.0);
        Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);

        Body.bOnGround = true;
        Body.bGroundKnown = true;
        Body.GroundZ = Ground;
        Body.GroundFor = FVector(NX, NY, Ground + Half + 2.0);

        SetVelocity(DX / Dist * Speed, DY / Dist * Speed, 0.0);
    }

    static void DriftToLandingSpot(AFortPlayerPawnAthena* Pawn, void* ControllerKey, const FVector& Loc, float Dt, FBotBody& Body, bool bScriptedMove)
    {
        auto Spot = BotLandingSpot.find(ControllerKey);
        if (!Pawn || Spot == BotLandingSpot.end())
            return;

        // ---- tune these ----
        const double kDiveRadius = 2500.0; // closer than this to the spot (flat distance) = dive straight down
        const double kTravelPitch = -35.0;  // nose-down angle while flying towards the spot (negative = down)

        const double Dx = Spot->second.X - Loc.X;
        const double Dy = Spot->second.Y - Loc.Y;
        const double Dist = sqrt(Dx * Dx + Dy * Dy);

        const bool bGlider = Pawn->HasbIsParachuteOpen() && Pawn->bIsParachuteOpen;

        double Yaw = Pawn->K2_GetActorRotation().Yaw;
        if (Dist > 1.0)
            Yaw = atan2(Dy, Dx) * (180.0 / 3.14159265358979323846);

        // Pitch: under the glider look roughly forward, in freefall fly towards the spot,
        // and once above the spot go fully head-down (the fast dive).
        double Pitch;
        if (bGlider)
            Pitch = -10.0;
        else if (Dist < kDiveRadius)
            Pitch = -89.0;
        else
            Pitch = kTravelPitch;

        // "Hold W" towards the landing spot (only while there is still distance to cover).
        if (Dist >= 400.0)
        {
            Body.bHasGoal = true;
            Body.Goal = Spot->second;
            Body.AcceptRadius = 300.f;
        }
        else
        {
            Body.bHasGoal = false;
        }

        // Body faces the spot (yaw only)...
        FRotator Face{};
        Face.Pitch = 0.0;
        Face.Yaw = Yaw;
        Face.Roll = 0.0;
        Pawn->K2_SetActorRotation(Face, false);

        // ...the CONTROL rotation carries the pitch. That is what the skydive animation and
        // the game's dive speed read (and it is what the other players see).
        if (Pawn->Controller)
        {
            FRotator Look{};
            Look.Pitch = Pitch;
            Look.Yaw = Yaw;
            Look.Roll = 0.0;
            ((::AAIController*)Pawn->Controller)->SetControlRotation(Look);
        }

        // Only used if the game's own movement is not moving the bot (scripted mode).
        if (bScriptedMove && Dist >= 400.0)
        {
            double Step = 1200.0 * (double)Dt;
            if (Step > Dist)
                Step = Dist;

            FVector To(Loc.X + Dx / Dist * Step, Loc.Y + Dy / Dist * Step, Loc.Z);
            Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);
        }
    }

    static void MakeBotSprint(AFortPlayerPawnAthena* Pawn, bool bSprint, int TickNow);
    static bool BotGaitWantsRun(AFortPlayerPawnAthena* Pawn);

    static void PushSkydiveSpeed(AFortPlayerPawnAthena* Pawn, void* ControllerKey, const FVector& Loc, FBotBody& Body, float Dt)

    {
        // ---- tune these ----
        const double kDiveRadius = 2500.0;  // closer than this to the spot (flat distance): straight down
        const double kTravelAngle = 35.0;    // degrees below horizontal while flying to the spot
        const double kLetGoHeight = 3000.0;  // below this height above ground the game takes over (glider)

        if (!Pawn || !Pawn->HasbIsParachuteOpen() || Pawn->bIsParachuteOpen)
            return;   // glider open: leave it alone

        auto SpotIt = BotLandingSpot.find(ControllerKey);
        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? Pawn->CharacterMovement : nullptr;
        if (SpotIt == BotLandingSpot.end() || !Move || !Move->HasVelocity())
            return;

        // The game's own head-down speed (read once).
        static double DiveSpeed = -1.0;
        if (DiveSpeed < 0.0)
        {
            const float Read = ReadAirTerminalVelocity((UObject*)Move, "SkydivingControlParamsActive");
            DiveSpeed = (Read >= 1000.f && Read <= 20000.f) ? (double)Read : 4500.0;
            LogLine("[DIVE] Bots dive at %.0f units/s (%s).", DiveSpeed, (Read >= 1000.f && Read <= 20000.f) ? "the game's own dive speed" : "fallback, the game's could not be read");
        }

        // Ground height below the bot (cached, refreshed when it has moved a long way).
        if (!Body.bGroundKnown || fabs(Body.GroundFor.X - Loc.X) > 1500.0 || fabs(Body.GroundFor.Y - Loc.Y) > 1500.0)
        {
            double G = 0.0;
            Body.GroundZ = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, G) ? G : 0.0;
            Body.bGroundKnown = true;
            Body.GroundFor = FVector(Loc.X, Loc.Y, Loc.Z);
        }

        if ((double)Loc.Z - Body.GroundZ < kLetGoHeight)
            return;

        const double Dx = SpotIt->second.X - Loc.X;
        const double Dy = SpotIt->second.Y - Loc.Y;
        const double Dist = sqrt(Dx * Dx + Dy * Dy);

        const double Angle = (Dist < kDiveRadius ? 89.0 : kTravelAngle) * 3.14159265358979323846 / 180.0;
        double Horizontal = DiveSpeed * cos(Angle);
        const double Down = DiveSpeed * sin(Angle);

        if (Dt > 0.001f && Horizontal > Dist / (double)Dt)
            Horizontal = Dist / (double)Dt;   // don't overshoot the spot

        FVector V(0.0, 0.0, -Down);
        if (Dist > 1.0)
        {
            V.X = Dx / Dist * Horizontal;
            V.Y = Dy / Dist * Horizontal;
        }

        Move->Velocity = V;
    }

    static void TickBotBodies(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (!bBusDeparted || !World || !GameState || !GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        float Dt = (GLastBodyTick < 0.f) ? 0.033f : (Now - GLastBodyTick);
        GLastBodyTick = Now;

        if (!(Dt > 0.001f))
            Dt = 0.001f;
        if (Dt > 0.25f)
            Dt = 0.25f;

        int OnReal = 0;
        int OnScript = 0;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            if (EliminatedBots.find((void*)Controller) != EliminatedBots.end())
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f)
                continue;

            if (MovedToIsland.find((void*)Pawn) == MovedToIsland.end())
                continue;

            auto& Body = BotBodies[(void*)Pawn];

            if (Body.Owner != (void*)Controller)
            {
                if (Body.Owner)
                    Body = FBotBody{};

                Body.Owner = (void*)Controller;
            }

            const FVector Loc = Pawn->K2_GetActorLocation();
            const bool bAirborne = GlidingBots.find((void*)Pawn) != GlidingBots.end();
            const bool bReal = GBodyVerdict != 2 && !Body.bScripted && ResolveServerMove(Pawn);
            if (bAirborne)
                DriftToLandingSpot(Pawn, (void*)Controller, Loc, Dt, Body, !bReal);

            if (!bReal)
            {
                BodyScriptedStep(Pawn, Body, Loc, Dt, bAirborne);
                OnScript++;
                continue;
            }

            if (bAirborne)
                HoldBotGlideDescent(Pawn);

            if (!bAirborne)
            {
                BotOpenDoorsAhead(World, Controller, Pawn, Body, Loc);
                TickBotBreak(Controller, Pawn, Body, Loc);
            }

            BodyRealStep(Controller, Pawn, Body, Loc, Dt);

            if (bAirborne)
                PushSkydiveSpeed(Pawn, (void*)Controller, Loc, Body, Dt);

            if (!bAirborne)
            {
                auto FireNow = BotFire.find(Pawn);
                const bool bShootingNow = FireNow != BotFire.end() && FireNow->second.bFiring;
                MakeBotSprint(Pawn, Body.bHasGoal && !bShootingNow && BotGaitWantsRun(Pawn), GStormTick);
            }


            if (GBodyVerdict == 2)
            {
                OnScript++;
                continue;
            }

            OnReal++;

            if (Body.bChecked)
                continue;

            if (Body.WatchSince < 0.f)
            {
                double Ground = 0.0;
                const bool bHigh = BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, Ground)
                    ? (((double)Loc.Z - Ground) > 1500.0)
                    : ((double)Loc.Z > 8000.0);

                if (!bHigh)
                {
                    Body.bChecked = true;
                    continue;
                }

                Body.WatchSince = Now;
                Body.WatchZ = (float)Loc.Z;
                continue;
            }

            if ((float)Loc.Z > Body.WatchZ)
                Body.WatchZ = (float)Loc.Z;

            if ((Now - Body.WatchSince) < 1.f)
                continue;

            Body.bChecked = true;

            const float Fell = Body.WatchZ - (float)Loc.Z;
            const bool bFell = Fell >= 150.f;

            static int GoodReports = 0;
            if (bFell && ++GoodReports <= 3)
                LogLine("[BODY] '%s' came down %.0f units in 1s under real movement -- the game's own movement moves it.",
                    SafeNameOf(PlayerState).c_str(), Fell);

            if (GBodyVerdict == 0)
            {
                GBodyChecks++;
                if (bFell)
                    GBodyChecksFell++;

                if (GBodyChecks >= 6)
                {
                    GBodyVerdict = (GBodyChecksFell * 2 >= GBodyChecks) ? 1 : 2;

                    if (GBodyVerdict == 1)
                        LogLine("[BODY] Real movement works for bots on this build: %d of %d bots checked came down at least 150 units in 1s. "
                            "Bots skydive, glide, land and walk on the game's own movement.", GBodyChecksFell, GBodyChecks);
                    else
                        LogLine("[BODY] Real movement does NOT move bots on this build: only %d of %d bots checked came down in 1s. "
                            "Every bot is moved by script from now on.", GBodyChecksFell, GBodyChecks);
                }
            }

            if (!bFell && GBodyVerdict != 2 && !Body.bScripted)
            {
                Body.bScripted = true;
                GBodyScriptedOnTheirOwn++;

                if (GBodyScriptedOnTheirOwn <= 5)
                    LogLine("[BODY] '%s' came down only %.0f units in 1s under real movement -- this one is moved by script.",
                        SafeNameOf(PlayerState).c_str(), Fell);
            }
        }

        static float LastReport = -1000.f;

        if ((Now - LastReport) >= 30.f && (OnReal + OnScript) > 0)
        {
            LastReport = Now;
            LogLine("[BODY] %d bot(s) on real movement (%d moves sent so far), %d moved by script (%d scripted steps refused: wall, cliff or no ground) | "
                "verdict: %s.", OnReal, GBodyMovesSent, OnScript, GBodyScriptedBlocked,
                GBodyVerdict == 1 ? "real movement works" : (GBodyVerdict == 2 ? "scripted for everyone" : "still checking"));
        }
    }

    static bool BotHoldingGun(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->HasCurrentWeapon() || !Pawn->CurrentWeapon)
            return false;

        auto Weapon = (AFortWeapon*)Pawn->CurrentWeapon;

        return Weapon->HasWeaponData() && Weapon->WeaponData &&
            BotGunKindOf((const UFortItemDefinition*)Weapon->WeaponData) != BotGun_None;
    }

    static bool BotHasAnyGun(AFortPlayerControllerAthena* Controller)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory)
            return false;

        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            if (BotGunKindOf(Entries.Get(e, FFortItemEntry::Size()).ItemDefinition) != BotGun_None)
                return true;
        }

        return false;
    }

    static std::map<void*, float> BotArmRetryAt;
    static int GBotsArmed = 0;

    // Bots hold their pickaxe until they have a gun, then take out the best gun they carry.
    static void ArmLandedBots(UWorld* World, AFortGameStateAthena* GameState)
    {
        if (!bBusDeparted || !World || !GameState || !GameState->HasPlayerArray())
            return;

        const float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || Pawn->GetHealth() <= 0.f || !BotHasLanded((void*)Pawn))
                continue;

            if (BotBreakActive(Pawn) || BotIsHealing(Pawn))
                continue;

            if (BotHoldingGun(Pawn))
            {
                auto Held = (AFortWeapon*)Pawn->CurrentWeapon;
                auto HeldDef = (const UFortItemDefinition*)Held->WeaponData;

                if (BotWeapons[(void*)PlayerState] != HeldDef)
                {
                    BotWeapons[(void*)PlayerState] = HeldDef;
                    BotWeaponClipSize[(void*)PlayerState] = BotGunMagazine(HeldDef);
                }

                // Only stay with this gun if nothing better is in the inventory.
                // Don't swap mid-fight.
                if (BotGunScore(HeldDef) >= BotBestGunScore(Controller) || BotCombatTarget.find(Pawn) != BotCombatTarget.end() && BotCombatTarget[Pawn])
                    continue;
            }

            auto& RetryAt = BotArmRetryAt[(void*)Pawn];
            if (Now < RetryAt)
                continue;
            RetryAt = Now + 1.f;

            AFortInventory* Inventory = Controller->WorldInventory;
            if (!Inventory)
            {
                auto It = BotInventories.find((void*)PlayerState);
                if (It != BotInventories.end())
                    Inventory = It->second;
            }

            if (!Inventory)
                continue;

            const UFortItemDefinition* Gun = nullptr;
            FGuid GunGuid{};
            const UFortItemDefinition* Pickaxe = nullptr;
            FGuid PickaxeGuid{};

            {
                int BestScore = -1;
                auto& Entries = Inventory->Inventory.ReplicatedEntries;

                for (int e = 0; e < Entries.Num(); e++)
                {
                    auto& Entry = Entries.Get(e, FFortItemEntry::Size());
                    const int Score = BotGunScore(Entry.ItemDefinition);

                    if (Score > BestScore)
                    {
                        BestScore = Score;
                        Gun = Entry.ItemDefinition;
                        GunGuid = Entry.ItemGuid;
                    }

                    if (!Pickaxe && Entry.ItemDefinition && ((const UObject*)Entry.ItemDefinition)->IsA<UFortWeaponMeleeItemDefinition>())
                    {
                        Pickaxe = Entry.ItemDefinition;
                        PickaxeGuid = Entry.ItemGuid;
                    }
                }
            }

            if (Gun)
            {
                auto GunDef = Gun->Cast<UFortWeaponItemDefinition>();
                if (!GunDef)
                    continue;

                Pawn->EquipWeaponDefinition(GunDef, GunGuid);

                BotWeapons[(void*)PlayerState] = Gun;
                if (BotInventories.find((void*)PlayerState) == BotInventories.end())
                    BotInventories[(void*)PlayerState] = Inventory;

                BotWeaponClipSize[(void*)PlayerState] = BotGunMagazine(Gun);

                GBotsArmed++;

                if (GBotsArmed <= 8 || (GBotsArmed % 25) == 0)
                    LogLine("[ARMS] '%s' took out its %s (%d armed so far; holding it now: %s).",
                        SafeNameOf(PlayerState).c_str(), BotObjectName(Gun).c_str(), GBotsArmed,
                        BotHoldingGun(Pawn) ? "yes" : "NO");
            }
            else if (Pickaxe && !BotIsHoldingAWeapon(Pawn))
            {
                if (auto PickaxeDef = Pickaxe->Cast<UFortWeaponItemDefinition>())
                    Pawn->EquipWeaponDefinition(PickaxeDef, PickaxeGuid);
            }
        }
    }
    static std::map<void*, int> BotSprintSetTick;
    static int GSprintSet = 0;
    static bool GSprintFaulted = false;


    // Movement style value for the OG hold-Shift run. The [SPRINT] lines in the console show what this build has.
    static const uint8 kRunStyle = 3;    // sprint (human log: style 3, IsSprinting 1, speed 550)
    static const uint8 kWalkStyle = 0;   // normal run (human log: style 0, speed 410)

    static const char* kRunFlags[] = { "bIsSprinting", "bWantsToSprint", "bIsRunning", "bWantsToRun", "bRunning" };
    static const int kRunFlagCount = (int)(sizeof(kRunFlags) / sizeof(kRunFlags[0]));

    static void WriteRunFlags(UObject* Target, signed char* Known, bool bOn)
    {
        for (int f = 0; f < kRunFlagCount; f++)
        {
            if (Known[f] == -1)
                Known[f] = (Target->GetProperty(kRunFlags[f], 0x20000) != nullptr) ? 1 : 0;

            if (Known[f] == 1)
                SetBoolByName(Target, kRunFlags[f], bOn);
        }
    }

    static void ListRunNames(UObject* Object, const char* Label)
    {
        if (!Object || !Object->Class)
            return;

        static const char* Keys[] = { "sprint", "run", "walk", "style" };
        std::string Found;
        int Count = 0;

        for (const UStruct* Cls = (const UStruct*)Object->Class; Cls && Found.size() < 700; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child && Found.size() < 700; Child = Child->GetNext())
            {
                auto RawName = Child->GetName().ToString();
                std::string Lowered;
                for (const char* p = RawName.c_str(); *p; p++)
                    Lowered += (char)tolower((unsigned char)*p);

                for (auto Key : Keys)
                {
                    if (Lowered.find(Key) != std::string::npos)
                    {
                        Found += std::string(RawName.c_str()) + ", ";
                        Count++;
                        break;
                    }
                }
            }
        }

        LogLine("[SPRINT] %s names with sprint/run/walk/style in them (%d): %s", Label, Count, Found.c_str());
    }

    static void MakeBotSprint(AFortPlayerPawnAthena* Pawn, bool bSprint, int TickNow)
    {
        if (!Pawn || GSprintFaulted)
            return;

        static signed char OnPawn[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        static signed char OnMove[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        static int32 StyleOffset = -2;
        static bool bListed = false;

        auto Move = (Pawn->HasCharacterMovement() && Pawn->CharacterMovement) ? Pawn->CharacterMovement : nullptr;

        if (!bListed)
        {
            bListed = true;
            ListRunNames((UObject*)Pawn, "The pawn's");
            if (Move)
                ListRunNames((UObject*)Move, "The movement component's");
        }

        const uint8 Style = bSprint ? kRunStyle : kWalkStyle;

        WriteRunFlags((UObject*)Pawn, OnPawn, bSprint);
        if (Move)
            WriteRunFlags((UObject*)Move, OnMove, bSprint);

        if (StyleOffset == -2)
        {
            StyleOffset = (int32)((UObject*)Pawn)->GetOffset("CurrentMovementStyle");
            LogLine("[SPRINT] Pawn MovementStyle property %s (offset %d).",
                (StyleOffset > 0 && StyleOffset < 0x10000) ? "found" : "NOT found", StyleOffset);
        }

        bool bWroteProp = false;
        if (StyleOffset > 0 && StyleOffset < 0x10000)
        {
            GetFromOffset<uint8>((UObject*)Pawn, (uint32)StyleOffset) = Style;
            InvokeNoArgsGuarded((UObject*)Pawn, "OnRep_CurrentMovementStyle");
            bWroteProp = true;
        }

        // Every 20 ticks, also call the game's own SetMovementStyle so it recalculates the speed.
        auto Last = BotSprintSetTick.find((void*)Pawn);
        if (Last != BotSprintSetTick.end() && (TickNow - Last->second) < 20)
            return;

        BotSprintSetTick[(void*)Pawn] = TickNow;

        bool bFaulted = false;
        const bool bCalled = InvokeOneByteGuarded((UObject*)Pawn, "SetMovementStyle", Style, &bFaulted);

        if (bFaulted)
        {
            GSprintFaulted = true;
            LogLine("[SPRINT] SetMovementStyle faulted -- switched off.");
            return;
        }

        GSprintSet++;

        if (GSprintSet <= 3)
        {
            LogLine("[SPRINT] Run %s: SetMovementStyle %s | style property %s, reads back %d | MaxWalkSpeed %.0f | flags on the pawn: %d of %d, on the movement: %d of %d.",
                bSprint ? "ON" : "OFF", bCalled ? "called" : "NOT on this build", bWroteProp ? "written" : "not found",
                bWroteProp ? (int)GetFromOffset<uint8>((UObject*)Pawn, (uint32)StyleOffset) : -1,
                (Move && Move->HasMaxWalkSpeed()) ? (float)Move->MaxWalkSpeed : -1.f,
                (int)(OnPawn[0] == 1) + (int)(OnPawn[1] == 1) + (int)(OnPawn[2] == 1) + (int)(OnPawn[3] == 1) + (int)(OnPawn[4] == 1), kRunFlagCount,
                (int)(OnMove[0] == 1) + (int)(OnMove[1] == 1) + (int)(OnMove[2] == 1) + (int)(OnMove[3] == 1) + (int)(OnMove[4] == 1), kRunFlagCount);
        }
    }

    // ---- gait: bots mostly run, and sometimes walk for a few seconds ----
    struct FBotGait
    {
        bool bRun = true;
        float SwitchAt = 0.f;
    };

    static std::map<void*, FBotGait> BotGaits;

    static float BotGaitSpeed(AFortPlayerPawnAthena* Pawn, bool bMustRun, float Now)
    {
        const float RunSpeed = 550.f;    // what the real player's sprint reads
        const float WalkSpeed = 410.f;   // what the real player's normal run reads

        FBotGait& Gait = BotGaits[(void*)Pawn];

        if (Now >= Gait.SwitchAt)
        {
            Gait.bRun = (rand() % 100) < 65;                       // tune: 65 = percent of the time spent running
            Gait.SwitchAt = Now + 3.f + (float)(rand() % 700) / 100.f;   // switch every 3-10 seconds
        }
        return (bMustRun || Gait.bRun) ? RunSpeed : WalkSpeed;
    }
    static bool BotGaitWantsRun(AFortPlayerPawnAthena* Pawn)
    {
        if (Pawn && Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone)
            return true;   // in the storm: never walk

        auto Combat = BotCombatTarget.find(Pawn);
        if (Combat != BotCombatTarget.end() && Combat->second)
            return true;   // fighting: always run

        auto It = BotGaits.find((void*)Pawn);
        return It == BotGaits.end() || It->second.bRun;
    }

    // A bot can never notice or hold a target further away than this (units: 8000 = 80 m).
    static constexpr double kBotSightRange = 8000.0;

    // True if this target is the player/bot that shot this bot recently.
    static bool BotWasShotBy(void* ControllerKey, AFortPlayerPawnAthena* Target)
    {
        if (!Target || !Target->Controller)
            return false;

        auto Agg = AggroedByHuman.find(ControllerKey);

        if (Agg == AggroedByHuman.end() || !Agg->second.first ||
            (GStormTick - Agg->second.second) > kGrudgeTicks)
            return false;

        return (void*)((AFortPlayerControllerAthena*)Target->Controller)->PlayerState == Agg->second.first;
    }

    struct FBotLoot
    {
        AActor* Chest = nullptr;
        bool bPickup = false;
        int GiveUpTick = 0;
        int NextSearchTick = 0;
        int ChestsOpened = 0;
        int Wanted = 0;
        int BlockedTicks = 0;
        int PressTick = -1;

        // new
        double BestDist = 1e18;         // closest this bot has got to its target
        int ProgressTick = 0;           // last tick it got meaningfully closer
        int SearchEndTick = -1;         // end of the "searching the chest" pause
        int NextObstacleTick = 0;       // throttle for the wall/door trace
        std::set<AActor*> Skip;         // targets THIS bot failed on
    };

    static std::map<void*, FBotLoot> BotLoot;
    static std::map<AActor*, std::pair<void*, int>> ChestClaims;
    static int GChestsOpenedByBots = 0;
    static int GChestsGivenUp = 0;
    static std::map<AActor*, int> GChestFailCount;   // how many bots failed on this target

    static void ReleaseChest(AActor* Chest, void* Owner)
    {
        auto It = ChestClaims.find(Chest);

        if (It != ChestClaims.end() && It->second.first == Owner)
            ChestClaims.erase(It);
    }

    static bool GuidsMatch(const FGuid& X, const FGuid& Y)
    {
        return X.A == Y.A && X.B == Y.B && X.C == Y.C && X.D == Y.D;
    }

    // ======================= INVENTORY SLOTS =======================
// 5 slots (the pickaxe does not count): 4 guns + ONE shared slot for bandage / medkit / shield potion.
    static constexpr int kBotMaxGuns = 4;
    static constexpr int kBotMaxConsumables = 1;
    static constexpr int kBotHealStack = 3;              // how many of it the slot carries
    static constexpr int kBotSwapCooldownTicks = 600;    // 20 s between consumable swaps

    // Kind: 1 = health item, 2 = shield item. Score ranks items of the same kind.
    struct FHealInfo
    {
        int Kind = 0;
        int Score = 0;
        float Seconds = 0.f;   // how long using it takes
        float Amount = 0.f;    // how much it gives
        float Cap = 0.f;       // it never takes health/shield above this
    };

    static FHealInfo BotHealInfo(const UFortItemDefinition* Def)
    {
        FHealInfo Info{};

        if (!Def)
            return Info;

        const UObject* Object = (const UObject*)Def;

        if (Object->IsA<UFortAmmoItemDefinition>() || Object->IsA<UFortResourceItemDefinition>())
            return Info;

        const std::string Name = LowerObjectName(Object);
        auto Has = [&](const char* Part) { return Name.find(Part) != std::string::npos; };

        if (Name.rfind("wid_", 0) == 0 || Has("trap") || Has("grenade"))
            return Info;

        if (Has("medkit"))
            Info = { 1, 5, 10.f, 100.f, 100.f };
        else if (Has("bandage"))
            Info = { 1, 1, 4.f, 15.f, 75.f };
        else if (Has("shieldsmall") || Has("smallshield") || Has("minishield") || Has("shields_small"))
            Info = { 2, 2, 2.f, 25.f, 50.f };
        else if (Has("purplestuff") || Has("shield"))
            Info = { 2, 3, 5.f, 50.f, 100.f };

        return Info;
    }

    static int BotHealScore(const UFortItemDefinition* Def)
    {
        return BotHealInfo(Def).Score;
    }

    // -1 = not wanted, 0 = gun, 1 = any consumable (bandage, medkit, shield)
    static int BotItemGroup(const UFortItemDefinition* Def)
    {
        if (BotGunKindOf(Def) != BotGun_None)
            return 0;

        return BotHealInfo(Def).Kind > 0 ? 1 : -1;
    }

    // How many points this consumable would restore RIGHT NOW (health counts 1.5x). Useless right now = 1, so an
    // empty slot still takes it but anything useful beats it.
    static int BotConsumableValue(AFortPlayerPawnAthena* Pawn, const UFortItemDefinition* Def)
    {
        const FHealInfo Info = BotHealInfo(Def);

        if (Info.Kind == 0)
            return 0;

        if (!Pawn)
            return 1 + (int)Info.Amount;

        const float Have = (Info.Kind == 1) ? Pawn->GetHealth() : Pawn->GetShield();
        float Room = Info.Cap - Have;
        if (Room < 0.f)
            Room = 0.f;

        float Restores = Info.Amount < Room ? Info.Amount : Room;

        if (Info.Kind == 1)
            Restores *= 1.5f;

        return 1 + (int)Restores;
    }

    static int BotGroupScore(AFortPlayerPawnAthena* Pawn, const UFortItemDefinition* Def, int Group)
    {
        return Group == 0 ? BotGunScore(Def) : BotConsumableValue(Pawn, Def);
    }

    static bool BotLootIsFull(AFortPlayerControllerAthena* Controller)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory)
            return false;

        int Count[2] = { 0, 0 };
        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            const int Group = BotItemGroup(Entries.Get(e, FFortItemEntry::Size()).ItemDefinition);

            if (Group >= 0)
                Count[Group]++;
        }

        return Count[0] >= kBotMaxGuns && Count[1] >= kBotMaxConsumables;
    }

    // Puts an item from the bot's inventory on the ground next to it (with its ammo), then takes it out of the inventory.
    static void BotDropItem(AFortPlayerPawnAthena* Pawn, AFortInventory* Inventory, const FGuid& Guid,
        const UFortItemDefinition* Def, int Count, int Ammo)
    {
        if (!Inventory || !Def)
            return;

        if (Pawn)
        {
            FVector Spot = Pawn->K2_GetActorLocation();
            const float Angle = (float)(rand() % 360) * (3.14159265f / 180.f);

            Spot.X += cosf(Angle) * (float)(80 + (rand() % 60));
            Spot.Y += sinf(Angle) * (float)(80 + (rand() % 60));
            Spot.Z += 50.0;

            AFortInventory::SpawnPickup(Spot, Def, Count > 0 ? Count : 1, Ammo,
                EFortPickupSourceTypeFlag::GetPlayer(),
                EFortPickupSpawnSource::GetPlayerElimination());
        }

        Inventory->Remove(Guid);
    }

    // Decides if the bot takes this item, and (bApply) takes it.
    // Returns "" if it does not want it. With bApply false it only answers "would you take it?".
    static std::string BotConsiderItem(AFortPlayerControllerAthena* Controller, const UFortItemDefinition* Def, int Count, int Ammo, bool bApply)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory || !Def)
            return std::string();

        const int Group = BotItemGroup(Def);
        if (Group < 0)
            return std::string();

        auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

        const int NewScore = BotGroupScore(Pawn, Def, Group);
        if (NewScore <= 0)
            return std::string();

        if (InventoryHolds(Inventory, Def))
            return std::string();

        FGuid HeldGuid{};
        bool bHaveHeld = false;

        if (Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
        {
            auto Held = (AFortWeapon*)Pawn->CurrentWeapon;

            if (Held->HasItemEntryGuid())
            {
                HeldGuid = Held->ItemEntryGuid;
                bHaveHeld = true;
            }
        }

        int Have = 0;
        int WorstScore = 100000;
        FGuid WorstGuid{};
        const UFortItemDefinition* WorstDef = nullptr;
        int WorstCount = 1, WorstAmmo = 0;
        bool bHaveWorst = false;

        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            auto& Entry = Entries.Get(e, FFortItemEntry::Size());

            if (BotItemGroup(Entry.ItemDefinition) != Group)
                continue;

            const int S = BotGroupScore(Pawn, Entry.ItemDefinition, Group);
            if (S <= 0)
                continue;

            Have++;

            if (S < WorstScore && !(bHaveHeld && GuidsMatch(Entry.ItemGuid, HeldGuid)))
            {
                WorstScore = S;
                WorstGuid = Entry.ItemGuid;
                WorstDef = Entry.ItemDefinition;
                WorstCount = Entry.HasCount() ? (int)Entry.Count : 1;
                WorstAmmo = Entry.HasLoadedAmmo() ? (int)Entry.LoadedAmmo : 0;
                bHaveWorst = true;
            }
        }

        const int Max = Group == 0 ? kBotMaxGuns : kBotMaxConsumables;
        bool bSwap = false;

        if (Have >= Max)
        {
            if (!bHaveWorst || NewScore <= WorstScore)
                return std::string();

            if (Group == 1)
            {
                // must be clearly better, not busy healing, and not swapped a moment ago
                if (NewScore < WorstScore + 6 || BotIsHealing(Pawn))
                    return std::string();

                static std::map<void*, int> LastSwap;
                auto Last = LastSwap.find((void*)Controller);

                if (Last != LastSwap.end() && (GStormTick - Last->second) < kBotSwapCooldownTicks)
                    return std::string();

                if (bApply)
                    LastSwap[(void*)Controller] = GStormTick;
            }

            bSwap = true;
        }

        if (!bApply)
            return "wanted";

        std::string What = BotObjectName((const UObject*)Def);

        if (bSwap)
        {
            const std::string Dropped = BotObjectName((const UObject*)WorstDef);

            BotDropItem(Pawn, Inventory, WorstGuid, WorstDef, WorstCount, WorstAmmo);
            What = "swapped (dropped " + Dropped + ") for " + What;
        }

        if (Group == 0)
        {
            const int Clip = Ammo > 0 ? Ammo : BotGunMagazine(Def);
            Inventory->GiveItem(Def, 1, Clip);
            What += GiveBotSpareRounds(Inventory, Def);

            GBotLootGuns[BotGunKindOf(Def)]++;
            GBotLootRarity[BotGunRarity(Def)]++;
        }
        else
        {
            int Give = Count > 0 ? Count : 1;

            if (Give > kBotHealStack)
                Give = kBotHealStack;

            const int MaxStack = Def->GetMaxStackSize();
            if (MaxStack > 0 && Give > MaxStack)
                Give = MaxStack;

            Inventory->GiveItem(Def, Give, 0);
            What += " x" + std::to_string(Give);
        }

        if (Pawn)
            BotArmRetryAt.erase((void*)Pawn);

        return What;
    }

    // ======================= HEALING =======================
    struct FBotHeal
    {
        bool bActive = false;
        const UFortItemDefinition* Def = nullptr;
        FGuid Guid{};
        FHealInfo Info{};
        std::string Name;
        int StartTick = 0;
        int EndTick = 0;
        int LastTick = 0;
        int NextCheckTick = 0;
        float HealthSeen = 0.f;

        // new
        bool bTriggerHeld = false;     // trigger pulled so the game's own consumable ability runs
        bool bAbilitySeen = false;     // the game's ability really started
        bool bSoundPlayed = false;     // fallback sound played
        int CountAtStart = 0;
        float HealthAtStart = 0.f;
        float ShieldAtStart = 0.f;
    };

    static std::map<void*, FBotHeal> BotHeals;
    static int GHealsStarted = 0;
    static int GHealsFinished = 0;
    static int GHealsInterrupted = 0;
    static int GHealsByGame = 0;
    static int GHealsByScript = 0;

    static bool BotIsHealing(AFortPlayerPawnAthena* Pawn)
    {
        auto It = BotHeals.find((void*)Pawn);
        return It != BotHeals.end() && It->second.bActive && (GStormTick - It->second.LastTick) <= 5;
    }

    static bool BotFindHeal(AFortInventory* Inventory, int Kind, bool bStrongest, const UFortItemDefinition*& OutDef, FGuid& OutGuid)
    {
        int Best = bStrongest ? -1 : 100000;
        bool bFound = false;
        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            auto& Entry = Entries.Get(e, FFortItemEntry::Size());
            const FHealInfo Info = BotHealInfo(Entry.ItemDefinition);

            if (Info.Kind != Kind)
                continue;

            if (Entry.HasCount() && Entry.Count <= 0)
                continue;

            if (bStrongest ? (Info.Score > Best) : (Info.Score < Best))
            {
                Best = Info.Score;
                OutDef = Entry.ItemDefinition;
                OutGuid = Entry.ItemGuid;
                bFound = true;
            }
        }

        return bFound;
    }

    static bool BotConsumeOne(AFortInventory* Inventory, const FGuid& Guid)
    {
        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            auto& Entry = Entries.Get(e, FFortItemEntry::Size());

            if (!GuidsMatch(Entry.ItemGuid, Guid))
                continue;

            if (Entry.HasCount() && Entry.Count > 1)
            {
                Entry.Count--;
                Inventory->Update(&Entry);
            }
            else
            {
                Inventory->Remove(Guid);
            }

            return true;
        }

        return false;
    }

    // 0 if the item is gone from the inventory
    static int BotItemCount(AFortInventory* Inventory, const FGuid& Guid)
    {
        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            auto& Entry = Entries.Get(e, FFortItemEntry::Size());

            if (GuidsMatch(Entry.ItemGuid, Guid))
                return Entry.HasCount() ? (int)Entry.Count : 1;
        }

        return 0;
    }

    // ---- drinking / bandage sounds (fallback, only played when the game's own ability did not run) ----
    // Put the real asset names here once the [HEAL-SOUND] log shows them. Empty = auto-scan.
    static const char* kHealSoundOverride[3] = { "", "", "" };   // [0] bandage, [1] medkit, [2] shield potion

    static UObject* FindHealSound(int Slot)
    {
        static UObject* Cache[3] = {};
        static int RetryAt[3] = {};

        if (Cache[Slot])
            return Cache[Slot];

        if (GStormTick < RetryAt[Slot])
            return nullptr;

        UObject* Pick = kHealSoundOverride[Slot][0] ? FindSoundByName(kHealSoundOverride[Slot]) : nullptr;

        if (!Pick)
        {
            static const char* Keys[3][2] = { { "bandage", "bandaid" }, { "medkit", "firstaid" }, { "shield", "shields" } };
            std::string Listed;
            int Count = 0;

            for (int i = 0; i < TUObjectArray::Num(); i++)
            {
                const UObject* Object = TUObjectArray::GetObjectByIndex(i);
                if (!Object || !Object->Class || Object->IsDefaultObject())
                    continue;

                if (LowerText(BotClassName(Object)).find("soundcue") == std::string::npos)
                    continue;

                const std::string Name = BotObjectName(Object);
                const std::string Lower = LowerText(Name);

                if (Lower.find(Keys[Slot][0]) == std::string::npos && Lower.find(Keys[Slot][1]) == std::string::npos)
                    continue;

                // hit / break / impact sounds are not the "using it" sound
                if (Lower.find("impact") != std::string::npos || Lower.find("break") != std::string::npos ||
                    Lower.find("hit") != std::string::npos || Lower.find("pickup") != std::string::npos)
                    continue;

                if (!Pick)
                    Pick = (UObject*)Object;

                if (Count++ < 30)
                    Listed += Name + ", ";
            }

            LogLine("[HEAL-SOUND] Sounds for slot %d (%d candidate(s)): %s -> using %s", Slot, Count, Listed.c_str(),
                Pick ? BotObjectName(Pick).c_str() : "NOTHING");
        }

        if (Pick)
            Cache[Slot] = Pick;
        else
            RetryAt[Slot] = GStormTick + 300;

        return Pick;
    }

    static void BotCancelHeal(FBotHeal& H, AFortPlayerPawnAthena* Pawn, AFortPlayerStateAthena* PlayerState, const char* Why)
    {
        if (!H.bActive)
            return;

        if (H.bTriggerHeld)
        {
            BotTrigger(Pawn, false);
            BotTriggerUpTick[(void*)Pawn] = GStormTick;
            H.bTriggerHeld = false;
        }

        H.bActive = false;
        H.NextCheckTick = GStormTick + 60;
        GHealsInterrupted++;

        BotArmRetryAt.erase((void*)Pawn);   // ArmLandedBots puts the gun back in its hands

        if (GHealsInterrupted <= 10)
            LogLine("[HEAL] '%s' stopped using %s (%s) -- %d interrupted so far.", SafeNameOf(PlayerState).c_str(), H.Name.c_str(), Why,
                GHealsInterrupted);
    }

    // Returns true while the bot is busy healing (the caller then skips looting/wandering).
    static bool TickBotHealing(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
    {
        if (!Controller || !PlayerState || !Pawn)
            return false;

        auto Inventory = Controller->WorldInventory;
        if (!Inventory)
            return false;

        FBotHeal& H = BotHeals[(void*)Pawn];

        const float Health = Pawn->GetHealth();
        const float Shield = Pawn->GetShield();

        auto Hit = BotLastDamager.find((void*)Controller);
        const int SinceHit = (Hit != BotLastDamager.end()) ? (GStormTick - Hit->second.second) : 100000;

        // ---- one already running ----
        if (H.bActive)
        {
            if (GStormTick - H.LastTick > 5)
                BotCancelHeal(H, Pawn, PlayerState, "it had to deal with a fight");
            else if (Health < H.HealthSeen - 0.5f || SinceHit < 15)
                BotCancelHeal(H, Pawn, PlayerState, "it got hurt");
        }

        if (H.bActive)
        {
            H.LastTick = GStormTick;
            H.HealthSeen = Health;

            auto Body = BotBodies.find((void*)Pawn);
            if (Body != BotBodies.end())
                Body->second.bHasGoal = false;   // stand still while using it

            AActor* Held = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;

            // ~0.5 s after taking it out: pull the trigger so the game's OWN consumable ability runs
            // (the real use animation, sound and effect, visible to everyone)
            if (!H.bTriggerHeld && GStormTick >= H.StartTick + 15)
            {
                if (Held && GunIsEquipping(Held) == 1)
                    WriteGunFlag(Held, GGunEquippingFlag, false);

                BotTrigger(Pawn, true);
                H.bTriggerHeld = true;
            }

            if (Held && WeaponAbilityIsRunning(Held))
                H.bAbilitySeen = true;

            // the game's ability did not start: play the sound ourselves (the item is still in the bot's hands)
            if (!H.bSoundPlayed && !H.bAbilitySeen && GStormTick >= H.StartTick + 30)
            {
                H.bSoundPlayed = true;

                const int Slot = (H.Info.Kind == 2) ? 2 : (H.Info.Score >= 5 ? 1 : 0);
                PlaySoundNearby(FindHealSound(Slot), Pawn->K2_GetActorLocation(), 5000.0);
            }

            if (GStormTick < H.EndTick)
                return true;

            // did the game's own ability already do it? (item used up, or health/shield went up)
            const bool bGameDid = BotItemCount(Inventory, H.Guid) < H.CountAtStart ||
                (H.Info.Kind == 1 ? Health > H.HealthAtStart + 0.5f : Shield > H.ShieldAtStart + 0.5f);

            // the game's ability is running but not done yet: give it up to 2.5 s more
            if (!bGameDid && H.bAbilitySeen && GStormTick < H.EndTick + 75)
                return true;

            if (H.bTriggerHeld)
            {
                BotTrigger(Pawn, false);
                BotTriggerUpTick[(void*)Pawn] = GStormTick;
                H.bTriggerHeld = false;
            }

            float NewHealth = Health, NewShield = Shield;

            if (!bGameDid)
            {
                // fallback: use it up and give the effect ourselves
                if (!BotConsumeOne(Inventory, H.Guid))
                {
                    BotCancelHeal(H, Pawn, PlayerState, "the item is gone");
                    return false;
                }

                if (H.Info.Kind == 1)
                {
                    NewHealth = Health + H.Info.Amount;
                    if (NewHealth > H.Info.Cap) NewHealth = H.Info.Cap;
                    if (NewHealth < Health)     NewHealth = Health;
                    ForceHealth(Pawn, Health, NewHealth);
                }
                else
                {
                    NewShield = Shield + H.Info.Amount;
                    if (NewShield > H.Info.Cap) NewShield = H.Info.Cap;
                    if (NewShield < Shield)     NewShield = Shield;
                    ForceShield(Pawn, Shield, NewShield);
                }

                GHealsByScript++;
            }
            else
            {
                GHealsByGame++;
            }

            LastKnownHealth[(void*)Controller] = Pawn->GetHealth();

            H.bActive = false;
            H.NextCheckTick = GStormTick + 20;
            GHealsFinished++;
            BotArmRetryAt.erase((void*)Pawn);

            if (GHealsFinished <= 15 || (GHealsFinished % 25) == 0)
                LogLine("[HEAL] '%s' used %s (%s): health %.0f -> %.0f, shield %.0f -> %.0f | %d heal(s) so far (game's own ability %d, script %d).",
                    SafeNameOf(PlayerState).c_str(), H.Name.c_str(), bGameDid ? "the GAME's own ability" : "the bot code's fallback",
                    H.HealthAtStart, Pawn->GetHealth(), H.ShieldAtStart, Pawn->GetShield(), GHealsFinished, GHealsByGame, GHealsByScript);

            return false;
        }

        // ---- should it start one? ----
        if (GStormTick < H.NextCheckTick)
            return false;

        H.NextCheckTick = GStormTick + 15;

        if (Health <= 0.f || SinceHit < 90 || BotBreakActive(Pawn))
            return false;

        if (Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone)
            return false;   // never stop in the storm

        const UFortItemDefinition* Pick = nullptr;
        FGuid PickGuid{};

        // health first: a medkit only at 50 or less, otherwise a bandage
        if (Health < 75.f)
        {
            if (Health <= 50.f && BotFindHeal(Inventory, 1, true, Pick, PickGuid))
            {
            }
            else
            {
                const UFortItemDefinition* Weak = nullptr;
                FGuid WeakGuid{};

                if (BotFindHeal(Inventory, 1, false, Weak, WeakGuid) && BotHealInfo(Weak).Score < 5)
                {
                    Pick = Weak;
                    PickGuid = WeakGuid;
                }
            }
        }

        // then shields: a small one first, then a big one
        if (!Pick && Shield < 50.f)
            BotFindHeal(Inventory, 2, false, Pick, PickGuid);

        if (!Pick && Shield <= 50.f)
            BotFindHeal(Inventory, 2, true, Pick, PickGuid);

        if (!Pick)
            return false;

        const FHealInfo Info = BotHealInfo(Pick);

        if (Info.Kind == 1 && Health >= Info.Cap - 1.f)
            return false;
        if (Info.Kind == 2 && Shield >= Info.Cap - 1.f)
            return false;

        LetGoOfTheTrigger(Pawn, GStormTick);

        // take it out so everybody sees it in the bot's hands
        Pawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)Pick, PickGuid);

        H = FBotHeal{};
        H.bActive = true;
        H.Def = Pick;
        H.Guid = PickGuid;
        H.Info = Info;
        H.Name = BotObjectName((const UObject*)Pick);
        H.StartTick = GStormTick;
        H.EndTick = GStormTick + (int)(Info.Seconds * 30.f);
        H.LastTick = GStormTick;
        H.HealthSeen = Health;
        H.HealthAtStart = Health;
        H.ShieldAtStart = Shield;
        H.CountAtStart = BotItemCount(Inventory, PickGuid);

        auto Body = BotBodies.find((void*)Pawn);
        if (Body != BotBodies.end())
            Body->second.bHasGoal = false;

        GHealsStarted++;

        if (GHealsStarted <= 15 || (GHealsStarted % 25) == 0)
            LogLine("[HEAL] '%s' (health %.0f, shield %.0f) starts using %s -- %.0f s | holding it now: %s | %d started so far.",
                SafeNameOf(PlayerState).c_str(), Health, Shield, H.Name.c_str(), Info.Seconds,
                (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon &&
                    ((AFortWeapon*)Pawn->CurrentWeapon)->HasWeaponData() &&
                    (const UFortItemDefinition*)((AFortWeapon*)Pawn->CurrentWeapon)->WeaponData == Pick) ? "yes" : "no (effect still applies)",
                GHealsStarted);

        return true;
    }


    // Opens the chest (it opens on everybody's screen) and offers everything in it to the bot.
    static void BotOpenChest(AActor* Chest, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState)
    {
        SetBoolByName((UObject*)Chest, "bAlreadySearched", true);
        InvokeNoArgs((UObject*)Chest, "OnRep_bAlreadySearched");
        InvokeNoArgs((UObject*)Chest, "ForceNetUpdate");

        auto Inventory = Controller ? Controller->WorldInventory : nullptr;

        if (!Inventory || !ResolveBotLootGroup())
            return;

        std::vector<std::pair<const UFortItemDefinition*, int>> Items;
        bool bGun = false;

        // a chest always holds a weapon: roll again if the roll had none
        for (int Try = 0; Try < 4 && !bGun; Try++)
        {
            Items.clear();

            TArray<FFortItemEntry*> Drops{};
            UFortLootPackage::ChooseLootForContainer(Drops, GBotLootGroup);

            for (int d = 0; d < Drops.Num(); d++)
            {
                FFortItemEntry* Entry = Drops[d];
                if (!Entry)
                    continue;

                const UFortItemDefinition* Def = Entry->ItemDefinition;
                const int Count = Entry->Count;
                free(Entry);

                if (!Def)
                    continue;

                Items.push_back({ Def, Count > 0 ? Count : 1 });

                if (BotGunKindOf(Def) != BotGun_None)
                    bGun = true;
            }

            Drops.Free();
        }

        std::string Got;

        for (auto& [Def, Count] : Items)
        {
            std::string Taken;

            if (((const UObject*)Def)->IsA<UFortAmmoItemDefinition>())
            {
                Inventory->GiveItem(Def, Count, 0);
                Taken = BotObjectName((const UObject*)Def) + " x" + std::to_string(Count);
            }
            else
            {
                Taken = BotConsiderItem(Controller, Def, Count, 0, true);
            }

            if (Taken.empty())
                continue;

            if (!Got.empty())
                Got += ", ";

            Got += Taken;
        }

        GChestsOpenedByBots++;

        if (GChestsOpenedByBots <= 15 || (GChestsOpenedByBots % 25) == 0)
            LogLine("[LOOT] '%s' opened a chest and took: %s (%d chest(s) opened by bots so far).",
                SafeNameOf(PlayerState).c_str(), Got.empty() ? "nothing it wanted" : Got.c_str(), GChestsOpenedByBots);
    }

    // ---- guns lying on the floor (house loot etc.) ----
    static std::vector<AActor*> GFloorGunCache;
    static float GFloorGunCacheAt = -1000.f;
    static int32 GPickupEntryOffset = -2;

    static FFortItemEntry* PickupEntry(AActor* Pickup)
    {
        if (!Pickup)
            return nullptr;

        if (GPickupEntryOffset == -2)
        {
            const uint32 Off = ((UObject*)Pickup)->GetOffset("PrimaryPickupItemEntry");
            GPickupEntryOffset = (Off == (uint32)-1 || Off > 0x1000) ? -1 : (int32)Off;

            LogLine("[LOOT] The floor pickups' item entry %s.", GPickupEntryOffset >= 0 ? "was found" : "is NOT on this build -- bots cannot loot the floor");
        }

        if (GPickupEntryOffset < 0)
            return nullptr;

        return (FFortItemEntry*)((uint8*)Pickup + GPickupEntryOffset);
    }

    static const UFortItemDefinition* PickupDefinition(AActor* Pickup, int* OutCount = nullptr, int* OutAmmo = nullptr)
    {
        auto Entry = PickupEntry(Pickup);

        if (!Entry || !Entry->HasItemDefinition())
            return nullptr;

        if (OutCount)
            *OutCount = Entry->HasCount() ? (int)Entry->Count : 1;

        if (OutAmmo)
            *OutAmmo = Entry->HasLoadedAmmo() ? (int)Entry->LoadedAmmo : 0;

        return Entry->ItemDefinition;
    }

    // Every gun lying on the floor (refreshed every 10 s).
    static const std::vector<AActor*>& GetFloorGuns(UWorld* World)
    {
        const float Now = World ? (float)UGameplayStatics::GetTimeSeconds(World) : 0.f;

        if (World && (Now - GFloorGunCacheAt) > 10.f)
        {
            GFloorGunCacheAt = Now;
            GFloorGunCache.clear();

            static auto PickupClass = FindClass("FortPickupAthena");

            if (PickupClass)
            {
                TArray<AActor*> All;
                UGameplayStatics::GetAllActorsOfClass(World, PickupClass, &All);

                for (int i = 0; i < All.Num(); i++)
                {
                    AActor* Candidate = All[i];

                    if (!Candidate || ReadBoolByName((const UObject*)Candidate, "bPickedUp") == 1)
                        continue;

                    const UFortItemDefinition* FloorDef = PickupDefinition(Candidate);

                    if (BotGunKindOf(FloorDef) == BotGun_None && BotHealScore(FloorDef) <= 0)
                        continue;

                    GFloorGunCache.push_back(Candidate);
                }
            }

            static int Reports = 0;
            if (++Reports <= 4)
                LogLine("[LOOT] Found %d gun pickup(s) on the floor (%s).", (int)GFloorGunCache.size(),
                    PickupClass ? "FortPickupAthena class found" : "FortPickupAthena class NOT found");
        }

        return GFloorGunCache;
    }

    static int BotBestGunScore(AFortPlayerControllerAthena* Controller)
    {
        auto Inventory = Controller ? Controller->WorldInventory : nullptr;
        if (!Inventory)
            return -1;

        int Best = -1;
        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            const int Score = BotGunScore(Entries.Get(e, FFortItemEntry::Size()).ItemDefinition);

            if (Score > Best)
                Best = Score;
        }

        return Best;
    }

    // The bot picks the item up if it wants it (free slot or an upgrade). Otherwise it stays on the floor.
    static void BotTakeFloorGun(AActor* Pickup, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState)
    {
        int Count = 1, Ammo = 0;
        auto Def = PickupDefinition(Pickup, &Count, &Ammo);

        if (!Def || !Controller || !Controller->WorldInventory)
            return;

        const std::string Taken = BotConsiderItem(Controller, Def, Count, Ammo, true);

        if (Taken.empty())
            return;

        SetBoolByName((UObject*)Pickup, "bPickedUp", true);
        Pickup->K2_DestroyActor();

        static int TakenCount = 0;
        if (++TakenCount <= 20 || (TakenCount % 25) == 0)
            LogLine("[LOOT] '%s' picked up %s off the floor (%d floor item(s) taken by bots so far).",
                SafeNameOf(PlayerState).c_str(), Taken.c_str(), TakenCount);
    }

    // True if the straight line from the bot's eyes to the loot is free.
// The loot itself, other players and pickups don't count as blocking.
    static bool BotEyesClearTo(AFortPlayerPawnAthena* Pawn, const FVector& MyLoc, AActor* Loot, std::string* Blocker)
    {
        if (!BotTraceAvailable())
            return true;

        const FVector At = Loot->K2_GetActorLocation();
        FVector Eyes(MyLoc.X, MyLoc.Y, MyLoc.Z + 60.0);
        FVector Point(At.X, At.Y, At.Z + 20.0);

        const FBotHit Hit = BotTrace((AActor*)Pawn, Eyes, Point, 0);

        if (!Hit.bHit || !Hit.Actor || Hit.Actor == Loot || (AActor*)Hit.Actor->Owner == Loot)
            return true;

        if (Hit.Actor->IsA<AFortPlayerPawnAthena>())
            return true;

        if (BotClassName(Hit.Actor).find("Pickup") != std::string::npos)
            return true;

        if (Blocker)
            *Blocker = BotObjectName(Hit.Actor) + " (" + BotClassName(Hit.Actor) + ")";

        return false;
    }

    static bool HandlePickupGuarded(UObject* Pawn, UFunction* Fn, uint8* Params)
    {
        __try
        {
            Pawn->ProcessEvent(Fn, Params);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The same server call a real player's E press makes on a pickup (AFortPlayerPawn::ServerHandlePickup).
    // Returns true only if the gun really ended up in the bot's inventory.
    static bool BotPressEOnPickup(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller,
        AFortPlayerStateAthena* PlayerState, AActor* Pickup)
    {
        static UFunction* Fn = nullptr;
        static bool bLooked = false, bBroken = false;
        static uint32 OPickup = 0, OFly = 0, OSound = 0, Size = 0;
        static bool bHasFly = false, bHasSound = false;

        auto Inventory = Controller ? Controller->WorldInventory : nullptr;

        if (bBroken || !Pawn || !Inventory || !Pickup)
            return false;

        if (!bLooked)
        {
            bLooked = true;
            Fn = ((UObject*)Pawn)->GetFunction("ServerHandlePickup");
            bool bHasPickup = false;

            if (Fn)
            {
                auto P = Fn->GetParamsNamed();
                Size = P.Size;

                for (auto& E : P.NameOffsetMap)
                {
                    if (E.Name == "Pickup" && E.ElementSize == 8) { OPickup = E.Offset; bHasPickup = true; }
                    else if (E.Name == "InFlyTime" && E.ElementSize == 4) { OFly = E.Offset; bHasFly = true; }
                    else if (E.Name == "bPlayPickupSound" && E.ElementSize == 1) { OSound = E.Offset; bHasSound = true; }
                }

                if (!bHasPickup || Size == 0 || Size > 0x100)
                    Fn = nullptr;
            }

            LogLine("[PICKUP] The game's ServerHandlePickup (the E press) %s.",
                Fn ? "was found -- bots press E on guns with it" : "is NOT usable on this build -- bots fall back to the direct pickup");
        }

        if (!Fn)
            return false;

        const UFortItemDefinition* Def = PickupDefinition(Pickup);
        if (!Def)
            return false;

        const bool bHadIt = InventoryHolds(Inventory, Def);

        alignas(16) uint8 Params[0x100];
        memset(Params, 0, sizeof(Params));
        *(void**)(Params + OPickup) = (void*)Pickup;
        if (bHasFly)
            *(float*)(Params + OFly) = 0.4f;
        if (bHasSound)
            Params[OSound] = 1;

        if (!HandlePickupGuarded((UObject*)Pawn, Fn, Params))
        {
            bBroken = true;
            LogLine("[PICKUP] ServerHandlePickup faulted -- not used from now on.");
            return false;
        }

        const bool bGot = InventoryHolds(Inventory, Def);

        static int Lines = 0;
        if (++Lines <= 10)
            LogLine("[PICKUP] '%s' pressed E on %s: %s.", SafeNameOf(PlayerState).c_str(), BotObjectName((const UObject*)Def).c_str(),
                bGot ? "it is in the inventory now" : "the game did NOT put it in the inventory -- using the direct pickup");

        if (bGot && !bHadIt)
        {
            GiveBotSpareRounds(Inventory, Def);
            GBotLootGuns[BotGunKindOf(Def)]++;
            GBotLootRarity[BotGunRarity(Def)]++;
            BotArmRetryAt.erase((void*)Pawn);
        }

        return bGot;
    }

    // Returns true while the bot is busy looting (the caller then skips wandering).
    static bool TickBotLooting(UWorld* World, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn, ::AAIController* AIController, const FVector& MyLoc)
    {
        FBotLoot& L = BotLoot[(void*)Controller];

        if (L.Wanted == 0)
            L.Wanted = 3 + (int)(BotPersonalSeed((void*)Controller) % 4);   // each bot loots 3-6 chests

        const bool bArmed = BotHasAnyGun(Controller);
        const bool bFull = BotLootIsFull(Controller);
        const bool bWantsMore = !bFull || L.ChestsOpened < L.Wanted;

        // ---- is the current target still valid? ----
        if (L.Chest)
        {
            bool bGone = !ActorIsLive((const UObject*)L.Chest);

            if (!bGone)
                bGone = L.bPickup ? (ReadBoolByName((const UObject*)L.Chest, "bPickedUp") == 1)
                : (ReadBoolByName((const UObject*)L.Chest, "bAlreadySearched") == 1);

            // new: it filled up on the way, or already found something better
            if (!bGone && L.bPickup && BotConsiderItem(Controller, PickupDefinition(L.Chest), 1, 0, false).empty())
                bGone = true;

            if (bGone)
            {
                ReleaseChest(L.Chest, (void*)Controller);
                L.Chest = nullptr;
                L.SearchEndTick = -1;
            }
        }

        // ---- pick a new target ----
        if (!L.Chest)
        {
            if (GStormTick < L.NextSearchTick)
                return false;

            L.NextSearchTick = GStormTick + 30;

            // not full: look wide. full: only chests right next to it, and only until its chest quota is used up
            const double ChestRange = !bArmed ? 20000.0 : (!bFull ? 12000.0 : (bWantsMore ? 2500.0 : 0.0));

            struct FCandidate { double Flat; AActor* Actor; bool bPickup; };
            std::vector<FCandidate> Candidates;

            for (AActor* Chest : GetChests(World))
            {
                if (!Chest || GBadChests.count(Chest) || L.Skip.count(Chest) || !ActorIsLive((const UObject*)Chest))
                    continue;

                if (ReadBoolByName((const UObject*)Chest, "bAlreadySearched") == 1)
                    continue;

                auto Claim = ChestClaims.find(Chest);
                if (Claim != ChestClaims.end() && Claim->second.first != (void*)Controller && (GStormTick - Claim->second.second) < 1200)
                    continue;

                const FVector At = Chest->K2_GetActorLocation();
                const double Dz = At.Z - MyLoc.Z;

                // a bot can't climb to a second floor (or drop down a cliff) in a straight line
                if (Dz > 220.0 || Dz < -400.0)
                    continue;

                const double Dx = At.X - MyLoc.X;
                const double Dy = At.Y - MyLoc.Y;
                const double Flat = sqrt(Dx * Dx + Dy * Dy);

                if (Flat < ChestRange)
                    Candidates.push_back({ Flat, Chest, false });
            }

            for (AActor* Pickup : GetFloorGuns(World))
            {
                if (!Pickup || GBadChests.count(Pickup) || L.Skip.count(Pickup) || !ActorIsLive((const UObject*)Pickup))
                    continue;

                if (ReadBoolByName((const UObject*)Pickup, "bPickedUp") == 1)
                    continue;

                auto Claim = ChestClaims.find(Pickup);
                if (Claim != ChestClaims.end() && Claim->second.first != (void*)Controller && (GStormTick - Claim->second.second) < 1200)
                    continue;

                // only walk to it if the bot would actually take it (free slot or an upgrade)
                if (BotConsiderItem(Controller, PickupDefinition(Pickup), 1, 0, false).empty())
                    continue;

                const FVector At = Pickup->K2_GetActorLocation();
                const double Dz = At.Z - MyLoc.Z;

                if (Dz > 220.0 || Dz < -400.0)
                    continue;

                const double Dx = At.X - MyLoc.X;
                const double Dy = At.Y - MyLoc.Y;
                const double Flat = sqrt(Dx * Dx + Dy * Dy);

                if (Flat < (bFull ? 3000.0 : (bArmed ? 8000.0 : 20000.0)))
                    Candidates.push_back({ Flat, Pickup, true });
            }

            if (Candidates.empty())
                return false;

            // trace only the 6 nearest: a target with a clear line beats a nearer one behind a wall
            std::sort(Candidates.begin(), Candidates.end(), [](const FCandidate& A, const FCandidate& B) { return A.Flat < B.Flat; });

            AActor* Best = nullptr;
            bool bBestIsPickup = false;
            double BestCost = 1e18, BestFlat = 0.0;
            const size_t Look = Candidates.size() < 6 ? Candidates.size() : 6;

            for (size_t i = 0; i < Look; i++)
            {
                const FCandidate& C = Candidates[i];
                const bool bClear = BotEyesClearTo(Pawn, MyLoc, C.Actor, nullptr);

                double Cost = C.Flat + (bClear ? 0.0 : 2500.0);

                if (C.bPickup && !bFull)
                    Cost -= 1000.0;   // a bot that still has room prefers things lying around

                if (Cost < BestCost)
                {
                    BestCost = Cost;
                    Best = C.Actor;
                    bBestIsPickup = C.bPickup;
                    BestFlat = C.Flat;
                }
            }

            if (!Best)
                return false;

            L.Chest = Best;
            L.bPickup = bBestIsPickup;
            L.BlockedTicks = 0;
            L.PressTick = -1;
            L.SearchEndTick = -1;
            L.BestDist = BestFlat;
            L.ProgressTick = GStormTick;
            L.GiveUpTick = GStormTick + 900 + (int)(BestFlat / 400.0 * 45.0);
            ChestClaims[Best] = { (void*)Controller, GStormTick };

            static int Reports = 0;
            if (++Reports <= 12)
                LogLine("[LOOT] '%s' is going for a %s %.0f m away (%s).", SafeNameOf(PlayerState).c_str(),
                    bBestIsPickup ? "gun on the floor" : "chest", (float)(BestFlat / 100.0),
                    bArmed ? "it has a gun already" : "it has no gun yet");
        }

        const FVector TargetAt = L.Chest->K2_GetActorLocation();
        const double Dx = TargetAt.X - MyLoc.X;
        const double Dy = TargetAt.Y - MyLoc.Y;
        const double Dz = TargetAt.Z - MyLoc.Z;
        const double Flat = sqrt(Dx * Dx + Dy * Dy);

        auto GiveUpOnTarget = [&](const char* Why)
            {
                L.Skip.insert(L.Chest);

                // only after 3 different bots fail does the target get blacklisted for everybody
                if (++GChestFailCount[L.Chest] >= 3)
                    GBadChests.insert(L.Chest);

                ReleaseChest(L.Chest, (void*)Controller);
                GChestsGivenUp++;

                if (GChestsGivenUp <= 15)
                    LogLine("[LOOT] '%s' gave up on its loot (%s) -- %d given up so far.", SafeNameOf(PlayerState).c_str(), Why, GChestsGivenUp);

                L.Chest = nullptr;
                L.bPickup = false;
                L.SearchEndTick = -1;
                L.NextSearchTick = GStormTick + 10;

                auto Body = BotBodies.find((void*)Pawn);
                if (Body != BotBodies.end())
                    Body->second.bHasGoal = false;
            };

        // ---- something solid between the bot and the loot? (throttled: a trace every 8 ticks) ----
        if (Flat > 1.0 && Flat <= 1500.0 && !BotBreakActive(Pawn) && GStormTick >= L.NextObstacleTick)
        {
            L.NextObstacleTick = GStormTick + 8;

            auto BodyIt = BotBodies.find((void*)Pawn);

            if (BodyIt != BotBodies.end())
            {
                bool bDoor = false;
                AActor* InTheWay = BotFindObstacle(Pawn, MyLoc, BotHalfHeight(Pawn), Dx / Flat, Dy / Flat, &bDoor, 330.0);

                if (InTheWay && InTheWay != L.Chest && BotBeginBreak(Controller, Pawn, BodyIt->second, MyLoc, InTheWay, bDoor))
                    return true;
            }
        }

        // ---- progress watchdog: no closer by 1.5 m in 8 s = this target is not reachable for this bot ----
        if (BotBreakActive(Pawn))
        {
            L.ProgressTick = GStormTick;   // mining a wall or opening a door counts as progress
        }
        else if (Flat < L.BestDist - 150.0)
        {
            L.BestDist = Flat;
            L.ProgressTick = GStormTick;
        }
        else if ((GStormTick - L.ProgressTick) > 240 && Flat > 220.0)
        {
            GiveUpOnTarget("no progress for 8 seconds");
            return false;
        }

        // ---- arrived ----
        if (Flat <= 220.0 && Dz <= 220.0 && Dz >= -400.0)
        {
            if (L.bPickup)
            {
                std::string Blocker;

                if (!BotEyesClearTo(Pawn, MyLoc, L.Chest, &Blocker))
                {
                    L.BlockedTicks++;
                    L.PressTick = -1;

                    if (L.BlockedTicks == 1)
                    {
                        static int BlockedLines = 0;
                        if (++BlockedLines <= 10)
                            LogLine("[PICKUP] '%s' is close to a gun but cannot see it (%s is in the way) -- no pickup.",
                                SafeNameOf(PlayerState).c_str(), Blocker.c_str());
                    }

                    if (L.BlockedTicks > 90)
                    {
                        GiveUpOnTarget("the gun is behind something");
                        return false;
                    }

                    BotMoveTo(AIController, Pawn, TargetAt, 100.f);
                    return true;
                }

                L.BlockedTicks = 0;

                if (L.PressTick < 0)
                {
                    L.PressTick = GStormTick + 12;
                    BotFaceActor(Controller, Pawn, MyLoc, L.Chest);

                    auto BodyIt = BotBodies.find((void*)Pawn);
                    if (BodyIt != BotBodies.end())
                        BodyIt->second.bHasGoal = false;

                    return true;
                }

                if (GStormTick < L.PressTick)
                    return true;

                // the game's own E press would swap out whatever the bot holds when its slots are full,
                // so the bot code applies the 4-weapons + 1-heal rule itself
                BotTakeFloorGun(L.Chest, Controller, PlayerState);

                L.PressTick = -1;
            }
            else
            {
                // a short "searching the chest" pause: stop, face it, then it opens
                if (L.SearchEndTick < 0)
                {
                    L.SearchEndTick = GStormTick + 30 + (int)(BotPersonalSeed((void*)Pawn) % 25);

                    auto BodyIt = BotBodies.find((void*)Pawn);
                    if (BodyIt != BotBodies.end())
                        BodyIt->second.bHasGoal = false;
                }

                if (GStormTick < L.SearchEndTick)
                {
                    BotFaceActor(Controller, Pawn, MyLoc, L.Chest);
                    return true;
                }

                BotOpenChest(L.Chest, Controller, PlayerState);
                L.ChestsOpened++;
            }

            ReleaseChest(L.Chest, (void*)Controller);
            L.Chest = nullptr;
            L.bPickup = false;
            L.SearchEndTick = -1;
            L.NextSearchTick = GStormTick + 20;

            auto Body = BotBodies.find((void*)Pawn);
            if (Body != BotBodies.end())
                Body->second.bHasGoal = false;

            return true;
        }

        if (GStormTick > L.GiveUpTick)
        {
            GiveUpOnTarget("took too long");
            return false;
        }

        BotMoveTo(AIController, Pawn, TargetAt, 100.f);
        return true;
    }

    // Before combat opens, a bot with nothing to loot walks somewhere instead of standing still.
    static void RoamWhileWaiting(AFortPlayerPawnAthena* Pawn, ::AAIController* AIController, const FVector& MyLoc)
    {
        auto It = BotBodies.find((void*)Pawn);

        if (It != BotBodies.end() && It->second.bHasGoal)
            return;   // still walking to the last spot

        BotMoveTo(AIController, Pawn, PickWalkableWanderDest(Pawn, MyLoc), 50.f);
    }

    struct FStormPlan
    {
        bool bKnown = false;
        bool bRotate = false;
        FVector Centre{};
        float Radius = 0.f;
        FVector Goal{};
        double DistToGoal = 0.0;
        float Slack = 0.f;
    };

    static std::map<void*, std::pair<int, bool>> BotStormLatch;
    static std::map<void*, std::pair<FVector, FVector>> BotStormHop;
    static int GStormRotationsStarted = 0;

    static FStormPlan PlanStormRotation(AFortGameMode* GameMode, AFortPlayerPawnAthena* Pawn, const FVector& MyLoc,
        bool bOutsideNow, float Now)
    {
        FStormPlan Plan{};

        if (!FConfiguration::bBotsFleeTheStorm || !GameMode || !Pawn)
            return Plan;

        FVector Centre{};
        float Radius = 0.f;

        if (!GetSafeZoneCircle(GameMode, Centre, Radius))
            return Plan;

        auto Indicator = GameMode->SafeZoneIndicator;
        const float Start = Indicator->HasSafeZoneStartShrinkTime() ? (float)Indicator->SafeZoneStartShrinkTime : -1.f;
        const float Finish = Indicator->HasSafeZoneFinishShrinkTime() ? (float)Indicator->SafeZoneFinishShrinkTime : -1.f;
        const int Phase = GetStormPhase(GameMode);
        const uint32 Seed = BotPersonalSeed((void*)Pawn);

        Plan.bKnown = true;
        Plan.Centre = Centre;
        Plan.Radius = Radius;

        const double Dx = MyLoc.X - Centre.X;
        const double Dy = MyLoc.Y - Centre.Y;
        const double Dist = sqrt(Dx * Dx + Dy * Dy);

        double Margin = (double)Radius * 0.12;
        if (Margin > 1500.0)
            Margin = 1500.0;

        const double EdgeRadius = (double)Radius - Margin;
        const bool bInside = Dist <= EdgeRadius;

        {
            const uint32 GoalSeed = Seed ^ (uint32)((uint32)Phase * 2654435761u);
            const double Angle = ((double)(GoalSeed % 3600) / 10.0) * 3.14159265358979323846 / 180.0;
            const double Out = (double)Radius * (0.15 + (double)((GoalSeed >> 12) % 45) / 100.0);

            Plan.Goal = FVector(Centre.X + cos(Angle) * Out, Centre.Y + sin(Angle) * Out, MyLoc.Z);

            const double Gx = Plan.Goal.X - MyLoc.X;
            const double Gy = Plan.Goal.Y - MyLoc.Y;
            Plan.DistToGoal = sqrt(Gx * Gx + Gy * Gy);
        }

        double Speed = (FConfiguration::BotSprintSpeed > 0.f) ? (double)FConfiguration::BotSprintSpeed : 510.0;
        Speed *= 0.85;

        const double ToEdge = (Dist > EdgeRadius) ? (Dist - EdgeRadius) : 0.0;
        const float Eta = (float)(ToEdge / Speed) + 5.f;

        auto& Latch = BotStormLatch[(void*)Pawn];

        if (Latch.first != Phase)
            Latch = { Phase, false };

        if (Latch.second && Dist <= (double)Radius * 0.75)
            Latch.second = false;

        const bool bHolding = Start > 0.f && Now < Start;
        const bool bClosing = Start > 0.f && Now >= Start && Now < Finish;

        bool bShouldStart = false;

        if (bOutsideNow)
        {
            bShouldStart = true;
        }
        else if (!bInside)
        {
            if (bHolding)
            {
                Plan.Slack = (Start - Now) - Eta;
                const float Window = 30.f + (float)(Seed % 31);

                if (Plan.Slack < Window)
                    bShouldStart = true;
            }
            else
            {
                bShouldStart = true;
            }
        }


        if (bShouldStart && !Latch.second)
        {
            Latch.second = true;
            GStormRotationsStarted++;

            if (GStormRotationsStarted <= 15 || (GStormRotationsStarted % 50) == 0)
                LogLine("[STORM-AI] Bot rotating (phase %d): %.0f m from the next circle's centre (radius %.0f m), %s%s | slack %.0fs | "
                    "ETA %.0fs | %d rotation(s) so far.",
                    Phase, (float)(Dist / 100.0), Radius / 100.f,
                    bHolding ? "storm holding" : (bClosing ? "storm CLOSING" : "storm timing unknown"),
                    bOutsideNow ? ", already in the storm" : "", Plan.Slack, Eta, GStormRotationsStarted);
        }

        Plan.bRotate = Latch.second;
        return Plan;
    }

    // ======================= RANGE-BASED GUN SWITCHING =======================
    static constexpr double kCloseRange = 1500.0;      // under 15 m = close
    static constexpr double kFarRange = 4500.0;        // over 45 m = far
    static constexpr int kGunSwitchCooldownTicks = 60; // 2 s between switches
    static constexpr int kGunSwitchMargin = 15;        // new gun must beat the held one by this much (stops flip-flopping at the borders)

    static std::map<void*, int> BotLastGunSwitchTick;
    static int GGunSwitches = 0;

    // How good this gun is at this distance. Higher = better. -1 = not a gun.
    static int GunRangeScore(const UFortItemDefinition* Def, double Distance)
    {
        const int Kind = BotGunKindOf(Def);

        if (Kind == BotGun_None)
            return -1;

        // columns: close, medium, far
        static const int Table[BotGun_KindCount][3] = {
            {   0,   0,   0 },   // none
            {   0,  85,  60 },   // launcher (never up close, so it can't blow the bot up)
            {   5,  50, 100 },   // sniper
            {  50,  30,  10 },   // pistol
            { 100,  30,   5 },   // shotgun
            {  90,  60,  30 },   // SMG
            {  40, 100,  90 },   // assault rifle
        };

        const int Band = Distance < kCloseRange ? 0 : (Distance < kFarRange ? 1 : 2);

        return Table[Kind][Band] + BotGunRarity(Def) * 2;   // rarity only breaks ties
    }

    static void SwitchGunForRange(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* Controller,
        AFortPlayerStateAthena* PlayerState, double Distance, int TickNow)
    {
        if (!Pawn || !Controller || !PlayerState)
            return;

        auto Inventory = Controller->WorldInventory;
        if (!Inventory)
            return;

        // never switch in the middle of a burst, a reload, a heal or while mining
        if (BotIsHealing(Pawn) || BotIsReloading(Pawn) || BotBreakActive(Pawn))
            return;

        auto FireIt = BotFire.find(Pawn);
        if (FireIt != BotFire.end() && FireIt->second.bFiring)
            return;

        auto Last = BotLastGunSwitchTick.find((void*)Pawn);
        if (Last != BotLastGunSwitchTick.end() && (TickNow - Last->second) < kGunSwitchCooldownTicks)
            return;

        const UFortItemDefinition* HeldDef = nullptr;

        if (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
        {
            auto Held = (AFortWeapon*)Pawn->CurrentWeapon;

            if (Held->HasWeaponData() && Held->WeaponData)
                HeldDef = (const UFortItemDefinition*)Held->WeaponData;
        }

        const int HeldScore = GunRangeScore(HeldDef, Distance);

        const UFortItemDefinition* BestDef = nullptr;
        FGuid BestGuid{};
        int BestScore = -1;

        auto& Entries = Inventory->Inventory.ReplicatedEntries;

        for (int e = 0; e < Entries.Num(); e++)
        {
            auto& Entry = Entries.Get(e, FFortItemEntry::Size());
            const int Score = GunRangeScore(Entry.ItemDefinition, Distance);

            if (Score > BestScore)
            {
                BestScore = Score;
                BestDef = Entry.ItemDefinition;
                BestGuid = Entry.ItemGuid;
            }
        }

        if (!BestDef || BestDef == HeldDef)
            return;

        // holding a pickaxe or a bandage = switch right away, otherwise it has to be clearly better
        const int Margin = HeldScore < 0 ? 0 : kGunSwitchMargin;

        if (BestScore <= HeldScore + Margin)
            return;

        auto GunDef = BestDef->Cast<UFortWeaponItemDefinition>();
        if (!GunDef)
            return;

        BotLastGunSwitchTick[(void*)Pawn] = TickNow;

        LetGoOfTheTrigger(Pawn, TickNow);
        Pawn->EquipWeaponDefinition(GunDef, BestGuid);

        // keep the reload / refill code pointed at the gun it holds now
        BotWeapons[(void*)PlayerState] = BestDef;
        BotWeaponClipSize[(void*)PlayerState] = BotGunMagazine(BestDef);
        BotInventories[(void*)PlayerState] = Inventory;

        // give the gun a moment to come out before the next burst
        FBotFireState& Fire = BotFire[Pawn];
        if (Fire.CooldownTicksLeft < 20)
            Fire.CooldownTicksLeft = 20;

        GGunSwitches++;

        if (GGunSwitches <= 20 || (GGunSwitches % 50) == 0)
            LogLine("[SWITCH] '%s' switched from %s to %s at %.0f m (%d switch(es) so far).",
                SafeNameOf(PlayerState).c_str(), HeldDef ? BotObjectName((const UObject*)HeldDef).c_str() : "nothing",
                BotObjectName((const UObject*)BestDef).c_str(), (float)(Distance / 100.0), GGunSwitches);
    }

    // ======================= BUILD FIGHTING (v2) =======================
    static constexpr double kStairForwardShift = 0.0;   // slide the stair along its tile if the probe says it sits off-centre
    static constexpr int kBuildStepTicks = 3;           // ticks between pieces
    static constexpr int kMaxRampLevels = 8;            // most levels a ramp can climb
    static constexpr int kPeeksPerFight = 3;            // edit-peeks before the fight ends
    static constexpr double kBuildCloseRange = 1200.0;  // under 12 m = close-range sequence

    static double GStairYawOffset = 0.0;                // set by itself after the first stair
    static bool GStairCalibrated = false;

    struct FBuildSet { UClass* Wall = nullptr; UClass* Floor = nullptr; UClass* Stair = nullptr; UClass* Roof = nullptr; };
    static FBuildSet GBuildSets[3];     // wood, brick, metal
    static UClass* GEditWall[3] = {};   // the "window" version of a wall, per material
    static bool GBuildSetsResolved = false;

    static UClass* FindBuildClass(int Material, const wchar_t* Kind)
    {
        static const wchar_t* Folders[3] = { L"Wood", L"Stone", L"Metal" };
        static const wchar_t* Codes[3] = { L"W1", L"S1", L"M1" };

        std::wstring Name = std::wstring(L"PBWA_") + Codes[Material] + L"_" + Kind;
        std::wstring Path = std::wstring(L"/Game/Building/ActorBlueprints/Player/") + Folders[Material] + L"/L1/" + Name + L"." + Name + L"_C";
        std::string Short(Name.begin(), Name.end());
        Short += "_C";

        return ResolveBuildClass(Path.c_str(), Short.c_str());
    }

    static void ResolveBuildSets()
    {
        if (GBuildSetsResolved)
            return;

        GBuildSetsResolved = true;

        static const wchar_t* EditKinds[] = { L"Window", L"Door", L"Doorway", L"Archway", L"Arch" };
        std::string Report;

        for (int m = 0; m < 3; m++)
        {
            GBuildSets[m].Wall = FindBuildClass(m, L"Solid");
            GBuildSets[m].Floor = FindBuildClass(m, L"Floor");
            GBuildSets[m].Stair = FindBuildClass(m, L"StairW");
            GBuildSets[m].Roof = FindBuildClass(m, L"RoofC");

            std::string EditName = "none";

            for (auto Kind : EditKinds)
            {
                if (auto Cls = FindBuildClass(m, Kind))
                {
                    GEditWall[m] = Cls;
                    std::wstring W(Kind);
                    EditName = std::string(W.begin(), W.end());
                    break;
                }
            }

            Report += std::string(m == 0 ? "wood" : (m == 1 ? "brick" : "metal")) + ": wall " + (GBuildSets[m].Wall ? "ok" : "MISSING") +
                ", stair " + (GBuildSets[m].Stair ? "ok" : "MISSING") + ", roof " + (GBuildSets[m].Roof ? "ok" : "MISSING") +
                ", edit piece " + EditName + " | ";
        }

        LogLine("[BUILDFIGHT] Build pieces: %s", Report.c_str());

        // list every loaded wood piece class so the real edit class names can be read from the log
        std::string Line;
        int InLine = 0, Total = 0;
        UClass* ScanWindow = nullptr;

        for (int i = 0; i < TUObjectArray::Num() && Total < 90; i++)
        {
            const UObject* O = TUObjectArray::GetObjectByIndex(i);

            if (!O || !O->Class || !(O->Class->GetCastFlags() & 0x20))
                continue;

            const std::string N = O->Name.ToString().c_str();

            if (N.rfind("PBWA_W1_", 0) != 0)
                continue;

            Total++;
            Line += N + ", ";

            if (!ScanWindow && LowerText(N).find("window") != std::string::npos)
                ScanWindow = (UClass*)O;

            if (++InLine >= 6)
            {
                LogLine("[BUILDFIGHT] Loaded wood piece classes: %s", Line.c_str());
                Line.clear();
                InLine = 0;
            }
        }

        if (!Line.empty())
            LogLine("[BUILDFIGHT] Loaded wood piece classes: %s", Line.c_str());

        if (!GEditWall[0] && ScanWindow)
        {
            GEditWall[0] = ScanWindow;
            LogLine("[BUILDFIGHT] Edit piece taken from the class scan: %s", BotObjectName((const UObject*)ScanWindow).c_str());
        }

        LogLine("[BUILDFIGHT] %d wood piece class(es) loaded. Edit-peeks %s.", Total,
            GEditWall[0] ? "are ON" : "are OFF (no window/door class found -- send me the class list above)");
    }

    // Spawns a piece at an exact spot (no wall snapping).
    static ABuildingSMActor* SpawnPieceRaw(UClass* Class, const FVector& Loc, double Yaw, AFortPlayerControllerAthena* Controller,
        AFortPlayerStateAthena* PS)
    {
        if (!Class || !Controller)
            return nullptr;

        FRotator Rot{};
        Rot.Pitch = 0.0;
        Rot.Yaw = Yaw;
        Rot.Roll = 0.0;

        ABuildingSMActor* Building = UWorld::SpawnActorUnfinished<ABuildingSMActor>(Class, Loc, Rot, Controller);
        if (!Building)
            return nullptr;

        Building->InitializeKismetSpawnedBuildingActor(Building, Controller, true, nullptr, false);
        UWorld::FinishSpawnActor(Building, Loc, Rot);

        Building->CurrentBuildingLevel = 0;
        Building->OnRep_CurrentBuildingLevel();
        Building->SetMirrored(false);
        Building->bPlayerPlaced = true;

        if (PS && PS->HasTeamIndex())
            Building->Team = PS->TeamIndex;
        if (Building->HasTeamIndex())
            Building->TeamIndex = Building->Team;

        Building->ForceNetUpdate();
        return Building;
    }

    struct FBuiltPiece
    {
        AActor* Actor = nullptr;
        UClass* Class = nullptr;
        FVector Loc{};
        double Yaw = 0.0;
    };

    // An "edit": the piece is replaced by another class in the same spot.
    static bool EditPieceTo(FBuiltPiece& P, UClass* NewClass, AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PS)
    {
        if (!NewClass)
            return false;

        if (P.Actor && ActorIsLive((const UObject*)P.Actor))
            P.Actor->K2_DestroyActor();

        auto Replacement = SpawnPieceRaw(NewClass, P.Loc, P.Yaw, Controller, PS);

        if (!Replacement)
        {
            P.Actor = nullptr;
            return false;
        }

        P.Actor = (AActor*)Replacement;
        P.Class = NewClass;
        return true;
    }

    struct FBotBuildFight
    {
        bool bActive = false;
        int Mode = 0;            // 1 = ramp rush, 2 = box, 3 = close range (wall, ramp, wall, peek)
        int Set = 0;             // 0 wood, 1 brick, 2 metal
        int Step = 0;
        int Level = 0;
        int NextActTick = 0;
        int StepTick = 0;
        int EndTick = 0;
        int NextTryTick = 0;
        int Peeks = 0;
        double DirX = 1.0, DirY = 0.0, DirYaw = 0.0;
        double BaseZ = 0.0;
        double PlacedYaw = 0.0;
        double StairCX = 0.0, StairCY = 0.0;
        double ClimbX = 0.0, ClimbY = 0.0, ClimbFeet = 0.0;
        FVector Tile{};
        FBuiltPiece Front;       // the wall that gets edited

        // NEW
        int ClimbStallTicks = 0;
        double ClimbLastZ = 0.0;
        int PeekWaitTick = -1;   // when the bot started waiting behind cover for an opening
        int PeekHoldUntil = 0;   // keep the window open at most until this tick
        int TargetLevels = 3;    // how many levels this fight climbs (re-worked out after every level)
    };

    static std::map<void*, FBotBuildFight> BotBuildFights;
    static int GBuildFightsStarted = 0;
    static int GBuildPiecesPlaced = 0;
    static int GBuildEdits = 0;

    static void SetBuildDir(FBotBuildFight& F, int Dir)   // 0 = +X, 1 = +Y, 2 = -X, 3 = -Y
    {
        static const double DX[4] = { 1.0, 0.0, -1.0, 0.0 };
        static const double DY[4] = { 0.0, 1.0, 0.0, -1.0 };

        F.DirX = DX[Dir & 3];
        F.DirY = DY[Dir & 3];
        F.DirYaw = (double)(Dir & 3) * 90.0;
    }

    static void StopBuildFight(AFortPlayerPawnAthena* Pawn)
    {
        auto It = BotBuildFights.find((void*)Pawn);
        if (It == BotBuildFights.end() || !It->second.bActive)
            return;

        It->second.bActive = false;
        It->second.Front = FBuiltPiece{};
        It->second.NextTryTick = GStormTick + 120;   // 4 s before it may start another

        auto Body = BotBodies.find((void*)Pawn);
        if (Body != BotBodies.end())
        {
            Body->second.bDirectMove = false;
            Body->second.bHasGoal = false;
            Body->second.ProgressTick = -1;
            Body->second.StuckTick = -1;
        }
    }

    // Lifts / slides the bot towards F.ClimbX/Y/Feet by script (walking up the stairs does not work with this movement).
    // Walks up the stair with the game's real movement. If the bot stalls, it falls back to a SLOW scripted climb
    // (about run speed, no teleport flag) so clients see it move instead of snap.
    static bool ClimbStairs(AFortPlayerPawnAthena* Pawn, FBotBody& Body, FBotBuildFight& F, const FVector& Loc, double Half)
    {
        const double TargetZ = F.ClimbFeet + Half + 4.0;
        const double Dx = F.ClimbX - Loc.X;
        const double Dy = F.ClimbY - Loc.Y;
        const double Dz = TargetZ - Loc.Z;
        const double Flat = sqrt(Dx * Dx + Dy * Dy);

        // the climb step just started: reset the stall tracking
        if ((GStormTick - F.StepTick) <= 1)
        {
            F.ClimbLastZ = Loc.Z;
            F.ClimbStallTicks = 0;
        }

        if (Flat < 60.0 && fabs(Dz) < 40.0)
        {
            Body.bHasGoal = false;
            F.ClimbStallTicks = 0;
            return true;
        }

        // 1) walk towards the top of the stair with real movement
        Body.bHasGoal = true;
        Body.Goal = FVector(F.ClimbX, F.ClimbY, TargetZ);
        Body.AcceptRadius = 40.f;

        // 2) are we actually rising?
        if (Loc.Z > F.ClimbLastZ + 4.0)
        {
            F.ClimbLastZ = Loc.Z;
            F.ClimbStallTicks = 0;
        }
        else
        {
            F.ClimbStallTicks++;
        }

        // small hop to get over the first step lip
        if (Dz > 60.0 && (F.ClimbStallTicks == 6 || F.ClimbStallTicks == 14))
        {
            FVector Hopup{};
            Hopup.X = (Flat > 1.0) ? Dx / Flat * 120.0 : 0.0;
            Hopup.Y = (Flat > 1.0) ? Dy / Flat * 120.0 : 0.0;
            Hopup.Z = 520.0;
            Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);
        }

        // 3) still stuck after ~0.7 s: slow scripted climb, ~420 units/s, NOT a teleport
        if (F.ClimbStallTicks >= 20)
        {
            double NX = Loc.X, NY = Loc.Y, NZ = Loc.Z;

            if (Flat > 1.0)
            {
                const double S = Flat < 14.0 ? Flat : 14.0;
                NX += Dx / Flat * S;
                NY += Dy / Flat * S;
            }

            if (Dz > 0.0)
                NZ += Dz < 14.0 ? Dz : 14.0;

            FVector To(NX, NY, NZ);
            Pawn->K2_SetActorLocation(To, false, (void*)nullptr, false);   // last argument false = no teleport
        }

        return false;
    }

    // Is this player/bot an easy target right now?
    static bool BotTargetVulnerable(AFortPlayerPawnAthena* Target, const char** Why)
    {
        const char* Ignored = "";
        const char*& Reason = Why ? *Why : Ignored;
        Reason = "";

        if (!Target || Target->GetHealth() <= 0.f)
            return false;

        if (BotIsHealing(Target)) { Reason = "healing";            return true; }
        if (BotIsReloading(Target)) { Reason = "reloading";          return true; }

        if (Target->HasCurrentWeapon() && Target->CurrentWeapon)
        {
            auto Weapon = (AActor*)Target->CurrentWeapon;

            if (GunIsReloading(Weapon) == 1) { Reason = "reloading";           return true; }
            if (GunIsEquipping(Weapon) == 1) { Reason = "switching weapons";   return true; }

            if (BotHoldingGun(Target))
            {
                auto Ammo = WeaponAmmoCount(Weapon);
                if (Ammo && *Ammo <= 0) { Reason = "empty gun";           return true; }
            }
        }
        else
        {
            Reason = "nothing in hands";
            return true;
        }

        if (Target->GetHealth() + Target->GetShield() <= 45.f)
        {
            Reason = "low life";
            return true;
        }

        return false;
    }

    // Returns true while the bot is build fighting (the caller then skips its normal chase movement;
    // aiming and shooting still run as usual).
    static bool TickBotBuildFight(AFortPlayerControllerAthena* Controller, AFortPlayerStateAthena* PS, AFortPlayerPawnAthena* Pawn,
        AFortPlayerPawnAthena* Target, const FVector& Loc, double Flat, bool bTargetVisible)
    {
        if (!Controller || !PS || !Pawn || !Target)
            return false;

        auto BodyIt = BotBodies.find((void*)Pawn);
        if (BodyIt == BotBodies.end())
            return false;

        FBotBody& Body = BodyIt->second;
        FBotBuildFight& F = BotBuildFights[(void*)Pawn];

        const double Half = BotHalfHeight(Pawn);
        const double Feet = Loc.Z - Half;
        const float Life = Pawn->GetHealth() + Pawn->GetShield();

        auto Hurt = BotLastDamager.find((void*)Controller);
        const int SinceHit = (Hurt != BotLastDamager.end()) ? (GStormTick - Hurt->second.second) : 100000;

        // ---------------- not building yet: should it start? ----------------
        if (!F.bActive)
        {
            if (GStormTick < F.NextTryTick)
                return false;

            F.NextTryTick = GStormTick + 60;

            uint8 Mode = 0;
            if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasMovementMode())
                Mode = Pawn->CharacterMovement->MovementMode;

            if (Mode != 1 || BotBreakActive(Pawn) || BotIsHealing(Pawn) || BotIsReloading(Pawn))
                return false;

            const bool bUnderFire = SinceHit < 60;

            const char* VulnWhy = "";
            const bool bVuln = BotTargetVulnerable(Target, &VulnWhy);

            bool bTargetIsBot = false;
            if (auto TC = (AFortPlayerControllerAthena*)Target->Controller)
                if (auto TS = (AFortPlayerStateAthena*)TC->PlayerState)
                    bTargetIsBot = TS->HasbIsABot() && TS->bIsABot;


            const bool bEnemyAbove = Target->K2_GetActorLocation().Z > Loc.Z + 300.0;

            const bool bWantsFight = bTargetVisible && Flat < 4500.0;

            if (!bUnderFire && !bWantsFight)
                return false;

            ResolveBuildSets();

            int Set = 0;
            const int Roll = rand() % 100;
            if (Roll >= 90) Set = 2; else if (Roll >= 70) Set = 1;
            if (!GBuildSets[Set].Stair || !GBuildSets[Set].Wall)
                Set = 0;
            if (!GBuildSets[Set].Stair || !GBuildSets[Set].Wall)
            {
                static int NoSetLines = 0;
                if (++NoSetLines <= 3)
                    LogLine("[BUILDFIGHT] No wall+stair classes found -- cannot build (see the 'Build pieces' line).");

                return false;
            }

            const FVector TLoc = Target->K2_GetActorLocation();
            const double Dx = TLoc.X - Loc.X;
            const double Dy = TLoc.Y - Loc.Y;

            F = FBotBuildFight{};
            F.NextTryTick = GStormTick + 60;
            F.Set = Set;

            int Dir;
            if (fabs(Dx) >= fabs(Dy)) Dir = (Dx >= 0.0) ? 0 : 2;
            else Dir = (Dy >= 0.0) ? 1 : 3;
            SetBuildDir(F, Dir);

            F.Tile = FVector(SnapTile(Loc.X - 256.0) + 256.0, SnapTile(Loc.Y - 256.0) + 256.0, 0.0);
            F.BaseZ = Feet;

            if (Life < 30.f)
                F.Mode = 2;
            else if (Flat < kBuildCloseRange)
                F.Mode = 3;
            else
                F.Mode = 1;

            {
                // build until the bot is 1-2 floors above the enemy's feet
                const double Rise = (TLoc.Z - Half) - Feet;
                int Need = (int)ceil(Rise / 384.0) + 2;
                if (Need < 3) Need = 3;
                if (Need > kMaxRampLevels) Need = kMaxRampLevels;
                F.TargetLevels = Need;
            }

            F.bActive = true;
            F.StepTick = GStormTick;
            F.NextActTick = GStormTick;
            F.EndTick = GStormTick + 30 * (F.Mode == 2 ? 16 : 30);   // ramps take longer than a box

            GBuildFightsStarted++;
            if (GBuildFightsStarted <= 15 || (GBuildFightsStarted % 25) == 0)
                LogLine("[BUILDFIGHT] '%s' starts a %s against '%s' (%.0f m, life %.0f, %s) | tile X=%.0f Y=%.0f feet Z=%.0f | %d so far.",
                    SafeNameOf(PS).c_str(), F.Mode == 1 ? "RAMP RUSH" : (F.Mode == 3 ? "CLOSE FIGHT" : "BOX"),
                    BotPawnName(Target).c_str(), (float)(Flat / 100.0), Life, bUnderFire ? "under fire" : "its own choice",
                    (float)F.Tile.X, (float)F.Tile.Y, (float)F.BaseZ, GBuildFightsStarted);
        }

        // ---------------- running ----------------
        if (GStormTick > F.EndTick || Life <= 0.f || Target->GetHealth() <= 0.f)
        {
            if (F.Mode == 2 && F.Front.Actor && ActorIsLive((const UObject*)F.Front.Actor))
                F.Front.Actor->K2_DestroyActor();   // a box opens when it is over

            StopBuildFight(Pawn);
            return false;
        }

        Body.bDirectMove = true;

        const FBuildSet& Set = GBuildSets[F.Set];
        UClass* EditClass = GEditWall[F.Set] ? GEditWall[F.Set] : GEditWall[0];
        const double Z = F.BaseZ + (double)F.Level * 384.0;

        auto Put = [&](UClass* Class, double X, double Y, double PZ, double Yaw) -> FBuiltPiece
            {
                FBuiltPiece P;
                P.Class = Class;
                P.Loc = FVector(X, Y, PZ);
                P.Yaw = Yaw;

                if (Class)
                {
                    auto Piece = SpawnPieceRaw(Class, P.Loc, Yaw, Controller, PS);

                    if (Piece)
                    {
                        P.Actor = (AActor*)Piece;
                        GBuildPiecesPlaced++;
                    }
                }

                return P;
            };

        // Looks at the ground on the 4 sides of a stair: the highest side is where it rises. Fixes the climb target, the direction,
        // and (once) the rotation the next stairs are placed with.
        auto ProbeStair = [&](double CX, double CY, double PZ) -> bool
            {
                static const double DX[4] = { 1.0, 0.0, -1.0, 0.0 };
                static const double DY[4] = { 0.0, 1.0, 0.0, -1.0 };
                double H[4] = { -99999.0, -99999.0, -99999.0, -99999.0 };
                double Best = -1e9, Lowest = 1e9;
                int BestDir = -1;

                for (int d = 0; d < 4; d++)
                {
                    double G = 0.0;

                    if (!BotGroundBelow(Pawn, CX + DX[d] * 190.0, CY + DY[d] * 190.0, PZ + 700.0, PZ - 100.0, G))
                        continue;

                    H[d] = G;
                    if (G > Best) { Best = G; BestDir = d; }
                    if (G < Lowest) Lowest = G;
                }

                static int Lines = 0;
                if (++Lines <= 10)
                    LogLine("[BUILDFIGHT] Stair probe at X=%.0f Y=%.0f (level floor Z=%.0f): ground +X %.0f, +Y %.0f, -X %.0f, -Y %.0f | placed yaw %.0f | %s.",
                        (float)CX, (float)CY, (float)PZ, (float)H[0], (float)H[1], (float)H[2], (float)H[3], (float)F.PlacedYaw,
                        (BestDir >= 0 && Best - Lowest >= 80.0) ? "a slope was found" : "NO slope found (the stair is missing or tilted oddly)");

                if (BestDir < 0 || Best - Lowest < 80.0)
                {
                    // the probe could not read the stair: assume it rises the way it was placed, and climb that
                    F.StairCX = CX;
                    F.StairCY = CY;
                    F.ClimbX = CX + F.DirX * 190.0;
                    F.ClimbY = CY + F.DirY * 190.0;
                    F.ClimbFeet = PZ + 335.0;

                    static int GuessLines = 0;
                    if (++GuessLines <= 10)
                        LogLine("[BUILDFIGHT] The stair probe could not read the slope -- climbing the way the stair was placed.");

                    return true;
                }

                if (!GStairCalibrated)
                {
                    GStairCalibrated = true;

                    double Off = fmod(F.PlacedYaw - (double)BestDir * 90.0, 360.0);
                    if (Off < 0.0) Off += 360.0;
                    Off = fmod(round(Off / 90.0) * 90.0, 360.0);

                    LogLine("[BUILDFIGHT] Stair rises at %.0f degrees when placed at %.0f -- stairs are placed with a rotation offset of %.0f from now on.",
                        (float)(BestDir * 90.0), (float)F.PlacedYaw, (float)Off);

                    GStairYawOffset = Off;
                }

                SetBuildDir(F, BestDir);
                F.StairCX = CX;
                F.StairCY = CY;
                F.ClimbX = CX + DX[BestDir] * 190.0;
                F.ClimbY = CY + DY[BestDir] * 190.0;
                F.ClimbFeet = Best;
                return true;
            };

        auto Abort = [&](const char* Why)
            {
                LogLine("[BUILDFIGHT] '%s' gave up its %s (step %d): %s.", SafeNameOf(PS).c_str(),
                    F.Mode == 1 ? "ramp rush" : (F.Mode == 3 ? "close fight" : "box"), F.Step, Why);
                StopBuildFight(Pawn);
            };

        const bool bTime = GStormTick >= F.NextActTick;

        // ============ steps shared by every mode: edit-peek loop (10-13) ============
        // ============ steps shared by every mode: edit-peek loop (10-13) ============
        if (F.Step >= 10)
        {
            Body.bHasGoal = false;

            const char* VulnWhy = "";
            const bool bVuln = BotTargetVulnerable(Target, &VulnWhy);

            switch (F.Step)
            {
            case 10:   // wait behind the wall for an opening, then open it
            {
                if (!bTime) break;

                if (F.PeekWaitTick < 0)
                    F.PeekWaitTick = GStormTick;

                // stay covered until the enemy is vulnerable (or 2.5 s pass)
                if (!bVuln && (GStormTick - F.PeekWaitTick) < 75)
                    break;

                F.PeekWaitTick = -1;

                // make sure the wall exists first
                if (!F.Front.Actor || !ActorIsLive((const UObject*)F.Front.Actor))
                    F.Front = Put(Set.Wall, F.Front.Loc.X, F.Front.Loc.Y, F.Front.Loc.Z, F.Front.Yaw);

                bool bOpened = false;

                if (EditClass)
                {
                    bOpened = EditPieceTo(F.Front, EditClass, Controller, PS);
                }
                else
                {
                    // no edit pieces on this build: fake the edit by taking the wall out for a moment
                    if (F.Front.Actor && ActorIsLive((const UObject*)F.Front.Actor))
                        F.Front.Actor->K2_DestroyActor();

                    F.Front.Actor = nullptr;
                    bOpened = true;
                }

                if (bOpened)
                {
                    GBuildEdits++;

                    if (GBuildEdits <= 15)
                        LogLine("[BUILDFIGHT] '%s' opens its wall (%s) -- enemy %s | %d edit(s) so far.", SafeNameOf(PS).c_str(),
                            EditClass ? "real edit piece" : "wall removed", bVuln ? VulnWhy : "not vulnerable, waited long enough", GBuildEdits);

                    F.Step = 11;
                    F.PeekHoldUntil = GStormTick + (bVuln ? 90 : 25);
                    F.NextActTick = GStormTick + (bVuln ? 25 : 22 + (rand() % 14));
                }
                else
                {
                    F.Step = 13;
                    F.StepTick = GStormTick;
                }
                break;
            }

            case 11:   // keep it open while the enemy stays vulnerable, then close it
                if (!bTime) break;

                if (bVuln && GStormTick < F.PeekHoldUntil)
                {
                    F.NextActTick = GStormTick + 5;
                    break;
                }

                if (EditClass)
                    EditPieceTo(F.Front, Set.Wall, Controller, PS);
                else
                    F.Front = Put(Set.Wall, F.Front.Loc.X, F.Front.Loc.Y, F.Front.Loc.Z, F.Front.Yaw);

                F.Peeks++;
                F.NextActTick = GStormTick + 12 + (rand() % 10);

                if (F.Peeks >= kPeeksPerFight)
                {
                    F.Step = 13;
                    F.StepTick = GStormTick;
                }
                else
                {
                    F.Step = 10;
                }
                break;

            default:   // 13: a moment behind cover, then climb higher or hold the high ground
                if ((GStormTick - F.StepTick) > 45)
                {
                    if (F.Mode == 2)
                    {
                        StopBuildFight(Pawn);
                        return false;
                    }

                    const FVector TL = Target->K2_GetActorLocation();
                    const double TargetFeet = TL.Z - Half;
                    const double OurFeet = Loc.Z - Half;

                    if (TargetFeet > OurFeet - 150.0 && F.Level < kMaxRampLevels)
                    {
                        // the enemy is level with us or higher: build more ramps
                        if (F.Front.Actor && ActorIsLive((const UObject*)F.Front.Actor))
                            F.Front.Actor->K2_DestroyActor();

                        F.Front = FBuiltPiece{};
                        F.Mode = 1;
                        F.Tile = FVector(F.StairCX, F.StairCY, 0.0);

                        int Need = F.Level + 2;
                        if (Need > kMaxRampLevels) Need = kMaxRampLevels;
                        F.TargetLevels = Need;

                        F.Step = 0;
                        F.NextActTick = GStormTick + 2;
                    }
                    else
                    {
                        // we are above them: stay up here and keep peeking until the fight ends
                        F.Peeks = 0;
                        F.Step = 10;
                        F.NextActTick = GStormTick + 10;
                    }
                }
                break;
            }

            return true;
        }

        // ============ RAMP RUSH: stair, wall on its high edge, climb, repeat ============
        if (F.Mode == 1)
        {
            switch (F.Step)
            {
            case 0:   // stair in the tile ahead
                if (!bTime) break;

                F.StairCX = F.Tile.X + F.DirX * (512.0 + kStairForwardShift);
                F.StairCY = F.Tile.Y + F.DirY * (512.0 + kStairForwardShift);
                F.PlacedYaw = F.DirYaw + GStairYawOffset;
                Put(Set.Stair, F.StairCX, F.StairCY, Z, F.PlacedYaw);

                F.Step = 1;
                F.NextActTick = GStormTick + kBuildStepTicks;
                break;

            case 1:   // find which way it rises, wall on the high edge
                if (!bTime) break;

                if (!ProbeStair(F.StairCX, F.StairCY, Z))
                {
                    Abort("could not tell which way the stair rises");
                    return false;
                }

                Put(Set.Wall, F.StairCX + F.DirX * 256.0, F.StairCY + F.DirY * 256.0, Z, F.DirYaw);

                {
                    // a wall on the side too, alternating left and right each level
                    const double SideYaw = F.DirYaw + ((F.Level & 1) ? -90.0 : 90.0);
                    const double SideRad = SideYaw * 3.14159265358979323846 / 180.0;
                    Put(Set.Wall, F.StairCX + cos(SideRad) * 256.0, F.StairCY + sin(SideRad) * 256.0, Z, SideYaw);
                }

                F.Step = 2;
                F.StepTick = GStormTick;
                break;

            case 2:   // climb the stair
                if (ClimbStairs(Pawn, Body, F, Loc, Half))
                {
                    F.Level++;
                    F.Tile = FVector(F.StairCX, F.StairCY, 0.0);
                    F.NextActTick = GStormTick + 2;
                    {
                        // the enemy may have built higher (or dropped): work the height out again
                        const FVector TL = Target->K2_GetActorLocation();
                        int Need = (int)ceil(((TL.Z - Half) - F.BaseZ) / 384.0) + 2;
                        if (Need < 3) Need = 3;
                        if (Need > kMaxRampLevels) Need = kMaxRampLevels;
                        F.TargetLevels = Need;
                    }

                    F.Step = (F.Level >= F.TargetLevels) ? 3 : 0;
                }
                else if ((GStormTick - F.StepTick) > 60)
                {
                    Abort("the climb took too long");
                    return false;
                }
                break;

            default:  // 3: top wall towards the enemy, then peek over it
                if (!bTime) break;

                F.Front = Put(Set.Wall, F.Tile.X + F.DirX * 256.0, F.Tile.Y + F.DirY * 256.0, Z, F.DirYaw);
                F.Peeks = 0;
                F.Step = 10;
                F.NextActTick = GStormTick + 8;
                break;
            }

            return true;
        }

        // ============ CLOSE FIGHT: wall, stair in own tile, climb, wall on top, peek ============
        if (F.Mode == 3)
        {
            switch (F.Step)
            {
            case 0:   // cover first
                F.Front = Put(Set.Wall, F.Tile.X + F.DirX * 256.0, F.Tile.Y + F.DirY * 256.0, Z, F.DirYaw);
                Body.bHasGoal = false;
                F.Step = 1;
                F.NextActTick = GStormTick + kBuildStepTicks;
                break;

            case 1:   // stair in its own tile
                if (!bTime) break;

                F.StairCX = F.Tile.X + F.DirX * kStairForwardShift;
                F.StairCY = F.Tile.Y + F.DirY * kStairForwardShift;
                F.PlacedYaw = F.DirYaw + GStairYawOffset;
                Put(Set.Stair, F.StairCX, F.StairCY, Z, F.PlacedYaw);

                F.Step = 2;
                F.NextActTick = GStormTick + kBuildStepTicks;
                break;

            case 2:   // probe, wall on top of the high edge
                if (!bTime) break;

                if (!ProbeStair(F.StairCX, F.StairCY, Z))
                {
                    Abort("could not tell which way the stair rises");
                    return false;
                }

                F.Front = Put(Set.Wall, F.StairCX + F.DirX * 256.0, F.StairCY + F.DirY * 256.0, Z + 384.0, F.DirYaw);
                F.Step = 3;
                F.StepTick = GStormTick;
                break;

            default:  // 3: climb, then keep ramping up (or peek if it is high enough)
                if (ClimbStairs(Pawn, Body, F, Loc, Half))
                {
                    F.Level++;

                    if (F.Level < F.TargetLevels)
                    {
                        // the top wall would sit on the next stair's low edge: take it out, then carry on as a ramp rush
                        if (F.Front.Actor && ActorIsLive((const UObject*)F.Front.Actor))
                            F.Front.Actor->K2_DestroyActor();

                        F.Front = FBuiltPiece{};
                        F.Mode = 1;
                        F.Tile = FVector(F.StairCX, F.StairCY, 0.0);
                        F.Step = 0;
                        F.NextActTick = GStormTick + 2;
                    }
                    else
                    {
                        F.Peeks = 0;
                        F.Step = 10;
                        F.NextActTick = GStormTick + 6;
                    }
                }
                else if ((GStormTick - F.StepTick) > 60)
                {
                    Abort("the climb took too long");
                    return false;
                }
                break;
            }

            return true;
        }

        // ============ BOX: 4 walls and a roof, heal, then peek out ============
        Body.bHasGoal = false;

        if (F.Step <= 3)
        {
            if (bTime)
            {
                static const double Turn[4] = { 0.0, 90.0, -90.0, 180.0 };
                const double Yaw = F.DirYaw + Turn[F.Step];
                const double Rad = Yaw * 3.14159265358979323846 / 180.0;

                auto Wall = Put(Set.Wall, F.Tile.X + cos(Rad) * 256.0, F.Tile.Y + sin(Rad) * 256.0, F.BaseZ, Yaw);

                if (F.Step == 0)
                    F.Front = Wall;   // the wall towards the enemy is the one that gets edited

                F.Step++;
                F.NextActTick = GStormTick + kBuildStepTicks;
            }
        }
        else if (F.Step == 4)
        {
            if (bTime)
            {
                Put(Set.Roof ? Set.Roof : Set.Floor, F.Tile.X, F.Tile.Y, F.BaseZ + 384.0, 0.0);
                F.Step = 5;
                F.StepTick = GStormTick;
            }
        }
        else
        {
            TickBotHealing(Controller, PS, Pawn);

            if ((GStormTick - F.StepTick) > 120)
            {
                F.Peeks = 0;
                F.Step = 10;
                F.NextActTick = GStormTick;
            }
        }

        return true;
    }

    static void TickMovement(UWorld* World, AFortGameStateAthena* GameState, AFortGameMode* GameMode)
    {
        int nativeBrainCount = 0;
        int fallbackCount = 0;

        static int reportCounter_forDir = 0;
        static std::map<void*, int> BotWanderDirTick;
        static std::map<void*, int> BotTargetSeenTick;

        std::set<void*> LivePawns;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto LiveState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!LiveState || !LiveState->HasOwner() || !LiveState->Owner)
                continue;

            auto LivePawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)LiveState->Owner)->Pawn;

            if (LivePawn && LivePawn->GetHealth() > 0.f)
                LivePawns.insert((void*)LivePawn);
        }

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PlayerState = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PlayerState || !PlayerState->HasbIsABot() || !PlayerState->bIsABot || NPCs::IsNPC(PlayerState))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PlayerState->Owner;
            if (!Controller)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn)
                continue;

            if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
            {
                auto Move = Pawn->CharacterMovement;

                if (Move->HasMaxWalkSpeed())
                {
                    // Always run when in the storm or fighting; otherwise a mix of running and walking.
                    const auto GaitTarget = BotCombatTarget.find(Pawn);
                    const bool bHurrying = (Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone) ||
                        (GaitTarget != BotCombatTarget.end() && GaitTarget->second);

                    Move->MaxWalkSpeed = BotGaitSpeed(Pawn, bHurrying, (float)UGameplayStatics::GetTimeSeconds(World));


                    static int SpeedLines = 0;
                    if (Move->HasVelocity() && (reportCounter_forDir % 300) == 0 && SpeedLines < 12)
                    {
                        const FVector V = Move->Velocity;
                        const double Flat = sqrt(V.X * V.X + V.Y * V.Y);
                        if (Flat > 5.0)
                        {
                            SpeedLines++;
                            LogLine("[SPEED] A bot is moving at %.0f units/s (MaxWalkSpeed %.0f, BotSprintSpeed %.0f).", (float)Flat, (float)Move->MaxWalkSpeed, (float)FConfiguration::BotSprintSpeed);
                        }
                    }
                }


                if (FConfiguration::bBotsFaceTheirDirection && Move->HasVelocity() &&
                    GlidingBots.find((void*)Pawn) == GlidingBots.end())
                {
                    const FVector Velocity = Move->Velocity;
                    const double SpeedX = Velocity.X;
                    const double SpeedY = Velocity.Y;
                    const double FlatSpeed = sqrt(SpeedX * SpeedX + SpeedY * SpeedY);

                    float Threshold = FConfiguration::BotFacingMinimumSpeed;
                    if (!(Threshold > 0.f))
                        Threshold = 40.f;

                    if (FlatSpeed > (double)Threshold)
                    {
                        const double TravelYaw = atan2(SpeedY, SpeedX) * (180.0 / 3.14159265358979323846);

                        auto Faced = BotFacingYaw.find(Pawn);
                        double Difference = (Faced != BotFacingYaw.end()) ? (TravelYaw - (double)Faced->second) : 999.0;

                        while (Difference > 180.0) Difference -= 360.0;
                        while (Difference < -180.0) Difference += 360.0;

                        if (fabs(Difference) > 4.0)
                        {
                            BotFacingYaw[Pawn] = (float)TravelYaw;

                            FRotator Heading{};
                            double HeadPitch = 0.0, HeadYaw = TravelYaw, HeadRoll = 0.0;
                            Heading.Pitch = HeadPitch;
                            Heading.Yaw = HeadYaw;
                            Heading.Roll = HeadRoll;

                            Pawn->K2_SetActorRotation(Heading, false);

                            ((::AAIController*)Controller)->SetControlRotation(Heading);
                        }
                    }
                }
            }

            auto AIController = (::AAIController*)Controller;
            {
                auto FireNow = BotFire.find(Pawn);
                const bool bShooting = FireNow != BotFire.end() && FireNow->second.bFiring;
                const bool bInAir = GlidingBots.find((void*)Pawn) != GlidingBots.end();

                if (!bInAir)
                    MakeBotSprint(Pawn, !bShooting && BotGaitWantsRun(Pawn), reportCounter_forDir);
            }
            SilenceNativeSenses(AIController);

            const bool bHasNativeBrain = AIController->HasBrainComponent() && AIController->BrainComponent;

            if (bHasNativeBrain)
            {
                nativeBrainCount++;

                if (!FConfiguration::bAllBotsFight)
                    continue;
            }
            else
            {
                fallbackCount++;
            }

            if (!bHasNativeBrain && BTRetried.find((void*)Controller) == BTRetried.end())
            {
                BTRetried.insert((void*)Controller);

                static UObject* PhoebeBT = nullptr;
                static bool bLookedUp = false;
                if (!bLookedUp)
                {
                    bLookedUp = true;
                    PhoebeBT = (UObject*)FindObject<UObject>(L"/Game/Athena/AI/Phoebe/BT_Phoebe.BT_Phoebe");
                    LogLine("[BotAI] BT_Phoebe asset lookup: %s\n", PhoebeBT ? "found" : "NOT FOUND");
                }

                if (PhoebeBT)
                {
                    bool started = AIController->RunBehaviorTree(PhoebeBT);
                    static int reported = 0;
                    if (reported < 3)
                    {
                        reported++;
                        LogLine("[BotAI] Manual RunBehaviorTree returned %s.\n", started ? "true" : "false");
                    }
                }
            }

            FVector MyLoc = Pawn->K2_GetActorLocation();

            FVector Direction;
            bool bOutsideSafeZone = Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone;
            const FStormPlan StormPlan = PlanStormRotation(GameMode, Pawn, MyLoc, bOutsideSafeZone,
                (float)UGameplayStatics::GetTimeSeconds(World));

            auto combatIt = BotCombatTarget.find(Pawn);
            AFortPlayerPawnAthena* Target = (combatIt != BotCombatTarget.end()) ? combatIt->second : nullptr;
            if (StormPlan.bRotate && Target)
            {
                Target = nullptr;
                BotCombatTarget[Pawn] = nullptr;
                BotLockedOnName.erase(Pawn);
                BotLastSaw.erase((void*)Pawn);
                BotLastSeenSpot.erase((void*)Pawn);
                BotStoppedForFight.erase((void*)Pawn);
            }

            const bool bCombatOpen = BotCombatIsOpen() && BotHasLanded((void*)Pawn);

            {
                static std::map<void*, int> WhyTick;
                int& LastWhy = WhyTick[(void*)Pawn];

                if (BotHasLanded((void*)Pawn) && (reportCounter_forDir - LastWhy) >= 150)
                {
                    LastWhy = reportCounter_forDir;

                    std::string HeldName = "nothing";
                    if (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon)
                    {
                        auto HeldWeapon = (AFortWeapon*)Pawn->CurrentWeapon;
                        if (HeldWeapon->HasWeaponData() && HeldWeapon->WeaponData)
                            HeldName = BotObjectName((const UObject*)HeldWeapon->WeaponData);
                    }

                    auto LootIt = BotLoot.find((void*)Controller);

                    LogLine("[WHY] '%s': combatOpen=%d busDeparted=%d gamePhase=%d hasAnyGun=%d holdingGun=%d held=%s target=%s rotating=%d lootingSomething=%d",
                        SafeNameOf(PlayerState).c_str(), (int)bCombatOpen, (int)bBusDeparted,
                        GameState->HasGamePhase() ? (int)GameState->GamePhase : -1,
                        (int)BotHasAnyGun(Controller), (int)BotHoldingGun(Pawn), HeldName.c_str(),
                        Target ? BotPawnName(Target).c_str() : "none", (int)StormPlan.bRotate,
                        (LootIt != BotLoot.end() && LootIt->second.Chest) ? 1 : 0);
                }
            }

            WatchBotTrigger(Pawn, PlayerState, reportCounter_forDir);

            UpdateBurstCheck(Pawn, PlayerState, LivePawns);

            if (!bCombatOpen)
            {
                if (BotHasLanded((void*)Pawn) && !StormPlan.bRotate)
                {
                    if (!TickBotLooting(World, Controller, PlayerState, Pawn, AIController, MyLoc))
                        RoamWhileWaiting(Pawn, AIController, MyLoc);
                }

                Target = nullptr;
                StopBuildFight(Pawn);
                BotCombatTarget[Pawn] = nullptr;
                BotLastSaw.erase((void*)Pawn);
                AggroedByHuman.erase((void*)Controller);
                BotUnderFireSince.erase((void*)Controller);

                BotStopAiming(AIController);
                BotIntendedAim.erase((void*)Pawn);
                BotShotAim.erase((void*)Pawn);

                auto FireIt = BotFire.find(Pawn);

                if (FireIt != BotFire.end())
                {
                    if (FireIt->second.bFiring || FireIt->second.BurstTicksLeft > 0)
                    {
                        BotTrigger(Pawn, false);
                        BotTriggerUpTick[(void*)Pawn] = reportCounter_forDir;
                        FireIt->second.bFiring = false;
                    }

                    FireIt->second.BurstTicksLeft = 0;
                    FireIt->second.ReactionTicksLeft = -1;
                    FireIt->second.CooldownTicksLeft = 0;
                }

                continue;
            }

            if (IsPlayerControllerBot((AActor*)Controller))
                LetTheServerRunBotAbilities(Controller, PlayerState);

            TickBotReload(Pawn, PlayerState, Controller, Target && LivePawns.find((void*)Target) != LivePawns.end(), reportCounter_forDir);

            if (Target && LivePawns.find((void*)Target) == LivePawns.end())
            {
                GLocksEnded++;

                auto Locked = BotLockedOnName.find(Pawn);

                if (GLocksEnded <= 10)
                    LogLine("[LOCK] %s's target '%s' is down -- free to lock onto somebody new.", SafeNameOf(PlayerState).c_str(),
                        Locked != BotLockedOnName.end() ? Locked->second.c_str() : "?");

                if (Locked != BotLockedOnName.end())
                    BotLockedOnName.erase(Locked);

                Target = nullptr;
                BotCombatTarget[Pawn] = nullptr;
                BotLastSaw.erase((void*)Pawn);
                BotLastSeenSpot.erase((void*)Pawn);
                BotIntendedAim.erase((void*)Pawn);
                BotShotAim.erase((void*)Pawn);
                BotGroundCheck.erase((void*)Pawn);
                BotStoppedForFight.erase((void*)Pawn);
            }

            // No lock-on: a target is only kept while the bot can see them and they are within sight range.
 // EXCEPT someone who recently shot this bot: it keeps fighting them even if they build.
            if (Target)
            {
                const FVector HeldLoc = Target->K2_GetActorLocation();

                const double HeldDx = HeldLoc.X - MyLoc.X;
                const double HeldDy = HeldLoc.Y - MyLoc.Y;
                const bool bInSight = (HeldDx * HeldDx + HeldDy * HeldDy) <= (kBotSightRange * kBotSightRange);

                const bool bSeesNow = bInSight &&
                    BotCanSee(AIController, Pawn, MyLoc, Target, (AActor*)Target, reportCounter_forDir, true) &&
                    !BotGroundInTheWay(Pawn, MyLoc, Target, HeldLoc);

                if (bSeesNow)
                    BotTargetSeenTick[(void*)Pawn] = reportCounter_forDir;

                const bool bShotByTarget = BotWasShotBy((void*)Controller, Target);

                auto SeenIt = BotTargetSeenTick.find((void*)Pawn);
                const bool bGraceOver = (SeenIt == BotTargetSeenTick.end()) || ((reportCounter_forDir - SeenIt->second) > 15);

                if (!bSeesNow && !bShotByTarget && bGraceOver)
                {
                    BotLockedOnName.erase(Pawn);
                    Target = nullptr;
                    BotCombatTarget[Pawn] = nullptr;
                    BotLastSaw.erase((void*)Pawn);
                    BotLastSeenSpot.erase((void*)Pawn);
                    BotIntendedAim.erase((void*)Pawn);
                    BotShotAim.erase((void*)Pawn);
                    BotGroundCheck.erase((void*)Pawn);
                    BotStoppedForFight.erase((void*)Pawn);
                    AggroedByHuman.erase((void*)Controller);
                    BotTargetSeenTick.erase((void*)Pawn);
                }
            }

            if (!Target && !StormPlan.bRotate && BotHasAnyGun(Controller))
            {
                AFortPlayerPawnAthena* Best = nullptr;

                const double BotRange = (FConfiguration::BotEngageRange > 0.f) ? (double)FConfiguration::BotEngageRange : 12000.0;
                const double HuntRange = (FConfiguration::BotHuntPlayerRange > 0.f) ? (double)FConfiguration::BotHuntPlayerRange : 24000.0;
                const double Preference = (FConfiguration::BotPlayerPreference > 0.f) ? (double)FConfiguration::BotPlayerPreference : 0.35;

                double bestScore = -1.0;

                for (int e = 0; e < GameState->PlayerArray.Num(); e++)
                {
                    auto OtherState = (AFortPlayerStateAthena*)GameState->PlayerArray[e];
                    if (!OtherState || OtherState == PlayerState)
                        continue;
                    auto OtherController = (AFortPlayerControllerAthena*)OtherState->Owner;
                    if (!OtherController)
                        continue;
                    auto OtherPawn = (AFortPlayerPawnAthena*)OtherController->Pawn;
                    if (!OtherPawn || OtherPawn->GetHealth() <= 0.0f)
                        continue;

                    const bool bIsHuman = OtherState->HasbIsABot() && !OtherState->bIsABot && !NPCs::IsNPC(OtherState);

                    if (!bIsHuman && !FConfiguration::bBotsFightEachOther)
                        continue;

                    if (bIsHuman && !FConfiguration::bBotsFightRealPlayers)
                        continue;

                    FVector OLoc = OtherPawn->K2_GetActorLocation();
                    const double odx = OLoc.X - MyLoc.X;
                    const double ody = OLoc.Y - MyLoc.Y;
                    const double Distance = sqrt(odx * odx + ody * ody);

                    const double kBotOnBotMaxRange = (FConfiguration::BotShootRealPlayerRange > 0.f)
                        ? (double)FConfiguration::BotShootRealPlayerRange : 5000.0;
                    double MaxRange = bIsHuman
                        ? (double)FConfiguration::BotShootRealPlayerRange
                        : kBotOnBotMaxRange;
                    if (bIsHuman && (MaxRange <= 0.0 || MaxRange > HuntRange))
                        MaxRange = HuntRange;

                    if (Distance > MaxRange || Distance > kBotSightRange)
                        continue;

                    if (!BotCanSee(AIController, Pawn, MyLoc, OtherPawn, (AActor*)OtherPawn, reportCounter_forDir, bIsHuman))
                        continue;

                    if (BotGroundInTheWay(Pawn, MyLoc, OtherPawn, OLoc))
                        continue;

                    const double Score = bIsHuman ? (Distance * Preference) : Distance;

                    if (bestScore < 0.0 || Score < bestScore)
                    {
                        bestScore = Score;
                        Best = OtherPawn;
                    }
                }


                {
                    auto Agg = AggroedByHuman.find((void*)Controller);
                    if (Agg != AggroedByHuman.end() && Agg->second.first &&
                        (GStormTick - Agg->second.second) <= kGrudgeTicks)
                    {
                        auto AggState = (AFortPlayerStateAthena*)Agg->second.first;
                        if (AggState && AggState->HasOwner() && AggState->Owner)
                        {
                            auto AggCtrl = (AFortPlayerControllerAthena*)AggState->Owner;
                            auto AggPawn = (AFortPlayerPawnAthena*)AggCtrl->Pawn;
                            if (AggPawn && AggPawn->GetHealth() > 0.f)
                                Best = AggPawn;
                        }
                        else
                            AggroedByHuman.erase((void*)Controller);
                    }
                }

                Target = Best;
                BotCombatTarget[Pawn] = Target;
                if (Target)
                    BotTargetSeenTick[Pawn] = reportCounter_forDir;


                if (Target)
                {
                    GLocksMade++;

                    bool bLockedOnHuman = false;
                    if (auto LockController = (AFortPlayerControllerAthena*)Target->Controller)
                        if (auto LockState = (AFortPlayerStateAthena*)LockController->PlayerState)
                            bLockedOnHuman = LockState->HasbIsABot() && !LockState->bIsABot && !NPCs::IsNPC(LockState);

                    const std::string TargetName = BotPawnName(Target);
                    BotLockedOnName[Pawn] = TargetName;

                    const FVector LockLoc = Target->K2_GetActorLocation();
                    const double LockDx = LockLoc.X - MyLoc.X;
                    const double LockDy = LockLoc.Y - MyLoc.Y;

                    static int LockReports = 0;
                    if (bLockedOnHuman || ++LockReports <= 10)
                        LogLine("[LOCK] '%s' locked onto '%s' (%s) %.0f m away -- it fights only them now, until one of them dies.",
                            SafeNameOf(PlayerState).c_str(), TargetName.c_str(), bLockedOnHuman ? "a real player" : "a bot",
                            (float)(sqrt(LockDx * LockDx + LockDy * LockDy) / 100.0));
                }
            }

            bool bInCombat = false;
            bool bLongRangeAggro = false;

            double EngageRange = (FConfiguration::BotEngageRange > 0.f) ? (double)FConfiguration::BotEngageRange : 12000.0;
            if (Target)
            {
                if (auto TgtController = (AFortPlayerControllerAthena*)Target->Controller)
                    if (auto TgtState = (AFortPlayerStateAthena*)TgtController->PlayerState)
                        if (TgtState->HasbIsABot() && TgtState->bIsABot)
                            EngageRange = (FConfiguration::BotShootRealPlayerRange > 0.f)
                            ? (double)FConfiguration::BotShootRealPlayerRange : 5000.0;
            }
            const double PreferredRange = 600.0;   // it closes in to 6 m before it stops walking

            const bool bMayFight = FConfiguration::bBotCombatEnabled && bCombatOpen && !StormPlan.bRotate;

            bool bTargetIsHuman = false;
            if (Target)
            {
                if (auto TgtController = (AFortPlayerControllerAthena*)Target->Controller)
                    if (auto TgtState = (AFortPlayerStateAthena*)TgtController->PlayerState)
                        bTargetIsHuman = TgtState->HasbIsABot() && !TgtState->bIsABot && !NPCs::IsNPC(TgtState);
            }

            bool bHumanTooFar = false;
            if (Target && bTargetIsHuman)
            {
                FVector TgtLoc = Target->K2_GetActorLocation();
                const double hx = TgtLoc.X - MyLoc.X;
                const double hy = TgtLoc.Y - MyLoc.Y;
                const double Dist = sqrt(hx * hx + hy * hy);
                bHumanTooFar = Dist > (double)FConfiguration::BotShootRealPlayerRange;

                if (bHumanTooFar)
                {
                    auto Agg = AggroedByHuman.find((void*)Controller);
                    if (Agg != AggroedByHuman.end() && Agg->second.first &&
                        (GStormTick - Agg->second.second) <= kGrudgeTicks)
                    {
                        auto AggState = (AFortPlayerStateAthena*)Agg->second.first;
                        if (AggState)
                        {
                            if (auto TgtController = (AFortPlayerControllerAthena*)Target->Controller)
                                if (TgtController->PlayerState == AggState)
                                {
                                    bHumanTooFar = false;
                                    bLongRangeAggro = true;
                                }
                        }
                    }
                }
            }

            if (bMayFight && Target && Target->GetHealth() > 0.0f)
            {
                FVector TLoc = Target->K2_GetActorLocation();

                const double tdx = TLoc.X - MyLoc.X;
                const double tdy = TLoc.Y - MyLoc.Y;
                const double tdz = (TLoc.Z + 20.0) - (MyLoc.Z + 60.0);   // from the eyes to the upper chest
                const double Flat = sqrt(tdx * tdx + tdy * tdy);

                const bool bGrudge = BotWasShotBy((void*)Controller, Target);

                const double EffectiveEngage = (bLongRangeAggro || bGrudge)
                    ? ((FConfiguration::BotHuntPlayerRange > 0.f) ? (double)FConfiguration::BotHuntPlayerRange : 50000.0)
                    : EngageRange;

                {
                    bInCombat = true;

                    const bool bInShootRange = !bHumanTooFar && Flat < EffectiveEngage;
                    const bool bGameSaysVisible = BotHasLineOfSight(AIController, (AActor*)Target);

                    bool bGroundInTheWay = false;

                    if (bGameSaysVisible)
                    {
                        auto Checked = BotGroundCheck.find((void*)Pawn);

                        if (Checked == BotGroundCheck.end() || (reportCounter_forDir - Checked->second.first) >= 6)
                        {
                            std::string Blocker;
                            const bool bBlocked = BotGroundInTheWay(Pawn, MyLoc, Target, TLoc, &Blocker);

                            BotGroundCheck[(void*)Pawn] = { reportCounter_forDir, bBlocked };

                            if (bBlocked)
                            {
                                static int GroundReports = 0;

                                if (++GroundReports <= 8)
                                    LogLine("[EYES] The game's check said '%s' can see '%s' (%.0f m), but the ground (%s) is in "
                                        "the way -- no shot.",
                                        SafeNameOf(PlayerState).c_str(), BotPawnName(Target).c_str(),
                                        (float)(Flat / 100.0), Blocker.c_str());
                            }
                        }

                        bGroundInTheWay = BotGroundCheck[(void*)Pawn].second;

                        if (bGroundInTheWay)
                            GTerrainBlocks++;
                    }
                    else if (bInShootRange)
                    {

                        static int BlockedSamples = 0;

                        if (BlockedSamples < 6 && BotTraceAvailable())
                        {
                            BlockedSamples++;

                            FVector Eyes = MyLoc;
                            Eyes.Z = MyLoc.Z + 60.0;

                            FVector Chest = TLoc;
                            Chest.Z = TLoc.Z + 30.0;

                            const FBotHit Hit = BotTrace((AActor*)Pawn, Eyes, Chest, 0);

                            LogLine("[EYES] Sample: '%s' cannot see '%s' (%.0f m) -- a straight line to their chest "
                                "hits %s (%s).",
                                SafeNameOf(PlayerState).c_str(), BotPawnName(Target).c_str(), (float)(Flat / 100.0),
                                Hit.bHit ? BotObjectName(Hit.Actor).c_str() : "nothing",
                                Hit.bHit ? BotClassName(Hit.Actor).c_str() : "-");
                        }
                    }

                    const bool bTargetVisible = bGameSaysVisible && !bGroundInTheWay;
                    // as long as it can see its attacker, the grudge never runs out
                    if (bGrudge && bTargetVisible)
                        AggroedByHuman[(void*)Controller].second = GStormTick;
                    const bool bCanShoot = bInShootRange && bTargetVisible;
                    SwitchGunForRange(Pawn, Controller, PlayerState, sqrt(Flat * Flat + tdz * tdz), GStormTick);

                    if (bTargetVisible)
                    {
                        BotLastSeenSpot[(void*)Pawn] = TLoc;
                        BotTargetSeenTick[(void*)Pawn] = reportCounter_forDir;
                    }

                    const double Yaw = atan2(tdy, tdx) * (180.0 / 3.14159265358979323846);
                    const double Pitch = atan2(tdz, Flat > 1.0 ? Flat : 1.0) * (180.0 / 3.14159265358979323846);

                    {
                        auto Was = BotIntendedAim.find((void*)Pawn);
                        auto FireNow = BotFire.find((void*)Pawn);

                        if (Was != BotIntendedAim.end() && FireNow != BotFire.end() && FireNow->second.bFiring && !GAimReadFaulted)
                        {
                            double ActualYaw = 0.0;

                            if (BotReadAimGuarded(AIController, &ActualYaw))
                            {
                                double Off = ActualYaw - Was->second.Yaw;
                                while (Off > 180.0) Off -= 360.0;
                                while (Off < -180.0) Off += 360.0;
                                Off = fabs(Off);

                                GAimChecks++;

                                if (Off <= 2.0)
                                    GAimHeld++;

                                GAimOffSum += Off;

                                if (Off > GAimOffMax)
                                    GAimOffMax = Off;
                            }
                        }
                    }

                    bool bAimSettled = false;

                    if (bTargetVisible)
                    {
                        const double RadToDeg = 180.0 / 3.14159265358979323846;
                        const double AimDist = sqrt(Flat * Flat + tdz * tdz);
                        const double SafeDist = AimDist > 1.0 ? AimDist : 1.0;
                        const double HalfWide = atan2(45.0, SafeDist) * RadToDeg;
                        const double HalfTall = atan2(85.0, SafeDist) * RadToDeg;

                        auto Norm = [](double A) { while (A > 180.0) A -= 360.0; while (A < -180.0) A += 360.0; return A; };
                        auto Rand = []() { return ((double)(rand() % 2001) / 1000.0) - 1.0; };   // -1..1

                        FBotAimState& Aim = BotAim[(void*)Pawn];

                        // ---- new target: the bot has to notice them, then turn ----
                        if (!Aim.bInit || Aim.Target != (void*)Target)
                        {
                            const FRotator Now = AIController->GetControlRotation();
                            Aim.Yaw = Norm((double)Now.Yaw);
                            Aim.Pitch = Norm((double)Now.Pitch);
                            Aim.Target = (void*)Target;
                            Aim.bInit = true;
                            Aim.Seed = BotPersonalSeed((void*)Pawn);
                            Aim.NextOffsetTick = 0;

                            // the bot first "sees" them where they are now
                            Aim.SeenX = TLoc.X;
                            Aim.SeenY = TLoc.Y;
                            Aim.SeenZ = TLoc.Z;

                            // reaction time: a bit different for every bot and every spot
                            int React = FConfiguration::BotAimReactionTicks;
                            if (React < 0) React = 0;
                            Aim.ReactUntilTick = reportCounter_forDir + React + (int)(Aim.Seed % 8) + (rand() % 6);

                            // overshoot: the first swing goes a bit past them, then corrects
                            const double TrueYawNow = Yaw;
                            const double Turn = Norm(TrueYawNow - Aim.Yaw);
                            double Over = fabs(Turn) * (double)FConfiguration::BotAimOvershoot;
                            if (Over > 12.0) Over = 12.0;
                            Aim.OffYaw = (Turn >= 0.0) ? Over : -Over;
                            Aim.OffPitch = 0.0;
                            Aim.WantOffYaw = 0.0;
                            Aim.WantOffPitch = 0.0;
                        }

                        // ---- where the bot thinks the target is: catches up to the real spot, never instantly ----
                        double Lag = (double)FConfiguration::BotAimTrackingLag;
                        if (Lag < 0.05) Lag = 0.05;
                        if (Lag > 1.0) Lag = 1.0;

                        Aim.SeenX += (TLoc.X - Aim.SeenX) * Lag;
                        Aim.SeenY += (TLoc.Y - Aim.SeenY) * Lag;
                        Aim.SeenZ += (TLoc.Z - Aim.SeenZ) * Lag;

                        const double Sx = Aim.SeenX - MyLoc.X;
                        const double Sy = Aim.SeenY - MyLoc.Y;
                        const double Sz = (Aim.SeenZ + 20.0) - (MyLoc.Z + 60.0);
                        const double SeenFlat = sqrt(Sx * Sx + Sy * Sy);
                        const double SeenYaw = atan2(Sy, Sx) * RadToDeg;
                        const double SeenPitch = atan2(Sz, SeenFlat > 1.0 ? SeenFlat : 1.0) * RadToDeg;

                        // ---- how shaky the bot is right now ----
                        double Hit = (double)FConfiguration::BotAimHitChance;
                        if (Hit < 0.05) Hit = 0.05;
                        if (Hit > 0.99) Hit = 0.99;

                        double Far = AimDist / 20000.0;
                        if (Far > 0.5) Far = 0.5;

                        double MovingFactor = 0.0;
                        if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasVelocity())
                        {
                            const FVector V = Pawn->CharacterMovement->Velocity;
                            MovingFactor = sqrt(V.X * V.X + V.Y * V.Y) / 550.0;
                            if (MovingFactor > 1.0) MovingFactor = 1.0;
                        }

                        const double Spread = (0.5 + (1.0 - Hit) * 3.0) * (1.0 + Far) *
                            (1.0 + MovingFactor * (double)FConfiguration::BotAimMovingPenalty);

                        // every 12-30 ticks the bot drifts towards a new "slightly off" point
                        if (reportCounter_forDir >= Aim.NextOffsetTick)
                        {
                            Aim.NextOffsetTick = reportCounter_forDir + 12 + (rand() % 19);
                            Aim.WantOffYaw = Rand() * HalfWide * Spread;
                            Aim.WantOffPitch = Rand() * HalfTall * Spread * 0.6;
                        }

                        // the offset eases there instead of jumping
                        Aim.OffYaw += (Aim.WantOffYaw - Aim.OffYaw) * 0.12;
                        Aim.OffPitch += (Aim.WantOffPitch - Aim.OffPitch) * 0.12;

                        // constant small hand shake (two slow waves, different for each bot)
                        const double T = (double)reportCounter_forDir;
                        const double Ph = (double)(Aim.Seed % 1000) * 0.01;
                        const double Shake = (double)FConfiguration::BotAimJitterDegrees * (1.0 + MovingFactor);
                        const double JitYaw = (sin(T * 0.37 + Ph) + 0.5 * sin(T * 0.91 + Ph * 2.0)) * Shake;
                        const double JitPitch = (sin(T * 0.43 + Ph * 3.0) + 0.5 * sin(T * 1.07 + Ph)) * Shake * 0.6;

                        // ---- turning: smooth, speed limited, and nothing happens until the reaction time is over ----
                        const bool bReacting = reportCounter_forDir < Aim.ReactUntilTick;

                        if (!bReacting)
                        {
                            const double WantYaw = SeenYaw + Aim.OffYaw + JitYaw;
                            const double WantPitch = SeenPitch + Aim.OffPitch + JitPitch;

                            double MaxTurn = (double)FConfiguration::BotAimTurnSpeed;
                            if (MaxTurn < 1.0) MaxTurn = 1.0;

                            auto Step = [](double D, double Max)
                                {
                                    // eased: big gaps close fast, small ones slowly, with a tiny floor so it never crawls
                                    double S = D * 0.25;

                                    if (fabs(D) < 0.4)
                                        S = D;
                                    else if (fabs(S) < 0.4)
                                        S = (D > 0.0) ? 0.4 : -0.4;

                                    if (S > Max) S = Max;
                                    if (S < -Max) S = -Max;
                                    return S;
                                };

                            Aim.Yaw = Norm(Aim.Yaw + Step(Norm(WantYaw - Aim.Yaw), MaxTurn));
                            Aim.Pitch = Aim.Pitch + Step(WantPitch - Aim.Pitch, MaxTurn * 0.7);

                            if (Aim.Pitch > 89.0) Aim.Pitch = 89.0;
                            if (Aim.Pitch < -89.0) Aim.Pitch = -89.0;
                        }

                        // on target = where the gun REALLY points is inside the body at their REAL position
                        // (the lag is what makes a bot miss a strafing player)
                        const double ErrYaw = fabs(Norm(Aim.Yaw - Yaw));
                        const double ErrPitch = fabs(Aim.Pitch - Pitch);
                        const bool bOnTarget = !bReacting && ErrYaw <= HalfWide && ErrPitch <= HalfTall;

                        bAimSettled = !bReacting && ErrYaw <= HalfWide * 2.5 + 3.0 && ErrPitch <= HalfTall * 2.5 + 3.0;

                        if (!bOnTarget)
                            GAimDeliberateMisses++;

                        FRotator AimRot{};
                        double AimPitch = Aim.Pitch, AimYaw = Aim.Yaw, AimRoll = 0.0;
                        AimRot.Pitch = AimPitch;
                        AimRot.Yaw = AimYaw;
                        AimRot.Roll = AimRoll;

                        AIController->SetControlRotation(AimRot);

                        if (IsPlayerControllerBot((AActor*)Controller))
                            KeepBotCameraOnItsEyes(Controller, Pawn, AimRot);

                        FRotator Face{};
                        double FacePitch = 0.0, FaceYaw = Aim.Yaw, FaceRoll = 0.0;
                        Face.Pitch = FacePitch;
                        Face.Yaw = FaceYaw;
                        Face.Roll = FaceRoll;
                        Pawn->K2_SetActorRotation(Face, false);

                        BotAimAt(AIController, nullptr, MyLoc, AimYaw, AimPitch, AimDist);

                        FBotAimRecord AimRecord{};
                        AimRecord.Yaw = AimYaw;
                        AimRecord.Pitch = AimPitch;
                        BotIntendedAim[(void*)Pawn] = AimRecord;

                        FBotShotAim& ShotAim = BotShotAim[(void*)Pawn];
                        ShotAim.Target = (void*)Target;
                        ShotAim.bOnTarget = bOnTarget;
                        ShotAim.bCanShoot = bCanShoot;
                        ShotAim.Distance = (float)AimDist;
                    }
                    else
                    {

                        BotStopAiming(AIController);
                        BotIntendedAim.erase((void*)Pawn);
                        BotShotAim.erase((void*)Pawn);
                    }

                    const bool bBuilding = TickBotBuildFight(Controller, PlayerState, Pawn, Target, MyLoc, Flat, bTargetVisible);

                    if (bBuilding)
                    {
                        BotStoppedForFight.erase((void*)Pawn);   // movement is being handled by the build fight
                    }
                    else if (Flat > PreferredRange || !bTargetVisible)
                    {
                        BotStoppedForFight.erase((void*)Pawn);

                        FVector Goal = TLoc;

                        if (!bTargetVisible)
                        {
                            auto Spot = BotLastSeenSpot.find((void*)Pawn);

                            if (Spot != BotLastSeenSpot.end())
                            {
                                const double SpotX = Spot->second.X - MyLoc.X;
                                const double SpotY = Spot->second.Y - MyLoc.Y;

                                if ((SpotX * SpotX + SpotY * SpotY) > (500.0 * 500.0))
                                    Goal = Spot->second;
                                else
                                    BotLastSeenSpot.erase(Spot);
                            }
                        }

                        // Can't see them and they didn't shoot us: don't walk to where they really are.
                        if (!bTargetVisible && Goal.X == TLoc.X && Goal.Y == TLoc.Y && !BotWasShotBy((void*)Controller, Target))
                            Goal = MyLoc;

                        const double GoalDx = Goal.X - MyLoc.X;
                        const double GoalDy = Goal.Y - MyLoc.Y;
                        const double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                        auto ct = BotCombatTick.find(Pawn);
                        if (ct == BotCombatTick.end() || (reportCounter_forDir - ct->second) > 12)
                        {
                            BotCombatTick[Pawn] = reportCounter_forDir;

                            BotMoveTo(AIController, Pawn, Goal, 400.f);
                        }

                        static std::map<void*, FVector> ChaseOrigin;
                        static std::map<void*, int> ChaseCheckTick;

                        auto ChaseTick = ChaseCheckTick.find(Pawn);

                        if (ChaseTick == ChaseCheckTick.end() || (reportCounter_forDir - ChaseTick->second) >= 30)
                        {
                            auto ChaseFrom = ChaseOrigin.find(Pawn);

                            if (ChaseTick != ChaseCheckTick.end() && ChaseFrom != ChaseOrigin.end() && GoalLen > 1.0)
                            {
                                const double MovedX = MyLoc.X - ChaseFrom->second.X;
                                const double MovedY = MyLoc.Y - ChaseFrom->second.Y;

                                if ((MovedX * MovedX + MovedY * MovedY) < (150.0 * 150.0) &&
                                    !BotLedgeAhead(Pawn, MyLoc, GoalDx, GoalDy))
                                {
                                    FVector Push{};
                                    Push.X = GoalDx / GoalLen;
                                    Push.Y = GoalDy / GoalLen;
                                    Push.Z = 0.0;
                                    Pawn->AddMovementInput(Push, 1.f, false);

                                    FVector Hopup{};
                                    Hopup.X = Push.X * 120.0;
                                    Hopup.Y = Push.Y * 120.0;
                                    Hopup.Z = 520.0;
                                    Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);

                                    GChaseHops++;
                                }
                            }

                            ChaseOrigin[Pawn] = MyLoc;
                            ChaseCheckTick[Pawn] = reportCounter_forDir;
                        }
                    }
                    else if (!GStopFaulted && BotStoppedForFight.insert((void*)Pawn).second)
                    {

                        BotStopWalkingGuarded(AIController);
                    }

                    FBotFireState& Fire = BotFire[Pawn];

                    if (!bCanShoot)
                    {

                        if (Fire.bFiring)
                        {
                            BotTrigger(Pawn, false);
                            BotTriggerUpTick[(void*)Pawn] = reportCounter_forDir;

                            if (bInShootRange && !bTargetVisible)
                                GBurstsCutOutOfSight++;
                        }

                        Fire.bFiring = false;
                        Fire.BurstTicksLeft = 0;
                        Fire.ReactionTicksLeft = -1;

                        if (bInShootRange && !bTargetVisible)
                            GTicksHeldFireOutOfSight++;
                    }
                    else if (Fire.ReactionTicksLeft < 0)
                    {
                        Fire.ReactionTicksLeft = 3 + (rand() % 8);
                    }

                    if (BotIsReloading(Pawn))
                    {

                        if (Fire.bFiring)
                        {
                            BotTrigger(Pawn, false);
                            BotTriggerUpTick[(void*)Pawn] = reportCounter_forDir;
                        }

                        Fire.bFiring = false;
                        Fire.BurstTicksLeft = 0;
                        Fire.RoundsSinceReload = 0;

                        if (Fire.CooldownTicksLeft < 4)
                            Fire.CooldownTicksLeft = 4 + (rand() % 8);
                    }
                    else if (!bCanShoot)
                    {

                    }
                    else if (Fire.ReactionTicksLeft > 0)
                    {
                        Fire.ReactionTicksLeft--;
                    }
                    else if (Fire.BurstTicksLeft > 0)
                    {
                        Fire.BurstTicksLeft--;

                        if (Fire.BurstTicksLeft == 0)
                        {
                            BotTrigger(Pawn, false);
                            BotTriggerUpTick[(void*)Pawn] = reportCounter_forDir;
                            Fire.bFiring = false;
                            Fire.CooldownTicksLeft = 4 + (rand() % 10);
                        }
                    }
                    else if (Fire.CooldownTicksLeft > 0)
                    {
                        Fire.CooldownTicksLeft--;
                    }
                    else if (!bAimSettled)
                    {
                        // still swinging onto the target: wait, the burst starts once the aim is close
                    }
                    else
                    {
                        if (BotHoldingGun(Pawn))
                        {
                            BotTrigger(Pawn, true);
                            Fire.bFiring = true;
                            Fire.BurstTicksLeft = 12 + (rand() % 12);

                            Fire.RoundsSinceReload += 1 + (Fire.BurstTicksLeft / 5);

                            GCombatBurstsFired++;

                            StartBurstCheck(Pawn, Controller, Target, (float)(Flat / 100.0));

                            if (auto TargetController = (AActor*)Target->Controller)
                                ShotAtBy[(void*)TargetController] = { (void*)PlayerState, GStormTick };
                        }
                        else
                        {
                            GCombatNoWeapon++;

                            Fire.CooldownTicksLeft = 60;

                            auto WeaponIt = BotWeapons.find((void*)PlayerState);
                            if (WeaponIt != BotWeapons.end() && WeaponIt->second)
                            {
                                FGuid ReEquipGuid{};
                                bool bFoundEntry = false;

                                auto InvIt = BotInventories.find((void*)PlayerState);
                                auto Inventory = (InvIt != BotInventories.end()) ? InvIt->second : nullptr;

                                if (Inventory && Inventory->HasInventory())
                                {
                                    for (int q = 0; q < Inventory->Inventory.ItemInstances.Num(); q++)
                                    {
                                        auto Item = Inventory->Inventory.ItemInstances[q];
                                        if (!Item || !Item->HasItemEntry())
                                            continue;
                                        if (!FFortItemEntry::HasItemDefinition() || Item->ItemEntry.ItemDefinition != WeaponIt->second)
                                            continue;
                                        if (FFortItemEntry::HasItemGuid())
                                            ReEquipGuid = Item->ItemEntry.ItemGuid;
                                        bFoundEntry = true;
                                        break;
                                    }

                                    if (!bFoundEntry)
                                    {
                                        int Clip = BotMagazineSize((void*)PlayerState);

                                        if (auto Fresh = Inventory->GiveItem(WeaponIt->second, 1, Clip))
                                        {
                                            if (Fresh->HasItemEntry() && FFortItemEntry::HasItemGuid())
                                                ReEquipGuid = Fresh->ItemEntry.ItemGuid;

                                            GWeaponsReissued++;
                                        }
                                    }
                                }

                                Pawn->EquipWeaponDefinition(WeaponIt->second, ReEquipGuid);
                            }
                        }
                    }

                    GCombatEngagements++;
                }
            }

            if (!bInCombat)
            {
                StopBuildFight(Pawn);
                auto FireIt = BotFire.find(Pawn);
                if (FireIt != BotFire.end())
                {
                    if (FireIt->second.bFiring)
                    {
                        BotTrigger(Pawn, false);
                        BotTriggerUpTick[(void*)Pawn] = reportCounter_forDir;
                    }

                    BotFire.erase(FireIt);
                }

                BotStopAiming(AIController);
                BotIntendedAim.erase((void*)Pawn);
                BotShotAim.erase((void*)Pawn);
                BotStoppedForFight.erase((void*)Pawn);
                BotLastSeenSpot.erase((void*)Pawn);
            }

            if (StormPlan.bRotate)
            {
                if (bOutsideSafeZone)
                    GZoneBotsInStorm++;

                {
                    auto WasAt = BotLastZoneDistance.find(Pawn);
                    if (WasAt != BotLastZoneDistance.end() && bOutsideSafeZone)
                    {
                        if (StormPlan.DistToGoal < WasAt->second - 1.0)
                            GZoneBotsClosing++;
                        else if (StormPlan.DistToGoal < WasAt->second + 1.0)
                            GZoneBotsStuck++;
                    }
                    BotLastZoneDistance[Pawn] = (float)StormPlan.DistToGoal;
                }

                FVector Goal = StormPlan.Goal;

                FVector Hop = Goal;

                {
                    bool bReuse = false;
                    auto Cached = BotStormHop.find(Pawn);

                    if (Cached != BotStormHop.end())
                    {
                        const double GoalShift = fabs(Cached->second.second.X - Goal.X) + fabs(Cached->second.second.Y - Goal.Y);
                        const double Hx = Cached->second.first.X - MyLoc.X;
                        const double Hy = Cached->second.first.Y - MyLoc.Y;

                        if (GoalShift < 100.0 && sqrt(Hx * Hx + Hy * Hy) > 1500.0)
                        {
                            Hop = Cached->second.first;
                            bReuse = true;
                        }
                    }

                    if (!bReuse)
                    {
                        Hop = PickWalkableHopToward(Pawn, MyLoc, Goal);
                        BotStormHop[Pawn] = { Hop, Goal };
                    }
                }

                auto it = BotWanderGoals.find(Pawn);
                auto zt = BotZoneMoveTick.find(Pawn);

                const bool bGoalMoved = (it == BotWanderGoals.end()) ||
                    (fabs(it->second.X - Hop.X) > 100.0) || (fabs(it->second.Y - Hop.Y) > 100.0);
                const bool bDueAgain = (zt == BotZoneMoveTick.end()) || ((reportCounter_forDir - zt->second) > 30);

                if (bGoalMoved || bDueAgain)
                {
                    BotWanderGoals[Pawn] = Hop;
                    BotZoneMoveTick[Pawn] = reportCounter_forDir;

                    BotMoveTo(AIController, Pawn, Hop, 100.f);

                    double GoalDx = Goal.X - MyLoc.X;
                    double GoalDy = Goal.Y - MyLoc.Y;
                    double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                    if (GoalLen > 1.0)
                    {
                        FRotator Look{};
                        Look.Pitch = 0.0;
                        Look.Yaw = (double)(atan2f((float)GoalDy, (float)GoalDx) * (180.f / 3.14159265f));
                        Look.Roll = 0.0;
                        Pawn->K2_SetActorRotation(Look, false);
                    }

                    GZoneOrdersIssued++;
                }

                {
                    static std::map<void*, FVector> ZoneMoveOrigin;
                    static std::set<void*> ZoneStuck;

                    auto Origin = ZoneMoveOrigin.find(Pawn);

                    if (bGoalMoved || bDueAgain || Origin == ZoneMoveOrigin.end())
                    {
                        if (Origin != ZoneMoveOrigin.end())
                        {
                            const double MovedX = MyLoc.X - Origin->second.X;
                            const double MovedY = MyLoc.Y - Origin->second.Y;

                            if ((MovedX * MovedX + MovedY * MovedY) < (200.0 * 200.0))
                                ZoneStuck.insert(Pawn);
                            else
                                ZoneStuck.erase(Pawn);
                        }

                        ZoneMoveOrigin[Pawn] = MyLoc;
                    }

                    if (ZoneStuck.find(Pawn) != ZoneStuck.end() && Pawn->HasCharacterMovement() && Pawn->CharacterMovement &&
                        !BotLedgeAhead(Pawn, MyLoc, Hop.X - MyLoc.X, Hop.Y - MyLoc.Y))
                    {
                        double GoalDx = Hop.X - MyLoc.X;
                        double GoalDy = Hop.Y - MyLoc.Y;
                        double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                        if (GoalLen > 1.0)
                        {
                            FVector Push{};
                            Push.X = GoalDx / GoalLen;
                            Push.Y = GoalDy / GoalLen;
                            Push.Z = 0.0;
                            Pawn->AddMovementInput(Push, 1.f, false);

                            if ((reportCounter_forDir % 22) == 0)
                            {
                                FVector Hopup{};
                                Hopup.X = Push.X * 120.0;
                                Hopup.Y = Push.Y * 120.0;
                                Hopup.Z = 520.0;
                                Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);
                            }
                        }
                    }
                }

                continue;
            }

            if (bInCombat)
                continue;

            if (BotHasLanded((void*)Pawn) && TickBotHealing(Controller, PlayerState, Pawn))
                continue;

            if (BotHasLanded((void*)Pawn) && TickBotLooting(World, Controller, PlayerState, Pawn, AIController, MyLoc))
                continue;

            {

                auto it = BotWanderDirTick.find(Pawn);

                // Keep walking to the same destination until the bot arrives (or ~20 s pass).
                auto BodyForArrival = BotBodies.find((void*)Pawn);
                const bool bArrived = BodyForArrival != BotBodies.end() && !BodyForArrival->second.bHasGoal;

                bool bNeedsNewDest = (it == BotWanderDirTick.end()) || bArrived ||
                    ((reportCounter_forDir - it->second) > 600);
                if (bNeedsNewDest)
                {
                    BotWanderDirTick[Pawn] = reportCounter_forDir;

                    FVector Dest = PickWalkableWanderDest(Pawn, MyLoc);

                    if (StormPlan.bKnown)
                    {
                        const double Cx = Dest.X - StormPlan.Centre.X;
                        const double Cy = Dest.Y - StormPlan.Centre.Y;
                        const double CLen = sqrt(Cx * Cx + Cy * Cy);
                        const double Limit = (double)StormPlan.Radius * 0.75;

                        if (CLen > Limit && CLen > 1.0)
                        {
                            Dest.X = StormPlan.Centre.X + (Cx / CLen) * Limit;
                            Dest.Y = StormPlan.Centre.Y + (Cy / CLen) * Limit;
                        }
                    }

                    BotWanderGoals[Pawn] = Dest;

                    uint8_t moveResult = BotMoveTo(AIController, Pawn, Dest, 50.f);

                    if (moveResult == 0) MoveResult_Failed++;
                    else if (moveResult == 1) MoveResult_AlreadyAtGoal++;
                    else if (moveResult == 2) MoveResult_Success++;
                    else MoveResult_Other++;
                }

                {
                    static std::map<void*, FVector> WanderOrigin;
                    static std::map<void*, int> WanderStuckSince;

                    auto Origin = WanderOrigin.find(Pawn);

                    if (Origin == WanderOrigin.end())
                    {
                        WanderOrigin[Pawn] = MyLoc;
                    }
                    else if ((reportCounter_forDir % 20) == 0)
                    {
                        const double MovedX = MyLoc.X - Origin->second.X;
                        const double MovedY = MyLoc.Y - Origin->second.Y;
                        const bool bBarelyMoved = (MovedX * MovedX + MovedY * MovedY) < (150.0 * 150.0);

                        if (bBarelyMoved)
                            WanderStuckSince[Pawn] = WanderStuckSince.count(Pawn) ? WanderStuckSince[Pawn] + 1 : 1;
                        else
                            WanderStuckSince.erase(Pawn);

                        WanderOrigin[Pawn] = MyLoc;
                    }

                    auto StuckIt = WanderStuckSince.find(Pawn);

                    if (StuckIt != WanderStuckSince.end() && StuckIt->second >= 1 &&
                        Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
                    {
                        auto GoalIt = BotWanderGoals.find(Pawn);

                        if (GoalIt != BotWanderGoals.end())
                        {
                            double GoalDx = GoalIt->second.X - MyLoc.X;
                            double GoalDy = GoalIt->second.Y - MyLoc.Y;
                            double GoalLen = sqrt(GoalDx * GoalDx + GoalDy * GoalDy);

                            if (GoalLen > 1.0 && !BotLedgeAhead(Pawn, MyLoc, GoalDx, GoalDy))
                            {
                                FVector Push{};
                                Push.X = GoalDx / GoalLen;
                                Push.Y = GoalDy / GoalLen;
                                Push.Z = 0.0;
                                Pawn->AddMovementInput(Push, 1.f, false);

                                if ((reportCounter_forDir % 8) == 0)
                                {
                                    const double FaceYaw = atan2(GoalDy, GoalDx) * (180.0 / 3.14159265358979323846);
                                    FRotator Face = Pawn->K2_GetActorRotation();
                                    Face.Yaw = FaceYaw;
                                    Pawn->K2_SetActorRotation(Face, true);
                                }

                                if ((reportCounter_forDir % 24) == 0)
                                {
                                    FVector Hopup{};
                                    Hopup.X = Push.X * 120.0;
                                    Hopup.Y = Push.Y * 120.0;
                                    Hopup.Z = 520.0;
                                    Pawn->LaunchCharacterJump(Hopup, false, nullptr, true);
                                }
                            }
                        }

                        if (StuckIt->second >= 3)
                        {
                            BotWanderDirTick.erase(Pawn);
                            BotWanderGoals.erase(Pawn);
                            WanderStuckSince.erase(Pawn);
                            GWanderUnstuck++;
                        }
                    }
                }
            }
        }

        if (FConfiguration::bBotLog && (reportCounter_forDir % 150) == 0 && GZoneBotsInStorm > 0)
        {
            LogLine("[ZONE] In the storm: %d | closing on the circle: %d | not moving: %d | orders given: %d (straight-line, no navmesh)",
                GZoneBotsInStorm, GZoneBotsClosing, GZoneBotsStuck, GZoneOrdersIssued);
        }

        if (FConfiguration::bBotLog && (reportCounter_forDir % 300) == 0 && GCombatEngagements > 0)
        {
            LogLine("[COMBAT] Engagements: %d | bursts fired: %d | shots that actually landed bot-on-bot: %d | "
                "tried to fire holding nothing: %d | weapons handed back: %d | reloads: %d | magazines refilled: %d | "
                "unstuck while wandering: %d",
                GCombatEngagements, GCombatBurstsFired, GCombatBotOnBotHits,
                GCombatNoWeapon, GWeaponsReissued, GReloadsStarted, GMagazinesRefilled, GWanderUnstuck);

            LogEliminationSummary();
            LogReloadSummary();
            LogBotDamageSummary();
            LogBotLootSummary();

            int LockedNow = 0;
            for (auto& Lock : BotLockedOnName)
                if (LivePawns.find(Lock.first) != LivePawns.end())
                    LockedNow++;

            LogLine("[LOCK] Locked on: %d | ended because the target died: %d | locked right now: %d | bursts cut off when the target "
                "went out of sight: %d | ticks holding fire with the target in range but out of sight: %d | deliberate misses: %d | "
                "hops while chasing: %d",
                GLocksMade, GLocksEnded, LockedNow, GBurstsCutOutOfSight, GTicksHeldFireOutOfSight, GAimDeliberateMisses, GChaseHops);

            LogLine("[TRIGGER] Ticks a bot's gun actually went off: in its own bursts %d | with nothing in the bot code pulling the "
                "trigger %d (let go of every time -- this should stay at 0).",
                GShotTicksOurs, GShotTicksStray);

            LogLine("[FIRE] Bursts watched to the end: %d | the gun lost rounds in %d of them | trigger: %s | bots with the player "
                "controller out of their ability system: %zu (cleared %d time(s) -- the game puts it back on every new pawn) | "
                "camera kept on the eyes: %s | full bursts that fired nothing, the gun saying: \"equipping\" %d, \"reloading\" %d, "
                "neither %d.",
                GBurstsChecked, GBurstsThatFired,
                GTriggerMode == 1 ? "as a local player" : (GTriggerMode == 2 ? "plain (neither way fired)" : "plain"),
                GBotsRunOnServer.size(), GActorInfoCleared, (GCameraResolved && GCamManagerOffset >= 0) ? "yes" : "no",
                GDryBurstsEquipping, GDryBurstsReloading, GDryBurstsNeither);

            LogLine("[EYES] Straight-line sight checks: clear %d, blocked %d%s%s.",
                GSightByLineClear, GSightByLineBlocked,
                GSightFirstBlocker.empty() ? "" : " | the first thing that was in the way: ", GSightFirstBlocker.c_str());

            LogLine("[AIM] Mid-burst, the gun was still pointed within 2 degrees of where the bot code aimed it on %d of %d checks "
                "(off by %.1f degrees on average, worst %.0f) | built-in senses switched off on %d bot(s).",
                GAimHeld, GAimChecks, GAimChecks > 0 ? GAimOffSum / (double)GAimChecks : 0.0, GAimOffMax, GSensesSilencedBots);

            LogLine("[HITS] Bot-on-bot hits on the one the shooter is locked onto: %d | on somebody else: %d | ticks a shot was "
                "held because the ground was in the way: %d",
                GHitsOnLockedTarget, GHitsOnSomebodyElse, GTerrainBlocks);
        }

        GZoneBotsInStorm = 0;
        GZoneBotsClosing = 0;
        GZoneBotsStuck = 0;

        reportCounter_forDir++;

        static int reportCounter = 0;
        if (((++reportCounter) % 900) == 0 && (nativeBrainCount + fallbackCount) > 0)
        {
            int movers = 0, stuck = 0;
            double totalDist = 0.0;
            double maxDist = 0.0;

            for (int i = 0; i < GameState->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                if (!PS || !PS->HasbIsABot() || !PS->bIsABot) continue;
                auto PC = (AFortPlayerControllerAthena*)PS->Owner;
                if (!PC) continue;
                auto Pw = (AFortPlayerPawnAthena*)PC->Pawn;
                if (!Pw) continue;

                FVector Now = Pw->K2_GetActorLocation();
                auto prev = MovementProbe.find((void*)Pw);
                if (prev != MovementProbe.end())
                {
                    double dx = Now.X - prev->second.X;
                    double dy = Now.Y - prev->second.Y;
                    double dz = Now.Z - prev->second.Z;
                    double d = sqrt(dx * dx + dy * dy + dz * dz);
                    totalDist += d;
                    if (d > maxDist) maxDist = d;
                    if (d > 200.0) movers++; else stuck++;
                }
                MovementProbe[(void*)Pw] = Now;
            }

            int probed = movers + stuck;
            LogLine("[BotAI] Movement: %d native-brain, %d fallback | moved-since-last-report: %d, stationary: %d, avg %.0f units, max %.0f units\n",
                nativeBrainCount, fallbackCount, movers, stuck,
                probed > 0 ? (float)(totalDist / probed) : 0.f, (float)maxDist);

            LogLine("[BotAI] Wander MoveOrders(total): success=%d failed=%d alreadyAtGoal=%d other=%d | all straight-line (navmesh removed)\n",
                MoveResult_Success, MoveResult_Failed, MoveResult_AlreadyAtGoal, MoveResult_Other);
        }
    }

    static int ReadBoolFunction(UObject* Object, const char* Name)
    {
        auto Fn = Object ? Object->GetFunction(Name) : nullptr;
        if (!Fn)
            return -1;

        alignas(16) uint8 Buffer[0x40] = {};
        Object->ProcessEvent(Fn, Buffer);
        return Buffer[0] ? 1 : 0;
    }

    // Names of the properties of an object's class, by their offset.
    static const std::map<uint32, std::string>& PropNamesByOffset(const UObject* Obj)
    {
        static std::map<const void*, std::map<uint32, std::string>> Cache;
        auto& Names = Cache[(const void*)Obj->Class];

        if (!Names.empty())
            return Names;

        for (const UStruct* Cls = (const UStruct*)Obj->Class; Cls; Cls = Cls->GetSuper())
        {
            for (const UField* Child = Cls->GetChildren(); Child; Child = Child->GetNext())
            {
                if (!Child->Class || (Child->Class->GetCastFlags() & 0x80000))
                    continue;   // skip functions

                auto Raw = Child->GetName().ToString();
                const std::string Name(Raw.c_str());
                const uint32 Off = ((UObject*)Obj)->GetOffset(Name.c_str());

                if (Off == (uint32)-1 || Off > 0x2000)
                    continue;

                auto& Slot = Names[Off];
                if (!Slot.empty())
                    Slot += "/";
                Slot += Name;
            }
        }

        return Names;
    }

    // Watches what a REAL player's pickaxe does to the wall in front of them and logs every field that changed.
    // Compares an object's memory with the last snapshot of it and returns one line per changed 4-byte slot.
    static std::string DiffSnapshot(const UObject* Obj, std::map<void*, std::vector<uint8>>& Snaps, int& OutCount)
    {
        OutCount = 0;

        if (!Obj || !Obj->Class)
            return std::string();

        uint32 Size = (uint32)Obj->Class->GetPropertiesSize();
        if (Size < 0x40 || Size > 0x2000)
            Size = 0x400;

        std::vector<uint8> Now(Size);
        memcpy(Now.data(), (const void*)Obj, Size);

        std::string Changes;
        auto Old = Snaps.find((void*)Obj);

        if (Old != Snaps.end() && Old->second.size() == Size)
        {
            const auto& Names = PropNamesByOffset(Obj);

            for (uint32 Off = 0x30; Off + 4 <= Size && OutCount < 30; Off += 4)
            {
                if (memcmp(&Old->second[Off], &Now[Off], 4) == 0)
                    continue;

                uint32 A = 0, B = 0;
                float FA = 0.f, FB = 0.f;
                memcpy(&A, &Old->second[Off], 4);
                memcpy(&B, &Now[Off], 4);
                memcpy(&FA, &A, 4);
                memcpy(&FB, &B, 4);

                std::string Name = "?";
                for (uint32 Back = 0; Back < 4; Back++)
                {
                    auto Found = Names.find(Off - Back);
                    if (Found != Names.end())
                    {
                        Name = Found->second;
                        break;
                    }
                }

                char Line[256];
                snprintf(Line, sizeof(Line), "    +0x%X [%s] 0x%08X -> 0x%08X (float %.2f -> %.2f)\n", Off, Name.c_str(), A, B, FA, FB);
                Changes += Line;
                OutCount++;
            }
        }

        Snaps[(void*)Obj] = Now;
        return Changes;
    }

    // Watches what a REAL player's pickaxe swing does to the wall in front of them (and to the pickaxe itself)
    // and logs every field that changed, so the bots' swing can be made to do the same.
    static void LogHumanMiningReference(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        static std::map<void*, std::vector<uint8>> WallSnapshots;
        static std::map<void*, std::vector<uint8>> PickaxeSnapshots;
        static std::map<void*, float> LastHealth;
        static int Events = 0;

        if (Events >= 25)
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || (PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS) || !PS->Owner)
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;
            if (!Pawn || !BotHoldingMelee(Pawn))
                continue;

            const FVector At = Pawn->K2_GetActorLocation();
            const FRotator Look = Controller->GetControlRotation();
            const double R = 3.14159265358979323846 / 180.0;

            FVector Eyes(At.X, At.Y, At.Z + 60.0);
            FVector End(Eyes.X + cos(Look.Pitch * R) * cos(Look.Yaw * R) * 350.0,
                Eyes.Y + cos(Look.Pitch * R) * sin(Look.Yaw * R) * 350.0,
                Eyes.Z + sin(Look.Pitch * R) * 350.0);

            const FBotHit Hit = BotTrace((AActor*)Pawn, Eyes, End, 0);
            if (!Hit.bHit || !Hit.Actor || !Hit.Actor->IsA<ABuildingSMActor>())
                continue;

            AActor* Wall = Hit.Actor;
            AActor* Pickaxe = (Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon) ? (AActor*)Pawn->CurrentWeapon : nullptr;

            const float Health = BuildingHealth(Wall);
            auto Prev = LastHealth.find((void*)Wall);
            const bool bHealthDropped = Prev != LastHealth.end() && Health < Prev->second - 0.5f;
            const float HealthBefore = Prev != LastHealth.end() ? Prev->second : Health;
            LastHealth[(void*)Wall] = Health;

            int WallCount = 0, PickaxeCount = 0;
            const std::string WallChanges = DiffSnapshot((const UObject*)Wall, WallSnapshots, WallCount);
            const std::string PickaxeChanges = Pickaxe ? DiffSnapshot((const UObject*)Pickaxe, PickaxeSnapshots, PickaxeCount) : std::string();

            if (WallCount > 0 || PickaxeCount > 0)
            {
                Events++;

                LogLine("[SWING-FX] Real swing on %s (%s)%s | wall health %.0f -> %.0f | %d field(s) changed on the wall, %d on the pickaxe (%s):\n"
                    "  WALL:\n%s  PICKAXE:\n%s",
                    BotObjectName((const UObject*)Wall).c_str(), BotClassName((const UObject*)Wall).c_str(),
                    bHealthDropped ? " [damage landed]" : "", HealthBefore, Health, WallCount, PickaxeCount,
                    Pickaxe ? BotObjectName((const UObject*)Pickaxe).c_str() : "no pickaxe",
                    WallChanges.empty() ? "    (nothing)\n" : WallChanges.c_str(),
                    PickaxeChanges.empty() ? "    (nothing)\n" : PickaxeChanges.c_str());
            }

            return;   // one wall per tick is enough
        }
    }

    static void LogHumanMovementState(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        static int Lines = 0;
        static int LastStyle = -999, LastSprinting = -999;
        static float LastSpeed = -1.f;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || (PS->HasbIsABot() && PS->bIsABot) || NPCs::IsNPC(PS) || !PS->Owner)
                continue;

            auto Pawn = (AFortPlayerPawnAthena*)((AFortPlayerControllerAthena*)PS->Owner)->Pawn;
            if (!Pawn || !Pawn->HasCharacterMovement() || !Pawn->CharacterMovement)
                continue;

            static int32 StyleOff = -2;
            if (StyleOff == -2)
                StyleOff = (int32)((UObject*)Pawn)->GetOffset("CurrentMovementStyle");

            const int Style = (StyleOff > 0 && StyleOff < 0x10000) ? (int)GetFromOffset<uint8>((UObject*)Pawn, (uint32)StyleOff) : -1;
            const int Sprinting = ReadBoolFunction((UObject*)Pawn, "IsSprinting");
            const float Speed = Pawn->CharacterMovement->HasMaxWalkSpeed() ? (float)Pawn->CharacterMovement->MaxWalkSpeed : -1.f;

            if (Style == LastStyle && Sprinting == LastSprinting && fabsf(Speed - LastSpeed) < 1.f)
                return;

            LastStyle = Style;
            LastSprinting = Sprinting;
            LastSpeed = Speed;

            if (Lines++ < 60)
                LogLine("[SPRINT-REF] CurrentMovementStyle=%d IsSprinting=%d MaxWalkSpeed=%.0f", Style, Sprinting, Speed);

            return;
        }
    }

    struct FBotSafety
    {
        FVector SafePos{};
        bool bHaveSafe = false;
        int SafeTick = -1000;
        int RescueCheckTick = -1000;

        bool bWasAlive = false;
        bool bLastLanded = false;
        bool bReported = false;
        FVector LastLoc{};
        double LastVelZ = 0.0;
        int LastMode = 0;
        bool bLastOutside = false;
        float LastHealth = 0.f;
        float LastShield = 0.f;
    };

    static std::map<void*, FBotSafety> BotSafety;
    static int GFallRescues = 0;

    // Remembers the last solid ground each bot stood on, puts a bot back there if it falls into the void,
    // and says WHY a bot died (shot / fell / storm).
    static void TrackBotSafety(AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->HasPlayerArray())
            return;

        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
            if (!PS || !PS->HasbIsABot() || !PS->bIsABot || NPCs::IsNPC(PS))
                continue;

            auto Controller = (AFortPlayerControllerAthena*)PS->Owner;
            if (!Controller || !IsPlayerControllerBot((AActor*)Controller))
                continue;

            FBotSafety& S = BotSafety[(void*)Controller];
            auto Pawn = (AFortPlayerPawnAthena*)Controller->Pawn;

            if (!Pawn || Pawn->GetHealth() <= 0.f)
            {
                if (S.bWasAlive && S.bLastLanded && !S.bReported && !Controller->IsInAircraft() && !BotIsBeingFixed((void*)Controller))
                {
                    S.bReported = true;

                    auto HitIt = LastPlayerDamageTick.find((void*)Controller);
                    const int SinceHit = (HitIt != LastPlayerDamageTick.end()) ? (GStormTick - HitIt->second) : 100000;
                    const double Fell = S.bHaveSafe ? (S.SafePos.Z - S.LastLoc.Z) : 0.0;

                    const char* Verdict = "UNKNOWN";
                    if (SinceHit < 150)
                        Verdict = "shot by a player or bot";
                    else if (S.LastMode == 3 && (Fell > 500.0 || S.LastVelZ < -1500.0))
                        Verdict = "FELL";
                    else if (S.bLastOutside)
                        Verdict = "STORM";

                    LogLine("[DEATHCAUSE] '%s' died: %s | last seen at X=%.0f Y=%.0f Z=%.0f, %.0f units below its last solid ground, "
                        "movement mode %d (1 walking, 3 falling, 4 swimming), vertical speed %.0f | health %.0f shield %.0f a tick "
                        "before | outside the circle: %s | ticks since a player or bot damaged it: %d.",
                        SafeNameOf(PS).c_str(), Verdict, (float)S.LastLoc.X, (float)S.LastLoc.Y, (float)S.LastLoc.Z, (float)Fell,
                        S.LastMode, (float)S.LastVelZ, S.LastHealth, S.LastShield, S.bLastOutside ? "YES" : "no", SinceHit);
                }

                S.bWasAlive = false;
                continue;
            }

            S.bReported = false;

            const FVector Loc = Pawn->K2_GetActorLocation();
            uint8 Mode = 0;
            double VelZ = 0.0;

            if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
            {
                auto Move = Pawn->CharacterMovement;

                if (Move->HasMovementMode())
                    Mode = Move->MovementMode;
                if (Move->HasVelocity())
                    VelZ = (double)Move->Velocity.Z;
            }

            const bool bGliding = GlidingBots.find((void*)Pawn) != GlidingBots.end();
            const bool bLanded = BotHasLanded((void*)Pawn);

            if (bLanded && !bGliding)
            {
                if (Mode == 1 && (GStormTick - S.SafeTick) >= 30)
                {
                    S.SafePos = FVector(Loc.X, Loc.Y, Loc.Z);
                    S.bHaveSafe = true;
                    S.SafeTick = GStormTick;
                }

                if (Mode == 3 && S.bHaveSafe && (GStormTick - S.RescueCheckTick) >= 10)
                {
                    S.RescueCheckTick = GStormTick;

                    const double Fell = S.SafePos.Z - Loc.Z;
                    bool bRescue = Fell > 8000.0;

                    if (!bRescue && Fell > 1200.0 && BotTraceAvailable())
                    {
                        double G = 0.0;
                        bRescue = !BotGroundBelow(Pawn, Loc.X, Loc.Y, Loc.Z, Loc.Z - 40000.0, G);   // nothing under it: void
                    }

                    if (bRescue)
                    {
                        FVector To(S.SafePos.X, S.SafePos.Y, S.SafePos.Z + 30.0);
                        Pawn->K2_SetActorLocation(To, false, (void*)nullptr, true);

                        if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement && Pawn->CharacterMovement->HasVelocity())
                        {
                            FVector Zero(0.0, 0.0, 0.0);
                            Pawn->CharacterMovement->Velocity = Zero;
                        }

                        auto BodyIt = BotBodies.find((void*)Pawn);
                        if (BodyIt != BotBodies.end())
                            BodyIt->second.bHasGoal = false;

                        if (++GFallRescues <= 10)
                            LogLine("[DEATHCAUSE] '%s' was falling out of the world (%.0f units below its last ground) -- put back on it "
                                "(%d rescue(s) so far).", SafeNameOf(PS).c_str(), (float)Fell, GFallRescues);

                        continue;
                    }
                }
            }

            S.bWasAlive = true;
            S.bLastLanded = bLanded;
            S.LastLoc = FVector(Loc.X, Loc.Y, Loc.Z);
            S.LastVelZ = VelZ;
            S.LastMode = (int)Mode;
            S.bLastOutside = Pawn->HasbIsInsideSafeZone() && !Pawn->bIsInsideSafeZone;
            S.LastHealth = Pawn->GetHealth();
            S.LastShield = Pawn->GetShield();
        }
    }

    static void GameThreadTick_Inner();

    static int SehFaultCount = 0;

    void GameThreadTick()
    {
        FCrashReporter::EnterGuardedSection();

        __try
        {
            GameThreadTick_Inner();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SehFaultCount++;
            if (SehFaultCount <= 20)
            {
                LogLine("[GUARD] Caught an access violation inside the bot tick (fault #%d). Server kept alive; "
                    "this tick was skipped. Repeated faults point at Epic's native AI running with "
                    "missing skillset data.", SehFaultCount);
            }
            if (SehFaultCount == 21)
                LogLine("[GUARD] Further faults will not be logged individually.");
        }

        FCrashReporter::LeaveGuardedSection();
    }

    static void GameThreadTick_Inner()
    {
        static bool bAnnounced = false;
        if (!bAnnounced)
        {
            bAnnounced = true;
            LogLine("[BotAI] Game-thread tick is running.\n");

            FindObject<UObject>(L"/Game/Athena/GameplayCueNotifies/GCN_Athena_OutsideSafeZoneDamage.GCN_Athena_OutsideSafeZoneDamage_C");
            FindObject<UObject>(L"/Game/Athena/GameplayCueNotifies/GCN_Athena_PetrolPickup_EnterWater.GCN_Athena_PetrolPickup_EnterWater_C");
            FindObject<UObject>(L"/Game/Athena/GameplayCueNotifies/GCN_Athena_PetrolPickup_HitWorld.GCN_Athena_PetrolPickup_HitWorld_C");
            LogLine("[BotAI] Preloaded storm-damage gameplay cue.\n");
        }

        KeepTheClockHonest();

        NPCs::GameThreadTick();

        if (FConfiguration::bDisableStormSurge)
        {
            auto SurgeWorld = UWorld::GetWorld();

            if (SurgeWorld && SurgeWorld->AuthorityGameMode)
            {
                auto SurgeMode = (AFortGameMode*)SurgeWorld->AuthorityGameMode;

                static int Reported = 0;
                static bool bFoundAny = false;

                if (SurgeMode->HasbDisableStormCapSystem() && !SurgeMode->bDisableStormCapSystem)
                {
                    SurgeMode->bDisableStormCapSystem = true;
                    bFoundAny = true;
                }

                if (SurgeMode->HasbStormCapSystemEnabled() && SurgeMode->bStormCapSystemEnabled)
                {
                    SurgeMode->bStormCapSystemEnabled = false;
                    bFoundAny = true;
                }

                if (SurgeMode->HasPlaylistSupportsStormCapSystem() && SurgeMode->PlaylistSupportsStormCapSystem)
                {
                    SurgeMode->PlaylistSupportsStormCapSystem = false;
                    bFoundAny = true;
                }

                if (SurgeWorld->GameState)
                {
                    auto SurgeState = (AFortGameStateAthena*)SurgeWorld->GameState;
                    if (SurgeState->HasStormCapState() && SurgeState->StormCapState != 0)
                    {
                        uint8 Off = 0;
                        SurgeState->StormCapState = Off;
                        bFoundAny = true;
                    }
                }

                if (bFoundAny && ++Reported == 1)
                    LogLine("[SURGE] Storm surge turned off (Epic calls it the Storm Cap system). It will be held off for the whole match.");
            }
        }

        if (FConfiguration::AutoFillTargetPlayerCount <= 0)
            return;

        auto World = UWorld::GetWorld();
        if (!World || !World->GameState || !World->AuthorityGameMode)
            return;

        auto GameState = (AFortGameStateAthena*)World->GameState;
        auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
        if (!GameState->HasPlayerArray() || !GameMode->HasAlivePlayers())
            return;

        float Now = (float)UGameplayStatics::GetTimeSeconds(World);

        GStormTick++;

        KeepBotsOnTheirOwnPawns(GameState);

        PlaceDump::EnsureLoaded();
        PlaceDump::ReportOnce();
        PlaceDump::AnnounceHotkey();
        PlaceDump::RecordTick();

        if (!FConfiguration::bDisableStorm)
        {
            SilenceDistantStorm();
            EnsureStormTimingData(GameState);
            ApplyCustomZoneTimings(GameMode);
            LogStormState(GameMode, GameState);
            LogStormChanges(GameMode, GameState);

            WipeTheSlateAtBusTime(GameState);

            TrackEveryBotAlive(GameState, GStormTick);

            ApplyStormDamage(GameMode, GameState, GStormTick);
        }

        if (FConfiguration::bDisableStorm)
        {
            float FarFutureTime = Now + 999999999.f;

            if (GameState->HasSafeZonesStartTime())
                GameState->SafeZonesStartTime = Now + 60.f;

            if (GameMode->HasSafeZonePhase())
                GameMode->SafeZonePhase = 0;

            if (GameMode->HasbSafeZonePaused())
                GameMode->bSafeZonePaused = true;
            if (GameMode->HasbSafeZoneActive())
                GameMode->bSafeZoneActive = false;

            if (GameMode->SafeZoneIndicator)
            {
                auto Ind = GameMode->SafeZoneIndicator;

                if (Ind->HasSafeZoneStartShrinkTime())
                    Ind->SafeZoneStartShrinkTime = FarFutureTime;
                if (Ind->HasSafeZoneFinishShrinkTime())
                    Ind->SafeZoneFinishShrinkTime = FarFutureTime;
                if (Ind->HasCurrentPhase())
                    Ind->CurrentPhase = 0;

                if (Ind->HasNextRadius())
                    Ind->NextRadius = 1000000.f;
                if (Ind->HasNextNextRadius())
                    Ind->NextNextRadius = 1000000.f;
                if (Ind->HasPreviousRadius())
                    Ind->PreviousRadius = 1000000.f;
                if (Ind->HasLastRadius())
                    Ind->LastRadius = 1000000.f;
            }

            static bool bAnnouncedStormOff = false;
            if (!bAnnouncedStormOff)
            {
                bAnnouncedStormOff = true;
                LogLine("[STORM] Freeze active. SafeZonesStartTime=%s SafeZonePhase=%s Paused=%s Active=%s Indicator=%s",
                    GameState->HasSafeZonesStartTime() ? "pinned" : "PROPERTY MISSING",
                    GameMode->HasSafeZonePhase() ? "pinned" : "PROPERTY MISSING",
                    GameMode->HasbSafeZonePaused() ? "set" : "PROPERTY MISSING",
                    GameMode->HasbSafeZoneActive() ? "set" : "PROPERTY MISSING",
                    GameMode->SafeZoneIndicator ? "present" : "null (warmup)");
            }
        }
        else if (GameMode->SafeZoneIndicator)
        {
            auto Ind = GameMode->SafeZoneIndicator;

            static int LastStormPhaseSeen = -1;

            if (GameMode->HasSafeZonePhase() && Ind->HasSafeZoneStartShrinkTime() && Ind->HasSafeZoneFinishShrinkTime())
            {
                const int StormPhaseNow = (int)GameMode->SafeZonePhase;

                if (StormPhaseNow != LastStormPhaseSeen)
                {
                    LastStormPhaseSeen = StormPhaseNow;

                    const bool bDegenerate = !((float)Ind->SafeZoneFinishShrinkTime > Now + 1.f);

                    const float LastR = Ind->HasLastRadius() ? (float)Ind->LastRadius : -1.f;
                    const float NextR = Ind->HasNextRadius() ? (float)Ind->NextRadius : -1.f;
                    const bool bWallStays = LastR > 0.f && NextR > 0.f && NextR >= LastR - 1.f;

                    if (bDegenerate && (StormPhaseNow <= 0 || bWallStays))
                    {
                        LogLine("[Storm] Phase %d is the game's no-storm-yet step (radius %.0f -> %.0f, no time on it) -- left alone, so "
                            "the game goes straight on to the first real circle instead of an empty 30s countdown.",
                            StormPhaseNow, LastR, NextR);
                    }
                    else if (bDegenerate)
                    {

                        float RescueWait = 0.f, RescueShrink = 0.f;
                        SeasonOneStormTiming(StormPhaseNow, RescueWait, RescueShrink);

                        Ind->SafeZoneStartShrinkTime = Now + RescueWait;
                        Ind->SafeZoneFinishShrinkTime = Now + RescueWait + RescueShrink;

                        LogLine("[Storm] Phase %d arrived with its schedule already over (radius %.0f -> %.0f) -- given Season 1's timing "
                            "for phase %d: %.0fs hold, %.0fs close, so it cannot run away.",
                            StormPhaseNow, LastR, NextR, StormPhaseNow, RescueWait, RescueShrink);
                    }
                    else if (FConfiguration::bStormLog)
                    {
                        printf("[Storm] Tick check on phase %d: schedule looks fine -- starts closing in %.1fs, done in %.1fs. Left alone.\n",
                            StormPhaseNow,
                            (float)Ind->SafeZoneStartShrinkTime - Now,
                            (float)Ind->SafeZoneFinishShrinkTime - Now);
                        fflush(stdout);
                    }
                }
            }
        }

        {
            static bool bAnnouncedWaiting = false;
            int humanCount = 0;
            for (int i = 0; i < GameState->PlayerArray.Num(); i++)
            {
                auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];
                if (PS && !(PS->HasbIsABot() && PS->bIsABot))
                    humanCount++;
            }

            static float FirstTickTime = -1.f;
            if (FirstTickTime < 0.f)
                FirstTickTime = Now;
            const bool bWaitedLongEnoughNoHuman = (Now - FirstTickTime) >= 20.f;

            if (humanCount == 0 && !bBusDeparted && !bWaitedLongEnoughNoHuman)
            {
                if (!bAnnouncedWaiting)
                {
                    bAnnouncedWaiting = true;
                    LogLine("Waiting for a player to join before spawning bots...");
                }
                return;
            }
            if (humanCount == 0 && bWaitedLongEnoughNoHuman)
            {
                static bool bForced = false;
                if (!bForced)
                {
                    bForced = true;
                    LogLine("[BotAI] No human detected after 20s -- starting bot fill anyway.");
                }
            }
        }

        if (!bBusDeparted && bBotSpawningPaused)
        {
            static bool bSaidPaused = false;
            static int LastSeenCount = -1;

            const int WaitingCount = GameMode->AlivePlayers.Num();

            if (!bSaidPaused || WaitingCount != LastSeenCount)
            {
                bSaidPaused = true;
                LastSeenCount = WaitingCount;
                LogLine("[BotAI] Bot spawning is paused -- %d player(s) in the lobby, waiting for \"cheat resume bot spawn\".",
                    WaitingCount);
            }

            BusCountdownStart = -1.f;
            LastBusSecondAnnounced = -1;
        }
        else if (!bBusDeparted)
        {
            int currentCount = GameMode->AlivePlayers.Num();

            if (currentCount >= FConfiguration::AutoFillTargetPlayerCount)
            {
                if (BusCountdownStart < 0.f)
                {
                    BusCountdownStart = Now;
                    LogLine("[BotAI] Lobby full (%d players). Bus starting in 10 seconds...", currentCount);
                    ServerConsole::Say("Player count reached %d! Starting bus...", currentCount);

                    if (GameState->HasWarmupCountdownStartTime())
                        GameState->WarmupCountdownStartTime = Now;
                    if (GameState->HasWarmupCountdownEndTime())
                        GameState->WarmupCountdownEndTime = Now + 10.f;

                    if (GameMode->HasWarmupCountdownDuration())
                        GameMode->WarmupCountdownDuration = 10.f;
                    if (GameMode->HasWarmupEarlyCountdownDuration())
                        GameMode->WarmupEarlyCountdownDuration = 10.f;

                    LogLine("[BotAI] HUD countdown set (WarmupCountdownEndTime = now + 10s).");
                }

                float remaining = 10.f - (Now - BusCountdownStart);

                int secsLeft = (int)ceil(remaining);
                if (secsLeft != LastBusSecondAnnounced && secsLeft >= 1 && secsLeft <= 10)
                {
                    LastBusSecondAnnounced = secsLeft;
                    LogLine("[BotAI] Bus starting in %d...", secsLeft);
                }

                if (remaining <= 0.f)
                {
                    LogLine("[BotAI] Bus launching now.");
                    ClearBusForLaunch("the lobby filled up and the countdown finished");
                    UKismetSystemLibrary::ExecuteConsoleCommand(World, FString(L"startaircraft"), nullptr);
                    bBusDeparted = true;
                    BusStartedTime = Now;
                }
            }
            else if ((Now - LastFillTime) >= (float)FConfiguration::AutoFillIntervalSeconds)
            {
                LastFillTime = Now;
                int remaining = FConfiguration::AutoFillTargetPlayerCount - currentCount;
                int batch = remaining < FConfiguration::AutoFillBatchSize ? remaining : FConfiguration::AutoFillBatchSize;
                SpawnBots(FVector(0, 0, 0), batch);
                LogLine("[BotAI] AutoFill: %d/%d players.\n", currentCount, FConfiguration::AutoFillTargetPlayerCount);
            }
        }

        int BotsInLobby = 0;
        for (int i = 0; i < GameState->PlayerArray.Num(); i++)
        {
            auto PS = (AFortPlayerStateAthena*)GameState->PlayerArray[i];

            if (PS && PS->HasbIsABot() && PS->bIsABot && !NPCs::IsNPC(PS))
                BotsInLobby++;
        }

        const bool bEveryBotThanked = (BotsInLobby > 0) && ((int)ThankedBots.size() >= BotsInLobby);
        const bool bWaitedLongEnough = bBusDeparted && (Now - BusStartedTime) >= 12.f;

        if (bBusDeparted && !bMovedToIsland && !bEveryBotThanked && bWaitedLongEnough)
        {
            static bool bSaidSo = false;
            if (!bSaidSo)
            {
                bSaidSo = true;
                LogLine("[BotAI] %d of %d bot(s) thanked the driver before the drop -- going anyway so nobody is left on the bus.",
                    (int)ThankedBots.size(), BotsInLobby);
            }
        }

        const bool bThanksDone = bEveryBotThanked || bWaitedLongEnough;

        if (bBusDeparted && !bMovedToIsland && bThanksDone)
        {
            if (MoveSomeBotsToIsland(4))
            {
                bMovedToIsland = true;
                LogLine("[BotAI] Finished moving bots onto the map.\n");
            }
        }

        {
            static int lastLoggedPhase = -999;
            if (GameMode->HasSafeZonePhase())
            {
                int phase = GameMode->SafeZonePhase;
                if (phase != lastLoggedPhase)
                {
                    lastLoggedPhase = phase;
                    LogLine("[BotAI] === STORM PHASE -> %d | AlivePlayers=%d PlayerArray=%d PlayersLeft=%d eliminated=%zu ===\n",
                        phase,
                        GameMode->AlivePlayers.Num(),
                        GameState->PlayerArray.Num(),
                        GameState->HasPlayersLeft() ? (int)GameState->PlayersLeft : -1,
                        EliminatedBots.size());
                }
            }
        }

        HoldTheStorm(GameMode, Now);
        ReleaseTheStorm(GameMode, Now);

        ApplyWeaponHotfixes();
        TopUpGrapplers(GameState, GStormTick);

        ClaimServerOwner(GameState);

        GiveFreshBotPawnsHealth(GameState);
        LogHudState();
        UpdateHudCounts();

        TrackBotSafety(GameState);
        TrackBotDamage();
        TrackBotDamage();
        RemoveDeadBots();

        CheckForVictory();
        ApplyDamageProtection();
        RegisterBotsAsAlive();
        AssignBotTeams();
        GiveBotPickaxes();
        GiveBotLoadouts();
        ThankAllBots();
        RenameUnnamedBots();
        DressUndressedBots();
        ArmLandedBots(World, GameState);
        TickMovement(World, GameState, GameMode);
        TickBotBodies(World, GameState);
        LogHumanMovementState(GameState);
        LogHumanMiningReference(GameState);
        TickParachutes(World);
        TickLandings(World);
    }
}
